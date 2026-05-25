// mcp_server.c -- Model Context Protocol (MCP 2024-11-05) JSON-RPC 2.0 server
//
// Two transport modes, selected at init time:
//
//   stdio (--mcp-stdio):
//     Background thread reads stdin; main thread writes to stdout.
//     Claude Code must be the parent process (spawns quake.exe via .mcp.json).
//
//   HTTP/SSE (--mcp-http PORT):
//     Background thread accepts TCP connections on PORT:
//       GET  /sse      -> SSE stream (server→client events)
//       POST /message  -> client→server requests, queued like stdin lines
//     Main thread writes responses as "data: {json}\n\n" SSE frames.
//     Claude Code connects to the already-running game; no spawn required.
//     .mcp.json uses { "type": "sse", "url": "http://localhost:PORT/sse" }.
//
// Tools (Phase 2 MVP + Phase 7 editor self-drive):
//   get_player_state  -- position, health, ammo, map name
//   list_entities     -- all live edicts (classname + origin)
//   set_cvar          -- set a named cvar to a string value
//   console_exec      -- queue any engine console command (Cbuf_AddText)
//   console_tail      -- last N lines from the engine console scrollback
//   editor_get_scene  -- structured JSON of the current edit_scene
//   editor_brush_add  -- append a 6-plane AABB brush at explicit mins/maxs
//   editor_entity_add -- append a point entity at explicit origin
//   editor_set_kv     -- upsert a key/value pair on a specific entity
//   editor_select     -- programmatic selection (no cursor needed)

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET mcp_raw_sock_t;
#define MCP_SOCK_INVALID  INVALID_SOCKET
#define mcp_sock_close(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
typedef int mcp_raw_sock_t;
#define MCP_SOCK_INVALID  (-1)
#define mcp_sock_close(s) close(s)
#endif

#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  define mcp_mkdir(p) _mkdir(p)
#else
#  define mcp_mkdir(p) mkdir((p), 0755)
#endif

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "quakedef.h"
#include "mcp_server.h"
#include "../platform/screenshot_path.h"
#include "edit_scene.h"        /* editor scene API for Phase 7 self-drive tools */
#include "editor.h"            /* Editor_IsOpen */
#include "edit_history.h"      /* History_Push */

// From platform/vid_sdl.c — defined alongside the other VID helpers.
extern int VID_SaveScreenshotPNG(const char *path);
extern int VID_SamplePixel(int x, int y, byte *r, byte *g, byte *b, byte *idx);

int mcp_active = 0;

// ---------------------------------------------------------------------------
// Circular request queue (background thread writes, main thread reads)
// ---------------------------------------------------------------------------

#define MCP_QUEUE_BITS  4
#define MCP_QUEUE_SIZE  (1 << MCP_QUEUE_BITS)   // 16 slots
#define MCP_QUEUE_MASK  (MCP_QUEUE_SIZE - 1)
#define MCP_LINE_MAX    4096
#define MCP_FRAME_MAX   65536      // SSE frame buffer + sync-call response slot.
                                   // Fixes the pre-existing 4 KB truncation on
                                   // large responses (e.g. list_entities).

typedef struct { char line[MCP_LINE_MAX]; } mcp_slot_t;

static mcp_slot_t   mcp_queue[MCP_QUEUE_SIZE];
static int          mcp_q_head = 0;   // write index (background thread)
static int          mcp_q_tail = 0;   // read  index (main thread)
static SDL_Mutex   *mcp_mutex  = NULL;
static SDL_Thread  *mcp_thread = NULL;

// ---------------------------------------------------------------------------
// Pending response slots — `wait_frames` parks its JSON-RPC id here and
// MCP_Frame drains them one frame at a time. Single-threaded (main thread
// only); no locking needed.
// ---------------------------------------------------------------------------

typedef struct {
    int  in_use;
    int  frames_remaining;
    int  frames_requested;
    char id_json[64];
} mcp_pending_wait_t;

#define MCP_MAX_PENDING_WAITS  8
static mcp_pending_wait_t mcp_pending_waits[MCP_MAX_PENDING_WAITS];

static void mcp_enqueue(const char *line)
{
    SDL_LockMutex(mcp_mutex);
    int next = (mcp_q_head + 1) & MCP_QUEUE_MASK;
    if (next != mcp_q_tail)  // drop silently when full
    {
        strncpy(mcp_queue[mcp_q_head].line, line, MCP_LINE_MAX - 1);
        mcp_queue[mcp_q_head].line[MCP_LINE_MAX - 1] = '\0';
        mcp_q_head = next;
    }
    SDL_UnlockMutex(mcp_mutex);
}

static int mcp_dequeue(mcp_slot_t *out)
{
    int result = 0;
    SDL_LockMutex(mcp_mutex);
    if (mcp_q_tail != mcp_q_head)
    {
        *out = mcp_queue[mcp_q_tail];
        mcp_q_tail = (mcp_q_tail + 1) & MCP_QUEUE_MASK;
        result = 1;
    }
    SDL_UnlockMutex(mcp_mutex);
    return result;
}

// ---------------------------------------------------------------------------
// HTTP/SSE transport state
// ---------------------------------------------------------------------------

static int              mcp_http_port   = 0;
static mcp_raw_sock_t   mcp_listen_sock = (mcp_raw_sock_t)MCP_SOCK_INVALID;
static mcp_raw_sock_t   mcp_sse_sock    = (mcp_raw_sock_t)MCP_SOCK_INVALID;
static SDL_Mutex       *mcp_sse_mutex   = NULL;

// `mcp_http_port` cvar — runtime control over the HTTP listener. Setting it
// non-zero brings the listener up on that port (lazy, from MCP_Frame); set
// to 0 to disable. Default is 9876 so devs can `mcp_call.py` / `curl`
// the running engine straight out of the box; matches scripts/mcp_call.py's
// hardcoded PORT. The default string lives in a mutable buffer so
// MCP_SetHttpPortDefault can override it from the --mcp-http flag before
// Cvar_RegisterVariable copies it onto the zone heap. Set-once-never-restart:
// once the listener is up, changing the cvar prints a warning and reverts
// it. Teardown / port-swap at runtime would need extra plumbing to drain
// in-flight HTTP workers -- see overnight.md.
static char    mcp_http_port_default[16] = "9876";
static cvar_t  mcp_http_port_cvar = { "mcp_http_port", mcp_http_port_default };
static int     mcp_http_started_port = 0;  // 0 = never started

static int mcp_start_http(int port);  // forward decl

// ---------------------------------------------------------------------------
// Synchronous HTTP /call slot.
//
// POST /call enqueues the JSON-RPC request and blocks the HTTP worker until
// the main thread's tool dispatch writes a response with a matching id. The
// response is then returned in the HTTP body, no SSE handshake needed --
// makes automated testing (curl, Python urllib) trivial.
//
// Only one /call may be in flight at a time; concurrent /call requests
// serialize on mcp_sync_mutex. /message (async, SSE-routed) remains the
// default for interactive Claude Code clients.
// ---------------------------------------------------------------------------
static SDL_Mutex *mcp_sync_mutex   = NULL;
static SDL_Mutex *mcp_sync_state_mu = NULL;
static char       mcp_sync_id[64]  = "";
static char       mcp_sync_resp[MCP_FRAME_MAX] = "";
static int        mcp_sync_ready    = 0;

// ---------------------------------------------------------------------------
// Background stdin reader thread (stdio mode)
// ---------------------------------------------------------------------------

static int SDLCALL mcp_reader_thread(void *userdata)
{
    (void)userdata;
    char line[MCP_LINE_MAX];
    while (fgets(line, sizeof(line), stdin))
    {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len > 0)
            mcp_enqueue(line);
    }
    return 0;
}

