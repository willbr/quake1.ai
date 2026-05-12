# Editor Live View — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In live view, every alive engine edict shows in the editor's brushes panel (with classname). In map view, only `.map`-authored ents show. Reuses the existing `edit_entity_t::transient` infrastructure.

**Architecture:** Add an eager `Editor_MaterialiseLiveTransients` sweep at the top of each editor frame in live view (wraps every unbound runtime edict via the existing `find_or_create_transient`). `draw_brush_list` gains a `brush_list_visible(i)` predicate that hides transients in map view and hides ents without engine presence in live view. View-mode-change clears selection.

**Tech Stack:** C (gnu89 for engine files), SDL3, Dear ImGui via the existing `IG_*` helpers. No tests — verification is `zig build` + in-game visual checks per the project's existing convention (`CLAUDE.md`).

**Spec:** `docs/superpowers/specs/2026-05-12-editor-live-view-phase1-design.md`

---

## Pre-flight

- [ ] **Read the spec.** Open `docs/superpowers/specs/2026-05-12-editor-live-view-phase1-design.md` and the existing transient helpers at `sdlquake/engine/editor/render_wire.c:1512-1601`.

- [ ] **Confirm clean tree.** Run `git status` — should be on `master`, clean.

- [ ] **Baseline build.** Run `zig build` from the repo root. Expected: exit 0, no output. This confirms the starting state is buildable before any change.

---

### Task 1: Add `Editor_MaterialiseLiveTransients` (private wrapper around existing helpers)

**Files:**
- Modify: `sdlquake/engine/editor/render_wire.c` (after `edict_is_already_bound`, before `Editor_PickAt`)
- Modify: `sdlquake/engine/editor/editor_internal.h` (declare the new function)

- [ ] **Step 1: Declare the new function in `editor_internal.h`.**

In `sdlquake/engine/editor/editor_internal.h`, after the existing `Editor_PushPreviewEntities` declaration (around line 155), add:

```c
// render_wire.c — eagerly materialise transient edit_entity_t entries
// for every alive runtime edict in sv.edicts (those not already bound
// to a .map-authored entry). Reaps stale transients first. No-op
// outside live view or when sv is inactive. Called once per editor
// frame from Editor_PreRender so the Brushes panel can list runtime
// ents (rockets, gibs, drops) with classnames without requiring the
// user to first click them in the 3D viewport.
void Editor_MaterialiseLiveTransients(void);
```

- [ ] **Step 2: Implement the function in `render_wire.c`.**

In `sdlquake/engine/editor/render_wire.c`, insert just before `int Editor_PickAt(...)` (around the current line 1603):

```c
// Walk sv.edicts and ensure every alive runtime edict has a transient
// edit_entity_t wrapping it. Drops dead transients first. Mirrors the
// edict-skip rules of Editor_PickAt's runtime-edict pass (degenerate
// bbox → skip; already-bound → skip).
void Editor_MaterialiseLiveTransients(void)
{
    extern cvar_t editor_view_mode;
    int n;
    int i;
    int view_live = (int)editor_view_mode.value == 0;

    if (!view_live) return;
    if (!sv.active) return;

    // Refresh stale live_ent links on .map-authored entries before
    // edict_is_already_bound consults them. Without this, a freed slot
    // reused by an unrelated runtime ent would block transient creation.
    for (i = 0; i < edit_scene.numentities; i++)
        detach_stale_live_ent(&edit_scene.entities[i]);

    reap_dead_transients();

    for (n = 1; n < sv.num_edicts; n++)
    {
        edict_t *ed = EDICT_NUM(n);
        const float *amn, *amx;
        if (ed->free) continue;
        if (edict_is_already_bound(ed)) continue;
        amn = ed->v.absmin;
        amx = ed->v.absmax;
        // Degenerate bbox = temp ent or fresh edict, not selectable.
        if (amx[0] <= amn[0] && amx[1] <= amn[1] && amx[2] <= amn[2])
            continue;
        find_or_create_transient(ed);
    }
}
```

- [ ] **Step 3: Build.**

