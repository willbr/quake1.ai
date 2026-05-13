# Decals — bullet holes, blood splats, scorch marks, blood pools

**Status:** design approved 2026-05-13.
**Scope:** permanent per-surface "stain" decals layered into the existing RGB lightmap. Triggered by client TE_ events (bullets, spikes, blood, lightning, explosions) and by a new `engine_api` call from game.dll on monster death (growing blood pools). World surfaces only. Single-player. Lightmap-resolution (16-unit luxel grid) — chunky-by-design.

**Excludes (deferred to v2+):** sub-luxel stain textures, decals on brush models (doors / lifts / trains), save/load persistence, network/multiplayer support, decals on water or sky, particle "blood mist" when monsters die over a non-floor surface.

## Motivation

Phase 8 (immersive sim) is in flight. Decals are the cheapest combat-feedback upgrade with the biggest readability win: the world remembers where weapons connected, monsters bled, and explosions scarred the walls. The recently-shipped coloured-lighting work (`2026-05-13-coloured-lighting-design.md`) gave the renderer a per-channel RGB lightmap path; stains slot into that path as one more additive source. No new render path is needed.

Aligns with the existing wishlist item *"permanent damage to a level (broken crates stay broken, corpses stay)"* (`ideas.md`), and is a precondition for several follow-on ideas (a `screenshot` MCP tool reading back stained walls; decal-aware AI noticing blood trails).

## Approach

Decals are signed RGB *deltas* added to the per-surface lightmap during `R_BuildLightMap_RGB`, after the static + dynamic light sum and before the clamp. The deltas live in a per-surface **stain buffer** at lightmap resolution, owned by `msurface_t`, allocated from a fixed-size pool at map load.

Three additions:

1. **Stain layer in the lightmap build.** `msurface_t` gains `stain_t *stain` (NULL by default). When non-NULL, `R_BuildLightMap_RGB` (and the mono fallback) reads the stain's signed `int16_t` per-luxel-per-channel deltas, adds them in 8.8 fixed point, and clamps. Zero new render code; the surface cache invalidation path is extended with a `cached_stain_gen` field so stain edits force a cache rebuild for that face exactly once.
2. **Impact spawn pipeline.** `CL_ParseTEnt` calls a new `R_SpawnDecal(pos, type)` after each impact-style TE_ message:
   - `TE_GUNSHOT` → `DECAL_BULLET`
   - `TE_SPIKE`, `TE_SUPERSPIKE`, `TE_KNIGHTSPIKE`, `TE_WIZSPIKE` → `DECAL_SPIKE`
   - `TE_BLOOD`, `TE_LIGHTNINGBLOOD` → `DECAL_BLOOD_SPLAT` (often whiffs — entity is rarely against a wall; that's fine, no decal then)
   - `TE_EXPLOSION`, `TE_TAREXPLOSION` → `DECAL_SCORCH`
   - `TE_LIGHTNING1`, `TE_LIGHTNING2`, `TE_LIGHTNING3` → `DECAL_LIGHTNING`, spawned at the beam's *endpoint* (these are beam messages carrying both start and end coords; the end is where the bolt terminates against geometry or an entity)

   `R_SpawnDecal` fires a short outward trace (~8 game units) against `cl.worldmodel` to find the impacted `msurface_t` and surface normal, projects the hit point into the surface's lightmap UV grid, and stamps a small kernel of signed RGB deltas into the stain buffer (allocating on first hit). If the retrace whiffs, the decal is dropped silently.
3. **Blood pools on monster death.** `engine_api_t` gains `void (*spawn_blood_pool)(const float origin[3])`. `game.dll` calls it from monster-death code. The engine traces straight down from `origin` to find the floor surface, allocates an entry in a fixed `bloodpool_t` active list, and over `r_decals_bloodpool_growtime` seconds paints concentric annular rings of blood-coloured deltas into the floor surface's stain buffer. Once at full radius, the pool record is removed from the active list — the painted luxels stay permanent.

The mono lightmap path (`R_BuildLightMap`, used when `r_coloredlight 0` or when the map has no `.lit` file) gets the same stain integration in the luminance domain — `0.3R + 0.59G + 0.11B` of the delta added to the mono `blocklights[]`.

Toggleable end-to-end via `r_decals` (master) and `r_decals_bloodpool` (independent). When `r_decals 0`, the spawn paths short-circuit and existing stains remain in the buffer until map reload but no new ones are written; the lightmap build still applies them (acceptable — turning the cvar off mid-firefight doesn't visibly purge the wall).

## File changes

| File | Change |
|---|---|
| `model.h` | `msurface_t` gains `stain_t *stain` and `int cached_stain_gen`. |
| `r_local.h` | Declarations: `stain_t`, `R_SpawnDecal`, `R_NewMap` decal hooks, `R_DecalsFrame` (per-frame pool growth), `r_decals*` cvars. `bloodpool_t` is private to `r_decals.c`. |
| `r_decals.c` (new) | All decal logic: stain pool allocator, LRU, `R_SpawnDecal`, `R_SpawnBloodPool`, `R_DecalsFrame`, kernel tables, projection math. ~400 lines. |
| `r_surf.c` | `R_BuildLightMap` gets a stain-apply pass after the static+dynamic sum, before the clamp. |
| `r_surf_rgb.c` | `R_BuildLightMap_RGB` gets the same stain-apply pass on the RGB block. |
| `r_main.c` | `R_NewMap` calls `R_DecalsClear()` to zero the pool and reset all `msurface_t::stain` to NULL. `R_RenderView` calls `R_DecalsFrame()` once per frame to advance growing pools. |
| `cl_tent.c` | After each of the impact TE_ cases, call `R_SpawnDecal(pos, type)`. |
| `engine_api.h` (and `hotreload.c`) | Add `spawn_blood_pool` entry; bump `GAME_API_VERSION`. |
| `game.dll` source (`sdlquake/game/`) | Call `engine_api->spawn_blood_pool(self.origin)` in the monster-death path (`PainSound` / `Killed` / wherever the corpse's spawn frame is set). |
| `ideas.md` | Drop "decals" from the Next list once shipped. |

No changes to `gl_*.c` (GL path is dead in this fork).

## Data model

### `stain_t` (per surface)

```c
typedef struct stain_s {
    int16_t   *rgb;             // points into the slot's payload area
    int        smax, tmax;      // == surface's lightmap dimensions
    int        generation;      // bumped on every modification
    int        last_touched_frame;
    msurface_t *surf;           // back-ref for eviction cleanup
    struct stain_s *lru_prev, *lru_next;
} stain_t;
```

`rgb` indexes as `rgb[(v*smax + u) * 3 + channel]`, channels are signed 16-bit deltas in `[−4096, +4096]` (well clear of overflow when added to `blocklights_rgb`'s 8.8 range, scaled by `<<8`).

### Stain pool (singleton, sized at map load)

A single hunk-allocated chunk of `r_decals_max * STAIN_SLOT_BYTES` bytes, where `STAIN_SLOT_BYTES = sizeof(stain_t) + MAX_LUXELS_PER_SURF * 3 * sizeof(int16_t)` and `MAX_LUXELS_PER_SURF = 18*18` (matches the existing `blocklights[18*18]` cap). `STAIN_SLOT_BYTES ≈ 2000` (`sizeof(stain_t) ≈ 56` + payload `1944`). At `r_decals_max = 512`, the pool is ~1MB. Slots are taken from a free list and returned on eviction — no per-impact heap churn. Most real surfaces use far less than the 18×18 worst case, so slot space is over-allocated; in exchange the pool is fixed-stride and trivially recycled. Sub-luxel sized slots / bucketed allocations are a future optimisation if the 1MB becomes a concern.

The pool is allocated in `R_NewMap` and lives until the next map load (`Hunk_FreeToLowMark`). The LRU list is intrusive — head = most recently touched, tail = oldest. Eviction pops the tail.

### `bloodpool_t` (active list, fixed-size array)

```c
typedef struct {
    vec3_t       origin;        // floor hit point (centre of pool)
    msurface_t  *surf;          // target surface
    float        spawn_time;    // cl.time at spawn
    float        radius_max;    // game units
    float        radius_painted;// last radius already painted into the stain
    qboolean     alive;
} bloodpool_t;

bloodpool_t cl_bloodpools[MAX_ACTIVE_BLOODPOOLS]; // 32
```

`R_DecalsFrame` walks `cl_bloodpools`. For each `alive` pool, computes the current target radius as `radius_max * min(1, (cl.time - spawn_time) / r_decals_bloodpool_growtime)`. If `target > radius_painted`, paints the annulus `[radius_painted, target]` of luxels in the pool's target surface, then updates `radius_painted`. Once `radius_painted >= radius_max`, the pool's `alive` flag clears and the slot is reusable.

Active-list overflow: oldest pool's remaining growth is collapsed to instant-finish (one final paint to full radius), slot recycled.

### Decal types

```c
typedef enum {
    DECAL_BULLET,       // TE_GUNSHOT
    DECAL_SPIKE,        // TE_SPIKE, TE_SUPERSPIKE, TE_KNIGHTSPIKE, TE_WIZSPIKE
    DECAL_BLOOD_SPLAT,  // TE_BLOOD, TE_LIGHTNINGBLOOD
    DECAL_SCORCH,       // TE_EXPLOSION, TE_TAREXPLOSION
    DECAL_LIGHTNING,    // future / lightning impact on wall (not in initial TE_ map)
    DECAL_NUM_TYPES
} decal_type_t;
```

Each type has a kernel and a `(dR, dG, dB)` centre delta:

| Type | Kernel | Centre delta | Notes |
|---|---|---|---|
| `BULLET` | 3×3 `[1 2 1 / 2 4 2 / 1 2 1]/16` | (−40, −40, −40) | Slight darkening; small footprint. |
| `SPIKE` | 3×3 same | (−40, −40, −40) | Same as bullet for now; differentiated later if needed. |
| `BLOOD_SPLAT` | 3×3 same | (+60, −40, −40) | Brighter red, slightly darker green/blue → reddish stain. |
| `SCORCH` | 5×5 Gaussian-ish | (−80, −80, −80) | Wider footprint matching explosion radius. |
| `LIGHTNING` | 3×3 same | (−50, −60, −40) | Cool tint, mostly dark. Spawned at the endpoint of `TE_LIGHTNING1/2/3` beam messages. |
| Blood pool ring | 1-luxel annulus painted per growth step | (+80, −60, −60) | Same hue as splat, deeper. |

Deltas are scaled by `r_decals_intensity` (default 1.0) on application.

## Projection math (impact pos → luxel coords)

Given hit point `P` and impacted `msurface_t *surf`:

```c
texinfo_t *tex = surf->texinfo;
float u = DotProduct(P, tex->vecs[0]) + tex->vecs[0][3];
float v = DotProduct(P, tex->vecs[1]) + tex->vecs[1][3];
int   lu = ((int)floorf(u) - surf->texturemins[0]) >> 4;
int   lv = ((int)floorf(v) - surf->texturemins[1]) >> 4;
int   smax = (surf->extents[0] >> 4) + 1;
int   tmax = (surf->extents[1] >> 4) + 1;
if (lu < 0 || lu >= smax || lv < 0 || lv >= tmax) return; // dropped
```

The kernel is applied centred on `(lu, lv)`. Out-of-bounds kernel cells are clipped (kernel pixels falling off the face's lightmap rectangle are simply not painted — slight truncation at face edges, accepted for v1).

## Retrace from TE_ position

```c
static qboolean R_RetraceForDecal(vec3_t pos, vec3_t out_normal, msurface_t **out_surf)
{
    // Trace a short sphere from pos toward the eye, then a few cardinal axes.
    // First worldmodel surface hit within 8 units wins.
    // Returns false if no surface within range or hit a non-decalable surface.
}
```

Tried directions: `(eye − pos)` normalised, then ±X, ±Y, ±Z. Stops at first hit. ~6 short traces worst case — trivial.

Filters out:
- `surf->flags & SURF_DRAWSKY` → no decal on sky.
- `surf->flags & SURF_DRAWTURB` → no decal on water/lava/slime.
- `surf->flags & SURF_DRAWTILED` → no lightmap → no place for stain.
- Hit on a `bmodel` (door, button, train): for v1, skip. Identified by the trace's `ent` field being non-world.

## Lightmap integration

### RGB path (`r_surf_rgb.c::R_BuildLightMap_RGB`)

After the existing static + dynamic accumulation, before clamp & inversion:

```c
if (surf->stain) {
    int n = smax * tmax;
    int16_t *s = surf->stain->rgb;
    for (i = 0; i < n; i++) {
        int br = (int)blocklights_rgb[i*3 + 0] + ((int)s[i*3 + 0] << 8);
        int bg = (int)blocklights_rgb[i*3 + 1] + ((int)s[i*3 + 1] << 8);
        int bb = (int)blocklights_rgb[i*3 + 2] + ((int)s[i*3 + 2] << 8);
        blocklights_rgb[i*3 + 0] = (br < 0) ? 0 : (br > 255*256) ? 255*256 : br;
        blocklights_rgb[i*3 + 1] = (bg < 0) ? 0 : (bg > 255*256) ? 255*256 : bg;
        blocklights_rgb[i*3 + 2] = (bb < 0) ? 0 : (bb > 255*256) ? 255*256 : bb;
    }
}
```

### Mono path (`r_surf.c::R_BuildLightMap`)

```c
if (surf->stain) {
    int n = smax * tmax;
    int16_t *s = surf->stain->rgb;
    for (i = 0; i < n; i++) {
        int dy = (3*s[i*3+0] + 6*s[i*3+1] + s[i*3+2]) / 10; // ~rec.601 luma
        int b = (int)blocklights[i] + (dy << 8);
        blocklights[i] = (b < 0) ? 0 : (b > 255*256) ? 255*256 : b;
    }
}
```

### Cache invalidation

The cache miss check (`R_DrawSurface` / `D_CacheSurface`) compares `surf->cached_stain_gen` against `surf->stain ? surf->stain->generation : 0`. Mismatch → cache rebuild. `R_SpawnDecal` bumps `surf->stain->generation` on every stain modification (impact, growth step) and updates the LRU touch field. The cache rebuild copies the new generation into `cached_stain_gen`.

## Cvars

| Cvar | Default | Archived | Purpose |
|---|---|---|---|
| `r_decals` | 1 | yes | Master toggle. 0 → spawn paths short-circuit. |
| `r_decals_max` | 512 | yes | Max stained surfaces; sets pool size at map load. Changes take effect on next map load. |
| `r_decals_bloodpool` | 1 | yes | Pool-on-death toggle, independent of impact decals. |
| `r_decals_bloodpool_radius` | 24 | yes | Max pool radius (game units). |
| `r_decals_bloodpool_growtime` | 3.0 | yes | Seconds 0 → full radius. |
| `r_decals_intensity` | 1.0 | yes | Global scalar on every stain delta. |

## ABI

`engine_api_t` gains one entry:

```c
void (*spawn_blood_pool)(const float origin[3]);
```

Bump `GAME_API_VERSION` per the rule in `engine_api.h`. The hot-reload loader rejects old DLLs on version mismatch.

`game_api_t` unchanged. `msurface_t` is engine-internal; no ABI surface there.

## Edge cases

| Case | Behaviour |
|---|---|
| Retrace whiffs (no surface within 8 units of `pos`) | Decal dropped silently. Common for `TE_BLOOD` and `TE_LIGHTNINGBLOOD` (entity origin is usually in open space) — expected, not a bug. |
| Surface is sky / water / drawtiled | Skipped (`surf->flags` check). |
| Hit on a brush model (door, lift, train) | Skipped in v1; decal would need to follow the brush's transform — deferred. |
| Impact lands within 1 luxel of face edge — kernel spills | Out-of-bounds kernel cells clipped; slight visible truncation at corners. Accepted. |
| Blood pool spawn over a pit / no floor within 64 units | No pool. |
| Blood pool spawn on water / slime / lava | No pool (target surface filtered). Future: spawn a red particle cloud instead. |
| Blood pool's target surface evicted while still growing | Pool cancelled, active slot freed. |
| `MAX_ACTIVE_BLOODPOOLS` exceeded | Oldest pool fast-forwards to full radius, slot recycled. |
| `r_decals_max` exceeded | Tail of LRU evicted: target surface's `stain` cleared to NULL, slot returned to free list, `cached_stain_gen` bump on the now-unstained surface triggers one final rebuild that removes the visible stain. |
| Map load (`map`, `restart`, `disconnect`+`connect`) | `R_NewMap` calls `R_DecalsClear` → all `msurface_t::stain` set NULL, pool zeroed, active pools cleared. Hunk reset frees the pool's memory naturally. |
| Save / restore | Decals NOT preserved. Save format is server-state only; decals are client-side cosmetic. Documented. v2 candidate. |
| `r_coloredlight 0` | Mono path applied; stains visible as luminance-only darkening/brightening. Blood looks gray rather than red, but still visible as "blood was here". |
| `r_decals 0` then 1 mid-game | New decals start spawning again; existing stains keep showing. Toggle has no purge action. |
| Demo playback | Works (decals are spawned from TE_ events which are in the demo stream). Pool spawn requires `spawn_blood_pool` to be called, which doesn't happen during demo replay — pools won't appear in demos. Documented. |

## Performance

- Per impact: 1–6 short traces (sphere-cast ~8 units, worldmodel only — already O(BSP-leaf) cheap), 1 stain alloc on first hit per surface, 9–25 luxel writes. < 10µs.
- Per stained surface, per `R_BuildLightMap_RGB`: extra `n * 9` ops where `n ≤ 324`. Marginal vs the existing static+dynamic loop.
- Per frame: `R_DecalsFrame` walks 32-entry pool active list, paints at most one annulus per growing pool. < 10µs unless many simultaneous deaths.
- Steady-state (no impacts): zero cost beyond the per-surface stain-apply when the cache rebuilds — and most cached surfaces never rebuild unless a dlight touches them.

Memory: `r_decals_max=512` × `STAIN_SLOT_BYTES (~672)` ≈ 340KB per map load. Frees on map change.

## Testing & verification

No automated test suite (`CLAUDE.md`). Verification is visual + behavioural via `zig build run`. Smoke-test sequence in order:

1. **Compile clean** — `zig build` succeeds with no new warnings beyond baseline.
2. **Impact decals on world**: `zig build run -- +map e1m1`.
   - Shotgun a brick wall close-up → cluster of dark luxels within ~1 frame.
   - Nailgun → small dark stains.
   - Rocket at wall → larger scorch (5×5 footprint).
   - Lightning gun a wall (`impulse 8` then fire) or get a shambler to bolt the wall → cool-tinted scorch at the beam endpoint.
3. **Blood decals**: kill a grunt at point-blank against a wall → red-tinted luxels on the wall behind it.
4. **Blood pools**: kill a grunt on a flat floor → red blob grows from 0 to ~24-unit radius over ~3s, then stays. Confirm by walking back.
5. **Surface filtering**: shoot sky → no decal. Shoot lava → no decal. Shoot the lift in e1m1 start hall → no decal (brush model).
6. **Persistence**: walk away from a stained wall, walk back → decals still there. Spawn a dynamic light near it (rocket flash) → decals still there after flash fades.
7. **Cap / eviction**: `r_decals_max 16` and spray bullets across many surfaces in start hall → oldest stains visibly evict, no crash, no slowdown.
8. **Master toggle**: `r_decals 0` mid-game → no new decals. `r_decals 1` → resumes.
9. **Map reload**: `restart` → all stains gone. `map e1m1` after carnage → fresh map.
10. **Mono fallback**: `r_coloredlight 0` → stains still appear in luminance; coloured-lighting toggle back on → coloured stains return.
11. **Stress**: rocket-spam e1m1 start hall, ~100 impacts in 10s. Frame time stable; surface-cache rebuild counter (imgui dev overlay) doesn't pin to 100%.

Verification before claiming done (per `feedback_smoke_test.md`): compile-clean is not enough. Must run `zig build run -- +map e1m1`, fire each weapon at a wall, kill a monster, and visually confirm each smoke-test point above.

## Future work (explicit non-goals for v1)

- Sub-luxel stain texture overlay (sharper bullet holes — only if v1 looks too patchy in practice).
- Decals on brush models (door / lift / train surfaces).
- Save / restore: persist stain buffers in the savegame.
- Multiplayer / network: replicate decals via a TE_DECAL protocol extension or as a side-band cosmetic stream.
- Particle "blood mist" / spray when pool target is water or sky.
- Per-decal-type sounds (squelch on blood, sizzle on scorch).
- `screenshot` MCP tool integration: read-back stained regions for AI-driven combat analysis.