// Forward decls (defined further down) used by the HTTP thread to extract
// the JSON-RPC id from /call request bodies and match it later, and to
// dispatch /call requests synchronously on the HTTP worker.
static void json_extract_id(const char *json, char *id_buf, int id_bufsz);
static void mcp_dispatch(const char *line);

// ---------------------------------------------------------------------------
// Background HTTP accept thread (HTTP/SSE mode)
// ---------------------------------------------------------------------------

// Read bytes from sock until "\r\n\r\n" header terminator or buffer full.
static int http_recv_headers(mcp_raw_sock_t sock, char *buf, int bufsz)
{
    int total = 0;
    while (total < bufsz - 1)
    {
        char c;
        int r = recv((mcp_raw_sock_t)sock, &c, 1, 0);
        if (r <= 0) break;
        buf[total++] = c;
        if (total >= 4 &&
            buf[total-4] == '\r' && buf[total-3] == '\n' &&
            buf[total-2] == '\r' && buf[total-1] == '\n')
            break;
    }
    buf[total] = '\0';
    return total;
}

static int SDLCALL mcp_http_thread(void *userdata)
{
    (void)userdata;

    while (1)
    {
        struct sockaddr_in addr;
        int alen = sizeof(addr);
        mcp_raw_sock_t conn = accept(mcp_listen_sock,
                                     (struct sockaddr *)&addr, &alen);
        if (conn == (mcp_raw_sock_t)MCP_SOCK_INVALID) break;

        char hdr[4096];
        http_recv_headers(conn, hdr, sizeof(hdr));

        if (strncmp(hdr, "GET /sse", 8) == 0)
        {
            // SSE stream: send headers + endpoint event, keep socket open.
            char response[512];
            int rlen = snprintf(response, sizeof(response),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n"
                "event: endpoint\n"
                "data: http://localhost:%d/message\n"
                "\n",
                mcp_http_port);
            send(conn, response, rlen, 0);

            // Replace any existing SSE client.
            SDL_LockMutex(mcp_sse_mutex);
            if (mcp_sse_sock != (mcp_raw_sock_t)MCP_SOCK_INVALID)
                mcp_sock_close(mcp_sse_sock);
            mcp_sse_sock = conn;
            SDL_UnlockMutex(mcp_sse_mutex);
            // conn stays open — don't close here.
        }
        else if (strncmp(hdr, "OPTIONS ", 8) == 0)
        {
            const char *preflight =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Content-Length: 0\r\n"
                "\r\n";
            send(conn, preflight, (int)strlen(preflight), 0);
            mcp_sock_close(conn);
        }
        else if (strncmp(hdr, "POST /message", 13) == 0)
        {
            // Find Content-Length.
            int clen = 0;
            const char *cl = strstr(hdr, "Content-Length: ");
            if (!cl) cl = strstr(hdr, "content-length: ");
            if (cl) clen = atoi(cl + 16);

            if (clen > 0 && clen < MCP_LINE_MAX - 1)
            {
                char body[MCP_LINE_MAX];
                int got = 0;
                while (got < clen)
                {
                    int r = recv(conn, body + got, clen - got, 0);
                    if (r <= 0) break;
                    got += r;
                }
                body[got] = '\0';
                mcp_enqueue(body);
            }
            const char *ok =
                "HTTP/1.1 202 Accepted\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: 0\r\n"
                "\r\n";
            send(conn, ok, (int)strlen(ok), 0);
            mcp_sock_close(conn);
        }
        else if (strncmp(hdr, "POST /call", 10) == 0)
        {
            // Synchronous JSON-RPC: body in, response in same HTTP reply.
            // The request is dispatched DIRECTLY on this HTTP thread rather
            // than via the main-thread queue, so /call works even when the
            // main loop is stalled (window unfocused on Windows, debug
            // breakpoint, slow load). Trade-off: tool handlers read engine
            // state without holding the main thread -- safe for read-only
            // tools (get_player_state, list_entities, console_tail) and
            // mostly-safe for Cbuf_AddText (which has its own locking).
            // Avoid sending /call from a thread that races the main-thread
            // gameplay logic in ways that matter.
            //
            // Serialized via mcp_sync_mutex -- one /call at a time -- so
            // concurrent HTTP workers don't collide on the response slot.
            int clen = 0;
            const char *cl = strstr(hdr, "Content-Length: ");
            if (!cl) cl = strstr(hdr, "content-length: ");
            if (cl) clen = atoi(cl + 16);
            if (clen <= 0 || clen >= MCP_LINE_MAX - 1) {
                const char *bad =
                    "HTTP/1.1 400 Bad Request\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Content-Length: 0\r\n\r\n";
                send(conn, bad, (int)strlen(bad), 0);
                mcp_sock_close(conn);
                continue;
            }

            char body[MCP_LINE_MAX];
            int got = 0;
            while (got < clen) {
                int r = recv(conn, body + got, clen - got, 0);
                if (r <= 0) break;
                got += r;
            }
            body[got] = '\0';

            SDL_LockMutex(mcp_sync_mutex);   // one /call at a time

            char want_id[64];
            json_extract_id(body, want_id, sizeof(want_id));

            SDL_LockMutex(mcp_sync_state_mu);
            strncpy(mcp_sync_id, want_id, sizeof(mcp_sync_id) - 1);
            mcp_sync_id[sizeof(mcp_sync_id) - 1] = '\0';
            mcp_sync_resp[0] = '\0';
            mcp_sync_ready   = 0;
            SDL_UnlockMutex(mcp_sync_state_mu);

            // Direct synchronous dispatch on this thread. mcp_write_line
            // will see the parked want_id and route the response into
            // mcp_sync_resp instead of out the SSE socket.
            mcp_dispatch(body);

            int  ready;
            char resp_local[MCP_FRAME_MAX];
            resp_local[0] = '\0';
            SDL_LockMutex(mcp_sync_state_mu);
            ready = mcp_sync_ready;
            if (ready) {
                strncpy(resp_local, mcp_sync_resp, sizeof(resp_local) - 1);
                resp_local[sizeof(resp_local) - 1] = '\0';
            }
            mcp_sync_id[0]   = '\0';
            mcp_sync_resp[0] = '\0';
            mcp_sync_ready   = 0;
            SDL_UnlockMutex(mcp_sync_state_mu);

            SDL_UnlockMutex(mcp_sync_mutex);

            if (!ready) {
                // wait_frames defers its response to a later MCP_Frame --
                // not compatible with synchronous /call. Tool-side reports
                // an error via the queue; /call returns 504 here.
                const char *to =
                    "HTTP/1.1 504 Gateway Timeout\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Content-Length: 0\r\n\r\n";
                send(conn, to, (int)strlen(to), 0);
                mcp_sock_close(conn);
                continue;
            }

            int body_len = (int)strlen(resp_local);
            char hdrbuf[256];
            int hlen = snprintf(hdrbuf, sizeof(hdrbuf),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %d\r\n\r\n", body_len);
            send(conn, hdrbuf, hlen, 0);
            if (body_len > 0) send(conn, resp_local, body_len, 0);
            mcp_sock_close(conn);
        }
        else
        {
            const char *notfound =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            send(conn, notfound, (int)strlen(notfound), 0);
            mcp_sock_close(conn);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no external dependency)
// ---------------------------------------------------------------------------

// Extract string value for "key":"value" in json -> out.  Returns 1 on success.
static int json_str(const char *json, const char *key, char *out, int outsz)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    char *d = out;
    while (*p && *p != '"' && d < out + outsz - 1)
    {
        if (*p == '\\' && p[1]) p++;  // skip one escape char
        *d++ = *p++;
    }
    *d = '\0';
    return 1;
}

// Returns 1 if an "id" field exists and is not null (i.e., it's a request not a notification)
static int json_has_id(const char *json)
{
    const char *p = strstr(json, "\"id\":");
    if (!p) return 0;
    p += 5;
    while (*p == ' ') p++;
    return strncmp(p, "null", 4) != 0;
}

// Copy the raw id token (number or "string") from the JSON line into id_buf.
// Result is suitable for embedding directly into a JSON-RPC response.
static void json_extract_id(const char *json, char *id_buf, int id_bufsz)
{
    const char *p = strstr(json, "\"id\":");
    if (!p) { strncpy(id_buf, "null", id_bufsz); return; }
    p += 5;
    while (*p == ' ') p++;

    if (*p == '"')
    {
        // string id — copy including quotes
        const char *start = p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        if (*p == '"') p++;
        int len = (int)(p - start);
        if (len >= id_bufsz) len = id_bufsz - 1;
        strncpy(id_buf, start, len);
        id_buf[len] = '\0';
    }
    else
    {
        // numeric id — copy until delimiter
        const char *start = p;
        while (*p && *p != ',' && *p != '}' && *p != ' ') p++;
        int len = (int)(p - start);
        if (len >= id_bufsz) len = id_bufsz - 1;
        strncpy(id_buf, start, len);
        id_buf[len] = '\0';
    }
}

// Extract numeric value for "key":NNN (integer). Returns 1 on success.
// `arg_obj` should point at the start of the JSON object that contains the key.
static int json_int(const char *json, const char *key, int *out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    char *endp = NULL;
    long v = strtol(p, &endp, 10);
    if (endp == p) return 0;
    *out = (int)v;
    return 1;
}

// Extract a 3-element JSON number array for "key":[x,y,z] -> out[0..2].
// Returns 1 on success. Permissive: skips whitespace, accepts ints/floats.
static int json_vec3(const char *json, const char *key, float out[3])
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '[') return 0;
    p++;
    int i;
    for (i = 0; i < 3; i++)
    {
        while (*p == ' ' || *p == ',') p++;
        char *endp = NULL;
        out[i] = strtof(p, &endp);
        if (endp == p) return 0;
        p = endp;
    }
    return 1;
}

