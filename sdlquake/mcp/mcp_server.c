// mcp_server.c -- Model Context Protocol (MCP 2024-11-05) stdio JSON-RPC 2.0 server
//
// Architecture:
//   Background thread reads stdin line-by-line and pushes raw lines onto a mutex-
//   protected circular queue.  The main thread calls MCP_Frame() once per game
//   frame; it drains the queue, parses each request, and writes a response to
//   stdout.  All Quake state access therefore happens on the main thread.
//
// Tools (Phase 2 MVP):
//   get_player_state  -- position, health, ammo, map name
//   list_entities     -- all live edicts (classname + origin)
//   set_cvar          -- set a named cvar to a string value

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "quakedef.h"
#include "mcp_server.h"

int mcp_active = 0;

// ---------------------------------------------------------------------------
// Circular request queue (background thread writes, main thread reads)
// ---------------------------------------------------------------------------

#define MCP_QUEUE_BITS  4
#define MCP_QUEUE_SIZE  (1 << MCP_QUEUE_BITS)   // 16 slots
#define MCP_QUEUE_MASK  (MCP_QUEUE_SIZE - 1)
#define MCP_LINE_MAX    4096

typedef struct { char line[MCP_LINE_MAX]; } mcp_slot_t;

static mcp_slot_t   mcp_queue[MCP_QUEUE_SIZE];
static int          mcp_q_head = 0;   // write index (background thread)
static int          mcp_q_tail = 0;   // read  index (main thread)
static SDL_Mutex   *mcp_mutex  = NULL;
static SDL_Thread  *mcp_thread = NULL;

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
// Background stdin reader thread
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
// Response helpers — all output goes to stdout, all writes on main thread
// ---------------------------------------------------------------------------

static void mcp_send(const char *id_json, const char *result_json)
{
    fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n",
            id_json, result_json);
    fflush(stdout);
}

static void mcp_error(const char *id_json, int code, const char *msg)
{
    fprintf(stdout,
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}\n",
            id_json, code, msg);
    fflush(stdout);
}

// Wrap an already-built text payload in the MCP content envelope.
// text_escaped must already have " and \ escaped.
static void mcp_text_result(const char *id_json, const char *text_escaped)
{
    fprintf(stdout,
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":"
            "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}\n",
            id_json, text_escaped);
    fflush(stdout);
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
#if NATIVE_GAME
            const char *cn = e->v.classname;
#else
            const char *cn = pr_strings + e->v.classname;
#endif
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
         "\"required\":[\"name\",\"value\"]}}" \
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

void MCP_Init(void)
{
    mcp_active = 1;
    mcp_mutex  = SDL_CreateMutex();
    mcp_thread = SDL_CreateThread(mcp_reader_thread, "mcp_reader", NULL);
}

void MCP_Frame(void)
{
    mcp_slot_t slot;
    while (mcp_dequeue(&slot))
        mcp_dispatch(slot.line);
}

void MCP_Shutdown(void)
{
    // Reader thread is blocked on fgets; it exits naturally when stdin closes.
    if (mcp_mutex) { SDL_DestroyMutex(mcp_mutex); mcp_mutex = NULL; }
    mcp_active = 0;
}