Run: `zig build`
Expected: exit 0, no errors. (Function isn't called yet — this confirms it links.)

- [ ] **Step 4: Commit.**

```bash
git add sdlquake/engine/editor/render_wire.c sdlquake/engine/editor/editor_internal.h
git commit -m "$(cat <<'EOF'
feat(editor): Editor_MaterialiseLiveTransients sweep helper

Wrap every alive runtime edict in a transient edit_entity_t each
frame in live view, so the Brushes panel can show them without
requiring a 3D-viewport pick first. Reuses find_or_create_transient
+ reap_dead_transients. Not yet wired into the per-frame path.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git push
```

---

### Task 2: Wire the sweep into `Editor_PreRender`

**Files:**
- Modify: `sdlquake/engine/editor/editor.c:1441-1458` (`Editor_PreRender`)

- [ ] **Step 1: Insert the call.**

In `sdlquake/engine/editor/editor.c`, find the block in `Editor_PreRender`:

```c
    // Push fake editor preview entities into cl_visedicts so the engine
    // renders alias + brushmodel previews via its normal entity pipeline.
    // Must happen after CL_RelinkEntities reset the list (which is in
    // host.c earlier in the frame) and before R_EdgeDrawing dispatches
    // brushmodel entries.
    Editor_PushPreviewEntities();
```

Insert above it:

```c
    // Live view: wrap every alive runtime edict in a transient
    // edit_entity_t so the Brushes panel + selection paths can see it.
    // No-op in map view. Must run before any pass that iterates
    // edit_scene.entities[].
    Editor_MaterialiseLiveTransients();

```

- [ ] **Step 2: Build.**

Run: `zig build`
Expected: exit 0.

- [ ] **Step 3: Manual smoke test.**

Run: `zig build run -- +map e1m1`
- Open editor (F1 or whichever key your keybinds use).
- `editor_view_mode 0` (default — live view).
- Open the Brushes panel.
- Expected: the panel includes runtime ents that haven't been clicked yet. Specifically, fire a rocket — within one frame, a row labeled `[N] missile` (or whatever the QC classname is) appears in the panel. Walk the rocket into a wall — the row disappears next frame.

If runtime ents still don't appear, check: is `editor_view_mode` actually 0? Does `sv.active` evaluate true? (Add `Con_Printf("materialise: nedicts=%d\n", sv.num_edicts);` temporarily inside the function if needed.)

- [ ] **Step 4: Commit.**

```bash
git add sdlquake/engine/editor/editor.c
git commit -m "$(cat <<'EOF'
feat(editor): call MaterialiseLiveTransients each frame in live view

Drives the new sweep from Editor_PreRender so runtime ents (rockets,
gibs, drops) show up in the Brushes panel with their classname,
without the user having to first click them in the viewport.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git push
```

---

### Task 3: Split category filter out of `Editor_EntityHidden`

`Editor_EntityHidden` currently does two jobs: (a) apply the user's category/skill/visible-only filters, (b) in live view, hide entities the engine isn't visibly rendering (info_player_*, info_intermission, etc.). The brushes panel wants (a) but not (b) — info_player_start is an alive edict and should appear in live view's list.

**Files:**
- Modify: `sdlquake/engine/editor/editor_ui.c` (function `Editor_EntityHidden`, around line 497-528)
- Modify: `sdlquake/engine/editor/editor_internal.h` (declare the new helper)

- [ ] **Step 1: Add `Editor_EntityHiddenByCategory`.**

In `sdlquake/engine/editor/editor_ui.c`, find `Editor_EntityHidden` (currently around line 497). Refactor it into two functions — pull the category-only checks into a new `Editor_EntityHiddenByCategory`, leave the view-live render-presence check in `Editor_EntityHidden` and have it call the new helper. Replace the entire `int Editor_EntityHidden(int e_idx) { ... }` block with:

```c
int Editor_EntityHiddenByCategory(int e_idx)
{
    int cat;
    edit_entity_t *e;
    if (e_idx < 0 || e_idx >= edit_scene.numentities) return 0;
    e = &edit_scene.entities[e_idx];
    cat = Editor_EntityCategory(e);
    if (cat > 0 && cat < EDIT_CAT_COUNT && s_hide_cat[cat]) return 1;
    if (s_visible_only && !Editor_EntityInView(e_idx)) return 1;
    if (entity_hidden_by_skill(e)) return 1;
    return 0;
}

int Editor_EntityHidden(int e_idx)
{
    edit_entity_t *e;
    if (Editor_EntityHiddenByCategory(e_idx)) return 1;
    if (e_idx < 0 || e_idx >= edit_scene.numentities) return 0;
    e = &edit_scene.entities[e_idx];

    // Live mode: hide entities the engine isn't visibly rendering. Once
    // realised as a live edict the engine's model decision is the truth —
    // info_player_*, info_intermission, info_null and similar metadata
    // edicts have v.model=NULL and the engine draws nothing for them, so
    // bbox + arrows + picking should also draw nothing. SV_MakeStatic'd
    // ents (live_static) keep their efrag chain so they stay visible.
    // Pending entities (no live_ent yet) and BSP-loaded brush entities
    // both pass through — the former is the user's authoring intent, the
    // latter has v.model="*N" which sets cl_entities[N].model.
    {
        extern cvar_t editor_view_mode;
        int view_live = (int)editor_view_mode.value == 0;
        if (view_live && e->live_ent && !e->live_ent->free)
        {
            int en = NUM_FOR_EDICT(e->live_ent);
            int engine_renders =
                (en > 0 && en < cl.num_entities && cl_entities[en].model);
            if (!engine_renders) return 1;
        }
    }
    return 0;
}
```

- [ ] **Step 2: Declare the new helper in `editor_internal.h`.**

In `sdlquake/engine/editor/editor_internal.h`, find the existing `Editor_EntityHidden` declaration (around line 165) and add a sibling above it:

```c
// editor_ui.c — like Editor_EntityHidden, but applies ONLY the user's
// category/skill/visible-only filters. The Brushes panel uses this so
// metadata edicts (info_player_*, info_intermission) show in live view
// even though they have no visible model.
int  Editor_EntityHiddenByCategory(int e_idx);

// editor_ui.c — 1 if the user filtered out this entity's category in the
// Brushes panel checkboxes, or it's outside the camera frustum / behind a
// wall when "visible only" is on, or live-view rendering would draw
// nothing for it. Render / pick paths use this.
int  Editor_EntityHidden  (int e_idx);
```

(Update the existing `Editor_EntityHidden` comment to remove the now-misleading "list" reference if present, leaving "Render / pick paths use this".)

- [ ] **Step 3: Add the brushes-panel visibility predicate.**

Below the refactored `Editor_EntityHidden` in `editor_ui.c`, add:

```c
// Combine category-filter visibility with view-mode visibility. The
// Brushes panel uses this rather than calling Editor_EntityHidden
// directly, so metadata edicts (info_player_*, info_intermission)
// still show in live view's list — they're alive engine edicts even
// though Editor_EntityHidden filters them from render/pick.
static int brush_list_visible(int e_idx)
{
    extern cvar_t editor_view_mode;
    int view_live = (int)editor_view_mode.value == 0;
    edit_entity_t *e = &edit_scene.entities[e_idx];

    if (Editor_EntityHiddenByCategory(e_idx)) return 0;

    if (view_live)
    {
        if (e->live_ent && !e->live_ent->free) return 1;
        if (e->live_static) return 1;
        return 0;
    }
    if (e->transient) return 0;
    return 1;
}
```

- [ ] **Step 4: Swap the filter call.**

In `draw_brush_list`, find the iteration line (around line 606-611):

```c
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        const char *cls = "(no classname)";
        if (Editor_EntityHidden(i)) continue;
        if (e->classname_idx >= 0) cls = e->kv[e->classname_idx].value;
```

Replace the `Editor_EntityHidden(i)` call with `brush_list_visible(i)`:

```c
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        const char *cls = "(no classname)";
        if (!brush_list_visible(i)) continue;
        if (e->classname_idx >= 0) cls = e->kv[e->classname_idx].value;
```

- [ ] **Step 5: Update the header count for live view.**

In the same function, find the count block at lines 545-554:

```c
    snprintf(buf, sizeof(buf), "%d entities, ? brushes",
             edit_scene.numentities);
    {
        int total = 0;
        for (i = 0; i < edit_scene.numentities; i++)
            total += edit_scene.entities[i].numbrushes;
        snprintf(buf, sizeof(buf), "%d entities, %d brushes",
                 edit_scene.numentities, total);
    }
    IG_TextUnformatted(buf);
```

Replace with a view-aware count:

```c
    {
        extern cvar_t editor_view_mode;
        int view_live = (int)editor_view_mode.value == 0;
        int visible_ents = 0, total_brushes = 0;
        for (i = 0; i < edit_scene.numentities; i++)
        {
            if (!brush_list_visible(i)) continue;
            visible_ents++;
            total_brushes += edit_scene.entities[i].numbrushes;
        }
        if (view_live)
            snprintf(buf, sizeof(buf), "%d live edicts, %d brushes",
                     visible_ents, total_brushes);
        else
            snprintf(buf, sizeof(buf), "%d entities, %d brushes",
                     visible_ents, total_brushes);
    }
    IG_TextUnformatted(buf);
```

- [ ] **Step 6: Build.**

Run: `zig build`
Expected: exit 0.

- [ ] **Step 7: Manual smoke test.**

Run: `zig build run -- +map e1m1`
- Open editor, default to live view.
- Brushes panel header should say something like `35 live edicts, 24 brushes` (numbers will vary).
- info_player_start is in the list (it's an alive metadata edict — was previously hidden).
- Toggle to map view (`editor_view_mode 1` in console, or via the editor's UI if there's a toggle).
- Header should switch to `N entities, M brushes` and runtime-spawned ents from earlier disappear.
- Toggle back to live view → runtime ents reappear.

- [ ] **Step 8: Commit.**

```bash
git add sdlquake/engine/editor/editor_ui.c sdlquake/engine/editor/editor_internal.h
git commit -m "$(cat <<'EOF'
feat(editor): Brushes panel filters by view mode

Split Editor_EntityHidden's category-only checks into the new
Editor_EntityHiddenByCategory; the panel uses just that, so live view
also includes alive metadata edicts (info_player_*, info_intermission)
that have no visible model. Render / pick paths keep using the full
Editor_EntityHidden.

Live view: only entries with engine presence (live_ent non-free, or
live_static set). Map view: hides transients (runtime-spawned ents)
since they're not authored data. Header count text adapts.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git push
```

---

### Task 4: Clear selection on view-mode change

**Files:**
- Modify: `sdlquake/engine/editor/editor.c:1441-1458` (`Editor_PreRender`)

- [ ] **Step 1: Add the change detector.**

In `Editor_PreRender`, at the top of the function (just inside the `if (!s_open) return;` guard, before `editor_check_map_change()`), insert:

```c
    // View-mode change: a selection from the previous mode may now refer
    // to a hidden entry (a transient that's about to be filtered out, or
    // a dead .map ent that just lost its live_ent). Drop selection so the
    // gizmo doesn't anchor on something the user can't see.
    {
        static int s_last_view_mode = -1;
        int vm = (int)editor_view_mode.value;
        if (s_last_view_mode != vm)
        {
            if (s_last_view_mode != -1)        // first frame: don't clear
            {
                Scene_SelectionClear();
                Scene_ClearActiveFace();
            }
            s_last_view_mode = vm;
        }
    }
```

The `s_last_view_mode != -1` guard means the very first frame after engine start doesn't gratuitously clear an as-yet-non-existent selection.

- [ ] **Step 2: Build.**

Run: `zig build`
Expected: exit 0.

- [ ] **Step 3: Manual smoke test.**

Run: `zig build run -- +map e1m1`
- Open editor, live view. Click a monster in the brushes panel → it's selected (highlighted).
- Type `editor_view_mode 1` in console → selection clears, no highlight.
- Click a `.map` entity in the now-map-view panel → selection.
- `editor_view_mode 0` → cleared again.

- [ ] **Step 4: Commit.**

```bash
git add sdlquake/engine/editor/editor.c
git commit -m "$(cat <<'EOF'
feat(editor): clear selection on view-mode change

Switching live↔map can leave a selection referring to a now-hidden
entry (transient filtered out, or .map ent whose live_ent went free).
Drop selection on transition so the gizmo doesn't anchor on something
the user can no longer see.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git push
```

---

### Task 5: End-to-end manual verification

- [ ] **Step 1: Fresh build.**

Run: `zig build`
Expected: exit 0.

- [ ] **Step 2: Run the spec's test plan in-game.**

Run: `zig build run -- +map e1m1`

Walk through each item from the spec's Testing section:

- [ ] Live view brushes panel shows worldspawn + every alive monster/item with classnames.
- [ ] Map view shows the same `.map`-authored set; runtime ents are absent.
- [ ] Fire a rocket in live view → row appears with classname (likely `missile`).
- [ ] Rocket hits wall → row disappears next frame.
- [ ] Kill an authored monster (e.g. via `notarget` off + give yourself rockets, or `kill` + watch a zombie self-fight) → its row disappears from live panel.
- [ ] Open map view → the killed monster's row is still there (authored data isn't dependent on engine state).
- [ ] Selection clears across view-mode toggle.
- [ ] Brushes panel header in live view reads `N live edicts, M brushes`.

If any item fails, return to Phase 1 task list and audit which task is responsible.

- [ ] **Step 3: Perf sanity.**

In live view on e1m1, observe `showfps` reading. Compare to a baseline (note the value before this branch's changes, or simply check no obviously-egregious drop). The eager sweep is O(num_edicts) with a small constant; on e1m1 (~250 edicts) the cost should be invisible. If `showfps` shows a dip > 1 ms attributable to live view, profile `edict_is_already_bound` — it's linear in `edit_scene.numentities` and may need a hash table later. **For Phase 1, document the perf observation in the final commit message rather than over-engineering.**

- [ ] **Step 4: Final commit (or skip if nothing changed).**

If anything was fixed during verification, commit it. If everything passed cleanly, no commit needed — the feature is done.

---

## Out of scope (Phase 2 — separate plan)

These are intentionally *not* implemented in Phase 1. Listed here only so the engineer doesn't accidentally pull them in:

- 3D-viewport click selecting `.map`-bound live edicts at their live position (currently still picks at `.map` origin or via existing transient pass).
- Gizmo translate/rotate on transient entries — needs an end-to-end audit (gizmo, drag math, history push, engine writeback).
- KV side-panel editing of live edicts.
- Per-entity facing/movedir/target-link arrow render passes for transients.

If any of those are needed before Phase 2, write a Phase 2 spec/plan first — don't sneak them into Phase 1 commits.
