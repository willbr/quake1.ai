# Editor Live View — Phase 1 Design Spec

**Date:** 2026-05-12
**Status:** Approved

---

## Overview

Split the editor's "Brushes" panel data source by `editor_view_mode`. In **live** view, the panel iterates the running engine's edict list (`sv.edicts`) and shows one row per alive edict with its classname. In **map** view, the panel keeps its current behaviour (iterates `edit_scene.entities[]`).

This is the first of two phases. Phase 1 makes live edicts **visible and selectable in the list panel** with a bbox highlight on the selected edict. Phase 2 (separate spec, not part of this design) extends 3D-viewport picking, gizmo drag, and KV editing to live edicts.

Phase 1 also fixes the reported bug that runtime-spawned ents (projectiles, gibs, drops) have no visible classname — reading `v.classname` via `PR_GetString` for every live row sidesteps the .map-data path that's currently the only source.

---

## Scope

**In scope (Phase 1):**

- New live-selection state, parallel to the existing .map selection.
- "Brushes" panel branches on view mode: live mode iterates `sv.edicts`, displays `[edict#] classname`.
- Clicking a live row selects that edict (single or shift-multi).
- Render: selected live edict's absmin/absmax bbox draws with the existing selection highlight colour.
- View-mode toggle clears the inactive selection list.

**Out of scope (Phase 2):**

- 3D-viewport picking on live edicts (`Editor_PickAt` extension).
- Gizmo translate/rotate of live edicts.
- KV side-panel editing of live edicts.
- Anchor / arrow / link-arrow render passes for live edicts.

The map-view code path is untouched.

---

## View-mode semantics (clarified)

| View mode | Brushes panel iterates | What's selectable |
|---|---|---|
| `editor_view_mode 0` (live) | `sv.edicts[1..num_edicts-1]`, non-free | Live edicts only |
| `editor_view_mode 1` (map) | `edit_scene.entities[]` | .map ents only |

Worldspawn (edict 0) is excluded from the live list — it's the BSP world, not a per-entity selectable thing.

---

## Selection model

Add a separate live-selection list alongside the existing `(e_idx, b_idx)` tuple list. The two are mutually exclusive — only one is active per view mode.

New API in `edit_scene.h` / scene.c:

```c
void Scene_LiveSelectionClear(void);
void Scene_LiveSelectionAdd(int edict_num);
void Scene_LiveSelectionToggle(int edict_num);
int  Scene_LiveSelectionContains(int edict_num);
int  Scene_NumLiveSelected(void);
int  Scene_GetLiveSelected(int i);     // returns edict_num, or -1 if i out of range
```

Storage: a flat `int[]` of edict numbers, grown like the existing selection arrays. Capacity matches the existing selection's growth pattern.

**View-mode-toggle behaviour** (in `editor.c` cvar-set hook for `editor_view_mode`, or wherever the mode flip is observed): when the mode changes, clear the now-inactive selection list. This prevents a stale .map selection from driving the gizmo while the user is browsing live edicts (and vice versa).

The existing gizmo, `Editor_PickAt`, and `Editor_EntityAnchor` are not changed in Phase 1 — they continue to read .map selection. In live view, the gizmo simply doesn't appear (because .map selection is empty after the toggle clear, and Phase 2 will add a separate live-gizmo path).

---

## Brushes panel — live branch

Pseudocode for the new live branch in `draw_brush_list`:

```c
if (view_live) {
    int shown = 0;
    for (int en = 1; en < sv.num_edicts; en++) {
        edict_t *ed = EDICT_NUM(en);
        if (ed->free) continue;
        const char *cls = PR_GetString(ed->v.classname);
        if (!cls || !cls[0]) cls = "(no classname)";

        int sel = Scene_LiveSelectionContains(en);
        char buf[128];
        snprintf(buf, sizeof(buf), "[%d] %s##le%d", en, cls, en);

        IG_PushID_Int(en);
        if (IG_Selectable(buf, sel, 0)) {
            SDL_Keymod mod = SDL_GetModState();
            int shift = (mod & SDL_KMOD_SHIFT) != 0;
            if (shift)            Scene_LiveSelectionToggle(en);
            else                  { Scene_LiveSelectionClear(); Scene_LiveSelectionAdd(en); }
        }
        IG_PopID();
        shown++;
    }
    {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "%d live edicts", shown);
        IG_TextUnformatted(hdr);    // (rendered before the list, in practice)
    }
    return;
}
// else: existing map-view code path, unchanged
```

