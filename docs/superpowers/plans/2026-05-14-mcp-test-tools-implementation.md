# MCP test-tools implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add five MCP tools (`screenshot`, `teleport`, `get_cvar`, `sample_pixel`, `wait_frames`) so an LLM client can reproduce and verify visual bugs autonomously.

**Architecture:** Vendor `stb_image_write.h` for PNG output. Add two helper functions in `sdlquake/platform/vid_sdl.c` that walk `vid.buffer` + `d_8to24table`. Add five tool implementations to `sdlquake/mcp/mcp_server.c` plus a small pending-response array so `wait_frames` can delay its JSON-RPC reply by N rendered frames.

**Tech Stack:** C (gnu89 engine, modern C platform layer), Zig 0.16 build system, SDL3, stdio/HTTP-SSE JSON-RPC MCP transport.

**Reference spec:** `docs/superpowers/specs/2026-05-14-mcp-test-tools-design.md`

**Codebase conventions (from CLAUDE.md and saved memory):**
- No test suite. Verification = `zig build run` + behavior probe in-game.
- Commit directly to master + push in one step. Never branch / PR.
- Platform-layer files (incl. `vid_sdl.c`, `mcp_server.c`) compile with modern C (no `-std=gnu89`).
- After writing/editing a plan, open it in `gvim` as a background task.

**Important deviation from the spec:** The spec's `get_cvar` description anticipated a `flags` bitmask and `default` field. This WinQuake fork's `cvar_t` (see `sdlquake/engine_src/cvar.h:56-64`) has separate `archive` and `server` bools and no default value. Task 7 returns those two bools directly instead. The spec's "Errors" sections remain unchanged.

---

### Task 1: Vendor `stb_image_write.h` and wire up the include path

**Files:**
- Create: `sdlquake/vendor/stb/stb_image_write.h`
- Modify: `build.zig:255` (insert one `addIncludePath` line in the include-path block)

- [ ] **Step 1.1: Download the single-header PNG encoder**

```powershell
New-Item -ItemType Directory -Force sdlquake/vendor/stb | Out-Null
Invoke-WebRequest `
  -Uri "https://raw.githubusercontent.com/nothings/stb/f0569113c93ad095470c54bf34a17b36646bbbb5/stb_image_write.h" `
  -OutFile "sdlquake/vendor/stb/stb_image_write.h"
```

(That commit is `stb_image_write 1.16`, the current upstream. The file is ~1700 lines, MIT/public-domain.)

- [ ] **Step 1.2: Verify the file is present**

```powershell
Get-Item sdlquake/vendor/stb/stb_image_write.h | Select-Object Length
```

Expected: a positive `Length` (~60 KB).

- [ ] **Step 1.3: Add the include path in build.zig**

Insert after `mod.addIncludePath(b.path("sdlquake/game"));` (currently `build.zig:254`):

```zig
    mod.addIncludePath(b.path("sdlquake/vendor/stb"));
```

The block should now read:

```zig
    mod.addIncludePath(b.path("sdlquake/platform"));
    mod.addIncludePath(b.path("sdlquake/mcp"));
    mod.addIncludePath(b.path("sdlquake/engine"));
    mod.addIncludePath(b.path("sdlquake/engine/editor"));
    mod.addIncludePath(b.path("sdlquake/game"));
    mod.addIncludePath(b.path("sdlquake/vendor/stb"));
    mod.addIncludePath(b.path(wq_dir));
```

- [ ] **Step 1.4: Build to verify the include path is valid**

Run: `zig build`
Expected: clean build, no errors.

- [ ] **Step 1.5: Commit**

```powershell
git add sdlquake/vendor/stb/stb_image_write.h build.zig
git commit -m @'
mcp: vendor stb_image_write.h for PNG screenshots

Single-header PNG encoder for the upcoming MCP screenshot tool. Public
domain / MIT.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
'@
git push
```

---

### Task 2: Add `VID_SaveScreenshotPNG` in `vid_sdl.c`

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c` (top-of-file include block, plus a new function near the end of the file)

- [ ] **Step 2.1: Add the stb_image_write include with implementation macro**

Insert at the top of `sdlquake/platform/vid_sdl.c`, after the existing `#include "debug_lines.h"` line (around line 12):

```c
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
```

(Default stdio-backed API is what we want — `stbi_write_png(path, ...)` opens the file itself.)

- [ ] **Step 2.2: Add the screenshot function**

