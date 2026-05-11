# Resolution Scaling Menu

## Summary

Add a Video Options menu to the SDL3 platform layer that lets the player choose window scale (1x, 2x, 3x) and persist the choice across restarts.

## Scope

All changes in `sdlquake/platform/vid_sdl.c`. No engine_src changes.

## Cvar

```c
cvar_t vid_scale = {"vid_scale", "0", true};
```

- `0` = auto-detect (largest integer scale that fits the usable desktop area). Stays `0` in config so the next launch also auto-detects.
- `1` / `2` / `3` = explicit scale; saved to `config.cfg`.

## Startup

`VID_Init` registers `vid_scale` before computing the window size. If the cvar value is non-zero, skip auto-detect and use it directly. If zero, auto-detect as before (do not write the cvar; leave it at `0`).

## Menu

`VID_MenuDraw` draws the classic Quake-style video menu:

- Header: `gfx/p_option.lmp` title pic (same as Options page)
- Three items at fixed y positions:
  - `1x  320x200`
  - `2x  640x400`
  - `3x  960x600`
- The item matching the active window scale has the animated cursor character beside it.

`VID_MenuKey` handles:

- `K_UPARROW` / `K_DOWNARROW` — move cursor, play `misc/menu1.wav`
- `K_ENTER` / `K_SPACE` — apply: call `SDL_SetWindowSize`, `Cvar_SetValue("vid_scale", n)`, play `misc/menu2.wav`
- `K_ESCAPE` — `M_Menu_Options_f()`

Both functions are wired at the end of `VID_Init`:

```c
vid_menudrawfn = VID_MenuDraw;
vid_menukeyfn  = VID_MenuKey;
```

## State

A file-static `int vid_menu_cursor` (0/1/2) tracks the highlighted item. Initialised from `(int)vid_scale.value - 1`, clamped to `[0, 2]`. When `vid_scale` is `0` (auto), cursor defaults to `vid_scale_active - 1` where `vid_scale_active` is a file-static int set during `VID_Init` to the scale that was actually applied.

## Out of scope

- Fullscreen toggle
- Scales beyond 3x
- ImGui alternative UI
