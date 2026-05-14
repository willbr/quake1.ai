# MCP test-tools design

**Date:** 2026-05-14
**Status:** Draft

## Motivation

The existing MCP server (`sdlquake/mcp/mcp_server.c`) exposes `console_exec` and
`console_tail`, which give an LLM client a powerful escape hatch into the
engine, but it cannot **see the rendered framebuffer**. Bugs like the recent
blood-pool-not-visible issue cannot be verified without the human running the
game, walking to a spot, and reporting what they see.

This spec adds the minimum set of MCP tools needed for an LLM client to
autonomously reproduce + verify a visual bug:

1. Position the camera at a known world location.
2. Wait for the next frame.
3. Capture the framebuffer as a PNG (so the LLM can `Read` it) **or** sample
   a single pixel.

`console_exec` already covers `noclip`, `kill`, `give`, `impulse 9`, etc., so we
do not add wrappers for those.

## Tools

Five new tools added under the existing `tools/call` dispatch. Schemas follow
the existing pattern in `MCP_TOOLS_RESULT`.

### 1. `screenshot`

Saves the current framebuffer to a PNG file and returns the absolute path so
the caller can read it.

**Params:**
- `path` (string, optional) — output path. Default:
  `<exe_dir>/screenshots/shot_<NNNN>.png`, where `NNNN` is the next unused
  zero-padded integer in that directory.

**Returns** (`text` content, JSON-encoded):
```json
{"path": "C:\\Users\\wjbr\\src\\quake1.ai\\screenshots\\shot_0001.png",
 "width": 320, "height": 200}
```

**Errors:**
- `-32603 "no framebuffer"` if `vid.buffer == NULL`.
- `-32603 "screenshot write failed"` if `stbi_write_png` fails.

**Implementation:** new function `VID_SaveScreenshotPNG(const char *path)` in
`sdlquake/platform/vid_sdl.c`. Allocates an `unsigned char[w*h*3]` buffer,
walks `vid.buffer` row by row (respecting `vid.rowbytes`), looks up
`d_8to24table[i]` per pixel, fills RGB triplets, calls `stbi_write_png` with
stride `w*3`. The MCP tool wraps this and synthesises a numbered default path
when none was supplied.

PNG is chosen because `Read` supports it natively. The encoder is vendored as
`sdlquake/vendor/stb_image_write.h` (single-header, MIT). Only the PNG path is
linked (define `STB_IMAGE_WRITE_IMPLEMENTATION` in exactly one .c file).

### 2. `teleport`

Moves the player edict to a new origin (and optionally a new view angle).

**Params:**
- `origin` (array of 3 numbers, required) — `[x, y, z]` in world units.
- `angles` (array of 3 numbers, optional) — `[pitch, yaw, roll]` in degrees.
  Defaults to the player's current angles.

**Returns:**
```json
{"origin": [115.0, 538.0, 50.0], "angles": [90.0, 0.0, 0.0]}
```

**Errors:**
- `-32602 "no active server"` if `sv.active == 0`.
- `-32602 "no player edict"` if `sv.num_edicts < 2` or edict 1 is freed.

**Implementation:** locates the player edict using the same pattern as
`tool_get_player_state` (edict 1 at `sv.edicts + pr_edict_size`). Sets
`player->v.origin` and `player->v.angles` to the new values, sets
`player->v.fixangle = 1` so the engine pushes a `svc_setangle` to the client,
and calls `SV_LinkEdict(player, false)` so the BSP partition is updated. The
client's `cl.viewangles` will pick up the new angles on the next server
snapshot.

### 3. `get_cvar`

Reads a cvar's current value, default value, and flags.

**Params:**
- `name` (string, required).

**Returns:**
```json
{"name": "r_decals_intensity", "value": "1", "default": "1", "flags": 1}
```

`flags` follows the existing `cvar_t` flags bitmask (CVAR_ARCHIVE=1,
CVAR_SERVERINFO=2, …).

**Errors:**
- `-32602 "cvar not found"`.

**Implementation:** `Cvar_FindVar(name)` returns a `cvar_t*` whose
`string`, `defaultvalue`, and `flags` fields are read directly. If
`defaultvalue` is absent in this engine fork (older WinQuake does not store
it), report `"default": ""` and document the limitation; do not block on it.

### 4. `sample_pixel`

