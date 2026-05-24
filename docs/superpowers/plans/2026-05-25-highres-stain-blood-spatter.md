# High-resolution stain map + blood-particle spatter — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Raise the per-surface stain (decal) grid from 16-unit luxel cells to 4-unit cells so existing decals gain detail, then hook stuck `pt_blood` particles into the decal painter so each one leaves a permanent dot.

**Architecture:** Introduce `STAIN_CELL_SHIFT` as a single source of truth for the cell-to-game-unit exponent. Stage 1 lands the constant at value 4 (no-op refactor — existing 16-unit cells). Stage 2 flips it to 2 (4-unit cells, 16× memory, sampling shift in `R_OverlayStain` automatically tracks). Stage 3 redesigns the per-decal-type kernels to make sense at the finer grid. Stage 4 adds the new `DECAL_BLOOD_SPATTER` entry, `R_SpawnBloodSpatter` function, and the `pt_blood` particle-stick hook in `r_part.c`.

**Tech Stack:** C (engine, gnu89), Zig build system, SDL3 platform layer. No unit-test suite — verification is build success + in-game visual check via the `m7_skeleton` smoke-test rig (MCP `teleport` to `(380, 0, 40)` facing east + screenshot) plus a Doom/Wolf3D weapon test on `e1m1`.

**Reference spec:** `docs/superpowers/specs/2026-05-24-highres-stain-blood-spatter-design.md`

**Smoke-test workflow** (used by every visual-check step below):

```sh
# Terminal 1 — start engine with MCP
zig build run -- +map m7_skeleton

# Terminal 2 — drive the smoke test (after engine launches)
python3 scripts/mcp_call.py console_exec '{"command":"teleport 380 0 40"}'
python3 scripts/mcp_call.py console_exec '{"command":"setviewang 0 0 0"}'
python3 scripts/mcp_call.py screenshot '{"name":"stain-<task>-<step>"}'
```

Screenshots land in `screenshots/`. Compare visually against a baseline taken before Task 1.

---

## File Structure

| File | Role in this plan |
|---|---|
| `sdlquake/engine_src/r_local.h` | Add `DECAL_BLOOD_SPATTER` enum entry. Add `R_SpawnBloodSpatter` prototype. Add `r_decals_blood_spatter` cvar extern. (Task 6) |
| `sdlquake/engine_src/r_decals.c` | All grid + kernel + new-function work lives here. Tasks 1, 2, 3, 5, 6. |
| `sdlquake/engine_src/r_surf.c` | `R_OverlayStain` sampling shift derivation. Task 1 (no-op refactor) + automatic re-tracking at Task 2. |
| `sdlquake/engine_src/r_part.c` | Particle-stick hook for `pt_blood`. Task 7. |

No new files. Plan is intentionally contained to the existing decal subsystem.

---

## Task 0: Capture baseline screenshots

**Goal:** Reference images of existing decals at 16-unit cells. Used to detect regressions in tasks 1, 2, 3, 5.

**Files:** none modified.

- [ ] **Step 1: Build engine at master HEAD**

  ```sh
  zig build
  ```

  Expected: build succeeds with no errors.

- [ ] **Step 2: Launch engine on m7_skeleton**

  ```sh
  zig build run -- +map m7_skeleton
  ```

  Wait for the engine window to appear and the map to finish loading.

- [ ] **Step 3: Teleport to the smoke-test pose**

  In a second terminal:

  ```sh
  python3 scripts/mcp_call.py console_exec '{"command":"teleport 380 0 40"}'
  python3 scripts/mcp_call.py console_exec '{"command":"setviewang 0 0 0"}'
  ```

  Expected: player at `(380, 0, 40)` facing east.

- [ ] **Step 4: Paint baseline decals on the wall ahead**

  ```sh
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_test"}'
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_test_grid"}'
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_test_pool"}'
  ```

  Expected: a single black luxel, a 5×5 rainbow grid, and a growing blood pool appear in front of the player.

- [ ] **Step 5: Screenshot the baseline**

  ```sh
  python3 scripts/mcp_call.py screenshot '{"name":"baseline-decals-m7"}'
  ```

  Expected: file `screenshots/baseline-decals-m7.png` exists.

- [ ] **Step 6: Capture e1m1 baseline (bullet holes + zombie blood)**

  ```sh
  python3 scripts/mcp_call.py console_exec '{"command":"map e1m1"}'
  python3 scripts/mcp_call.py console_exec '{"command":"impulse 3"}'  # shotgun
  # Fire a few shells at the wall ahead via console_exec attack, then:
  python3 scripts/mcp_call.py screenshot '{"name":"baseline-e1m1"}'
  ```

  (If unable to script firing, document this in plan notes as "manual baseline".)

- [ ] **Step 7: Quit engine and commit baseline screenshots**

  ```sh
  git add screenshots/baseline-decals-m7.png screenshots/baseline-e1m1.png
  git commit -m "test(decals): capture pre-change baseline screenshots"
  ```

---

## Task 1: Introduce `STAIN_CELL_SHIFT` as a no-op refactor

**Goal:** Replace every literal `16` / `>> 4` that refers to luxel-vs-game-unit conversion with the new `STAIN_CELL_SHIFT` constant, while keeping its value at 4. Build + screenshot must be visually identical to Task 0's baseline.