// Append src to dst (bounded), escaping " and \ for embedding in a JSON string.
// Returns updated end pointer.
static char *json_escape_append(char *dst, const char *dend, const char *src)
{
    while (*src && dst < dend - 1)
    {
        if (*src == '"' || *src == '\\')
        {
            if (dst < dend - 2) { *dst++ = '\\'; *dst++ = *src++; }
            else break;
        }
        else
            *dst++ = *src++;
    }
    return dst;
}

// ---------------------------------------------------------------------------
// Response helpers — route to stdout (stdio) or SSE socket (HTTP), main thread
// ---------------------------------------------------------------------------

static void mcp_write_line(const char *line)
{
    // If a /call is parked waiting on this id, hand the response to the HTTP
    // worker instead of sending it out via SSE/stdout. Eliminates the SSE
    // handshake for synchronous test automation.
    if (mcp_http_port > 0 && mcp_sync_state_mu) {
        char line_id[64];
        json_extract_id(line, line_id, sizeof(line_id));
        SDL_LockMutex(mcp_sync_state_mu);
        if (mcp_sync_id[0] && strcmp(mcp_sync_id, line_id) == 0 && !mcp_sync_ready) {
            strncpy(mcp_sync_resp, line, sizeof(mcp_sync_resp) - 1);
            mcp_sync_resp[sizeof(mcp_sync_resp) - 1] = '\0';
            mcp_sync_ready = 1;
            SDL_UnlockMutex(mcp_sync_state_mu);
            return;
        }
        SDL_UnlockMutex(mcp_sync_state_mu);
    }

    if (mcp_http_port > 0)
    {
        SDL_LockMutex(mcp_sse_mutex);
        mcp_raw_sock_t s = mcp_sse_sock;
        SDL_UnlockMutex(mcp_sse_mutex);
        if (s == (mcp_raw_sock_t)MCP_SOCK_INVALID) return;
        // Bumped from MCP_LINE_MAX+16 to MCP_FRAME_MAX -- the previous frame
        // buffer silently truncated responses larger than ~4 KB (notably
        // list_entities). mcp_text_result already builds 8 KB payloads, so
        // the SSE frame must accept at least that.
        static char frame[MCP_FRAME_MAX];
        int flen = snprintf(frame, sizeof(frame), "data: %s\n\n", line);
        if (flen > (int)sizeof(frame)) flen = (int)sizeof(frame);
        if (send(s, frame, flen, 0) < 0)
        {
            SDL_LockMutex(mcp_sse_mutex);
            if (mcp_sse_sock == s)
            {
                mcp_sock_close(mcp_sse_sock);
                mcp_sse_sock = (mcp_raw_sock_t)MCP_SOCK_INVALID;
            }
            SDL_UnlockMutex(mcp_sse_mutex);
        }
    }
    else
    {
        fprintf(stdout, "%s\n", line);
        fflush(stdout);
    }
}

static void mcp_send(const char *id_json, const char *result_json)
{
    // Static so the 64 KB buffer doesn't blow the stack; mcp_send and
    // mcp_text_result are only ever called from the dispatch thread (via
    // mcp_dispatch) or the HTTP worker holding mcp_sync_mutex -- never
    // re-entrantly, so the shared buffer is safe.
    static char buf[MCP_FRAME_MAX];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
             id_json, result_json);
    mcp_write_line(buf);
}

static void mcp_error(const char *id_json, int code, const char *msg)
{
    char buf[MCP_LINE_MAX];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
             id_json, code, msg);
    mcp_write_line(buf);
}

// Wrap an already-built text payload in the MCP content envelope.
// text_escaped must already have " and \ escaped.
static void mcp_text_result(const char *id_json, const char *text_escaped)
{
    static char buf[MCP_FRAME_MAX];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":"
             "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}",
             id_json, text_escaped);
    mcp_write_line(buf);
}

// ---------------------------------------------------------------------------
// Tool: get_player_state
// ---------------------------------------------------------------------------

static void tool_get_player_state(const char *id_json)
{
    extern client_state_t cl;
    extern server_t       sv;
    extern int            pr_edict_size;

    int   health = cl.stats[STAT_HEALTH];
    int   ammo   = cl.stats[STAT_AMMO];
    int   armor  = cl.stats[STAT_ARMOR];
    float ox = 0, oy = 0, oz = 0;
    const char *mapname = sv.active ? sv.name : cl.levelname;

    if (sv.active && sv.num_edicts > 1 && pr_edict_size > 0)
    {
        // Edict 1 is always the player in single-player
        edict_t *player = (edict_t *)((byte *)sv.edicts + pr_edict_size);
        if (!player->free)
        {
            ox = player->v.origin[0];
            oy = player->v.origin[1];
            oz = player->v.origin[2];
        }
    }
    else
    {
        // Demo playback: server edicts unavailable; use the render viewpoint
        extern refdef_t r_refdef;
        ox = r_refdef.vieworg[0];
        oy = r_refdef.vieworg[1];
        oz = r_refdef.vieworg[2];
    }

    char raw[256];
    snprintf(raw, sizeof(raw),
        "{\"map\":\"%s\",\"position\":[%.1f,%.1f,%.1f],"
        "\"health\":%d,\"ammo\":%d,\"armor\":%d}",
        mapname, ox, oy, oz, health, ammo, armor);

    char escaped[512];
    char *p = escaped;
    char *end = escaped + sizeof(escaped) - 1;
    p = json_escape_append(p, end, raw);
    *p = '\0';

    mcp_text_result(id_json, escaped);
}

// ---------------------------------------------------------------------------
// Tool: list_entities
// ---------------------------------------------------------------------------

#define ENTITY_BUF_SIZE  16384

