# Editor Live View — Phase 1 Design Spec

**Date:** 2026-05-12
**Status:** Approved (revised after discovering existing `transient` infrastructure)

---

## Overview

Split the editor's "Brushes" panel data source by `editor_view_mode`. In **live** view, every alive engine edict appears in the panel — both `.map`-authored ents (already represented by an `edit_entity_t` with `live_ent` set) and runtime-spawned ents (rockets, gibs, dropped backpacks). In **map** view, the panel shows only `.map`-authored ents and hides any transients.

This is Phase 1 of two. Phase 1 makes live edicts **visible and selectable**; Phase 2 (separate spec) extends 3D-viewport picking, gizmo drag, and KV editing to live edicts — though much of that already works via the existing `find_or_create_transient` rails.

This revision (May 12) leverages the codebase's existing transient infrastructure (`render_wire.c:1512-1601`) instead of adding a parallel selection model. The previous draft proposed `Scene_LiveSelection*` APIs; those are removed.

---

## Existing infrastructure we're building on

In `sdlquake/engine/editor/render_wire.c` the picker already maintains transient edit_entities for runtime edicts:

- `find_or_create_transient(edict_t *ed)` — looks up the existing transient for `ed` (`live_ent == ed && transient`), or appends a new one with `classname` and `origin` populated from `ed->v`.
- `reap_dead_transients()` — walks `edit_scene.entities[]`, removes transients whose `live_ent` is null or freed (unless they're currently selected).
- `edict_is_already_bound(ed)` — returns true if any `edit_entity_t` (transient or `.map`-authored) has `live_ent == ed`. Used to skip edicts that already have a handle.

`edit_entity_t::transient` (in `edit_scene.h:92`) marks these synthesised entries. `map_io.c:529` already excludes them from `.map` save.

The picker currently calls `find_or_create_transient` **lazily** — only when the user clicks an unbound runtime edict in the 3D viewport. Phase 1 adds an **eager** sweep at the start of each frame in live view so every alive runtime edict has an `edit_entity_t` and shows in the brushes panel.

---

## Scope

**In scope (Phase 1):**

- New helper `Editor_MaterialiseLiveTransients(void)`: walks `sv.edicts[1..num_edicts-1]`, skips free edicts and already-bound ones, calls `find_or_create_transient` for each runtime ent. Called once per editor frame in live view.
- `draw_brush_list` filters its iteration by view mode (see "Brushes panel filtering" below).
- `reap_dead_transients()` is also called from the eager sweep so dead transients clear out even without 3D picking activity.
- View-mode toggle clears any selection that's about to become invisible (selected dead `.map` ent in live, selected transient in map).

**Out of scope (Phase 2):**

- 3D-viewport picking on live edicts already works for unbound runtime ents (line 1685 of render_wire.c). Phase 2 will extend it to `.map`-bound live edicts at the live position.
- Gizmo translate/rotate on transients — partially works (gizmo reads `live_ent->v.origin`), but Phase 2 audits the path end-to-end.
- KV side-panel editing of live edicts.
- Anchor / arrow / link-arrow render passes for transients.

The map-view rendering and selection paths are untouched.

---

## View-mode semantics

| View mode | Brushes panel shows | Hidden in panel |
|---|---|---|
| `editor_view_mode 0` (live) | Entries with engine presence: `live_ent && !live_ent->free`, OR `live_static != NULL` | Pure-map ents with no engine presence (e.g. an authored monster that was killed and freed) |
| `editor_view_mode 1` (map) | Authored entries: `transient == 0` | Transient entries (runtime-spawned ents) |

Worldspawn (edict 0) is always shown in map view (it owns the world brushes) and skipped by the eager sweep in live view (it's the world, not a per-entity selectable thing).

---

## Brushes panel filtering

The new visibility predicate, added near the top of `draw_brush_list` in `editor_ui.c`:

```c
static int brush_list_visible(int i)
{
    extern cvar_t editor_view_mode;
    int view_live = (int)editor_view_mode.value == 0;
    edit_entity_t *e = &edit_scene.entities[i];

    if (Editor_EntityHidden(i)) return 0;  // existing category filter

    if (view_live) {
        // Live view: must have engine presence.
        if (e->live_ent && !e->live_ent->free) return 1;
        if (e->live_static) return 1;
        return 0;
    }
    // Map view: hide transients (they're not authored data).
    if (e->transient) return 0;
    return 1;
}
```

Existing `Editor_EntityHidden` category filters (triggers, lights, monsters, …) still apply in both views. The "visible only" toggle works as before — orthogonal to view-mode filtering.

The header count line is updated to reflect the filter: `"%d live edicts, %d brushes"` in live, `"%d entities, %d brushes"` in map (existing text).

---

## Eager materialisation

New function in `render_wire.c` (or wherever the transient helpers live), called from the editor's per-frame entry point in live view:

```c
void Editor_MaterialiseLiveTransients(void)
{
    extern cvar_t editor_view_mode;
    if ((int)editor_view_mode.value != 0) return;     // live view only
    if (!sv.active) return;

    reap_dead_transients();                            // drop stale entries first

    int n;
    for (n = 1; n < sv.num_edicts; n++) {
        edict_t *ed = EDICT_NUM(n);
        if (ed->free) continue;
        if (edict_is_already_bound(ed)) continue;      // .map ent or existing transient
        // Skip degenerate-bbox edicts (temp ents, fresh edicts). The picker
        // applies the same rule (line 1704). No bbox -> not selectable.
        const float *amn = ed->v.absmin;
        const float *amx = ed->v.absmax;
        if (amx[0] <= amn[0] && amx[1] <= amn[1] && amx[2] <= amn[2]) continue;
        find_or_create_transient(ed);
    }
}
```

Call site: at the start of the editor's per-frame work, before `draw_brush_list`. The natural home is wherever `Editor_PreRender` or `Editor_DrawUI` orchestrate the per-frame setup — exact spot pinned down during plan writing.

Cost: O(num_edicts) per frame in live view, mostly the `edict_is_already_bound` linear walk. For typical Quake maps with a few hundred edicts this is negligible compared to the renderer; can optimise if a profiler later flags it.

---

## Live presence on .map-authored ents

A `.map`-authored entity has `live_ent` set after the spawn flush (`map_io.c`). If the engine then `ED_Free`s that edict (a monster gets killed), the slot may later be reused for an unrelated runtime ent (a rocket). `detach_stale_live_ent` (already in `render_wire.c:1512`) handles the reuse case: if classnames mismatch, it clears `live_ent` on the authored entry. After that:

- The authored entry has `live_ent == NULL` → not visible in live view (no engine presence). Correct.
- The rocket gets its own transient on the next eager sweep. Correct.

`detach_stale_live_ent` currently runs inside `Editor_PickAt`. Phase 1 moves the call into the eager sweep too, so live-view filtering sees an accurate `live_ent` even without picking activity. (Detail to land in the plan.)

---

## Selection on view-mode toggle

When `editor_view_mode` changes, the current selection may contain entries that are now hidden. Clearing all selection is the simplest behaviour and matches the user's mental model (the two worlds are distinct):

- Wherever the view-mode cvar is observed for the flip (editor.c's `Editor_PreRender` checks `editor_view_mode.value` each frame already), compare against a static `s_last_view_mode`; if it changed, call `Scene_SelectionClear()`.
- Also call `Scene_ClearActiveFace()`.

No new API needed.

---

## Components touched

| File | Change |
|---|---|
| `sdlquake/engine/editor/render_wire.c` | New `Editor_MaterialiseLiveTransients`; expose internal helpers as needed; move `detach_stale_live_ent` call out of `Editor_PickAt`'s exclusive path. |
| `sdlquake/engine/editor/editor_internal.h` (or `editor.h`) | Declare `Editor_MaterialiseLiveTransients`. |
| `sdlquake/engine/editor/editor_ui.c` | `draw_brush_list` uses `brush_list_visible`; header count text updated for live view. |
| `sdlquake/engine/editor/editor.c` | Per-frame view-mode-change detector clears selection. Call `Editor_MaterialiseLiveTransients` once per frame. |

No `engine_api_t` ABI changes. No new public APIs in `edit_scene.h`.

---

## Bug: runtime ents missing classname

Before Phase 1: runtime ents only show in the brushes list after being picked in the 3D viewport (which creates a transient with classname populated). Pure rendering or list display does not trigger transient creation, so runtime ents are missing from the panel entirely.

After Phase 1: eager materialisation creates transients for all alive runtime edicts each frame. `find_or_create_transient` calls `Entity_SetKV("classname", ed->v.classname ? ed->v.classname : "(runtime)")` so every transient has a non-empty classname when the panel iterates `edit_scene.entities[]`.

---

## Testing

No test suite. Verification via running the editor:

- Load `e1m1`, open editor, switch to live view → brushes panel shows worldspawn + every alive monster/item with classnames.
- Switch to map view → panel shows the same `.map`-authored set, no transients.
- In live view, fire a rocket → a row appears for the missile (whatever the QC classname is) with no `.map` selection ambiguity.
- Shoot the rocket into a wall → row disappears next frame (reap).
- Kill an authored monster → its row disappears from the live panel (engine presence lost) but is still in the map panel.
- Toggle view mode while something is selected → selection clears.
- Verify no perf regression in live view at typical map sizes (e1m1 ≈ ~250 edicts).

---

## Open questions resolved during brainstorming

- **Q: What's the live source — runtime-spawned only, or all alive edicts?**
  A: All currently alive edicts.

- **Q: In live view, what can you do?**
  A: Phase 1: see and select. Phase 2: gizmo + KV edit.

- **Q: One spec or split?**
  A: Split. This is Phase 1.

- **Q: Add a parallel live-selection model, or reuse existing transient infrastructure?**
  A: Reuse transients. The picker already wraps runtime edicts in `edit_entity_t`; eager materialisation in live view makes them universally visible without duplicating selection state.
