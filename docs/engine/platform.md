# Platform Layer (SDL3)

Files: `sdlquake/platform/`. These replace the original DirectX/Win32
units one-for-one. The original engine assumed DirectDraw + DirectSound +
Winsock + a Win32 message pump; this layer fronts SDL3 instead.

## sys_sdl.c — system, timing, I/O

Replaces `sys_win.c`. Owns `main()`, sets up SDL, parses the command line,
and enters the host loop.

- **Timing**: `Sys_FloatTime()` returns seconds since startup, sourced from
  `SDL_GetTicksNS()`. The fractional resolution is enough for Quake's
  frame timing (which only needs millisecond accuracy).
- **File I/O**: `Sys_FileOpenRead/Write/Close`, etc., are thin wrappers
  around `fopen`/`fread`. The PAK virtual file system is implemented one
  layer up (`engine_src/common.c`).
- **Error handling**: `Sys_Error` writes to stderr, calls `SDL_Quit`, and
  exits the process. `sdlquake/platform/sys_crash.c` registers a Win32
  unhandled-exception filter; on crash it walks the stack via `dbghelp`
  and writes a `quake_crash_*.log` next to the executable. Without it,
  an SEH-level crash would silently drop with no diagnostic.
- **Command-line flags** (parsed in `main`, before `Host_Init`):
  - `--mcp-stdio` — stdio MCP transport, no stdout chatter
  - `--mcp-http PORT` — HTTP/SSE MCP transport on `localhost:PORT`
  - `--hot-reload` — poll `game.dll` mtime and reload on change
  - All other args (`+map e1m1`, `+set foo bar`, …) are forwarded to
    Quake's own cmdline parser.

## vid_sdl.c — window, framebuffer, present

Replaces `vid_win.c`. The fixed 320×200 8-bit framebuffer model is kept:
the engine writes palette indices into `vid.buffer`, we expand to ARGB
and present.

Layout:

```
SDL_Window  (resizable, defaults to ~3× scale)
  └── SDL_Renderer (any backend SDL picks)
        ├── SDL_Texture (320×200 ARGB8888, STREAMING) ← Quake framebuffer
        └── ImGui textures (font atlas, etc.)
```

Important details:

- `vid.conbuffer` is set to the same buffer as `vid.buffer` — the original
  engine had a separate console framebuffer; we don't. Without this,
  `Draw_Character` writes go into nowhere.
- `SDL_SetRenderLogicalPresentation(320, 200, INTEGER_SCALE)` keeps the
  image pixel-perfect at any window size. The renderer letterboxes when
  the window aspect doesn't match 16:10.
- `VID_SaveScreenshotPNG`/`VID_SamplePixel` read `vid.buffer` through
  `d_8to24table` (matching the on-screen pipeline) and write via
  `stb_image_write`. The byte order is ARGB8888-in-memory, i.e. B, G, R,
  A — see [rendering.md](rendering.md#screenshots--introspection).

## in_sdl.c — input

Replaces `in_win.c`. Polls SDL events each frame in
`Sys_SendKeyEvents` and translates them into Quake key events.

- **Scancode mapping**: SDL3's `SDL_Scancode` enum is mapped to Quake's
  flat ASCII-or-key constants (`K_UPARROW`, `K_F1`, …). This is the file
  to edit for new keybindings.
- **Mouse**: relative mode is used in-game (`SDL_SetWindowRelativeMouseMode`);
  the menu / editor flips it back to absolute. Mouse deltas drive
  `IN_Move` which updates `cl.viewangles`.
- **Pitch-drift fix**: `IN_Move` calls `V_StopPitchDrift()` every frame
  unconditionally — not just on mouse movement as the original did. Without
  this, holding forward for `v_centermove` seconds re-arms pitch drift and
  the view snaps awkwardly. This is the kind of one-line fix that's easy
  to lose in a future refactor; the comment in the source flags it.

## snd_sdl.c — audio

Replaces `snd_win.c`. SDL3's audio model is callback-driven, Quake's is a
DMA ring buffer the mixer fills as it sees fit. The bridge is
`SDL_AudioStream` in get-callback mode: SDL pulls from the stream, our
callback fills the stream from `shm->buffer` based on `shm->samplepos`.

```
Quake mixer (main thread, SCR_UpdateScreen step)
       │
       ▼
   shm->buffer (16-bit signed PCM ring, configured size)
       │   (no lock; producer writes, consumer reads, indexes are atomic
       │    enough on x86 that no torn-frame is audible)
       ▼
SDL audio thread (SDL_AudioStream get-callback)
       │
       ▼
   PortAudio / WASAPI / CoreAudio / ALSA …
```

Sample rate is fixed (default 11025 Hz mono — yes, 1996 was rough). The
`snd_*` cvars affect the mixer, not the SDL stream config. Music
playback (cdaudio) is stubbed to `cd_null.c`.

## net_sdl.c — networking

Replaces `net_wins.c`. Default config is loopback-only — split-screen single
player and demo playback never leave the process. Datagram (LAN /
internet) goes through Winsock unmodified; `ws2_32.lib` is linked from
the vendored SDL3 lib dir. See [networking.md](networking.md) for the
protocol level.

The MCP server's HTTP/SSE transport (`mcp_server.c`) also uses Winsock
directly — it doesn't share state with `net_sdl.c`, but on Windows both
end up in the same `WSAStartup` reference.

## winquake.h shim

`sdlquake/platform/winquake.h` is a **shadowing** header — it appears
ahead of `sdlquake/engine_src/winquake.h` on the include path so the
DirectDraw and DirectSound type declarations in the original become
empty no-op typedefs. Any engine `.c` that calls a DirectX function
either references a function we've replaced (e.g. `VID_Init`) or one we
stub. If you see "implicit function declaration" errors during a port,
that's the file to extend.

## mgraph.h, vid_palette.h

Same shadowing trick for `mgraph.h` (MGL — original software-renderer
abstraction; we don't use any of it) and `vid_palette.h` (the old
Win32-specific palette interface). Both are dummy headers that make the
engine's `#include`s resolve to harmless empty types.