static void tool_list_entities(const char *id_json)
{
    extern server_t sv;
    extern int      pr_edict_size;
    extern char    *pr_strings;

    static char raw[ENTITY_BUF_SIZE];
    static char escaped[ENTITY_BUF_SIZE * 2];

    char *p   = raw;
    char *end = raw + sizeof(raw) - 2;

    *p++ = '[';
    int first = 1;

    if (sv.active && sv.edicts && pr_strings && pr_edict_size > 0)
    {
        int limit = sv.num_edicts < 200 ? sv.num_edicts : 200;
        for (int i = 0; i < limit && p < end - 128; i++)
        {
            edict_t *e = (edict_t *)((byte *)sv.edicts + i * pr_edict_size);
            if (e->free) continue;
            const char *cn = e->v.classname;
            if (!cn || !cn[0]) continue;

            if (!first) { if (p < end) *p++ = ','; }
            first = 0;

            p += snprintf(p, end - p,
                "{\"id\":%d,\"classname\":\"%s\","
                "\"origin\":[%.0f,%.0f,%.0f]}",
                i, cn,
                e->v.origin[0], e->v.origin[1], e->v.origin[2]);
        }
    }

    if (p < end) *p++ = ']';
    *p = '\0';

    char *d = escaped;
    char *dend = escaped + sizeof(escaped) - 1;
    d = json_escape_append(d, dend, raw);
    *d = '\0';

    mcp_text_result(id_json, escaped);
}

// ---------------------------------------------------------------------------
// Tool: console_exec — Cbuf_AddText escape hatch
// ---------------------------------------------------------------------------

static void tool_console_exec(const char *id_json, const char *command)
{
    char buf[1024];
    if (!command || !command[0])
    {
        mcp_error(id_json, -32602, "missing command parameter");
        return;
    }
    /* Cbuf_AddText takes a non-const char* and expects a trailing newline.
     * Build a local copy so the queued command runs cleanly even if the
     * caller forgot to terminate. */
    snprintf(buf, sizeof(buf), "%s\n", command);
    Cbuf_AddText(buf);
    mcp_text_result(id_json, "ok");
}

// ---------------------------------------------------------------------------
// Tool: notify — mid-screen centerprint, optional golden screen flash + sound
// ---------------------------------------------------------------------------
//
// Lets an external agent (e.g. Claude finishing a task) surface a short
// message in-game without forcing the player to open the console. The
// centerprint is SCR_CenterPrint, the same channel maps use for level
// intro text, so it shows for `scr_centertime` seconds (default 2). With
// flash=true we also queue `bf` for a one-frame golden screen flash as a
// peripheral-vision cue, and with a non-empty sound we route `play <sfx>`
// for an audible ping.

static void tool_notify(const char *id_json, const char *message, int flash,
                        const char *sound)
{
    char copy[256];
    if (!message || !message[0])
    {
        mcp_error(id_json, -32602, "missing message parameter");
        return;
    }
    /* SCR_CenterPrint signature is non-const and writes scr_centerstring
     * via strncpy. Take a mutable copy so a caller's literal can't be
     * touched, and so we control the length cap. */
    strncpy(copy, message, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    SCR_CenterPrint(copy);
    /* Mirror to the console scrollback so the message persists after the
     * centerprint times out — useful when several notifies fire in quick
     * succession or when the player wants to scroll back later. */
    Con_Printf("notify: %s\n", copy);
    if (flash) Cbuf_AddText("bf\n");
    if (sound && sound[0])
    {
        /* Route through the `play` console command — handles precache,
         * channel allocation, and works whether or not a map is loaded
         * (snd_dma falls back to the local listener). Caller-supplied
         * path is treated as developer input; no shell-quoting concerns
         * since Cmd_TokenizeString parses on whitespace. */
        char buf[128];
        snprintf(buf, sizeof(buf), "play %s\n", sound);
        Cbuf_AddText(buf);
    }
    mcp_text_result(id_json, "ok");
}

// ---------------------------------------------------------------------------
// Tool: console_tail — last N lines from the engine console scrollback
// ---------------------------------------------------------------------------

extern int Con_GetLastLines(int n, char *out, int outsz);

static void tool_console_tail(const char *id_json, int lines)
{
    static char raw[8192];
    static char escaped[16384];
    int          n = (lines > 0) ? lines : 50;
    if (n > 200) n = 200;
    Con_GetLastLines(n, raw, sizeof(raw));
    char *d   = escaped;
    char *end = escaped + sizeof(escaped) - 1;
    /* Replace newlines with \n in the JSON-escaped output. */
    const char *s = raw;
    while (*s && d < end - 2)
    {
        if (*s == '\n')         { *d++ = '\\'; *d++ = 'n'; s++; }
        else if (*s == '"')     { *d++ = '\\'; *d++ = '"'; s++; }
        else if (*s == '\\')    { *d++ = '\\'; *d++ = '\\'; s++; }
        else                    { *d++ = *s++; }
    }
    *d = '\0';
    mcp_text_result(id_json, escaped);
}

// ---------------------------------------------------------------------------
// Tool: screenshot -- save the current framebuffer as a PNG
// ---------------------------------------------------------------------------

static void tool_screenshot(const char *id_json, const char *args)
{
    char input[512] = {0};
    if (args) json_str(args, "path", input, sizeof(input));

    char path[512] = {0};

    if (!input[0])
    {
        if (!Screenshot_NextPath(path, sizeof(path))) {
            mcp_error(id_json, -32603, "screenshot dir full");
            return;
        }
    }
    else
    {
        (void)mcp_mkdir("screenshots");
        /* Constrain every caller-supplied path to live inside screenshots/.
           Strip directory components (any '/' or '\\' on Windows) so neither
           absolute paths, drive letters, nor '..' traversals can escape. */
        const char *base = input;
        for (const char *p = input; *p; p++) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
        if (!base[0] || base[0] == '.') {
            mcp_error(id_json, -32602, "invalid screenshot filename");
            return;
        }
        for (const char *p = base; *p; p++) {
            unsigned char c = (unsigned char)*p;
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!ok) {
                mcp_error(id_json, -32602, "invalid screenshot filename");
                return;
            }
        }
        snprintf(path, sizeof(path), "screenshots/%s", base);
    }

    if (!VID_SaveScreenshotPNG(path))
    {
        mcp_error(id_json, -32603, "screenshot write failed");
        return;
    }

    /* Resolve to an absolute path so the client can Read it directly. */
    char abspath[1024];
#ifdef _WIN32
    if (!_fullpath(abspath, path, sizeof(abspath)))
        strncpy(abspath, path, sizeof(abspath) - 1);
#else
    if (!realpath(path, abspath))
        strncpy(abspath, path, sizeof(abspath) - 1);
#endif
    abspath[sizeof(abspath) - 1] = '\0';

    extern viddef_t vid;
    char raw[1024];
    snprintf(raw, sizeof(raw),
        "{\"path\":\"%s\",\"width\":%u,\"height\":%u}",
        abspath, vid.width, vid.height);

    /* Backslashes in the path must be escaped for JSON. */
    char escaped[2048];
    char *d = escaped;
    char *end = escaped + sizeof(escaped) - 1;
    const char *s = raw;
    while (*s && d < end - 2) {
        if (*s == '\\')      { *d++ = '\\'; *d++ = '\\'; s++; }
        else if (*s == '"')  { *d++ = '\\'; *d++ = '"';  s++; }
        else                 { *d++ = *s++; }
    }
    *d = '\0';
    mcp_text_result(id_json, escaped);
}

// ---------------------------------------------------------------------------
// Tool: sample_pixel -- read one pixel from the current framebuffer
// ---------------------------------------------------------------------------