The existing category-filter checkboxes (`Hide:` triggers/lights/spawns/…), skill preview dropdown, and "visible only" toggle are **hidden** in live view — they're .map-authoring affordances and don't apply to runtime state. The live branch is intentionally minimal: list + select. Phase 2 can reintroduce filters keyed on live classnames if useful.

Worldspawn (edict 0) is skipped. Free edicts are skipped. No sort — natural edict-number order matches typical Quake debugging muscle memory ("`edicts` console command" sorting).

---

## Render integration

One addition to the render pass in `render_wire.c`: after the existing selection-highlight pass, if `view_live` and `Scene_NumLiveSelected() > 0`, draw a bbox at each selected live edict's `v.absmin` / `v.absmax` using `EDIT_COLOR_SELECTED` (the existing selection-highlight colour).

```c
if (view_live && Scene_NumLiveSelected() > 0) {
    for (int i = 0; i < Scene_NumLiveSelected(); i++) {
        int en = Scene_GetLiveSelected(i);
        edict_t *ed = EDICT_NUM(en);
        if (ed->free) continue;                  // edict freed since selection
        const float *mn = ed->v.absmin;
        const float *mx = ed->v.absmax;
        if (mx[0] <= mn[0] && mx[1] <= mn[1] && mx[2] <= mn[2]) continue;
        editor_draw_bbox(mn, mx, EDIT_COLOR_SELECTED);
    }
}
```

If the user selects an edict and the engine then frees it (monster killed, projectile gone), the highlight silently disappears next frame — no special prune step needed; the `ed->free` check handles it. The selection list itself can hold the stale edict number; it gets cleared by the next view-mode toggle or new selection.

---

## Components touched

| File | Change |
|---|---|
| `sdlquake/engine/editor/edit_scene.h` | Declare `Scene_LiveSelection*` API |
| `sdlquake/engine/editor/edit_scene.c` (or wherever Scene_SelectionAdd lives) | Implement live-selection list + accessors |
| `sdlquake/engine/editor/editor_ui.c` | Branch `draw_brush_list` on view mode; new live branch |
| `sdlquake/engine/editor/render_wire.c` | Add live-selection bbox highlight pass |
| `sdlquake/engine/editor/editor.c` | Clear inactive selection list on `editor_view_mode` change |

No `engine_api_t` ABI changes — `sv.edicts`, `PR_GetString`, and `EDICT_NUM` are already accessible to editor code through the engine_src headers.

---

## Bug: runtime ents missing classname

The current `draw_brush_list` (line 606+ in `editor_ui.c`) iterates `edit_scene.entities[]`. Runtime-spawned edicts (projectiles, gibs, dropped items) have no `edit_entity_t`, so they never appear in the panel and never display a classname.

Phase 1 resolves this by iterating `sv.edicts` directly in live view and pulling `v.classname` via `PR_GetString`. No separate code change is needed — it falls out of the architecture.

---

## Open questions resolved during brainstorming

- **Q: Should "live" filter to runtime-spawned ents only, or all alive edicts?**
  A: All currently alive edicts. Includes .map-spawned monsters that have moved and runtime-spawned ents.

- **Q: In live view, what should you be able to do?**
  A: Phase 1: select and inspect. Phase 2: translate, rotate, edit KVs.

- **Q: One spec or split into phases?**
  A: Split. This spec is Phase 1. Phase 2 (picker + gizmo + render-of-edits) is a separate spec.

---

## Testing

No test suite. Verification via running the editor:

- Load e1m1, open editor, switch to live view → list shows all e1m1 edicts with classnames.
- Fire a rocket → new row appears with `[N] missile` (or whatever the QC classname is).
- Click a row → bbox highlight draws around the live edict's position.
- Shoot the rocket into a wall → row disappears, highlight disappears.
- Switch to map view → list reverts to .map authored ents; live highlight goes away; live selection was cleared by the toggle.
- Switch back to live → list re-iterates sv.edicts; .map selection was cleared by the toggle; live selection starts empty.
