# ImGui Docking Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the editor's hand-pinned floating-window UI with real ImGui docking — a passthrough host dockspace that frames the 3D viewport, with a baked default layout that persists across sessions.

**Architecture:** Vendor ImGui's `docking` branch (same files + backends as the current master build). Enable `DockingEnable`, submit a `DockSpaceOverViewport` with a `PassthruCentralNode` once per editor frame (so the 3D scene shows through the empty centre), and bake a default layout with `DockBuilder` on first run / on demand. The editor's per-window `SetNextWindowPos/Size` forcing is removed so windows obey their dock nodes. All dock-tree logic lives in `imgui_bridge.cpp` behind ~3 new `IG_*` C shims.

**Tech Stack:** C (engine + editor), C++ (ImGui bridge), Zig build, SDL3 + SDL_GPU3 ImGui backends, Dear ImGui docking branch (~1.92.x).

---

## Conventions for this plan

- **No unit-test suite exists** (per `CLAUDE.md`: "Build success and visual/audio correctness in-game are the verification methods"). Each task therefore verifies with `zig build` success **plus** a live in-game smoke test, in place of failing-test-first.
- **Build:** `zig build` from the repo root (`/Users/wjbr/src/quake1.ai`).
- **Run (smoke):** `zig build run -- -nosound -nofocus +map e1m1` — the `-nosound -nofocus` flags keep it from stealing focus/mouse/audio during a visible test. Open the editor from the in-game console with the `editor` command (it toggles); switch editor modes with `editor_mode 0|1|2` (0 Map, 1 Particle, 2 Actor). The user is at the machine — let them eyeball the result (lightweight live verify); ImGui panels are invisible to the software-framebuffer screenshot, so if a captured screenshot is needed use MCP `screenshot_gpu`, not `screenshot`.
- **Commits:** all commit messages end with the standard trailer:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- **Spec corrections folded into this plan** (the committed spec assumed otherwise):
  1. `imgui.ini` persistence is currently **disabled** (`imgui_layer.c:737` calls `IG_SetIniFilename(NULL)`). This plan **enables** it. The manual `s_dock_cond` re-snap hack exists precisely because there was no persistence.
  2. ImGui only runs `NewFrame`/`Render` when the editor is open or the perf HUD is showing (`imgui_layer.c:790-799`), not every gameplay frame. The host dockspace is submitted **inside the editor branch**, not unconditionally.
  3. Docking the F3 dev-overlay panels (the spec's bottom-tabbed `Console/Cvars/Entities/AI/Debug Render`) is split into the **optional Task 5** — the core ask is the editor. Confirm before doing Task 5.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `sdlquake/vendor/imgui-1.92.8-docking/**` | Create | Vendored ImGui docking-branch source + SDL3/SDL_GPU3 backends |
| `sdlquake/vendor/imgui-1.92.8/**` | Delete | Old master-branch vendored tree (after build verified) |
| `build.zig` (line 116) | Modify | Point `imgui_dir` at the new vendored tree |
| `sdlquake/engine/imgui_bridge.cpp` | Modify | `IG_EnableDocking`, `IG_HostDockSpace`, `IG_RequestDefaultLayout` + the `DockBuilder` bake |
| `sdlquake/engine/imgui_bridge.h` | Modify | Declarations for the three new shims |
| `sdlquake/engine/imgui_layer.c` | Modify | Init (enable docking + ini + first-run request); call `IG_HostDockSpace` in the editor branch |
| `sdlquake/engine/editor/editor_ui.c` | Modify | Remove forced pos/size for toolbar/Brushes/Inspector + `s_dock_cond` + resize-snap; add "Reset layout" button |
| `sdlquake/engine/editor/edit_particle.c` | Modify | Remove forced pos/size for the two particle panels |
| `sdlquake/engine/editor/edit_actor.c` | Modify | Remove forced pos/size for the Actor panel |
| `.gitignore` | Modify | Ignore the generated `imgui.ini` |

---

## Task 1: Vendor the ImGui docking branch

**Files:**
- Create: `sdlquake/vendor/imgui-1.92.8-docking/` (+ `backends/`)
- Delete: `sdlquake/vendor/imgui-1.92.8/`
- Modify: `build.zig:116`

- [ ] **Step 1: Fetch the docking branch**

Run (if the sandbox blocks network, run with the sandbox disabled, or ask the user to run it with a leading `!`):

```bash
rm -rf /tmp/imgui-docking
git clone --depth 1 --branch docking https://github.com/ocornut/imgui.git /tmp/imgui-docking
grep '#define IMGUI_VERSION ' /tmp/imgui-docking/imgui.h
```
Expected: a version string like `#define IMGUI_VERSION "1.92.8"` (docking branch may read a slightly newer WIP like `1.92.9`; that's fine — Task 2's build is the real compatibility gate). Confirm `/tmp/imgui-docking/backends/imgui_impl_sdlgpu3.cpp` exists.

- [ ] **Step 2: Copy the docking sources into a new vendored dir, preserving the local `imconfig.h`**

```bash
SRC=/tmp/imgui-docking
DST=sdlquake/vendor/imgui-1.92.8-docking
mkdir -p "$DST/backends"
cp "$SRC"/imgui.h "$SRC"/imgui.cpp "$SRC"/imgui_draw.cpp "$SRC"/imgui_tables.cpp \
   "$SRC"/imgui_widgets.cpp "$SRC"/imgui_internal.h \
   "$SRC"/imstb_rectpack.h "$SRC"/imstb_textedit.h "$SRC"/imstb_truetype.h "$DST"/
cp "$SRC"/backends/imgui_impl_sdl3.h "$SRC"/backends/imgui_impl_sdl3.cpp \
   "$SRC"/backends/imgui_impl_sdlgpu3.h "$SRC"/backends/imgui_impl_sdlgpu3.cpp \
   "$SRC"/backends/imgui_impl_sdlgpu3_shaders.h "$DST"/backends/
# Preserve the project's existing imconfig.h (may carry local edits):
cp sdlquake/vendor/imgui-1.92.8/imconfig.h "$DST"/imconfig.h
diff sdlquake/vendor/imgui-1.92.8/imconfig.h "$SRC"/imconfig.h || \
  echo "NOTE: upstream imconfig.h differs — kept the project's version"
```

- [ ] **Step 3: Point the build at the new dir**

Modify `build.zig:116`:

```zig
    const imgui_dir = "sdlquake/vendor/imgui-1.92.8-docking";
```
(was `"sdlquake/vendor/imgui-1.92.8"`)

- [ ] **Step 4: Build and verify it compiles + runs unchanged**

Run: `zig build`
Expected: clean build. If it fails on an ImGui backend API mismatch (the docking HEAD drifted from 1.92.8), check out an older docking commit nearer 1.92.8 in `/tmp/imgui-docking` (`git -C /tmp/imgui-docking log --oneline` then `git checkout <older-sha>`), re-copy (Step 2), rebuild.

Then: `zig build run -- -nosound -nofocus +map e1m1`, open the console, type `editor`. Expected: the editor opens and looks **exactly as before** (still floating panels — docking isn't wired yet). Close the game.

- [ ] **Step 5: Remove the old tree and commit**

```bash
git rm -r sdlquake/vendor/imgui-1.92.8
git add sdlquake/vendor/imgui-1.92.8-docking build.zig
git commit -m "build(imgui): vendor the docking branch (no behaviour change)"
```

---

## Task 2: Add the docking bridge API (dormant)

Adds the C shims + the `DockBuilder` bake to the bridge. Nothing calls them yet, so behaviour is unchanged — this task exists to prove the docking-branch `DockBuilder`/`DockSpaceOverViewport` API compiles against our vendored tree **before** the editor flip.

**Files:**
- Modify: `sdlquake/engine/imgui_bridge.cpp`
- Modify: `sdlquake/engine/imgui_bridge.h`

- [ ] **Step 1: Declare the three shims in the header**

In `sdlquake/engine/imgui_bridge.h`, immediately after the `IG_SetIniFilename` declaration (line ~75), add:

```c
// Docking (docking-branch ImGui). IG_EnableDocking sets the config flag once
// at init. IG_HostDockSpace submits the full-viewport passthrough host dock
// node — call it once per frame BEFORE the windows that dock into it.
// IG_RequestDefaultLayout (re)bakes the default layout on the next
// IG_HostDockSpace call (first run + the editor's "Reset layout" button).
void  IG_EnableDocking(void);
void  IG_HostDockSpace(void);
void  IG_RequestDefaultLayout(void);
```

- [ ] **Step 2: Add the include + implementations to the bridge**

In `sdlquake/engine/imgui_bridge.cpp`, add the internal header after the existing includes (after line 7, `#include "imgui_bridge.h"`):

```cpp
#include "imgui_internal.h"   // DockBuilder* lives here
```

Then, just before the closing `} // extern "C"` (line ~311), add:

```cpp
// --- Docking ---------------------------------------------------------------
static bool s_rebuild_layout = false;

void IG_EnableDocking(void)
{
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
}

void IG_RequestDefaultLayout(void) { s_rebuild_layout = true; }

// Carve top / left / right strips off the host node; the remaining `centre`
// node is left empty — with PassthruCentralNode it renders transparent and
// the 3D scene shows through it. Windows are docked by title; the per-mode
// panels share the left/right strips (only one mode draws at a time).
static void ig_build_default_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id,
        ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace_id, vp->Size);

    ImGuiID centre = dockspace_id;
    ImGuiID top   = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Up,    0.16f, NULL, &centre);
    ImGuiID left  = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left,  0.20f, NULL, &centre);
    ImGuiID right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.28f, NULL, &centre);

    ImGui::DockBuilderDockWindow("Editor",             top);
    ImGui::DockBuilderDockWindow("Editor Mode",        top);
    ImGui::DockBuilderDockWindow("Brushes",            left);
    ImGui::DockBuilderDockWindow("Particle Effects",   left);
    ImGui::DockBuilderDockWindow("Actor",              left);
    ImGui::DockBuilderDockWindow("Inspector",          right);
    ImGui::DockBuilderDockWindow("Particle Inspector", right);

    ImGui::DockBuilderFinish(dockspace_id);
}

void IG_HostDockSpace(void)
{
    ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(
        0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
    if (s_rebuild_layout) {
        s_rebuild_layout = false;
        ig_build_default_layout(dockspace_id);
    }
}
```

Note: if the build reports the wrong argument count for `DockSpaceOverViewport`, this vendored revision uses the older signature — drop the leading `0`: `ImGui::DockSpaceOverViewport(ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode)`.

- [ ] **Step 3: Build**

Run: `zig build`
Expected: clean build (the new functions compile but are unused — that's fine; non-`static` functions don't warn).

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine/imgui_bridge.cpp sdlquake/engine/imgui_bridge.h
git commit -m "feat(editor): add ImGui docking bridge shims (dormant)"
```

---

## Task 3: Flip the editor to docking

Wires everything: enable docking + ini at init, submit the host dockspace in the editor branch, and remove all the per-window forced positioning so windows obey their dock nodes.

**Files:**
- Modify: `sdlquake/engine/imgui_layer.c` (init + render branch)
- Modify: `sdlquake/engine/editor/editor_ui.c` (toolbar + brush list + inspector + `s_dock_cond` + resize-snap)
- Modify: `sdlquake/engine/editor/edit_particle.c`
- Modify: `sdlquake/engine/editor/edit_actor.c`

- [ ] **Step 1: Enable docking + ini persistence at init**

In `sdlquake/engine/imgui_layer.c`, replace lines 736-738:

```c
    IG_CreateContext();
    IG_SetIniFilename(NULL);
    IG_StyleColorsDark();
```
with:

```c
    IG_CreateContext();
    IG_EnableDocking();
    // Enable layout persistence (was disabled). ImGui writes "imgui.ini" to the
    // process CWD. If it's absent (first run / fresh build), bake the default
    // dock layout the first time the editor opens.
    {
        static const char *ini = "imgui.ini";
        FILE *f = fopen(ini, "rb");
        if (f) fclose(f);
        else   IG_RequestDefaultLayout();
        IG_SetIniFilename(ini);
    }
    IG_StyleColorsDark();
```
(`imgui_layer.c` already includes `<stdio.h>` via `quakedef.h`/its own use of `snprintf`; if the build reports `fopen` implicitly declared, add `#include <stdio.h>` near the top.)

- [ ] **Step 2: Submit the host dockspace in the editor branch**

In `sdlquake/engine/imgui_layer.c`, replace the editor branch at lines 816-819:

```c
        else if (Editor_IsOpen())
        {
            Editor_DrawUI();
        }
```
with:

```c
        else if (Editor_IsOpen())
        {
            IG_HostDockSpace();   // passthrough host node; windows dock into it
            Editor_DrawUI();
        }
```

- [ ] **Step 3: Remove forced positioning from the toolbar**

In `sdlquake/engine/editor/editor_ui.c` `draw_toolbar`, replace lines 237-242:

```c
    float disp_w = 1280, disp_h = 720;
    IG_GetDisplaySize(&disp_w, &disp_h);

    IG_SetNextWindowPos((float)UI_PAD, (float)UI_PAD, s_dock_cond);
    IG_SetNextWindowSize(disp_w - 2 * UI_PAD, (float)UI_TOOLBAR_H, s_dock_cond);
    if (!IG_Begin("Editor", NULL, IG_WF_None)) { IG_End(); return; }
```
with:

```c
    if (!IG_Begin("Editor", NULL, IG_WF_None)) { IG_End(); return; }
```

- [ ] **Step 4: Remove forced positioning from the brush list**

In `editor_ui.c` `draw_brush_list`, replace lines 580-588:

```c
    float disp_w = 1280, disp_h = 720;
    IG_GetDisplaySize(&disp_w, &disp_h);

    float y    = (float)(UI_PAD + UI_TOOLBAR_H + UI_PAD);
    float h    = disp_h - y - UI_PAD;

    IG_SetNextWindowPos((float)UI_PAD, y, s_dock_cond);
    IG_SetNextWindowSize((float)UI_LEFT_W, h, s_dock_cond);
    if (!IG_Begin("Brushes", NULL, IG_WF_None)) { IG_End(); return; }
```
with:

```c
    if (!IG_Begin("Brushes", NULL, IG_WF_None)) { IG_End(); return; }
```
(Keep the `char buf[128];` on line 579 — it's used later in the function.)

- [ ] **Step 5: Remove forced positioning from the inspector**

In `editor_ui.c` `draw_inspector`, replace lines 2342-2351:

```c
    float disp_w = 1280, disp_h = 720;
    IG_GetDisplaySize(&disp_w, &disp_h);

    float y = (float)(UI_PAD + UI_TOOLBAR_H + UI_PAD);
    float h = disp_h - y - UI_PAD;
    float x = disp_w - UI_RIGHT_W - UI_PAD;

    IG_SetNextWindowPos(x, y, s_dock_cond);
    IG_SetNextWindowSize((float)UI_RIGHT_W, h, s_dock_cond);
    if (!IG_Begin("Inspector", NULL, IG_WF_None)) { IG_End(); return; }
```
with:

```c
    if (!IG_Begin("Inspector", NULL, IG_WF_None)) { IG_End(); return; }
```

- [ ] **Step 6: Remove `s_dock_cond` and the resize-snap block**

In `editor_ui.c`:

(a) Delete the declaration at line 210:
```c
static int s_dock_cond = IG_Cond_FirstUseEver;
```

(b) In `MapMode_DrawUI`, replace lines 2616-2633 (the comment + the `{ static float ... }` resize-snap block):

```c
    // Refresh the dock-panel reflow condition for this frame: Always on the
    // first frame ever and on any frame where the SDL window size changed,
    // FirstUseEver otherwise so manual drags between resizes stay put.
    {
        static float s_last_w = -1, s_last_h = -1;
        float disp_w = 1280, disp_h = 720;
        IG_GetDisplaySize(&disp_w, &disp_h);
        if (disp_w != s_last_w || disp_h != s_last_h)
        {
            s_last_w = disp_w;
            s_last_h = disp_h;
            s_dock_cond = IG_Cond_Always;
        }
        else
        {
            s_dock_cond = IG_Cond_FirstUseEver;
        }
    }

    draw_toolbar();
```
with:

```c
    draw_toolbar();
```

- [ ] **Step 7: Remove forced positioning from the particle panels**

In `sdlquake/engine/editor/edit_particle.c` `panel_effect_list`, replace lines 125-127:

```c
    IG_SetNextWindowPos(8.0f, 120.0f, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize(220.0f, 360.0f, IG_Cond_FirstUseEver);
    if (!IG_Begin("Particle Effects", NULL, IG_WF_None)) { IG_End(); return; }
```
with:

```c
    if (!IG_Begin("Particle Effects", NULL, IG_WF_None)) { IG_End(); return; }
```

And in the particle inspector, replace lines 204-206:

```c
    IG_SetNextWindowPos(236.0f, 120.0f, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize(330.0f, 560.0f, IG_Cond_FirstUseEver);
    if (!IG_Begin("Particle Inspector", NULL, IG_WF_None)) { IG_End(); return; }
```
with:

```c
    if (!IG_Begin("Particle Inspector", NULL, IG_WF_None)) { IG_End(); return; }
```

- [ ] **Step 8: Remove forced positioning from the Actor panel**

In `sdlquake/engine/editor/edit_actor.c` `actor_draw_ui`, replace lines 453-455:

```c
    IG_SetNextWindowPos (8.0f, 120.0f, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize (286.0f, 520.0f, IG_Cond_FirstUseEver);
    if (!IG_Begin ("Actor", NULL, IG_WF_None)) { IG_End (); return; }
```
with:

```c
    if (!IG_Begin ("Actor", NULL, IG_WF_None)) { IG_End (); return; }
```

- [ ] **Step 9: Build**

Run: `zig build`
Expected: clean build. If unused-variable errors appear for `UI_PAD`/`UI_TOOLBAR_H`/`UI_LEFT_W`/`UI_RIGHT_W` (they're `#define`s — they won't warn) or for a leftover local, remove the flagged local.

- [ ] **Step 10: Live smoke test**

Run: `zig build run -- -nosound -nofocus +map e1m1`, then console `editor`.
Expected (have the user eyeball it; use MCP `screenshot_gpu` if a capture is wanted):
- The toolbar is docked across the **top**, `Brushes` docked **left**, `Inspector` docked **right**, and the map is visible in the **centre** (passthrough) — no free-floating panels.
- `editor_mode 1` → `Particle Effects` + `Particle Inspector` fill the left/right slots; `editor_mode 2` → `Actor` fills the left slot. Nothing floats.
- Mouse-pick / drag a brush in the centre still works (input passes through the central node). If picking is dead in the centre, STOP — see Risk note in the spec about `IG_WantCaptureMouse`.
- Drag a panel's tab onto another edge → it re-docks. Resize a split → panels resize.

- [ ] **Step 11: Commit**

```bash
git add sdlquake/engine/imgui_layer.c sdlquake/engine/editor/editor_ui.c \
        sdlquake/engine/editor/edit_particle.c sdlquake/engine/editor/edit_actor.c
git commit -m "feat(editor): dock editor panels via passthrough host dockspace"
```

---

## Task 4: "Reset layout" button + ignore imgui.ini

**Files:**
- Modify: `sdlquake/engine/editor/editor_ui.c` (toolbar)
- Modify: `.gitignore`

- [ ] **Step 1: Add the Reset layout button**

In `sdlquake/engine/editor/editor_ui.c` `draw_toolbar`, after the `Close (F2)` button (line 267):

```c
    if (ui_btn_same("Close (F2)"))              ui_exec("editor\n");
    if (ui_btn_same("Reset layout"))            IG_RequestDefaultLayout();
```
(`editor_ui.c` already includes `imgui_bridge.h` — it calls `IG_*` throughout — so `IG_RequestDefaultLayout` is declared.)

- [ ] **Step 2: Ignore the generated ini**

Add to `.gitignore`:

```
imgui.ini
```

- [ ] **Step 3: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 4: Live smoke test**

Run: `zig build run -- -nosound -nofocus +map e1m1`, console `editor`. Drag `Inspector` out to float, then click **Reset layout** in the toolbar.
Expected: the layout snaps back to the baked default (toolbar top / Brushes left / Inspector right / centre viewport). Quit and relaunch + `editor`: the layout is **remembered** (persisted to `imgui.ini`), not re-baked.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine/editor/editor_ui.c .gitignore
git commit -m "feat(editor): Reset layout button + gitignore imgui.ini"
```

---

## Task 5 (OPTIONAL — confirm scope first): Dock the F3 dev-overlay panels

This realises the spec's bottom-tabbed dev panels. It's optional because the F3 dev overlay already has a deliberate tiled layout (`compute_layout`) — it isn't "floating" in the annoying way the editor was. **Confirm with the user before doing this.** Leave `FPS` and `Profile` as floating HUD (they're `NoSavedSettings`).

**Files:**
- Modify: `sdlquake/engine/imgui_bridge.cpp` (`ig_build_default_layout` — add bottom split)
- Modify: `sdlquake/engine/imgui_layer.c` (call `IG_HostDockSpace` in the dev-overlay branch; drop forced pos in `draw_ai`/`draw_cvars`/`draw_console`/`draw_entities` and `DebugPanel_Draw`)

- [ ] **Step 1: Add a bottom node to the bake and dock the dev panels**

In `ig_build_default_layout` (`imgui_bridge.cpp`), add a bottom split after the `right` split and dock the dev windows into it:

```c
    ImGuiID right  = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.28f, NULL, &centre);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down,  0.26f, NULL, &centre);
```
and after the existing `DockBuilderDockWindow` calls:

```c
    ImGui::DockBuilderDockWindow("Console",      bottom);
    ImGui::DockBuilderDockWindow("Cvars",        bottom);
    ImGui::DockBuilderDockWindow("Entities",     bottom);
    ImGui::DockBuilderDockWindow("AI",           bottom);
    ImGui::DockBuilderDockWindow("Debug Render", bottom);
```

- [ ] **Step 2: Submit the host dockspace in the dev-overlay branch**

In `imgui_layer.c` `ImguiLayer_PrepareGPU`, in the final `else` branch (lines 820-830), add `IG_HostDockSpace();` as the first call (right after the `{` on line 821), before `compute_layout();`.

- [ ] **Step 3: Drop the forced positioning for the dockable dev panels**

For each of `draw_ai`, `draw_cvars`, `draw_console`, `draw_entities` (in `imgui_layer.c`) and `DebugPanel_Draw` (in `imgui_debug_panel.c`): locate the `IG_SetNextWindowPos(...)` / `IG_SetNextWindowSize(...)` pair that precedes the panel's `IG_Begin` (same two-line `IG_Cond_Always` pattern as `draw_fps` at `imgui_layer.c:132-133`) and delete those two lines, leaving the `IG_Begin`. **Do NOT touch `draw_fps` or `draw_profile`** — they stay floating HUD. `compute_layout()` still runs (it positions FPS/Profile); the now-unused `s_ai`/`s_cvars`/`s_console`/`s_entities`/`s_debug` rect fields it computes are harmless. If the build flags an unused static, leave the rect assignments in `compute_layout` (they're writes to file-scope statics — no warning) — only remove the `IG_SetNextWindow*` calls in the draw functions.

- [ ] **Step 4: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 5: Live smoke test**

Run: `zig build run -- -nosound -nofocus +map e1m1`, press **F3** (dev overlay). Expected: `Console`/`Cvars`/`Entities`/`AI`/`Debug Render` are tabbed in a bottom dock node; `FPS`/`Profile` still float as HUD. The map shows through the centre.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/engine/imgui_bridge.cpp sdlquake/engine/imgui_layer.c \
        sdlquake/engine/imgui_debug_panel.c
git commit -m "feat(editor): dock the F3 dev-overlay panels (bottom tabbed)"
```

---

## Self-Review

**Spec coverage:**
- Vendor docking branch → Task 1. ✓
- `DockingEnable` flag → Task 3 Step 1 (`IG_EnableDocking`, defined Task 2). ✓
- Passthrough host dockspace framing the 3D view → Task 2 (`IG_HostDockSpace`) + Task 3 Step 2. ✓
- Baked default layout (toolbar top / Brushes-left / Inspector-right / viewport centre) → Task 2 `ig_build_default_layout`. ✓
- Mode panels share slots → Task 2 (same `left`/`right` nodes for Brushes/Particle Effects/Actor and Inspector/Particle Inspector). ✓
- `Editor Mode` switcher placed → Task 2 (docked `top`, tabbed). ✓
- Persistence via `imgui.ini` → Task 3 Step 1 (note: spec wrongly assumed already-on; plan **enables** it). ✓
- Reset layout → Task 4. ✓
- Remove the manual pin code → Task 3 Steps 3-8. ✓
- New C shim surface → Task 2 (refined from the spec's "~6 wrappers" to **3** shims + an internal bake fn — simpler, keeps ids/ratios in the bridge). ✓
- Multi-viewport off → not enabled anywhere (only `DockingEnable`). ✓
- Dev panels in bottom dock → **optional Task 5** (spec verification item 5). Flagged for confirmation. ✓
- Transient dialogs / FPS / Profile stay floating → untouched by any task. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete before/after. The only intentional non-exactness is Task 5 Step 3 (locate-and-delete the 2-line pattern per dev panel) — bounded by the exact `draw_fps:132-133` reference and the explicit "don't touch FPS/Profile"; acceptable for an optional, confirm-first phase.

**Type/name consistency:** `IG_EnableDocking` / `IG_HostDockSpace` / `IG_RequestDefaultLayout` and `ig_build_default_layout` / `s_rebuild_layout` are spelled identically across Task 2 (def) and Tasks 3-5 (use). Window title strings used in the bake match the exact `IG_Begin(...)` titles found in the source (`"Editor"`, `"Brushes"`, `"Inspector"`, `"Particle Effects"`, `"Particle Inspector"`, `"Actor"`, `"Editor Mode"`, and for Task 5 `"Console"`/`"Cvars"`/`"Entities"`/`"AI"`/`"Debug Render"`). ✓

---

## Notes / risks (carried from the spec)

- **Primary risk — branch-pin parity:** Task 1 Step 4 + Task 2 Step 3 are the gates. If the docking HEAD's SDL_GPU3 backend drifted from 1.92.8, reconcile before proceeding (fall back to an older docking commit).
- **Input passthrough:** Task 3 Step 10 explicitly checks 3D picking/gizmo in the centre. The editor reads mouse gated on `IG_WantCaptureMouse`; the passthrough central node reports no capture when hovering the empty centre, so this should already work — but verify.
- **`imgui.ini` location:** written to the process CWD (matches ImGui's default). Consistent + writable for `zig build run`. If a different location is wanted later, build the path from `com_gamedir` in Task 3 Step 1 (easy change).
