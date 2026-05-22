# Coloured flat-shaded render mode (`r_drawflat 2`)

A debug/nav render mode that replaces texture sampling with a per-surface
flat colour chosen from the surface's geometric category, while keeping
the lightmap blend (and `.lit` colour data) intact. Items, monsters, and
brush-model entities (doors, buttons, plats) get distinct flat tints so
the level reads as a CAD-style colour-coded diagram.

## Cvar

Extend the existing `r_drawflat` cvar with sub-modes:

| Value | Behavior                                                          |
|-------|-------------------------------------------------------------------|
| 0     | Off (normal textured rendering).                                  |
| 1     | Original id mode: `D_DrawSolidSurface(s, hash(s->data))`. Kept.   |
| 2     | New coloured-by-normal lit mode (this design).                    |

Changes to `r_drawflat` flush the surface cache (`D_FlushCaches`) so
cached pixels from the previous mode are invalidated. Done in the same
"if cvar changed" block in `R_RenderView` that already handles
`r_lightmap`, `r_coloredlight`, etc.

## Categories and palette assignments

Six geometric buckets from the face plane normal (after `SURF_PLANEBACK`
flip), plus overrides for special surface flags and entity classes.
The "primary" bucket is chosen by the dominant axis of the world-space
face normal (max of |x|, |y|, |z|).

| Category               | Selector                                          | Palette idx |
|------------------------|---------------------------------------------------|-------------|
| Floor                  | normal.z >= +0.7                                  | 47  (cyan)  |
| Ceiling                | normal.z <= -0.7                                  | 144 (dk blue) |
| Wall +X (east)         | normal.x >= +0.7                                  | 79  (lt green) |
| Wall -X (west)         | normal.x <= -0.7                                  | 95  (dk green) |
| Wall +Y (north)        | normal.y >= +0.7                                  | 207 (lt red) |
| Wall -Y (south)        | normal.y <= -0.7                                  | 220 (dk red) |
| Slope/other            | none of the above                                 | 7   (gray)  |
| Brush-entity face      | `currententity != &cl_entities[0]` (overrides all)| 251 (gold)  |
| Sky                    | `SURF_DRAWSKY`                                    | 50  (lt blue) |
| Water                  | `SURF_DRAWTURB` + texture name starts with `*`    | 35  (blue)  |
| Lava                   | texture name starts with `*lava`                  | 240 (orange)|
| Slime                  | texture name starts with `*slime`                 | 56  (green) |
| Item pickup            | alias model `progs/g_*`, `progs/w_*`, `progs/b_*`, `progs/m_*` | 15  (white) |
| Monster                | alias model `progs/{soldier,dog,ogre,knight,hknight,wizard,demon,shambler,zombie,shalrath,enforcer,fish,boss,oldone,tarbaby}.mdl` | 251 (gold→red 196) |
| Player viewmodel       | `currententity == &cl.viewent`                    | hidden      |

The palette values are tunable starting defaults; the table lives in
`r_drawflat.c` so editing is one file. Sloped faces fall through to the
"other" bucket (mid gray) so non-axial surfaces are obvious.

## Architecture

Two intercept points and one new translation unit.

### `sdlquake/engine_src/r_drawflat.c` (new)

Owns the category logic and the uniform-source buffers.

```c
// Per-frame uniform source buffers (one byte filled to category color).
// 256*256 covers any texture extent; uniform fill makes UV wrap a no-op.
extern byte r_drawflat_src[R_FLAT_NUM_CATS][256 * 256];

// Initialize at startup: fill each buffer with the category's base color.
void R_DrawFlat_Init (void);

// Surface category for a world brush face. Selects from normal + flags
// + currententity. Returns one of R_FLAT_CAT_*.
int  R_DrawFlat_SurfCategory (msurface_t *surf);

// Alias-model category. Selects from currententity->model->name.
// Returns R_FLAT_CAT_VIEWMODEL_HIDE for cl.viewent.
int  R_DrawFlat_AliasCategory (entity_t *ent);
```

`R_DrawFlat_Init` is called once from `R_Init` after the palette is
loaded; it fills the static source buffers with their assigned palette
index. Buffers are static (not per-frame), so allocation cost is paid
once.