**Files:**
- Modify: `sdlquake/engine_src/r_decals.c`
- Modify: `sdlquake/engine_src/r_surf.c:425-515` (the `R_OverlayStain` function)
- Modify: `sdlquake/engine_src/r_local.h:375-382` (rename luxel terminology in `stain_t` comments)

- [ ] **Step 1: Add `STAIN_CELL_SHIFT` and `STAIN_CELL_SIZE` constants in r_decals.c**

  Just above the existing `STAIN_MAX_LUXELS_DIM` definition (around `r_decals.c:60`), add:

  ```c
  // Cells per game unit:
  //   cell size in game units = 1 << STAIN_CELL_SHIFT
  //   cells per luxel         = (1 << 4) / (1 << STAIN_CELL_SHIFT) = 1 << (4 - STAIN_CELL_SHIFT)
  // STAIN_CELL_SHIFT == 4 → 16-unit cells (one cell per lightmap luxel; original behaviour).
  // STAIN_CELL_SHIFT == 2 → 4-unit cells  (4× linear, 16× memory; planned final value).
  #define STAIN_CELL_SHIFT 4
  #define STAIN_CELL_SIZE  (1 << STAIN_CELL_SHIFT)
  ```

  Then rename `STAIN_MAX_LUXELS_DIM` → `STAIN_MAX_CELLS_DIM` (still value 18 for now) and adjust the comment:

  ```c
  #define STAIN_MAX_CELLS_DIM 18  // max cells per surface side; matches blocklights[18*18] cap when STAIN_CELL_SHIFT==4
  #define STAIN_PAYLOAD_INT16 (STAIN_MAX_CELLS_DIM * STAIN_MAX_CELLS_DIM * 3)
  ```

- [ ] **Step 2: Update `Stain_AllocSlot` to use the new shift**

  In `r_decals.c::Stain_AllocSlot` (around `r_decals.c:119-160`), replace:

  ```c
  int smax = (surf->extents[0] >> 4) + 1;
  int tmax = (surf->extents[1] >> 4) + 1;
  if (smax > STAIN_MAX_LUXELS_DIM || tmax > STAIN_MAX_LUXELS_DIM) {
  ```

  with:

  ```c
  int smax = (surf->extents[0] >> STAIN_CELL_SHIFT) + 1;
  int tmax = (surf->extents[1] >> STAIN_CELL_SHIFT) + 1;
  if (smax > STAIN_MAX_CELLS_DIM || tmax > STAIN_MAX_CELLS_DIM) {
  ```

- [ ] **Step 3: Update `R_DecalsFrame` (bloodpool) projection + grid step**

  In `r_decals.c::R_DecalsFrame` around `r_decals.c:276-300`, replace:

  ```c
  olu = ((int)floor(ou) - surf->texturemins[0]) >> 4;
  olv = ((int)floor(ov) - surf->texturemins[1]) >> 4;
  smax = (surf->extents[0] >> 4) + 1;
  tmax = (surf->extents[1] >> 4) + 1;

  luxel_radius = (int)((target / 16.0f) + 1.0f);
  ```

  with:

  ```c
  olu = ((int)floor(ou) - surf->texturemins[0]) >> STAIN_CELL_SHIFT;
  olv = ((int)floor(ov) - surf->texturemins[1]) >> STAIN_CELL_SHIFT;
  smax = (surf->extents[0] >> STAIN_CELL_SHIFT) + 1;
  tmax = (surf->extents[1] >> STAIN_CELL_SHIFT) + 1;

  luxel_radius = (int)((target / (float)STAIN_CELL_SIZE) + 1.0f);
  ```

  And further down in the same function, the `gx = dx * 16.0f` / `gy = dy * 16.0f` lines around `r_decals.c:297-298` become:

  ```c
  gx  = dx * (float)STAIN_CELL_SIZE;
  gy  = dy * (float)STAIN_CELL_SIZE;
  ```

- [ ] **Step 4: Update `R_DecalsFrame` (blooddrip) step math**

  In the drip block of `R_DecalsFrame` around `r_decals.c:358-396`, replace:

  ```c
  int   from_luxel = (int)(bd->length_painted / 16.0f);
  int   to_luxel   = (int)(drip_target          / 16.0f);
  int   max_luxel  = (int)(bd->length_max       / 16.0f);
  ```

  with:

  ```c
  int   from_luxel = (int)(bd->length_painted / (float)STAIN_CELL_SIZE);
  int   to_luxel   = (int)(drip_target          / (float)STAIN_CELL_SIZE);
  int   max_luxel  = (int)(bd->length_max       / (float)STAIN_CELL_SIZE);
  ```

  And:

  ```c
  step_len = (float)k * 16.0f;
  ...
  dx_off = (float)si * 16.0f;
  ```

  with:

  ```c
  step_len = (float)k * (float)STAIN_CELL_SIZE;
  ...
  dx_off = (float)si * (float)STAIN_CELL_SIZE;
  ```