Reads the colour of a single pixel from the current framebuffer without
saving an entire screenshot. Useful for cheap assertions ("is the centre
pixel red?").

**Params:**
- `x` (integer, required) — 0 ≤ x < `vid.width`.
- `y` (integer, required) — 0 ≤ y < `vid.height`.

**Returns:**
```json
{"r": 240, "g": 32, "b": 32, "palette_index": 251}
```

**Errors:**
- `-32602 "coords out of range"` if x or y is out of bounds.
- `-32603 "no framebuffer"` if `vid.buffer == NULL`.

**Implementation:** new function `VID_SamplePixel(int x, int y, byte *r,
byte *g, byte *b, byte *idx)` in `vid_sdl.c`. Reads
`vid.buffer[y * vid.rowbytes + x]`, looks up `d_8to24table`, splits into
RGB bytes, and returns both the RGB triple and the raw palette index.

### 5. `wait_frames`

Delays the JSON-RPC response by N rendered frames. Used as a synchronisation
primitive between `teleport` (which sets state but takes effect next frame)
and `screenshot` (which captures the current framebuffer).

**Params:**
- `frames` (integer, required) — clamped to `[1, 60]`.

**Returns** (after N frames have elapsed):
```json
{"frames_waited": 2}
```

**Errors:**
- `-32603 "too many pending waits"` if the pending queue is full (8 slots).

**Implementation:** a small fixed-size array of pending records in
`mcp_server.c`:
```c
typedef struct {
    int  in_use;
    int  frames_remaining;
    char id_json[64];
} mcp_pending_wait_t;
#define MCP_MAX_PENDING_WAITS  8
static mcp_pending_wait_t mcp_pending_waits[MCP_MAX_PENDING_WAITS];
```

When `wait_frames` is dispatched, it finds a free slot, copies the request id,
and stores `frames_remaining = N`. It does **not** call `mcp_send` yet.

`MCP_Frame` gains a second pass after dispatching queued requests: for each
in-use slot it decrements `frames_remaining`; when the counter reaches zero
it sends `{"frames_waited": N}` via `mcp_text_result` and frees the slot.

Eight slots is plenty — the client serialises JSON-RPC requests and only has
one in flight at a time in practice.

## Verification flow for the blood-pool bug

```
1. teleport(origin=[115, 538, 50], angles=[90, 0, 0])    // look straight down
2. wait_frames(2)
3. screenshot()                                           // -> shot_0001.png
4. (LLM Reads the PNG, looks for red blob near centre)
5. sample_pixel(x=160, y=100)                             // -> {r, g, b, idx}
```

This will conclusively answer the open question: is the painted stain
producing visible red in the rendered image, or is the paint mechanically
correct but visually a no-op?

## File touch list

- **new** `sdlquake/vendor/stb_image_write.h` — single-header PNG encoder.
- **new** function `VID_SaveScreenshotPNG` in `sdlquake/platform/vid_sdl.c`.
- **new** function `VID_SamplePixel`         in `sdlquake/platform/vid_sdl.c`.
- **declarations** added to whichever header `vid_sdl.c` exposes
  (`sdlquake/platform/vid_sdl.h` if it exists, else extern decls in
  `mcp_server.c`).
- `sdlquake/mcp/mcp_server.c`:
  - five new `tool_*` functions
  - five new `MCP_TOOLS_RESULT` entries
  - five new dispatch arms inside the `tools/call` branch of `mcp_dispatch`
  - pending-wait machinery (struct + array + tick pass in `MCP_Frame`)
- `sdlquake/build.zig` — add vendor path if not already on the include list;
  define `STB_IMAGE_WRITE_IMPLEMENTATION` in the compilation unit that owns
  the PNG writer (probably `vid_sdl.c`).

## Out of scope

- HTTP/SSE transport changes — already supports the new tools via the
  existing dispatch.
- Authentication or sandboxing of MCP tools.
- Demo-playback support for `teleport` (single-player only, since the player
  edict is on the server).
- Returning the screenshot bytes inline over JSON-RPC — files on disk are
  simpler, and the LLM client already has filesystem `Read` access.
- Trace queries, decal-introspection helpers, surface listing — explicitly
  deferred (these were the "comprehensive" scope and the user picked
  "targeted").

## Testing

Manual smoke test after implementation:

1. `zig build run -- +map e1m1 --mcp-http 8765` (or stdio mode).
2. From the MCP client: `get_cvar` `r_decals_intensity` → returns `"1"`.
3. `teleport([0, 0, 50])`, `wait_frames(2)`, `screenshot()` → produces a PNG
   that visually shows the map at origin.
4. `sample_pixel(160, 100)` → returns plausible RGB.
5. With a real blood pool present at `(115, 538, 0)`: teleport above it, look
   down, screenshot, verify red.

No automated test suite — the project has none. Build success + visual
correctness is the verification method, per `CLAUDE.md`.