static void tool_sample_pixel(const char *id_json, const char *args)
{
    int x = -1, y = -1;
    if (!args || !json_int(args, "x", &x) || !json_int(args, "y", &y))
    {
        mcp_error(id_json, -32602, "missing x or y");
        return;
    }

    byte r = 0, g = 0, b = 0, idx = 0;
    if (!VID_SamplePixel(x, y, &r, &g, &b, &idx))
    {
        mcp_error(id_json, -32602, "coords out of range");
        return;
    }

    char raw[128];
    snprintf(raw, sizeof(raw),
        "{\"r\":%u,\"g\":%u,\"b\":%u,\"palette_index\":%u}",
        r, g, b, idx);

    char escaped[256];
    char *d = escaped;
    char *end = escaped + sizeof(escaped) - 1;
    d = json_escape_append(d, end, raw);
    *d = '\0';
    mcp_text_result(id_json, escaped);
}

// ---------------------------------------------------------------------------
// Tool: teleport -- move the player edict to a new origin and (optionally)
// orient it. Sets fixangle so the engine pushes a svc_setangle to the
// client on the next snapshot.
// ---------------------------------------------------------------------------

static void tool_teleport(const char *id_json, const char *args)
{
    extern server_t sv;
    extern int      pr_edict_size;

    if (!sv.active || sv.num_edicts < 2 || pr_edict_size <= 0)
    {
        mcp_error(id_json, -32602, "no active server");
        return;
    }

    edict_t *player = (edict_t *)((byte *)sv.edicts + pr_edict_size);
    if (player->free)
    {
        mcp_error(id_json, -32602, "no player edict");
        return;
    }

    float origin[3] = {0,0,0};
    if (!args || !json_vec3(args, "origin", origin))
    {
        mcp_error(id_json, -32602, "missing origin");
        return;
    }

    float angles[3];
    angles[0] = player->v.angles[0];
    angles[1] = player->v.angles[1];
    angles[2] = player->v.angles[2];
    if (args) json_vec3(args, "angles", angles);   /* optional */

    player->v.origin[0] = origin[0];
    player->v.origin[1] = origin[1];
    player->v.origin[2] = origin[2];
    player->v.angles[0] = angles[0];
    player->v.angles[1] = angles[1];
    player->v.angles[2] = angles[2];
    player->v.fixangle  = 1;
    SV_LinkEdict(player, false);

    char raw[160];
    snprintf(raw, sizeof(raw),
        "{\"origin\":[%.1f,%.1f,%.1f],\"angles\":[%.1f,%.1f,%.1f]}",
        origin[0], origin[1], origin[2],
        angles[0], angles[1], angles[2]);

    char escaped[256];
    char *d = escaped;
    char *end = escaped + sizeof(escaped) - 1;
    d = json_escape_append(d, end, raw);
    *d = '\0';
    mcp_text_result(id_json, escaped);
}

// ---------------------------------------------------------------------------
// Tool: inspect_entity -- rich state dump for a single edict by id. Used to
// debug entity behavior (sliding gibs, runaway velocity, missing FL_ONGROUND).
// ---------------------------------------------------------------------------

static void tool_inspect_entity(const char *id_json, const char *args)
{
    extern server_t sv;
    extern int      pr_edict_size;
    extern char    *pr_strings;

    int eid = -1;
    if (!args || !json_int(args, "id", &eid) || eid < 0)
    {
        mcp_error(id_json, -32602, "missing id");
        return;
    }
    if (!sv.active || eid >= sv.num_edicts || pr_edict_size <= 0)
    {
        mcp_error(id_json, -32602, "no active server or id out of range");
        return;
    }
    edict_t *e = (edict_t *)((byte *)sv.edicts + eid * pr_edict_size);
    if (e->free)
    {
        mcp_error(id_json, -32602, "edict is free");
        return;
    }
    const char *cn    = e->v.classname ? e->v.classname : "";
    const char *model = e->v.model ? e->v.model : "";

    char raw[1536];
    int onground = ((int)e->v.flags & FL_ONGROUND) ? 1 : 0;
    int monster  = ((int)e->v.flags & FL_MONSTER) ? 1 : 0;
    int client   = ((int)e->v.flags & FL_CLIENT) ? 1 : 0;
    float dt_to_think = e->v.nextthink > 0 ? (float)(e->v.nextthink - sv.time) : -1.0f;

    snprintf(raw, sizeof(raw),
        "{\"id\":%d,\"classname\":\"%s\",\"model\":\"%s\","
        "\"origin\":[%.2f,%.2f,%.2f],"
        "\"velocity\":[%.2f,%.2f,%.2f],"
        "\"avelocity\":[%.2f,%.2f,%.2f],"
        "\"angles\":[%.1f,%.1f,%.1f],"
        "\"mins\":[%.1f,%.1f,%.1f],\"maxs\":[%.1f,%.1f,%.1f],"
        "\"movetype\":%d,\"solid\":%d,"
        "\"takedamage\":%g,\"deadflag\":%g,\"health\":%g,"
        "\"flags_raw\":%d,\"on_ground\":%s,\"monster\":%s,\"client\":%s,"
        "\"nextthink\":%g,\"sv_time\":%g,\"think_in\":%.3f,"
        "\"think_ptr\":\"%p\"}",
        eid, cn, model,
        e->v.origin[0], e->v.origin[1], e->v.origin[2],
        e->v.velocity[0], e->v.velocity[1], e->v.velocity[2],
        e->v.avelocity[0], e->v.avelocity[1], e->v.avelocity[2],
        e->v.angles[0], e->v.angles[1], e->v.angles[2],
        e->v.mins[0], e->v.mins[1], e->v.mins[2],
        e->v.maxs[0], e->v.maxs[1], e->v.maxs[2],
        (int)e->v.movetype, (int)e->v.solid,
        (double)e->v.takedamage, (double)e->v.deadflag, (double)e->v.health,
        (int)e->v.flags,
        onground ? "true" : "false",
        monster  ? "true" : "false",
        client   ? "true" : "false",
        (double)e->v.nextthink, (double)sv.time, dt_to_think,
        (void *)(uintptr_t)e->v.think);

    char escaped[3072];
    char *d = escaped;
    char *dend = escaped + sizeof(escaped) - 1;
    d = json_escape_append(d, dend, raw);
    *d = '\0';
    mcp_text_result(id_json, escaped);
}

// ---------------------------------------------------------------------------
// Tool: damage_entity -- deterministically apply damage to an edict.
// Routes through game_api->damage_entity, which thunks into T_Damage with
// the world as inflictor/attacker so corpse-overdamage and gib branches fire
// the same way they would from a normal hit.
// ---------------------------------------------------------------------------

static void tool_damage_entity(const char *id_json, const char *args)
{
    extern server_t sv;
    extern int      pr_edict_size;

    int eid = -1;
    int damage = 0;
    if (!args || !json_int(args, "id", &eid) || eid < 0)
    {
        mcp_error(id_json, -32602, "missing id");
        return;
    }
    if (!json_int(args, "damage", &damage) || damage <= 0)
    {
        mcp_error(id_json, -32602, "missing/invalid damage");
        return;
    }
    if (!sv.active || eid >= sv.num_edicts || pr_edict_size <= 0)
    {
        mcp_error(id_json, -32602, "no active server or id out of range");
        return;
    }
    edict_t *e = (edict_t *)((byte *)sv.edicts + eid * pr_edict_size);
    if (e->free)
    {
        mcp_error(id_json, -32602, "edict is free");
        return;
    }
    extern void MCP_DamageEntity(edict_t *targ, float damage);
    MCP_DamageEntity(e, (float)damage);

    char raw[128];
    snprintf(raw, sizeof(raw),
        "{\"id\":%d,\"health_after\":%g,\"deadflag\":%g}",
        eid, (double)e->v.health, (double)e->v.deadflag);
    mcp_text_result(id_json, raw);
}

// ---------------------------------------------------------------------------
// Tool: get_cvar -- read a cvar's current value and flags. This engine's
// cvar_t has no defaultvalue field (see engine_src/cvar.h:56-64), so the
// returned default is the empty string.
// ---------------------------------------------------------------------------

