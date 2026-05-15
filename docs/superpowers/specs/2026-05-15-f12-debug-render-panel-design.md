# F12 Debug Render panel — design

**Date:** 2026-05-15
**Status:** design approved, implementation in progress
**Goal:** Curated UI surface for the existing debug-rendering cvars so they're one click away in the F12 overlay instead of buried behind a console / cvars-filter search.

## Motivation

Every visualization toggle this needs (`r_drawbboxes`, `r_drawpaths`, `r_drawpaths_what`, `r_decals_debug`, `sim_nav_debug`, `sim_patrol_debug`, `sim_sense_debug`, `sim_wind_debug`, `sim_light_debug`, plus the renderer toggles `r_fullbright` / `r_lightmap` / `r_drawentities` / `r_drawviewmodel` / `r_dynamic` / `r_novis`) already exists as a cvar. They're reachable today only by:

1. typing them in the Cvars panel filter, or
2. typing them in the console.

Neither path is fast enough to support the "flip on patrol graph, look at it for two seconds, flip it off" debugging loop. The editor's Render toolbar already proves the idiom works (`ui_cvar_checkbox_same("navmesh", "sim_nav_debug")` in `editor_ui.c:300`); this brings the same idiom to the F12 dev overlay where the rest of the game is running.

## Architecture

New file `sdlquake/engine/imgui_debug_panel.c` defines `void DebugPanel_Draw(void)`, called from `imgui_layer.c` in the non-editor branch of `ImguiLayer_Render` alongside `draw_perf()` / `draw_ai()` / `draw_cvars()` / `draw_entities()` / `draw_console()`.

The panel owns **no local state**. Every widget is a thin reflection over a named cvar:

1. read the current value via `Cvar_VariableValue(name)` or `Cvar_VariableString(name)`,
2. render the widget,
3. on change write back via `Cvar_Set(name, value_string)`.

This is the same idiom the existing Cvars panel uses for free-form editing and the editor toolbar uses for its checkboxes. No new engine ABI, no `engine_api_t` / `game_api_t` bump — the `sim_*` cvars owned by `game.dll` are reachable by name through the engine's flat cvar table just like any other cvar.

### imgui_support extensions

Add three thin helpers to `imgui_support.h/.c` (parallel to the existing `ImguiSupport_CvarSet`):

```c
float ImguiSupport_CvarValue(const char *name);
void  ImguiSupport_CvarSetValue(const char *name, float v);
int   ImguiSupport_CvarExists(const char *name);   // for hot-reload safety
```

`CvarValue` returns 0 for missing cvars (matches `Cvar_VariableValue` semantics).
`CvarSetValue` formats and calls `Cvar_Set` so widget code doesn't `snprintf` at every call site.
`CvarExists` lets the panel skip a row whose backing cvar isn't registered yet — defensive against hot-reload mid-frame (the `sim_*` cvars vanish for one frame while the DLL swaps).

### imgui_bridge extensions

The bridge today exposes `IG_Checkbox`, `IG_SliderFloat`, `IG_CollapsingHeader`. We need three more:

```c
int  IG_RadioButton(const char *label, int active);   // returns 1 on click
void IG_BeginDisabled(int disabled);
void IG_EndDisabled(void);
```

All trivial wrappers over `ImGui::RadioButton` / `ImGui::BeginDisabled` / `ImGui::EndDisabled`. Same pattern as the rest of `imgui_bridge.cpp`.

## Layout & content

One window, three or four `IG_CollapsingHeader` sections. Window placement: `IG_SetNextWindowPos(950, 510, IG_Cond_Once)` + `IG_SetNextWindowSize(310, 280, IG_Cond_Once)` — sits below the Entities window so the existing horizontal footprint is preserved (Console runs along the bottom at `(10, 510)` width 1260; the new panel docks at the right end of that row).

### Section 1: AI overlays (default open)

| Row | Widget | Cvar |
|---|---|---|
| Bboxes (density) | `IG_SliderFloat` 0.0–1.0 | `r_drawbboxes` |
| Patrol graph | `IG_SliderFloat` 0.0–1.0 | `r_drawpaths` |
| └ Static graph | `IG_Checkbox` (bit 0 of mask) | `r_drawpaths_what` |
| └ Live links | `IG_Checkbox` (bit 1 of mask) | `r_drawpaths_what` |
| Navmesh | 3-state radio: Off / Ztested / Xray | `sim_nav_debug` |
| Nav range | `IG_SliderFloat` 0–4096 | `sim_nav_debug_range` |
| Patrol AI | 3-state radio | `sim_patrol_debug` |
| Senses | 3-state radio | `sim_sense_debug` |
| Wind | 3-state radio | `sim_wind_debug` |
| Light tier | 3-state radio | `sim_light_debug` |

The two `r_drawpaths_what` checkboxes are wrapped in `IG_BeginDisabled(r_drawpaths == 0)` so they grey out when the master density is zero.

### Section 2: Renderer (default open)

| Row | Widget | Cvar |
|---|---|---|
| Fullbright | checkbox | `r_fullbright` |
| Lightmap only | checkbox | `r_lightmap` |
| Draw entities | checkbox | `r_drawentities` |
| Draw view-model | checkbox | `r_drawviewmodel` |
| Dynamic lights | checkbox | `r_dynamic` |
| No PVS culling | checkbox | `r_novis` |

