/*
 * r_livelight.h — Live RGB lightmap delta system.
 *
 * Editor preview lights and gust-extinguished torches write into the
 * per-map mutable rgblightdata (model.live_rgblightdata) once per dirty
 * event; the per-frame dlight pipeline (R_PushDlights /
 * R_AddDynamicLights_RGB) is no longer involved for these long-lived
 * contributions.
 *
 * Owners:
 *   LIGHTMAP_OWNER_EDITOR    paint_light_preview entries.
 *   LIGHTMAP_OWNER_GAMEPLAY  DLL-driven (sim_light Light_AddOverride).
 *
 * The override list is the source of truth; live_rgblightdata is a
 * cache. ClearOwner walks the list, drops matching entries, then
 * restores baseline from rgblightdata and replays survivors.
 */
#ifndef R_LIVELIGHT_H
#define R_LIVELIGHT_H

/* Includer is responsible for pulling in quakedef.h first (engine
 * convention; quakedef.h has no header guard so re-including it via
 * sibling headers drags in bspfile.h twice and breaks the build). */

#define LIGHTMAP_OWNER_EDITOR    1
#define LIGHTMAP_OWNER_GAMEPLAY  2

#define LIGHTMAP_MAX_OVERRIDES   1024

void Lightmap_Init(void);
void Lightmap_Shutdown(void);

/* Called on level change (CL_ClearState). Drops every override; the
 * next Mod_LoadLighting re-seeds live_rgblightdata via the model loader. */
void Lightmap_ClearAll(void);

/* pos, radius in world units. color in 0..1 per channel; applied with
 * the same per-texel falloff as R_AddDynamicLights_RGB, then clamped
 * to byte range. Negative components darken (gust). */
void Lightmap_AddDelta(const vec3_t pos, float radius,
                       const vec3_t color, int owner);

/* Remove every override matching `owner`, rebuild buffer = baseline
 * + Σ(surviving overrides). D_FlushCaches at end. */
void Lightmap_ClearOwner(int owner);

#endif