Append to `sdlquake/platform/vid_sdl.c` (before the file's final closing brace if any, otherwise at end of file):

```c
// ---------------------------------------------------------------------------
// VID_SaveScreenshotPNG -- write the current 8-bit framebuffer as a 24-bit
// PNG. Returns 1 on success, 0 on failure. Caller is responsible for the
// destination directory existing.
// ---------------------------------------------------------------------------
int VID_SaveScreenshotPNG(const char *path)
{
    if (!path || !path[0]) return 0;
    if (!vid.buffer) return 0;

    int w = (int)vid.width;
    int h = (int)vid.height;
    int rowbytes = (int)vid.rowbytes;
    if (w <= 0 || h <= 0 || rowbytes < w) return 0;

    unsigned char *rgb = (unsigned char *)malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return 0;

    for (int y = 0; y < h; y++) {
        const byte    *src = vid.buffer + y * rowbytes;
        unsigned char *dst = rgb        + y * w * 3;
        for (int x = 0; x < w; x++) {
            unsigned c = d_8to24table[src[x]];
            dst[x*3 + 0] = (unsigned char)(c >>  0);  /* R */
            dst[x*3 + 1] = (unsigned char)(c >>  8);  /* G */
            dst[x*3 + 2] = (unsigned char)(c >> 16);  /* B */
        }
    }

    int ok = stbi_write_png(path, w, h, 3, rgb, w * 3);
    free(rgb);
    return ok ? 1 : 0;
}
```

- [ ] **Step 2.3: Build**

Run: `zig build`
Expected: clean build. (The `stb_image_write.h` implementation produces some warnings normally but `platform_c_flags` has `-w` which suppresses them.)

- [ ] **Step 2.4: Commit**

(End-to-end smoke test of this function happens in Task 4, when the MCP screenshot tool exercises it.)

```powershell
git add sdlquake/platform/vid_sdl.c
git commit -m @'
mcp: VID_SaveScreenshotPNG writes the framebuffer as PNG

Walks vid.buffer (8-bit palettized) through d_8to24table, encodes as
24-bit RGB via vendored stb_image_write. Used by the upcoming MCP
screenshot tool.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
'@
git push
```

---

### Task 3: Add `VID_SamplePixel` in `vid_sdl.c`

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c` (one new function next to `VID_SaveScreenshotPNG`)

- [ ] **Step 3.1: Add the pixel sample function**

Append to `sdlquake/platform/vid_sdl.c`, right after `VID_SaveScreenshotPNG`:

```c
// ---------------------------------------------------------------------------
// VID_SamplePixel -- read a single framebuffer pixel. Returns 1 on success,
// 0 if (x,y) is out of bounds or vid.buffer is NULL. Out params receive the
// RGB triple from d_8to24table[] and the raw 8-bit palette index.
// ---------------------------------------------------------------------------
int VID_SamplePixel(int x, int y, byte *r, byte *g, byte *b, byte *idx)
{
    if (!vid.buffer) return 0;
    if (x < 0 || y < 0) return 0;
    if (x >= (int)vid.width || y >= (int)vid.height) return 0;

    byte i = vid.buffer[y * (int)vid.rowbytes + x];
    unsigned c = d_8to24table[i];
    if (r)   *r   = (byte)(c >>  0);
    if (g)   *g   = (byte)(c >>  8);
    if (b)   *b   = (byte)(c >> 16);
    if (idx) *idx = i;
    return 1;
}
```

- [ ] **Step 3.2: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 3.3: Commit**

```powershell
git add sdlquake/platform/vid_sdl.c
git commit -m @'
mcp: VID_SamplePixel returns a single framebuffer pixel

Used by the upcoming MCP sample_pixel tool for cheap colour assertions
without a full screenshot.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
'@
git push
```

---

### Task 4: MCP `screenshot` tool

**Files:**
- Modify: `sdlquake/mcp/mcp_server.c` (forward declarations, new tool function, schema entry, dispatch arm)

- [ ] **Step 4.1: Add the forward declarations near the top of the file**

Open `sdlquake/mcp/mcp_server.c` and insert these extern decls just before the `int mcp_active = 0;` line (currently at line 55):

```c
// From platform/vid_sdl.c — defined alongside the other VID helpers.
extern int VID_SaveScreenshotPNG(const char *path);
extern int VID_SamplePixel(int x, int y, byte *r, byte *g, byte *b, byte *idx);
```

- [ ] **Step 4.2: Add the screenshot tool function**

Insert this function in `mcp_server.c` after `tool_console_tail` (around line 585) and before `tool_editor_get_scene`:

```c
// ---------------------------------------------------------------------------
// Tool: screenshot -- save the current framebuffer as a PNG
// ---------------------------------------------------------------------------

#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  define mcp_mkdir(p) _mkdir(p)
#else
#  define mcp_mkdir(p) mkdir((p), 0755)
#endif

static int mcp_next_screenshot_index(void)
{
    // Find the lowest free NNNN such that screenshots/shot_NNNN.png does
    // not exist. Linear scan from 1, cap at 9999.
    for (int i = 1; i <= 9999; i++) {
        char path[256];
        snprintf(path, sizeof(path), "screenshots/shot_%04d.png", i);
        struct stat st;
        if (stat(path, &st) != 0) return i;
    }
    return 0;
}

static void tool_screenshot(const char *id_json, const char *args)
{
    char path[512] = {0};
    if (args) json_str(args, "path", path, sizeof(path));

    if (!path[0])
    {
        mcp_mkdir("screenshots");   /* ignore errors -- may already exist */
        int n = mcp_next_screenshot_index();
        if (n == 0) { mcp_error(id_json, -32603, "screenshot dir full"); return; }
        snprintf(path, sizeof(path), "screenshots/shot_%04d.png", n);
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
```

- [ ] **Step 4.3: Add the schema entry**

In `MCP_TOOLS_RESULT` (currently lines 738-798), insert this entry just before the closing `"]}"` — i.e., after the `"editor_select"` entry and before `"]}"`:

```c
      "," \
      "{\"name\":\"screenshot\"," \
       "\"description\":\"Save the current framebuffer as a PNG. Returns the absolute path. Default location is screenshots/shot_NNNN.png\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"path\":{\"type\":\"string\",\"description\":\"Optional output path (relative or absolute)\"}}," \
         "\"required\":[]}}"
```

The block already ends with `"]}"`; you are inserting the comma + new entry just before that closing `"]}"`.

- [ ] **Step 4.4: Add the dispatch arm**

In `mcp_dispatch` (currently around line 897, just before the `else { mcp_error(id_json, -32602, "unknown tool"); }` line), add:

```c
        else if (strcmp(tool_name, "screenshot") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_screenshot(id_json, args ? args : "");
        }
```

- [ ] **Step 4.5: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 4.6: Smoke test**

```powershell
zig build run -- +map e1m1 --mcp-http 8765
```

In a second window, with `curl` (or any MCP client), POST a `tools/call` request. Quickest is via console_exec to seed the SSE channel first, then post. Easier path: open Claude Code on this project (which has `.mcp.json`) and let it dispatch a `screenshot` tool call.

If invoking by hand, send (single line, real JSON):

```
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"screenshot","arguments":{}}}
```

to `POST http://localhost:8765/message` after opening a `GET /sse` stream.

Expected SSE response (single `data:` event):
```
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"{\"path\":\"C:\\\\...\\\\quake1.ai\\\\screenshots\\\\shot_0001.png\",\"width\":320,\"height\":200}"}]}}
```

Verify the file exists and renders correctly: `screenshots/shot_0001.png`.

- [ ] **Step 4.7: Commit**

```powershell
git add sdlquake/mcp/mcp_server.c
git commit -m @'
mcp: screenshot tool saves framebuffer to PNG and returns absolute path

Default destination is screenshots/shot_NNNN.png with auto-incremented
NNNN. Caller can override with an explicit path.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
'@
git push
```

---

### Task 5: MCP `sample_pixel` tool

**Files:**
- Modify: `sdlquake/mcp/mcp_server.c` (new tool function, schema entry, dispatch arm)

- [ ] **Step 5.1: Add the tool function**

Insert in `mcp_server.c` right after `tool_screenshot`:

```c
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
```

- [ ] **Step 5.2: Add the schema entry**

In `MCP_TOOLS_RESULT`, insert just before the closing `"]}"`, after the `screenshot` entry from Task 4:

```c
      "," \
      "{\"name\":\"sample_pixel\"," \
       "\"description\":\"Read a single pixel from the current framebuffer. Returns 8-bit RGB plus the raw 8-bit palette index\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"x\":{\"type\":\"integer\",\"description\":\"0..vid.width-1\"}," \
           "\"y\":{\"type\":\"integer\",\"description\":\"0..vid.height-1\"}}," \
         "\"required\":[\"x\",\"y\"]}}"
```

- [ ] **Step 5.3: Add the dispatch arm**

In `mcp_dispatch`, just before the final `else { mcp_error(...) }`:

```c
        else if (strcmp(tool_name, "sample_pixel") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_sample_pixel(id_json, args ? args : "");
        }
```

- [ ] **Step 5.4: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 5.5: Smoke test**

Launch the game and call `sample_pixel` with `{x:160, y:100}` via MCP. Expected:

```
{"r":<int>,"g":<int>,"b":<int>,"palette_index":<int>}
```

The exact RGB depends on what's on-screen at the centre pixel. Also call with `{x:9999, y:0}` and verify the error `coords out of range`.

- [ ] **Step 5.6: Commit**

```powershell
git add sdlquake/mcp/mcp_server.c
git commit -m @'
mcp: sample_pixel tool returns RGB+palette index for one framebuffer pixel

Cheap alternative to a full screenshot for boolean/colour assertions
("is the centre pixel red?").

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
'@
git push
```

---

### Task 6: MCP `teleport` tool

**Files:**
- Modify: `sdlquake/mcp/mcp_server.c` (new tool function, schema entry, dispatch arm)

- [ ] **Step 6.1: Add the tool function**

Insert in `mcp_server.c` after `tool_sample_pixel`:

```c
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
```

- [ ] **Step 6.2: Add the schema entry**

In `MCP_TOOLS_RESULT`, insert just before the closing `"]}"`, after `sample_pixel`:

```c
      "," \
      "{\"name\":\"teleport\"," \
       "\"description\":\"Move the player edict to a new origin (and optionally new view angles). Requires an active single-player server. Takes effect on the next frame\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"origin\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"[x,y,z] world units\"}," \
           "\"angles\":{\"type\":\"array\",\"items\":{\"type\":\"number\"},\"minItems\":3,\"maxItems\":3,\"description\":\"[pitch,yaw,roll] degrees (optional)\"}}," \
         "\"required\":[\"origin\"]}}"
```

- [ ] **Step 6.3: Add the dispatch arm**

In `mcp_dispatch`, just before the final `else { mcp_error(...) }`:

```c
        else if (strcmp(tool_name, "teleport") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_teleport(id_json, args ? args : "");
        }
```

- [ ] **Step 6.4: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 6.5: Smoke test**

Launch `zig build run -- +map e1m1 --mcp-http 8765`. Wait for the level to load.

Via MCP:
1. `get_player_state` — note the current origin.
2. `teleport` with `{origin:[0,0,50], angles:[0,0,0]}`.
3. `get_player_state` — confirm origin is now `[0,0,50]`.

Visually, the player view should jump to the new spot.

- [ ] **Step 6.6: Commit**

```powershell
git add sdlquake/mcp/mcp_server.c
git commit -m @'
mcp: teleport tool moves the player edict and pushes svc_setangle

Sets v.origin, v.angles, v.fixangle on edict 1 and re-links. Single-player
only -- returns an error if sv.active is 0 or the player edict is freed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
'@
git push
```

---

### Task 7: MCP `get_cvar` tool

**Files:**
- Modify: `sdlquake/mcp/mcp_server.c` (new tool function, schema entry, dispatch arm)

- [ ] **Step 7.1: Add the tool function**

Insert in `mcp_server.c` after `tool_teleport`:

```c
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
```

- [ ] **Step 7.2: Add the schema entry**

In `MCP_TOOLS_RESULT`, after `teleport`:

```c
      "," \
      "{\"name\":\"get_cvar\"," \
       "\"description\":\"Read a cvar's current value (string and float) plus archive/server flags\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"name\":{\"type\":\"string\",\"description\":\"Cvar name\"}}," \
         "\"required\":[\"name\"]}}"
```

- [ ] **Step 7.3: Add the dispatch arm**

In `mcp_dispatch`, just before the final `else { mcp_error(...) }`:

```c
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
```

- [ ] **Step 7.4: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 7.5: Smoke test**

Via MCP: `get_cvar` with `{name:"r_decals_intensity"}`. Expected:

```
{"name":"r_decals_intensity","value":"1","value_float":1,"archive":true,"server":false}
```

(The exact archive/server flags depend on how that cvar is registered.)

Also try `{name:"does_not_exist"}` → expect error `cvar not found`.

- [ ] **Step 7.6: Commit**

```powershell
git add sdlquake/mcp/mcp_server.c
git commit -m @'
mcp: get_cvar tool returns value (string + float) and archive/server flags

This engine's cvar_t has no defaultvalue field, so default is omitted.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
'@
git push
```

---

### Task 8: MCP `wait_frames` tool + pending-response machinery

**Files:**
- Modify: `sdlquake/mcp/mcp_server.c` (pending-wait array, tool function, schema entry, dispatch arm, `MCP_Frame` tick pass)

- [ ] **Step 8.1: Add the pending-wait array**

Insert in `mcp_server.c` after the existing queue definitions (after line 73, around the static globals for `mcp_thread`):

```c
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
```

- [ ] **Step 8.2: Add the tool function**

Insert in `mcp_server.c` after `tool_get_cvar`:

```c
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
```

- [ ] **Step 8.3: Tick pending waits each frame in `MCP_Frame`**

Modify `MCP_Frame` (currently at line 959) so it ticks the pending array after draining the queue:

```c
void MCP_Frame(void)
{
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
```

- [ ] **Step 8.4: Add the schema entry**

In `MCP_TOOLS_RESULT`, after `get_cvar`:

```c
      "," \
      "{\"name\":\"wait_frames\"," \
       "\"description\":\"Delay this response by N rendered frames (clamped 1..60). Useful for syncing teleport+screenshot\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"frames\":{\"type\":\"integer\",\"description\":\"1..60\"}}," \
         "\"required\":[\"frames\"]}}"
```

- [ ] **Step 8.5: Add the dispatch arm**

In `mcp_dispatch`, just before the final `else { mcp_error(...) }`:

```c
        else if (strcmp(tool_name, "wait_frames") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_wait_frames(id_json, args ? args : "");
        }
```

- [ ] **Step 8.6: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 8.7: Smoke test**

Via MCP: `wait_frames` with `{frames:30}`. The response should arrive ~0.5 s later (at typical 60 fps). Try `{frames:0}` → expect error. Try issuing 9 wait_frames requests back to back — the 9th should fail with `too many pending waits` (until earlier ones complete).

- [ ] **Step 8.8: Commit**

```powershell
git add sdlquake/mcp/mcp_server.c
git commit -m @'
mcp: wait_frames tool delays response by N rendered frames

Adds an 8-slot pending-response array and a tick pass at the end of
MCP_Frame. Used as a sync primitive between teleport and screenshot.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
'@
git push
```

---

### Task 9: End-to-end smoke test — blood-pool visibility check

**Files:** none (verification only)

- [ ] **Step 9.1: Launch the game with MCP HTTP**

```powershell
zig build run -- +map e1m1 --mcp-http 8765
```

(Replace `e1m1` with whichever map the user was using when they spawned the blood pool that triggered this work — that pool is gone since maps reload, but any map with a floor works for the smoke test.)

- [ ] **Step 9.2: Spawn a fresh blood pool in a known location**

From an MCP client (or by hand via curl/the SSE protocol), run this sequence:

1. `console_exec` with `{command:"noclip"}`
2. `console_exec` with `{command:"impulse 9"}` — gives all weapons + ammo.
3. Use `teleport` to a known grunt's vicinity (or fly there manually), then kill it.
4. After the kill: read the console with `console_tail` `{lines:30}`. Look for the `R_SpawnBloodPool` diagnostic line — it prints `origin=X Y Z`. Capture those coords.

- [ ] **Step 9.3: Position the camera above the pool, looking down**

`teleport` with `{origin:[<pool.x>, <pool.y>, <pool.z + 50>], angles:[90, 0, 0]}`
`wait_frames` with `{frames:3}`

(`angles[0]=90` is straight down in Quake's pitch convention.)

- [ ] **Step 9.4: Capture and inspect**

`screenshot` with `{}` → returns the absolute path of the PNG.
`sample_pixel` with `{x:160, y:100}` → returns the centre pixel's RGB.

Open the PNG file. Expected: a red blob centred on the floor below the camera. If red is present, the painted stain reaches the rendered output and the original "I can't see it now" report was a viewing-angle issue.

If the centre pixel is not red and the PNG shows a normal floor texture with no red, the blood-pool paint is mechanically correct (per the earlier diagnostic) but never reaches the visible lightmap — meaning a real bug, and the next investigation is the surface-cache invalidation path (`D_CacheSurface` stain_gen check) or the lightmap-apply loop for that specific surface.

- [ ] **Step 9.5: No commit needed**

This task is verification only.

---

## Out-of-scope reminders

(Lifted from the spec — do **not** implement these as part of this plan.)

- HTTP/SSE transport changes (already supports new tools via existing dispatch).
- Authentication / sandboxing of MCP tools.
- Demo-playback `teleport` support.
- Inline JSON-RPC bytes for screenshots (files on disk are simpler).
- Trace queries, decal introspection, surface listing.