### Section 3: Decals (default collapsed)

| Row | Widget | Cvar |
|---|---|---|
| Enable decals | checkbox | `r_decals` |
| Debug overlay | checkbox | `r_decals_debug` |
| Intensity | slider 0.0–1.0 | `r_decals_intensity` |
| Max decals | drag-int 0–4096 | `r_decals_max` |

### Section 4: HUD (default collapsed)

| Row | Widget | Cvar |
|---|---|---|
| FPS counter | checkbox | `showfps` |
| Turtle icon | checkbox | `showturtle` |

## Window placement worked example

The current dev overlay layout at 1280×720:

```
+--- Perf (10,10) 180x60 -----+      +--- Cvars (440,10) 500x490 ---+   +--- Entities (950,10) 310x490 ---+
+--- AI (10,80) 420x420 ------+      |                              |   |                                 |
|                             |      |                              |   |                                 |
+-----------------------------+      +------------------------------+   +---------------------------------+
+--- Console (10,510) 1260x280 ---------------------------------+   +--- Debug Render (950,510) 310x280 --+
|                                                               |   |    (NEW — overlaps Console at x=950) |
+---------------------------------------------------------------+   +-------------------------------------+
```

Console is currently 1260 wide starting at x=10, so it ends at x=1270. The new panel at x=950 width 310 (ending at x=1260) **overlaps** the Console horizontally for the bottom 280 pixels.

Fix: shrink the Console width from 1260 to 930 when the new panel exists (so Console spans x=10 to x=940, panel spans x=950 to x=1260 — same as the row above). This is a one-line change to `imgui_layer.c:203` (`IG_SetNextWindowSize(1260, 280, IG_Cond_Once)` → `IG_SetNextWindowSize(930, 280, IG_Cond_Once)`).

`IG_Cond_Once` means existing sessions where the user has dragged the Console aren't affected — the change only seeds new layouts. ImGui has no persistent ini (we set `SetIniFilename(NULL)`), so every launch is a "new session".

## Edge cases & hot-reload safety

1. **Game DLL unloaded mid-reload.** `sim_*` cvars are registered through `hotreload.c:77` which calls `Cvar_RegisterVariable` on the engine's table. Even when `game.dll` is unloaded mid-reload, the cvar entries persist in the engine's flat list — they don't get freed and re-registered. So the panel rows continue to read sane values across reloads. `ImguiSupport_CvarExists` is still used defensively in case a future refactor changes that.

2. **`sim_nav_ztest` (different cvar from the radio).** The editor toolbar uses `sim_nav_ztest` as a separate checkbox alongside `sim_nav_debug`. The new panel does NOT include `sim_nav_ztest` — its 3-state radio (off/ztested/xray) covers the same axis via `sim_nav_debug` itself (per `Sim_DebugZTest` in `sim_main.c:10`, value 1 = ztested, 2+ = xray). Including both would create two UIs talking to each other. The editor toolbar is unchanged.

3. **`r_drawpaths_what` bit reads.** `Cvar_VariableValue` returns float; cast to int before masking. The two checkboxes need defensive reads — when the user types a non-3 value via the console (e.g. `r_drawpaths_what 1`), the panel still reflects the actual bit state.

4. **`r_drawbboxes`/`r_drawpaths` slider range.** Both cvars accept values >1 per the code, but the visible effect saturates. We clamp the slider to [0, 1] in the UI; if the cvar contains something larger from the console, the slider reads as 1.0 and writing back clamps it down — acceptable since values >1 had no extra visual effect.

5. **No persistence across sessions.** Same as every other panel — `IG_SetIniFilename(NULL)`. Toggles reset on game restart. Out of scope.

## Verification

1. `zig build` clean — no warnings.
2. `zig build run -- +map e1m1`:
   - Press F12 → six panels visible, including the new "Debug Render" panel below Entities.
   - **AI overlays**: drag the Bboxes slider — boxes fade in. Drag Patrol graph slider — graph fades in. Toggle "Static graph" off — only live links remain. Toggle "Live links" off — only static remains. Cycle the Navmesh radio through Off / Ztested / Xray — overlay disappears, then appears ztested, then xray. Same check for Patrol AI / Senses / Wind / Light tier radios.
   - **Renderer**: toggle Fullbright — world brightens. Lightmap only — textures replaced with raw lightmap. Draw entities off — monsters/items vanish. Draw view-model off — gun vanishes. Dynamic lights off — muzzle flashes don't light walls. No PVS culling — visible perf drop, all geometry rendered.
   - **Decals**: spawn blood by hurting a monster — verify decals appear, slider scales intensity. Debug overlay shows decal anchor markers.
   - **HUD**: FPS counter appears/disappears at top-right when toggling `showfps`.
3. **Hot-reload safety**: run with `--hot-reload`, edit `sim_ai.c` trivially, `zig build game` — the panel keeps reading `sim_*` values across the reload without crashing.

## Out of scope

- New debug visualizations (this is plumbing only — every cvar already exists).
- Persistence across sessions (matches existing panel behaviour).
- Reordering / customising the panel layout at runtime.
- Bumping `engine_api_t` / `game_api_t` versions (not needed; cvars are reachable by name).