static void tool_get_cvar(const char *id_json, const char *name)
{
    cvar_t *v = Cvar_FindVar((char *)name);
    if (!v)
    {
        mcp_error(id_json, -32602, "cvar not found");
        return;
    }

    char raw[512];
    snprintf(raw, sizeof(raw),
        "{\"name\":\"%s\",\"value\":\"%s\",\"value_float\":%g,"
        "\"archive\":%s,\"server\":%s}",
        v->name, v->string ? v->string : "", v->value,
        v->archive ? "true" : "false",
        v->server  ? "true" : "false");

    char escaped[1024];
    char *d = escaped;
    char *end = escaped + sizeof(escaped) - 1;
    d = json_escape_append(d, end, raw);
    *d = '\0';
    mcp_text_result(id_json, escaped);
}

// ---------------------------------------------------------------------------
// Tool: wait_frames -- delay this JSON-RPC response by N rendered frames
// ---------------------------------------------------------------------------

static void tool_wait_frames(const char *id_json, const char *args)
{
    int frames = 0;
    if (!args || !json_int(args, "frames", &frames) || frames < 1)
    {
        mcp_error(id_json, -32602, "missing or invalid frames");
        return;
    }
    if (frames > 60) frames = 60;

    int slot = -1;
    for (int i = 0; i < MCP_MAX_PENDING_WAITS; i++)
        if (!mcp_pending_waits[i].in_use) { slot = i; break; }

    if (slot < 0)
    {
        mcp_error(id_json, -32603, "too many pending waits");
        return;
    }

    mcp_pending_waits[slot].in_use           = 1;
    mcp_pending_waits[slot].frames_remaining = frames;
    mcp_pending_waits[slot].frames_requested = frames;
    strncpy(mcp_pending_waits[slot].id_json, id_json,
            sizeof(mcp_pending_waits[slot].id_json) - 1);
    mcp_pending_waits[slot].id_json[
        sizeof(mcp_pending_waits[slot].id_json) - 1] = '\0';
    /* No response yet — MCP_Frame will send it when the counter hits zero. */
}

// ---------------------------------------------------------------------------
// Tool: editor_get_scene — JSON dump of the current edit_scene
// ---------------------------------------------------------------------------

static void tool_editor_get_scene(const char *id_json)
{
    static char raw[16384];
    static char escaped[32768];
    Scene_SerializeJSON(raw, sizeof(raw));
    /* mcp_text_result wraps the payload in {content:[{text:...}]} and the
     * payload itself is JSON. Escape it for transport. */
    char *d   = escaped;
    char *end = escaped + sizeof(escaped) - 1;
    const char *s = raw;
    while (*s && d < end - 2)
    {
        if (*s == '"')      { *d++ = '\\'; *d++ = '"'; s++; }
        else if (*s == '\\'){ *d++ = '\\'; *d++ = '\\'; s++; }
        else                { *d++ = *s++; }
    }
    *d = '\0';
    mcp_text_result(id_json, escaped);
}

// ---------------------------------------------------------------------------
// Tool: editor_brush_add — append a 6-plane AABB brush at explicit coords
// ---------------------------------------------------------------------------

static void tool_editor_brush_add(const char *id_json, const char *args)
{
    float mins[3] = {0,0,0};
    float maxs[3] = {0,0,0};
    char  texname[64] = {0};
    char  result[128];
    if (!Editor_IsOpen()) { mcp_error(id_json, -32602, "editor not open"); return; }
    if (!json_vec3(args, "mins", mins) || !json_vec3(args, "maxs", maxs))
    {
        mcp_error(id_json, -32602, "missing mins/maxs vec3");
        return;
    }
    json_str(args, "texture", texname, sizeof(texname));   /* optional */
    History_Push("mcp brush add");
    if (!Scene_AddCubeBrush(mins, maxs, texname[0] ? texname : NULL))
    {
        mcp_error(id_json, -32602, "Scene_AddCubeBrush failed");
        return;
    }
    /* The newly-added brush is the last one in worldspawn (entity 0). */
    {
        int b_idx = (edit_scene.numentities > 0)
                    ? (edit_scene.entities[0].numbrushes - 1) : -1;
        snprintf(result, sizeof(result),
                 "{\\\"ent\\\":0,\\\"brush\\\":%d}", b_idx);
        mcp_text_result(id_json, result);
    }
}

// ---------------------------------------------------------------------------
// Tool: editor_entity_add — append a point entity at explicit origin
// ---------------------------------------------------------------------------

static void tool_editor_entity_add(const char *id_json, const char *args)
{
    char  classname[64] = {0};
    float origin[3] = {0,0,0};
    char  result[128];
    if (!Editor_IsOpen()) { mcp_error(id_json, -32602, "editor not open"); return; }
    if (!json_str(args, "classname", classname, sizeof(classname)) ||
        !classname[0])
    {
        mcp_error(id_json, -32602, "missing classname");
        return;
    }
    json_vec3(args, "origin", origin);          /* optional, defaults to 0 */
    History_Push("mcp entity add");
    if (!Scene_AddPointEntity(classname, origin))
    {
        mcp_error(id_json, -32602, "Scene_AddPointEntity failed");
        return;
    }
    snprintf(result, sizeof(result),
             "{\\\"ent\\\":%d}", edit_scene.numentities - 1);
    mcp_text_result(id_json, result);
}

// ---------------------------------------------------------------------------
// Tool: editor_set_kv — upsert key/value on a specific entity
// ---------------------------------------------------------------------------

static void tool_editor_set_kv(const char *id_json, const char *args)
{
    int  ent = -1;
    char key[64]    = {0};
    char value[256] = {0};
    if (!Editor_IsOpen()) { mcp_error(id_json, -32602, "editor not open"); return; }
    if (!json_int(args, "ent", &ent)
        || ent < 0 || ent >= edit_scene.numentities)
    {
        mcp_error(id_json, -32602, "bad ent index");
        return;
    }
    if (!json_str(args, "key", key, sizeof(key)) || !key[0])
    {
        mcp_error(id_json, -32602, "missing key");
        return;
    }
    json_str(args, "value", value, sizeof(value));   /* allow empty */
    History_Push("mcp set kv");
    Entity_SetKV(&edit_scene.entities[ent], key, value);
    mcp_text_result(id_json, "ok");
}

// ---------------------------------------------------------------------------
// Tool: editor_select — programmatic selection (no cursor needed)
// ---------------------------------------------------------------------------

static void tool_editor_select(const char *id_json, const char *args)
{
    int ent = -1, brush = -1;
    if (!Editor_IsOpen()) { mcp_error(id_json, -32602, "editor not open"); return; }
    if (!json_int(args, "ent", &ent)
        || ent < 0 || ent >= edit_scene.numentities)
    {
        mcp_error(id_json, -32602, "bad ent index");
        return;
    }
    json_int(args, "brush", &brush);    /* -1 = whole entity (point ent) */
    Scene_SelectionClear();
    Scene_SelectionAdd(ent, brush);
    mcp_text_result(id_json, "ok");
}

// ---------------------------------------------------------------------------
// Tool: set_cvar
// ---------------------------------------------------------------------------

static void tool_set_cvar(const char *id_json, const char *name, const char *value)
{
    if (!Cvar_FindVar((char *)name))
    {
        mcp_error(id_json, -32602, "cvar not found");
        return;
    }
    Cvar_Set((char *)name, (char *)value);
    mcp_text_result(id_json, "ok");
}

// ---------------------------------------------------------------------------
// MCP tools schema (returned by tools/list)
// ---------------------------------------------------------------------------