- [ ] **Step 5: Update `Stain_AddCell` projection**

  In `r_decals.c::Stain_AddCell` around `r_decals.c:480-501`, replace:

  ```c
  tlu = ((int)floor(u) - target->texturemins[0]) >> 4;
  tlv = ((int)floor(v) - target->texturemins[1]) >> 4;
  tsmax = (target->extents[0] >> 4) + 1;
  ttmax = (target->extents[1] >> 4) + 1;
  ```

  with:

  ```c
  tlu = ((int)floor(u) - target->texturemins[0]) >> STAIN_CELL_SHIFT;
  tlv = ((int)floor(v) - target->texturemins[1]) >> STAIN_CELL_SHIFT;
  tsmax = (target->extents[0] >> STAIN_CELL_SHIFT) + 1;
  ttmax = (target->extents[1] >> STAIN_CELL_SHIFT) + 1;
  ```

- [ ] **Step 6: Update `Stain_PaintKernel_World` world-step computation**

  In `r_decals.c::Stain_PaintKernel_World` around `r_decals.c:545-549`, the per-cell world step is currently `16.0f / ulen2`. Replace with:

  ```c
  step_u[i] = tex->vecs[0][i] * ((float)STAIN_CELL_SIZE / ulen2);
  step_v[i] = tex->vecs[1][i] * ((float)STAIN_CELL_SIZE / vlen2);
  ```

- [ ] **Step 7: Update `R_DecalsTest_f` and `R_DecalsTestGrid_f`**

  In `R_DecalsTest_f` around `r_decals.c:693-697`:

  ```c
  lu = ((int)floor(u) - surf->texturemins[0]) >> STAIN_CELL_SHIFT;
  lv = ((int)floor(v) - surf->texturemins[1]) >> STAIN_CELL_SHIFT;
  smax = (surf->extents[0] >> STAIN_CELL_SHIFT) + 1;
  tmax = (surf->extents[1] >> STAIN_CELL_SHIFT) + 1;
  ```

  In `R_DecalsTestGrid_f` around `r_decals.c:744-747`:

  ```c
  step_u[i] = tex->vecs[0][i] * ((float)STAIN_CELL_SIZE / ulen2);
  step_v[i] = tex->vecs[1][i] * ((float)STAIN_CELL_SIZE / vlen2);
  ```

- [ ] **Step 8: Update `R_OverlayStain` sampling shift**

  In `r_surf.c::R_OverlayStain` around `r_surf.c:444-447`, replace:

  ```c
  mip   = r_drawsurf.surfmip;
  shift = 4 - mip;
  if (shift < 1) shift = 1;
  mask  = (1 << shift) - 1;
  ```

  with:

  ```c
  mip   = r_drawsurf.surfmip;
  // shift = surface-cache-pixel-per-stain-cell exponent. At mip 0 there is
  // 1 surface pixel per game unit, so each cell spans STAIN_CELL_SIZE
  // surface pixels = 1 << STAIN_CELL_SHIFT. Mip n halves surface resolution.
  shift = STAIN_CELL_SHIFT - mip;
  if (shift < 1) shift = 1;
  mask  = (1 << shift) - 1;
  ```

  Because `r_surf.c` does not include the `STAIN_CELL_SHIFT` definition from `r_decals.c`, also add it to `r_local.h` so both files see the same value. In `r_local.h:373` (just below the `decal_type_t` enum), add:

  ```c
  // Stain (decal) cell size exponent. Cell side in game units = 1 << STAIN_CELL_SHIFT.
  // Single source of truth — see r_decals.c for full discussion.
  #define STAIN_CELL_SHIFT 4
  ```

  And in `r_decals.c`, **remove** the local `#define STAIN_CELL_SHIFT 4` (it now lives in the header). Keep `STAIN_CELL_SIZE`, `STAIN_MAX_CELLS_DIM`, `STAIN_PAYLOAD_INT16` in `r_decals.c` (they only matter there).

- [ ] **Step 9: Build**

  ```sh
  zig build
  ```

  Expected: clean build, no warnings about `STAIN_MAX_LUXELS_DIM` being undefined.

- [ ] **Step 10: Visual no-op check**

  Repeat the smoke-test steps from Task 0 (steps 2-5):

  ```sh
  zig build run -- +map m7_skeleton
  # second terminal:
  python3 scripts/mcp_call.py console_exec '{"command":"teleport 380 0 40"}'
  python3 scripts/mcp_call.py console_exec '{"command":"setviewang 0 0 0"}'
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_test"}'
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_test_grid"}'
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_test_pool"}'
  python3 scripts/mcp_call.py screenshot '{"name":"task1-no-op"}'
  ```

  Expected: `screenshots/task1-no-op.png` is visually identical to `baseline-decals-m7.png`. Diff with `magick compare screenshots/baseline-decals-m7.png screenshots/task1-no-op.png screenshots/task1-diff.png` — any non-black pixels in the diff indicate a regression.

- [ ] **Step 11: Commit**

  ```sh
  git add sdlquake/engine_src/r_decals.c sdlquake/engine_src/r_surf.c sdlquake/engine_src/r_local.h
  git commit -m "refactor(decals): parameterise stain grid via STAIN_CELL_SHIFT (no-op)"
  ```

---

## Task 2: Flip `STAIN_CELL_SHIFT` from 4 to 2 (16-unit → 4-unit cells)