### Surface path hook (`r_surf.c::R_DrawSurface`)

Mirroring the existing `r_lightmap` precedent at lines 547-555:

```c
if (r_drawflat.value == 2) {
    int cat = R_DrawFlat_SurfCategory (r_drawsurf.surf);
    r_source = r_drawflat_src[cat];
}
```

Inserted after the lightmap-mode override, before the texture-extent
calculations. The lightmap blend in `R_DrawSurfaceBlock8_mip*` then
modulates that one colour through Quake's colormap exactly as it
would a real texel, producing lit flat shading.

### Surface dispatch (`d_edge.c::D_DrawSurfaces`)

The `if (r_drawflat.value)` block at line 187 currently catches mode 1
and forces `D_DrawSolidSurface(s, hash)`. Change it to keep that
behavior only when `r_drawflat.value == 1`; for mode 2 fall through to
the normal textured path (which then picks up the override above).

Special flags need light handling in mode 2:
- `SURF_DRAWSKY`: skip `R_MakeSky` / `D_DrawSkyScans8` and just call
  `D_DrawSolidSurface(s, sky_color)` so the sky is one solid colour.
- `SURF_DRAWBACKGROUND`: unchanged (already a solid fill).
- `SURF_DRAWTURB`: `Turbulent8` reads from `cacheblock` rather than the
  surface cache. Easiest path: override `cacheblock` to point at the
  appropriate liquid uniform buffer before calling `Turbulent8`. The
  warp stays (and looks fine on a uniform source — it's a no-op), so
  water/lava/slime get their category colour without a special drawer.

### Alias path hook (`r_alias.c::R_AliasDrawModel`)

After `R_AliasSetupSkin()` (which sets `r_affinetridesc.pskin`):

```c
if (r_drawflat.value == 2) {
    int cat = R_DrawFlat_AliasCategory (currententity);
    if (cat == R_FLAT_CAT_VIEWMODEL_HIDE) return;
    r_affinetridesc.pskin = r_drawflat_src[cat];
    // skinwidth/height untouched — uniform buffer is large enough.
}
```

The polyset rasterizer samples `pskin[s + t*skinwidth]` then applies
`acolormap[shade<<8 | pix]`. Uniform source → every pixel becomes the
shaded category colour.

## Cache invalidation

Add `r_drawflat` to the "cvar changed → `D_FlushCaches`" block in
`R_RenderView_` (r_main.c:1208). Toggling between modes 0/1/2 forces a
one-frame surface-cache rebuild, after which everything is steady.

Within a single value of `r_drawflat == 2`, no extra flushing is needed:
each surface's `cachespots[]` lives on the surface itself and the cached
data correctly reflects the per-surface category + lightmap.

## Out of scope

- Distinguishing doors from buttons from plats: requires QuakeC
  classname info that the engine doesn't have without a `game_api`
  extension. All brush submodel entities share one bucket. If we want
  finer detail later, add a `game_api->Get_Entity_Color_Hint(edict)`
  call and tag from the game side.
- Smooth normal-to-colour gradient: rejected during brainstorming.
- A separate `r_drawnormals` cvar: rejected — `r_drawflat 2` is enough.
- Configurable palette indices via cvars: the table is editable in one
  file; per-category cvars would add 12+ cvars for marginal value.

## Risks

- **Cache key collisions:** Surface cache key includes `cache->texture`
  but not the source-content variant. Two surfaces sharing the same
  texture animation but different normals would *not* alias because
  `cachespots[]` is per-msurface, not per-texture. Safe.
- **Hot path branch:** Adds one `r_drawflat.value == 2` compare per
  call to `R_DrawSurface`. Negligible — already several such compares
  in that function.
- **Turbulent path warp:** Warping a uniform source is a no-op, so
  water/lava/slime end up as solid blocks (no shimmer). That's the
  intended look for nav mode.
- **Sloped surfaces:** Anything more than ~45° off the cardinal axes
  falls into the gray "other" bucket. Acceptable for nav use — the
  user can still pick out the geometry by relative orientation, and
  the lit shading hints at curvature.