#define MCP_TOOLS_RESULT \
    "{\"tools\":[" \
      "{\"name\":\"get_player_state\"," \
       "\"description\":\"Get current player position, health, ammo, armor and map name\"," \
       "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}}," \
      "{\"name\":\"list_entities\"," \
       "\"description\":\"List all live edicts with classname and origin (up to 200)\"," \
       "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}}," \
      "{\"name\":\"set_cvar\"," \
       "\"description\":\"Set a console variable by name to a new string value\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"name\":{\"type\":\"string\",\"description\":\"Cvar name\"}," \
           "\"value\":{\"type\":\"string\",\"description\":\"New value\"}}," \
         "\"required\":[\"name\",\"value\"]}}," \
      "{\"name\":\"console_exec\"," \
       "\"description\":\"Queue any engine console command (Cbuf_AddText). Supports all built-in editor commands such as editor, editor_new, editor_save, editor_compile, editor_undo, plus engine commands like map, disconnect, set\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"command\":{\"type\":\"string\",\"description\":\"Console command line\"}}," \
         "\"required\":[\"command\"]}}," \
      "{\"name\":\"notify\"," \
       "\"description\":\"Show a short message in-game via SCR_CenterPrint (the channel used for map intro text), with an optional golden screen flash and notification sound. Use to surface task-done events to a player without forcing them to open the console.\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"message\":{\"type\":\"string\",\"description\":\"Text to centerprint (max ~255 chars; \\\\n splits lines)\"}," \
           "\"flash\":{\"type\":\"boolean\",\"description\":\"Trigger bf golden flash too (default true)\"}," \
           "\"sound\":{\"type\":\"string\",\"description\":\"Sound to play (relative to id1/sound/, default misc/talk.wav; empty string = silent)\"}}," \
         "\"required\":[\"message\"]}}," \
      "{\"name\":\"console_tail\"," \
       "\"description\":\"Return the last N lines from the engine console scrollback (defaults to 50, max 200)\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"lines\":{\"type\":\"integer\",\"description\":\"Number of lines, 1..200\"}}," \
         "\"required\":[]}}," \
      "{\"name\":\"editor_get_scene\"," \
       "\"description\":\"Return a JSON description of the in-memory editor scene (open state, mapname, selection, entities with kv pairs, brushes with AABBs)\"," \
       "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}}," \
      "{\"name\":\"editor_brush_add\"," \
       "\"description\":\"Append a 6-plane AABB brush to worldspawn at explicit mins/maxs. Editor must be open. Returns the new brush index\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"mins\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3}," \
           "\"maxs\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3}," \
           "\"texture\":{\"type\":\"string\",\"description\":\"Texture name (optional, default wbrick1_5)\"}}," \
         "\"required\":[\"mins\",\"maxs\"]}}," \
      "{\"name\":\"editor_entity_add\"," \
       "\"description\":\"Append a point entity at an explicit origin. Editor must be open. Returns the new entity index\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"classname\":{\"type\":\"string\"}," \
           "\"origin\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3}}," \
         "\"required\":[\"classname\"]}}," \
      "{\"name\":\"editor_set_kv\"," \
       "\"description\":\"Set or upsert a key/value pair on a specific entity. Editor must be open\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"ent\":{\"type\":\"integer\",\"description\":\"Entity index from editor_get_scene\"}," \
           "\"key\":{\"type\":\"string\"}," \
           "\"value\":{\"type\":\"string\"}}," \
         "\"required\":[\"ent\",\"key\",\"value\"]}}," \
      "{\"name\":\"editor_select\"," \
       "\"description\":\"Programmatically set the editor selection (replaces the existing selection). Editor must be open. Use brush=-1 to select a point entity as a whole\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"ent\":{\"type\":\"integer\"}," \
           "\"brush\":{\"type\":\"integer\",\"description\":\"Brush index, or -1 for point entity\"}}," \
         "\"required\":[\"ent\"]}}" \
      "," \
      "{\"name\":\"screenshot\"," \
       "\"description\":\"Save the current framebuffer as a PNG. Returns the absolute path. Default location is screenshots/shot_NNNN.png\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"path\":{\"type\":\"string\",\"description\":\"Optional output path (relative or absolute)\"}}," \
         "\"required\":[]}}" \
      "," \
      "{\"name\":\"sample_pixel\"," \
       "\"description\":\"Read a single pixel from the current framebuffer. Returns 8-bit RGB plus the raw 8-bit palette index\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"x\":{\"type\":\"integer\",\"description\":\"0..vid.width-1\"}," \
           "\"y\":{\"type\":\"integer\",\"description\":\"0..vid.height-1\"}}," \
         "\"required\":[\"x\",\"y\"]}}" \
      "," \
      "{\"name\":\"teleport\"," \
       "\"description\":\"Move the player edict to a new origin (and optionally new view angles). Requires an active single-player server. Takes effect on the next frame\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"origin\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"[x,y,z] world units\"}," \
           "\"angles\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"[pitch,yaw,roll] degrees (optional)\"}}," \
         "\"required\":[\"origin\"]}}" \
      "," \
      "{\"name\":\"get_cvar\"," \
       "\"description\":\"Read a cvar's current value (string and float) plus archive/server flags\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"name\":{\"type\":\"string\",\"description\":\"Cvar name\"}}," \
         "\"required\":[\"name\"]}}" \
      "," \
      "{\"name\":\"wait_frames\"," \
       "\"description\":\"Delay this response by N rendered frames (clamped 1..60). Useful for syncing teleport+screenshot\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"frames\":{\"type\":\"integer\",\"description\":\"1..60\"}}," \
         "\"required\":[\"frames\"]}}" \
    "]}"

// ---------------------------------------------------------------------------
// Dispatch one JSON-RPC line (called on main thread from MCP_Frame)
// ---------------------------------------------------------------------------