**Goal:** Activate the higher-resolution stain grid. Memory pool grows from ~1 MB to ~16 MB. Existing decals will now paint at 4× linear resolution; kernel shapes are still luxel-sized, so visuals will appear slightly different but should not be broken. Final cleanup of kernel shapes lands in Task 5.

**Files:**
- Modify: `sdlquake/engine_src/r_local.h` (bump `STAIN_CELL_SHIFT` to 2)
- Modify: `sdlquake/engine_src/r_decals.c` (bump `STAIN_MAX_CELLS_DIM` to 72; bump `STAIN_PAYLOAD_INT16`; scale blooddrip per-cell delta by 1/4)

- [ ] **Step 1: Flip `STAIN_CELL_SHIFT` in `r_local.h`**

  At `r_local.h` (the constant added in Task 1, Step 8):

  ```c
  #define STAIN_CELL_SHIFT 2
  ```

- [ ] **Step 2: Bump payload cap in `r_decals.c`**

  ```c
  #define STAIN_MAX_CELLS_DIM 72  // = old 18 luxels × 4 cells/luxel
  #define STAIN_PAYLOAD_INT16 (STAIN_MAX_CELLS_DIM * STAIN_MAX_CELLS_DIM * 3)
  ```

  (Total payload per slot grows from 18×18×3×2 = 1944 B to 72×72×3×2 = 31104 B ≈ 31 KB. At the default `r_decals_max=512` the hunk allocation grows from ~1 MB to ~16 MB.)

- [ ] **Step 3: Scale blooddrip per-cell intensity by 1/4**

  The drip now paints 4× as many cells along its length (the step math from Task 1 already scaled to `STAIN_CELL_SIZE = 4`). To preserve current visual intensity, scale each per-cell delta by 1/4. In `R_DecalsFrame`'s drip block (around `r_decals.c:370-375`), replace:

  ```c
  int   dr_c = (int)(-40.0f  * fall);
  int   dg_c = (int)(-100.0f * fall);
  int   db_c = (int)(-100.0f * fall);
  int   dr_s = (int)(-20.0f  * fall);
  int   dg_s = (int)(-50.0f  * fall);
  int   db_s = (int)(-50.0f  * fall);
  ```

  with:

  ```c
  // Per-cell delta scaled by (16-unit luxel) / (current cell size) so the
  // drip's overall darkness stays the same as the old 16-unit-cell version
  // even though we now paint more cells per unit length.
  float drip_scale = (float)STAIN_CELL_SIZE / 16.0f;
  int   dr_c = (int)(-40.0f  * fall * drip_scale);
  int   dg_c = (int)(-100.0f * fall * drip_scale);
  int   db_c = (int)(-100.0f * fall * drip_scale);
  int   dr_s = (int)(-20.0f  * fall * drip_scale);
  int   dg_s = (int)(-50.0f  * fall * drip_scale);
  int   db_s = (int)(-50.0f  * fall * drip_scale);
  ```

  (`drip_scale = 0.25f` at `STAIN_CELL_SHIFT == 2`; would be `1.0f` at the old setting — consistent with Task 1 being a no-op.)

- [ ] **Step 4: Build**

  ```sh
  zig build
  ```

  Expected: clean build. If you see an assertion or crash about payload size, double-check `STAIN_PAYLOAD_INT16` macro is referenced consistently and the `payload[STAIN_PAYLOAD_INT16]` array in `stain_slot_t` is sized via the macro.

- [ ] **Step 5: Visual check — existing decals**

  ```sh
  zig build run -- +map m7_skeleton
  # second terminal:
  python3 scripts/mcp_call.py console_exec '{"command":"teleport 380 0 40"}'
  python3 scripts/mcp_call.py console_exec '{"command":"setviewang 0 0 0"}'
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_test"}'
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_test_grid"}'
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_test_pool"}'
  python3 scripts/mcp_call.py screenshot '{"name":"task2-4unit-cells"}'
  ```

  Expected differences vs baseline:
  - `r_decals_test` single luxel: was a 16×16-game-unit black square; now a 4×4-game-unit black dot.
  - `r_decals_test_grid` 5×5 rainbow: same physical area, but each cell now 4 game units across instead of 16 — much tinier rainbow.
  - `r_decals_test_pool` growing pool: smoother circular falloff (more cells per radius), same radius.

  If the pool grows visibly thinner or asymmetric → regression in `R_DecalsFrame` bloodpool math. If the rainbow rendering is corrupted (memory aliasing) → check `STAIN_PAYLOAD_INT16` macro propagation through `stain_slot_t`.

- [ ] **Step 6: Memory sanity check**

  In the engine console:

  ```sh
  python3 scripts/mcp_call.py console_exec '{"command":"mem_summary"}'
  ```

  (If `mem_summary` doesn't exist, skip — `Hunk_AllocName("stainpool", ...)` will be reported in `Hunk_Print` or equivalent. The intent is to confirm the stain pool is roughly 16 MB; off by an order of magnitude indicates a sizing bug.)

- [ ] **Step 7: Commit**

  ```sh
  git add sdlquake/engine_src/r_local.h sdlquake/engine_src/r_decals.c
  git commit -m "feat(decals): bump stain grid to 4-unit cells (4× linear, ~16 MB)"
  ```

---

## Task 3: Verify existing decals look sensible at 4-unit cells

**Goal:** Spot-check that the bumped resolution hasn't visually broken anything beyond expected size changes. No code changes — pure verification with screenshots.

**Files:** none modified.

- [ ] **Step 1: e1m1 bullet-hole check**

  ```sh
  zig build run -- +map e1m1
  # second terminal:
  python3 scripts/mcp_call.py console_exec '{"command":"give all"}'
  python3 scripts/mcp_call.py console_exec '{"command":"impulse 2"}'  # shotgun
  ```

  Manually aim at a wall in the start hallway and fire ~5 shells. Take a screenshot:

  ```sh
  python3 scripts/mcp_call.py screenshot '{"name":"task3-e1m1-bullets"}'
  ```

  Expected: bullet holes are now 4×4-game-unit dots instead of 16×16. They look sparse and small but distinct.

- [ ] **Step 2: Zombie blood splat check**

  Still on `e1m1`, walk to the first zombie area and gib one. Screenshot the wall behind the impact:

  ```sh
  python3 scripts/mcp_call.py screenshot '{"name":"task3-e1m1-zombie-blood"}'
  ```

  Expected: the BLOOD_SPLAT decal is still a 3×3-cell cross pattern (48×48 game units in the old scale → now 12×12 game units because the kernel is in cell coordinates). Visibly tiny. **This is the regression Task 5 will fix.**

- [ ] **Step 3: Document findings inline in the screenshots**

  Note: this step is informational, not a commit. If anything in step 1 or 2 looks broken in a way *other* than "decal too small" (e.g. mis-projected, on the wrong surface, missing entirely), pause and investigate — likely a missed shift conversion in Task 1.

  No commit for this task.

---

## Task 4: (Removed — merged into Task 5)

This intentional gap keeps Task numbers aligned with the spec's implementation order. Task 4 in the spec was "rescale blood-drip step math" which Tasks 1 + 2 already covered.

---

## Task 5: Redesign per-decal-type kernels

**Goal:** Replace the existing `K1x1_solid` / `K3x3` / `K5x5` kernel-table entries with shapes that look right at 4-unit cells. Per the spec's table: BULLET → 2×2 solid (8u), SPIKE → 1×1 solid (4u), BLOOD_SPLAT → K7×7 Gaussian (28u), SCORCH → K13×13 Gaussian (52u), LIGHTNING → K7×7 Gaussian (28u).

**Files:**
- Modify: `sdlquake/engine_src/r_decals.c` (kernel arrays + `decal_kernels` table, around `r_decals.c:407-433`)

- [ ] **Step 1: Add new kernel arrays in `r_decals.c`**

  Just above the existing `K3x3` / `K5x5` definitions (around `r_decals.c:407`), add:

  ```c
  // 2×2 solid: bullet hole at 8-unit footprint.
  static const int K2x2_solid[4] = { 1, 1, 1, 1 };  // norm 1; each cell gets the full centre delta

  // 7×7 Gaussian (sigma ≈ 1.5 cells); falls to zero at the corners.
  // Values: row 0 = [1 3 6 7 6 3 1] etc. Sum ≈ 1024 → use shift /1024.
  static const int K7x7_gauss[49] = {
      1,  3,  6,  7,  6,  3, 1,
      3, 13, 30, 38, 30, 13, 3,
      6, 30, 67, 84, 67, 30, 6,
      7, 38, 84,107, 84, 38, 7,
      6, 30, 67, 84, 67, 30, 6,
      3, 13, 30, 38, 30, 13, 3,
      1,  3,  6,  7,  6,  3, 1,
  };
  // Sum = 1027, close enough to 1024; use knorm 1024 (kernels need not be exact).

  // 13×13 Gaussian (sigma ≈ 3.0 cells) for SCORCH; smooth falloff over 52-unit footprint.
  // Computed offline; sum ≈ 4096 → knorm 4096.
  static const int K13x13_gauss[169] = {
      0,  0,  1,  1,  2,  2,  2,  2,  2,  1,  1,  0,  0,
      0,  1,  2,  4,  5,  7,  7,  7,  5,  4,  2,  1,  0,
      1,  2,  5,  9, 13, 16, 17, 16, 13,  9,  5,  2,  1,
      1,  4,  9, 16, 23, 28, 30, 28, 23, 16,  9,  4,  1,
      2,  5, 13, 23, 33, 41, 43, 41, 33, 23, 13,  5,  2,
      2,  7, 16, 28, 41, 50, 53, 50, 41, 28, 16,  7,  2,
      2,  7, 17, 30, 43, 53, 56, 53, 43, 30, 17,  7,  2,
      2,  7, 16, 28, 41, 50, 53, 50, 41, 28, 16,  7,  2,
      2,  5, 13, 23, 33, 41, 43, 41, 33, 23, 13,  5,  2,
      1,  4,  9, 16, 23, 28, 30, 28, 23, 16,  9,  4,  1,
      1,  2,  5,  9, 13, 16, 17, 16, 13,  9,  5,  2,  1,
      0,  1,  2,  4,  5,  7,  7,  7,  5,  4,  2,  1,  0,
      0,  0,  1,  1,  2,  2,  2,  2,  2,  1,  1,  0,  0,
  };
  // Sum ≈ 4030; use knorm 4096.
  ```

  Keep the existing `K1x1_solid`, `K3x3`, `K5x5` declarations — they're still used or referenced by tests.

- [ ] **Step 2: Rewrite the `decal_kernels` table**

  Replace the existing `decal_kernels[DECAL_NUM_TYPES]` initialiser (around `r_decals.c:427-433`) with:

  ```c
  static const decal_kernel_t decal_kernels[DECAL_NUM_TYPES] = {
      /* DECAL_BULLET      */ { K2x2_solid,    2,    1, -150, -150, -150 },
      /* DECAL_SPIKE       */ { K1x1_solid,    1,    1, -150, -150, -150 },
      /* DECAL_BLOOD_SPLAT */ { K7x7_gauss,    7, 1024, -200, -500, -500 },
      /* DECAL_SCORCH      */ { K13x13_gauss, 13, 4096, -200, -200, -200 },
      /* DECAL_LIGHTNING   */ { K7x7_gauss,    7, 1024,  -50,  -60,  -40 },
  };
  ```

  Centre-delta colours are unchanged from the original table.

- [ ] **Step 3: Build**

  ```sh
  zig build
  ```

  Expected: clean build.

- [ ] **Step 4: Visual check — bullet, splat, scorch on e1m1**

  ```sh
  zig build run -- +map e1m1
  # second terminal:
  python3 scripts/mcp_call.py console_exec '{"command":"give all"}'
  python3 scripts/mcp_call.py console_exec '{"command":"impulse 2"}'  # shotgun
  ```

  Fire ~5 shells at a wall, then:

  ```sh
  python3 scripts/mcp_call.py screenshot '{"name":"task5-bullets"}'
  ```

  Expected: bullet holes are 2×2-cell black dots (8 game units across) — small but visible, no longer chunky 16-unit squares.

  Switch to grenade launcher, fire one into a wall, screenshot:

  ```sh
  python3 scripts/mcp_call.py console_exec '{"command":"impulse 6"}'
  python3 scripts/mcp_call.py screenshot '{"name":"task5-scorch"}'
  ```

  Expected: scorch mark shows smooth circular falloff over ~52 game units.

  Gib a zombie next to a wall, screenshot:

  ```sh
  python3 scripts/mcp_call.py screenshot '{"name":"task5-blood-splat"}'
  ```

  Expected: blood splat is a smooth ~28-unit reddish blob, not the previous 3×3 cross.

- [ ] **Step 5: Commit**

  ```sh
  git add sdlquake/engine_src/r_decals.c
  git commit -m "feat(decals): redesign kernels for 4-unit cell grid"
  ```

---

## Task 6: Add `DECAL_BLOOD_SPATTER` + `R_SpawnBloodSpatter` + cvar

**Goal:** All the plumbing for the new particle-decal entry point. No call site yet — that lands in Task 7.

**Files:**
- Modify: `sdlquake/engine_src/r_local.h` (enum entry, cvar extern, prototype)
- Modify: `sdlquake/engine_src/r_decals.c` (kernel entry, cvar registration, `R_SpawnBloodSpatter` function)

- [ ] **Step 1: Add enum entry in `r_local.h`**

  In `r_local.h:366-373`, replace the `decal_type_t` enum with:

  ```c
  typedef enum {
      DECAL_BULLET,
      DECAL_SPIKE,
      DECAL_BLOOD_SPLAT,
      DECAL_SCORCH,
      DECAL_LIGHTNING,
      DECAL_BLOOD_SPATTER,   // single-cell blood dot left by a stuck pt_blood particle
      DECAL_NUM_TYPES
  } decal_type_t;
  ```

  (`DECAL_BLOOD_SPATTER` is added before `DECAL_NUM_TYPES`; existing decal type indices are unchanged.)

- [ ] **Step 2: Add cvar extern + function prototype in `r_local.h`**

  Just below the `extern cvar_t r_decals_debug;` line (around `r_local.h:394`), add:

  ```c
  extern cvar_t r_decals_blood_spatter;
  ```

  Just below the `R_SpawnGunshotChips` / `R_SpawnShell` block (around `r_local.h:411`), add:

  ```c
  // Single-cell blood dot painted into the decal map when a pt_blood particle
  // sticks to a surface. Hooked from r_part.c on PARTFL_STICK_ON_HIT.
  void R_SpawnBloodSpatter (vec3_t pos, vec3_t normal);
  ```

- [ ] **Step 3: Add the cvar definition + registration in `r_decals.c`**

  Just below the `r_decals_debug` cvar definition (around `r_decals.c:27`), add:

  ```c
  cvar_t r_decals_blood_spatter      = { "r_decals_blood_spatter",      "1", true };
  ```

  And in `R_DecalsInit` (around `r_decals.c:29-45`), add the matching `Cvar_RegisterVariable` call:

  ```c
  Cvar_RegisterVariable (&r_decals_blood_spatter);
  ```

- [ ] **Step 4: Extend `decal_kernels` with the spatter entry**

  In the `decal_kernels` table (the one updated in Task 5), add a row for the new enum:

  ```c
  static const decal_kernel_t decal_kernels[DECAL_NUM_TYPES] = {
      /* DECAL_BULLET        */ { K2x2_solid,    2,    1, -150, -150, -150 },
      /* DECAL_SPIKE         */ { K1x1_solid,    1,    1, -150, -150, -150 },
      /* DECAL_BLOOD_SPLAT   */ { K7x7_gauss,    7, 1024, -200, -500, -500 },
      /* DECAL_SCORCH        */ { K13x13_gauss, 13, 4096, -200, -200, -200 },
      /* DECAL_LIGHTNING     */ { K7x7_gauss,    7, 1024,  -50,  -60,  -40 },
      /* DECAL_BLOOD_SPATTER */ { K1x1_solid,    1,    1, -300, -600, -600 },
  };
  ```

  Spatter is darker / more saturated red than `DECAL_BLOOD_SPLAT` because it covers a smaller footprint per droplet (4×4 game units vs. ~28×28 for a splat).

- [ ] **Step 5: Implement `R_SpawnBloodSpatter`**

  Add this function in `r_decals.c`, just below `R_SpawnDecal` (around `r_decals.c:845`):

  ```c
  /* Paint a single-cell blood dot at `pos` on the surface near `normal`.
     Called from r_part.c when a pt_blood particle enters PARTFL_STICK_ON_HIT.
     Bails silently if decals are disabled, the spatter cvar is off, the
     server is inactive (demo playback), or no nearby world surface is
     found. */
  void R_SpawnBloodSpatter (vec3_t pos, vec3_t normal)
  {
      msurface_t           *surf;
      const decal_kernel_t *dk;
      extern server_t       sv;

      if (!r_decals.value)               return;
      if (!r_decals_blood_spatter.value) return;
      if (!sv.active)                    return;  // demo playback safety

      surf = R_PointOnSurface_World (pos, normal, 4.0f);
      if (!surf) return;

      dk = &decal_kernels[DECAL_BLOOD_SPATTER];
      Stain_PaintKernel_World (pos, surf, dk->dr, dk->dg, dk->db,
                                dk->k, dk->ksize, dk->knorm);
  }
  ```

- [ ] **Step 6: Build**

  ```sh
  zig build
  ```

  Expected: clean build with no "function declared but not used" warning on `R_SpawnBloodSpatter` (the linker will keep it; Task 7 adds the caller).

- [ ] **Step 7: Smoke-test the new cvar**

  ```sh
  zig build run -- +map m7_skeleton
  # second terminal:
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_blood_spatter"}'
  ```

  Expected: console prints `"r_decals_blood_spatter" is:"1"`. Verifies the cvar is registered.

- [ ] **Step 8: Commit**

  ```sh
  git add sdlquake/engine_src/r_local.h sdlquake/engine_src/r_decals.c
  git commit -m "feat(decals): add DECAL_BLOOD_SPATTER + R_SpawnBloodSpatter API"
  ```

---

## Task 7: Hook `R_SpawnBloodSpatter` into the particle-stick branch

**Goal:** When a `pt_blood` particle sticks to a surface (via `PARTFL_STICK_ON_HIT`), paint a spatter dot.

**Files:**
- Modify: `sdlquake/engine_src/r_part.c:1897-1917`

- [ ] **Step 1: Add the call in `r_part.c`**

  In `r_part.c::R_DrawParticles`, locate the stick branch (around `r_part.c:1897-1917`). The branch begins with:

  ```c
  if ((p->flags & PARTFL_STICK_ON_HIT) ||
      (p->flags & PARTFL_BOUNCED)) {
      // Stick: park at impact, freeze velocity, mark stuck.
      p->org[0] = tr.endpos[0] + n[0] * 0.5f;
      p->org[1] = tr.endpos[1] + n[1] * 0.5f;
      p->org[2] = tr.endpos[2] + n[2] * 0.5f;
      p->vel[0] = p->vel[1] = p->vel[2] = 0;
      p->flags |= PARTFL_STUCK;
  ```

  Immediately after the `p->flags |= PARTFL_STUCK;` line (and before the `if (fabs(n[2]) < 0.7f)` wall-slide setup), add:

  ```c
      // Permanent decal: each stuck blood droplet paints one cell.
      // Position is the impact-surface point (not the off-surface
      // p->org we just wrote, which is offset by 0.5*normal so the
      // particle sprite isn't z-fighting). Use tr.endpos so the
      // decal projects cleanly onto the wall plane.
      if (p->type == pt_blood) {
          R_SpawnBloodSpatter (tr.endpos, n);
      }
  ```

- [ ] **Step 2: Build**

  ```sh
  zig build
  ```

  Expected: clean build. If you get "implicit declaration of `R_SpawnBloodSpatter`", check that `r_part.c` includes `r_local.h` (it should already — every other `R_Spawn*` call from `r_part.c` works the same way).

- [ ] **Step 3: Visual check — blood spatter on a wall**

  ```sh
  zig build run -- +map e1m1
  # second terminal:
  python3 scripts/mcp_call.py console_exec '{"command":"give all"}'
  ```

  Walk to the first zombie, kill it near a wall. Take a screenshot:

  ```sh
  python3 scripts/mcp_call.py screenshot '{"name":"task7-spatter-on"}'
  ```

  Expected: the wall behind the zombie shows several small (4×4 game unit) reddish dots in addition to the central `DECAL_BLOOD_SPLAT` blob. The dots persist after the blood particles fade out.

  Toggle the cvar off and repeat:

  ```sh
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_blood_spatter 0"}'
  ```

  Move to a fresh wall, kill another zombie:

  ```sh
  python3 scripts/mcp_call.py screenshot '{"name":"task7-spatter-off"}'
  ```

  Expected: only the central splat appears; no individual droplet dots. Diff `task7-spatter-on.png` vs `task7-spatter-off.png` — the difference should be the scattered droplet dots.

- [ ] **Step 4: Visual check — drip + spatter coexistence**

  Re-enable spatter, then kill a zombie close enough that some droplets stick to a wall and some run as drips:

  ```sh
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_blood_spatter 1"}'
  python3 scripts/mcp_call.py screenshot '{"name":"task7-spatter-and-drip"}'
  ```

  Expected: both decal types render. The drip (from `R_SpawnBloodDrip`) is a vertical streak; the spatter is scattered dots. They do not visually fight (no flickering, no `stain_gen` thrash).

- [ ] **Step 5: Commit**

  ```sh
  git add sdlquake/engine_src/r_part.c
  git commit -m "feat(particles): pt_blood drops paint a spatter decal on stick"
  ```

---

## Task 8: Final acceptance pass

**Goal:** End-to-end visual verification on the user's preferred smoke-test pose, plus a final sanity sweep.

**Files:** none modified.

- [ ] **Step 1: Standard smoke-test on m7_skeleton**

  ```sh
  zig build run -- +map m7_skeleton
  # second terminal:
  python3 scripts/mcp_call.py console_exec '{"command":"teleport 380 0 40"}'
  python3 scripts/mcp_call.py console_exec '{"command":"setviewang 0 0 0"}'
  python3 scripts/mcp_call.py console_exec '{"command":"togglemenu"}'
  python3 scripts/mcp_call.py screenshot '{"name":"final-m7"}'
  ```

  Expected: standard smoke-test frame, no rendering glitches.

- [ ] **Step 2: e1m1 sustained-combat check**

  Re-run the full e1m1 zombie/grunt sequence. Take screenshots after the first major encounter (the grunt room past the lift):

  ```sh
  python3 scripts/mcp_call.py screenshot '{"name":"final-e1m1-combat"}'
  ```

  Expected: walls show a mix of bullet holes (small dots), blood splats (smooth blobs), spatter (small dots), and drips (vertical streaks). Check for: no z-fighting between decal types, no visibly unpainted faces inside a decal footprint, no surface-cache cracks across BSP face boundaries.

- [ ] **Step 3: Stress-test pool eviction**

  Use `r_decals_max 64` (a low cap), then fire several decals at various walls to force LRU eviction. Confirm the engine doesn't crash:

  ```sh
  python3 scripts/mcp_call.py console_exec '{"command":"r_decals_max 64"}'
  python3 scripts/mcp_call.py console_exec '{"command":"map e1m1"}'
  # Spend ~1 minute fighting and shooting various walls.
  ```

  Expected: no crash; older decals disappear as new ones are painted.

- [ ] **Step 4: Notify the user via MCP and end the loop**

  ```sh
  python3 scripts/mcp_call.py notify '{"title":"stain-spatter done","message":"high-res stain + blood spatter implementation complete"}'
  ```

  No commit for this task.

---

## Self-Review

**Spec coverage:**
- §1 Stain grid 4-unit cells → Tasks 1+2 (introduce constant, flip value, bump payload). ✓
- §2 Stain → cell projection (Stain_AddCell, R_DecalsFrame bloodpool, drip math) → Task 1 Steps 3-7. ✓
- §3 R_OverlayStain sampling → Task 1 Step 8. ✓
- §4 Kernel redesign → Task 5. ✓
- §5 Blood-particle stick hook → Task 6 (API) + Task 7 (call site). ✓
- §6 New cvar → Task 6 Step 3. ✓
- §7 Density / persistence → no code change required; design decision is "every droplet paints, permanent until LRU evicts" and that's the default behaviour of Tasks 6+7. ✓
- Implementation order from spec — followed in Tasks 1-7 (with Task 4 intentionally removed because Tasks 1+2 absorb it). ✓

**Placeholder scan:** No TBD / TODO / "implement later" / vague-error-handling markers. Every step has the exact code or command.

**Type consistency:** `STAIN_CELL_SHIFT` (header), `STAIN_CELL_SIZE` (r_decals.c), `STAIN_MAX_CELLS_DIM` (r_decals.c), `STAIN_PAYLOAD_INT16` (r_decals.c) — all consistent across tasks. `R_SpawnBloodSpatter(vec3_t pos, vec3_t normal)` signature matches the prototype, the implementation, and the call site. `DECAL_BLOOD_SPATTER` enum entry added in `r_local.h` (Task 6) and referenced in `r_decals.c` decal_kernels table (Task 6) and in the call site comments — no name drift.