static void mcp_dispatch(const char *line)
{
    // Notifications have no id — skip without responding
    if (!json_has_id(line))
        return;

    char id_json[64] = {0};
    char method[64]  = {0};
    json_extract_id(line, id_json, sizeof(id_json));
    json_str(line, "method", method, sizeof(method));

    if (strcmp(method, "initialize") == 0)
    {
        mcp_send(id_json,
            "{\"protocolVersion\":\"2024-11-05\","
             "\"capabilities\":{\"tools\":{}},"
             "\"serverInfo\":{\"name\":\"quake1.ai\",\"version\":\"0.1.0\"}}");
    }
    else if (strcmp(method, "ping") == 0)
    {
        mcp_send(id_json, "{}");
    }
    else if (strcmp(method, "tools/list") == 0)
    {
        mcp_send(id_json, MCP_TOOLS_RESULT);
    }
    else if (strcmp(method, "tools/call") == 0)
    {
        char tool_name[64] = {0};
        // "name" is at the params level, not inside arguments
        json_str(line, "name", tool_name, sizeof(tool_name));

        if (strcmp(tool_name, "get_player_state") == 0)
        {
            tool_get_player_state(id_json);
        }
        else if (strcmp(tool_name, "list_entities") == 0)
        {
            tool_list_entities(id_json);
        }
        else if (strcmp(tool_name, "set_cvar") == 0)
        {
            // arguments object is nested inside params
            const char *args = strstr(line, "\"arguments\":");
            char cvar_name[64]   = {0};
            char cvar_value[128] = {0};
            if (args)
            {
                json_str(args, "name",  cvar_name,  sizeof(cvar_name));
                json_str(args, "value", cvar_value, sizeof(cvar_value));
            }
            if (!cvar_name[0])
                mcp_error(id_json, -32602, "missing name parameter");
            else
                tool_set_cvar(id_json, cvar_name, cvar_value);
        }
        else if (strcmp(tool_name, "console_exec") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            char cmd[1024] = {0};
            if (args) json_str(args, "command", cmd, sizeof(cmd));
            tool_console_exec(id_json, cmd);
        }
        else if (strcmp(tool_name, "console_tail") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            int n = 50;
            if (args) json_int(args, "lines", &n);
            tool_console_tail(id_json, n);
        }
        else if (strcmp(tool_name, "notify") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            char msg[256] = {0};
            char sound[64];
            /* Default flash=true unless the args explicitly carry
             * "flash":false. No json_bool helper exists; a substring scan
             * is sufficient here since "flash" only appears as the key. */
            int flash = 1;
            strcpy(sound, "misc/talk.wav");
            if (args)
            {
                json_str(args, "message", msg, sizeof(msg));
                if (strstr(args, "\"flash\":false")) flash = 0;
                /* json_str overwrites sound only if key present; an
                 * explicit empty string ("sound":"") still lands in the
                 * buffer as '\0' and silences the cue. */
                json_str(args, "sound", sound, sizeof(sound));
            }
            tool_notify(id_json, msg, flash, sound);
        }
        else if (strcmp(tool_name, "editor_get_scene") == 0)
        {
            tool_editor_get_scene(id_json);
        }
        else if (strcmp(tool_name, "editor_brush_add") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_editor_brush_add(id_json, args ? args : "");
        }
        else if (strcmp(tool_name, "editor_entity_add") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_editor_entity_add(id_json, args ? args : "");
        }
        else if (strcmp(tool_name, "editor_set_kv") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_editor_set_kv(id_json, args ? args : "");
        }
        else if (strcmp(tool_name, "editor_select") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_editor_select(id_json, args ? args : "");
        }
        else if (strcmp(tool_name, "screenshot") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_screenshot(id_json, args ? args : "");
        }
        else if (strcmp(tool_name, "sample_pixel") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_sample_pixel(id_json, args ? args : "");
        }
        else if (strcmp(tool_name, "teleport") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_teleport(id_json, args ? args : "");
        }
        else if (strcmp(tool_name, "inspect_entity") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_inspect_entity(id_json, args ? args : "");
        }
        else if (strcmp(tool_name, "damage_entity") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_damage_entity(id_json, args ? args : "");
        }
        else if (strcmp(tool_name, "get_cvar") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            char name[64] = {0};
            if (args) json_str(args, "name", name, sizeof(name));
            if (!name[0])
                mcp_error(id_json, -32602, "missing name");
            else
                tool_get_cvar(id_json, name);
        }
        else if (strcmp(tool_name, "wait_frames") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_wait_frames(id_json, args ? args : "");
        }
        else
        {
            mcp_error(id_json, -32602, "unknown tool");
        }
    }
    else
    {
        mcp_error(id_json, -32601, "method not found");
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void MCP_SetHttpPortDefault(int port)
{
    if (port < 0 || port > 65535) return;
    SDL_snprintf(mcp_http_port_default, sizeof(mcp_http_port_default), "%d", port);
}

void MCP_RegisterCvars(void)
{
    Cvar_RegisterVariable(&mcp_http_port_cvar);
}

// Bring up the HTTP/SSE listener on `port`. Returns 1 on success, 0 on failure.
// Caller is responsible for setting mcp_active / picking an init mode.
static int mcp_start_http(int port)
{
    mcp_http_port = port;
    mcp_sse_mutex     = SDL_CreateMutex();
    mcp_sync_mutex    = SDL_CreateMutex();
    mcp_sync_state_mu = SDL_CreateMutex();

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    mcp_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mcp_listen_sock == (mcp_raw_sock_t)MCP_SOCK_INVALID)
    {
        Con_Printf("mcp: socket() failed\n");
        return 0;
    }

    int reuse = 1;
    setsockopt(mcp_listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&reuse, sizeof(reuse));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = htons((unsigned short)port);
    if (bind(mcp_listen_sock, (struct sockaddr *)&sa, sizeof(sa)) < 0
        || listen(mcp_listen_sock, 8) < 0)
    {
        mcp_sock_close(mcp_listen_sock);
        mcp_listen_sock = (mcp_raw_sock_t)MCP_SOCK_INVALID;
        Con_Printf("mcp: failed to bind port %d\n", port);
        return 0;
    }

    mcp_thread = SDL_CreateThread(mcp_http_thread, "mcp_http", NULL);
    Con_Printf("mcp: HTTP/SSE server on http://localhost:%d/sse\n", port);
    return 1;
}

// port == 0 → stdio transport; port > 0 → HTTP/SSE transport
void MCP_Init(int port)
{
    mcp_active = 1;
    if (!mcp_mutex) mcp_mutex = SDL_CreateMutex();

    if (port > 0)
    {
        if (mcp_start_http(port)) mcp_http_started_port = port;
    }
    else
    {
        mcp_thread = SDL_CreateThread(mcp_reader_thread, "mcp_reader", NULL);
    }
}

void MCP_Frame(void)
{
    // Drive mcp_http_port cvar: lazy-start the HTTP listener when set, refuse
    // restart once running. Stdio mode (--mcp-stdio) bypasses the cvar entirely.
    {
        int desired = (int)mcp_http_port_cvar.value;
        if (mcp_http_started_port == 0)
        {
            if (desired > 0 && desired <= 65535)
            {
                if (mcp_active && mcp_http_port == 0)
                {
                    Con_Printf("mcp: stdio transport active; mcp_http_port ignored\n");
                    Cvar_Set("mcp_http_port", "0");
                }
                else
                {
                    mcp_active = 1;
                    if (!mcp_mutex) mcp_mutex = SDL_CreateMutex();
                    if (mcp_start_http(desired)) mcp_http_started_port = desired;
                }
            }
        }
        else if (desired != mcp_http_started_port)
        {
            char keep[16];
            SDL_snprintf(keep, sizeof(keep), "%d", mcp_http_started_port);
            Con_Printf("mcp: HTTP server already on port %d; runtime restart "
                       "not supported, reverting mcp_http_port to %s\n",
                       mcp_http_started_port, keep);
            Cvar_Set("mcp_http_port", keep);
        }
    }

    if (!mcp_active) return;

    mcp_slot_t slot;
    while (mcp_dequeue(&slot))
        mcp_dispatch(slot.line);

    /* Tick any pending wait_frames responses. */
    for (int i = 0; i < MCP_MAX_PENDING_WAITS; i++)
    {
        if (!mcp_pending_waits[i].in_use) continue;
        mcp_pending_waits[i].frames_remaining--;
        if (mcp_pending_waits[i].frames_remaining <= 0)
        {
            char raw[64];
            snprintf(raw, sizeof(raw),
                "{\"frames_waited\":%d}",
                mcp_pending_waits[i].frames_requested);
            char escaped[128];
            char *d = escaped;
            char *end = escaped + sizeof(escaped) - 1;
            d = json_escape_append(d, end, raw);
            *d = '\0';
            mcp_text_result(mcp_pending_waits[i].id_json, escaped);
            mcp_pending_waits[i].in_use = 0;
        }
    }
}

void MCP_Shutdown(void)
{
    if (mcp_listen_sock != (mcp_raw_sock_t)MCP_SOCK_INVALID)
    {
        mcp_sock_close(mcp_listen_sock);
        mcp_listen_sock = (mcp_raw_sock_t)MCP_SOCK_INVALID;
    }
    if (mcp_sse_sock != (mcp_raw_sock_t)MCP_SOCK_INVALID)
    {
        mcp_sock_close(mcp_sse_sock);
        mcp_sse_sock = (mcp_raw_sock_t)MCP_SOCK_INVALID;
    }
    if (mcp_sync_state_mu) { SDL_DestroyMutex(mcp_sync_state_mu); mcp_sync_state_mu = NULL; }
    if (mcp_sync_mutex)    { SDL_DestroyMutex(mcp_sync_mutex);    mcp_sync_mutex    = NULL; }
    if (mcp_sse_mutex)     { SDL_DestroyMutex(mcp_sse_mutex);     mcp_sse_mutex     = NULL; }
    if (mcp_mutex)         { SDL_DestroyMutex(mcp_mutex);         mcp_mutex         = NULL; }
    mcp_active = 0;
}
