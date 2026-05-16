/*
 * light_bake_thread.c -- SDL_Thread worker that re-bakes lighting in
 * the background and atomically swaps the result into cl.worldmodel.
 *
 * Flow per refresh request:
 *   1. Main thread: edit_scene -> dentdata via serialize_scene_to_ents,
 *      then push it into LIGHT's globals through light_set_entdata.
 *   2. Main thread: SDL_CreateThread spawns the worker, which runs
 *      light_relight_in_place() (LoadEntities + MakeTnodes + LightWorld).
 *      qbsp's dfaces/dplanes/dnodes/dleafs etc. are read-only on the
 *      worker; only dentdata, dlightdata, dface[].lightofs, and
 *      light_rgb_dlightdata are mutated.
 *   3. Worker calls light_snapshot_result to copy the bake's output
 *      into worker-owned heap buffers and signals ready=1.
 *   4. Editor_LightBake_Poll on the main thread (called from
 *      Editor_PreRender) sees ready=1, copies the buffers into the
 *      editor-owned side buffer, repoints cl.worldmodel->lightdata and
 *      rgblightdata, walks surfaces fixing each `samples` and
 *      `rgb_samples` from the new lightofs, and bumps dlightframe on
 *      all surfaces so D_CacheSurface rebuilds with the new lightmap.
 *
 * Data-race notes:
 *   - dentdata is touched only on the main thread before spawning, and
 *     only read by the worker (LoadEntities). No race after spawn.
 *   - dfaces/dplanes/etc. are populated by qbsp_compile_to_memory on
 *     the main thread before any worker runs. Worker reads them; main
 *     thread does not touch them while a bake is in flight.
 *   - The result handoff is via worker-owned heap buffers + a single
 *     SDL_AtomicInt flag, so no shared-mutable-state aliasing.
 */

#include "quakedef.h"
#include "light_lib.h"
#include "light_bake_thread.h"
#include "edit_scene.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int  Editor_IsOpen(void);

/* The worker's snapshot buffers. Owned by the worker thread between
 * spawn and signal-ready; ownership transfers to the main thread on
 * Poll, which copies the contents into s_live_* and then frees these. */
typedef struct {
    unsigned char *mono;
    unsigned char *rgb;
    int           *lightofs;
    int            mono_size;
    int            face_count;
} bake_snapshot_t;

/* Editor-owned side buffer the engine renders out of after the first
 * successful bake. Sized at MAX_MAP_LIGHTING (matches LIGHT's globals)
 * so any bake's output fits. */
#define LIVE_LIGHTING_MAX (0x100000)
static unsigned char *s_live_lightdata    = NULL;
static unsigned char *s_live_rgblightdata = NULL;
static int            s_live_lit          = 0; /* 1 once we've ever painted into the side buffer */

static SDL_AtomicInt  s_in_progress;
static SDL_AtomicInt  s_result_ready;
static bake_snapshot_t s_snap;

/* Serialise the editor scene into a freshly malloc'd entity string of
 * the same shape qbsp produces ("{ \"key\" \"value\" ... }" blocks).
 * Caller frees. Returns the byte length excluding the NUL. */
static char *serialize_scene_to_ents(int *out_len)
{
    int cap = 64 * 1024;
    char *buf = (char *)malloc((size_t)cap);
    int len = 0;
    int i, j;
    if (!buf) { if (out_len) *out_len = 0; return NULL; }
    buf[0] = '\0';

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (e->transient) continue;
        if (e->numkv == 0) continue;

        /* Grow if we might exceed cap on this entity (rough overestimate
         * of 256 bytes per kv pair). */
        if (len + 256 * (e->numkv + 2) > cap)
        {
            int new_cap = cap * 2;
            char *nb = (char *)realloc(buf, (size_t)new_cap);
            if (!nb) { free(buf); if (out_len) *out_len = 0; return NULL; }
            buf = nb;
            cap = new_cap;
        }

        len += snprintf(buf + len, (size_t)(cap - len), "{\n");
        for (j = 0; j < e->numkv; j++)
        {
            len += snprintf(buf + len, (size_t)(cap - len),
                            "\"%s\" \"%s\"\n",
                            e->kv[j].key, e->kv[j].value);
        }
        len += snprintf(buf + len, (size_t)(cap - len), "}\n");
    }

    if (out_len) *out_len = len;
    return buf;
}

/* Worker thread body. Runs once per spawn; the main thread re-spawns
 * for each Trigger call. */
static int SDLCALL bake_thread_fn(void *ud)
{
    bake_snapshot_t *snap = (bake_snapshot_t *)ud;
    int rc;

    rc = light_relight_in_place();
    if (rc != 0)
    {
        /* Mark the snapshot invalid so Poll doesn't apply a half-finished
         * bake. The main thread sees ready=1 + mono=NULL and discards. */
        snap->mono      = NULL;
        snap->rgb       = NULL;
        snap->lightofs  = NULL;
        snap->mono_size = 0;
        snap->face_count = 0;
        SDL_SetAtomicInt(&s_result_ready, 1);
        return 0;
    }

    rc = light_snapshot_result(snap->mono, LIVE_LIGHTING_MAX,
                               snap->rgb,
                               snap->lightofs, MAX_MAP_FACES,
                               &snap->mono_size, &snap->face_count);
    if (rc != 0)
    {
        /* Capacity overflow -- treat as a failed bake. */
        snap->mono_size = 0;
        snap->face_count = 0;
    }

    SDL_SetAtomicInt(&s_result_ready, 1);
    return 0;
}

void Editor_LightBake_Init(void)
{
    SDL_SetAtomicInt(&s_in_progress, 0);
    SDL_SetAtomicInt(&s_result_ready, 0);
    if (!s_live_lightdata)
        s_live_lightdata = (unsigned char *)malloc(LIVE_LIGHTING_MAX);
    if (!s_live_rgblightdata)
        s_live_rgblightdata = (unsigned char *)malloc(LIVE_LIGHTING_MAX * 3);
    memset(&s_snap, 0, sizeof(s_snap));
}

void Editor_LightBake_Shutdown(void)
{
    /* If a worker is still running we'd have to wait or detach; in the
     * shutdown path the process is exiting so just leak the thread.
     * We do free our side buffer so leak detectors stay quiet. */
    free(s_live_lightdata);    s_live_lightdata = NULL;
    free(s_live_rgblightdata); s_live_rgblightdata = NULL;
    free(s_snap.mono);          s_snap.mono = NULL;
    free(s_snap.rgb);           s_snap.rgb = NULL;
    free(s_snap.lightofs);      s_snap.lightofs = NULL;
    s_live_lit = 0;
}

int Editor_LightBake_InProgress(void)
{
    return SDL_GetAtomicInt(&s_in_progress);
}

int Editor_LightBake_Trigger(void)
{
    char *ents;
    int   ents_len = 0;
    SDL_Thread *t;

    if (SDL_GetAtomicInt(&s_in_progress)) return 1;

    /* Allocate snapshot scratch on the heap so the worker owns it until
     * Poll consumes it. */
    s_snap.mono = (unsigned char *)malloc(LIVE_LIGHTING_MAX);
    s_snap.rgb  = (unsigned char *)malloc(LIVE_LIGHTING_MAX * 3);
    s_snap.lightofs = (int *)malloc(sizeof(int) * MAX_MAP_FACES);
    s_snap.mono_size = 0;
    s_snap.face_count = 0;
    if (!s_snap.mono || !s_snap.rgb || !s_snap.lightofs)
    {
        free(s_snap.mono); s_snap.mono = NULL;
        free(s_snap.rgb);  s_snap.rgb = NULL;
        free(s_snap.lightofs); s_snap.lightofs = NULL;
        Con_Printf("Editor_LightBake_Trigger: out of memory\n");
        return -1;
    }

    ents = serialize_scene_to_ents(&ents_len);
    if (!ents)
    {
        free(s_snap.mono);     s_snap.mono = NULL;
        free(s_snap.rgb);      s_snap.rgb  = NULL;
        free(s_snap.lightofs); s_snap.lightofs = NULL;
        Con_Printf("Editor_LightBake_Trigger: serialise failed\n");
        return -1;
    }
    light_set_entdata(ents, ents_len);
    free(ents);

    SDL_SetAtomicInt(&s_in_progress, 1);
    SDL_SetAtomicInt(&s_result_ready, 0);
    t = SDL_CreateThread(bake_thread_fn, "lightbake", &s_snap);
    if (!t)
    {
        SDL_SetAtomicInt(&s_in_progress, 0);
        free(s_snap.mono);     s_snap.mono = NULL;
        free(s_snap.rgb);      s_snap.rgb  = NULL;
        free(s_snap.lightofs); s_snap.lightofs = NULL;
        Con_Printf("Editor_LightBake_Trigger: SDL_CreateThread failed\n");
        return -1;
    }
    SDL_DetachThread(t);
    return 0;
}

/* Apply a finished bake on the main thread. Copies the snapshot into
 * s_live_*, repoints cl.worldmodel pointers, fixes up surface
 * samples/rgb_samples from the new lightofs, and bumps dlightframe. */
static void apply_snapshot(void)
{
    model_t  *mod = cl.worldmodel;
    msurface_t *surf;
    int i;
    int max_faces;

    if (!s_snap.mono || s_snap.mono_size <= 0 || !mod)
    {
        Con_Printf("light bake: empty snapshot; discarded\n");
        return;
    }
    if (!s_live_lightdata || !s_live_rgblightdata)
    {
        /* Init wasn't called for some reason. */
        return;
    }

    memcpy(s_live_lightdata, s_snap.mono, (size_t)s_snap.mono_size);
    memcpy(s_live_rgblightdata, s_snap.rgb, (size_t)s_snap.mono_size * 3);

    mod->lightdata    = s_live_lightdata;
    mod->rgblightdata = s_live_rgblightdata;
    s_live_lit = 1;

    /* If the engine's per-map mutable buffer exists and fits the new
     * bake, route surf->rgb_samples through it so the r_livelight
     * delta system (paint_light_preview, Gust overrides) keeps working
     * after a re-bake. Otherwise fall back to pointing at the bake
     * worker's own buffer — the old behaviour, which means the override
     * system is dormant until the next map load. */
    {
        extern int loadmodel_rgblightdata_size;
        int new_size = s_snap.mono_size * 3;
        int use_live = mod->live_rgblightdata &&
                       loadmodel_rgblightdata_size >= new_size;
        if (use_live) {
            memcpy(mod->live_rgblightdata, s_live_rgblightdata,
                   (size_t)new_size);
            loadmodel_rgblightdata_size = new_size;
        }

        max_faces = mod->numsurfaces < s_snap.face_count
                  ? mod->numsurfaces : s_snap.face_count;
        if (mod->numsurfaces != s_snap.face_count)
        {
            Con_Printf("light bake: surface count drift (engine=%d, bake=%d); "
                       "applying first %d faces only\n",
                       mod->numsurfaces, s_snap.face_count, max_faces);
        }

        surf = mod->surfaces;
        for (i = 0; i < max_faces; i++, surf++)
        {
            int ofs = s_snap.lightofs[i];
            if (ofs < 0 || ofs >= s_snap.mono_size)
            {
                surf->samples     = NULL;
                surf->rgb_samples = NULL;
            }
            else
            {
                surf->samples     = s_live_lightdata    + ofs;
                surf->rgb_samples = use_live
                    ? mod->live_rgblightdata + ofs * 3
                    : s_live_rgblightdata    + ofs * 3;
            }
            /* Force the surface cache to rebuild on the next frame so
             * the new lightmap actually paints. Same precedent the
             * dlight injection path uses (cf. r_surf.c). */
            surf->dlightframe = r_framecount;
        }

        /* Same cache-staleness fix as Editor_LightBake_ApplyFromDisk: a
         * dlightframe bump alone leaves visible surfaces serving stale
         * cached pixels until something else invalidates them. */
        D_FlushCaches();

        if (use_live) {
            /* Re-apply every outstanding override on top of the fresh
             * baseline (paint_light_preview entries + any Gust gameplay
             * overrides survive the bake). */
            extern void Lightmap_BaselineChanged(void);
            Lightmap_BaselineChanged();
        }
    }

    Con_Printf("light bake: applied (%d faces, %d bytes mono / %d bytes lit)\n",
               s_snap.face_count, s_snap.mono_size, s_snap.mono_size * 3);

    /* Snapshot consumed -- free the worker's scratch. */
    free(s_snap.mono);     s_snap.mono = NULL;
    free(s_snap.rgb);      s_snap.rgb  = NULL;
    free(s_snap.lightofs); s_snap.lightofs = NULL;
    s_snap.mono_size = 0;
    s_snap.face_count = 0;
}

void Editor_LightBake_Poll(void)
{
    if (!SDL_GetAtomicInt(&s_result_ready)) return;
    apply_snapshot();
    SDL_SetAtomicInt(&s_result_ready, 0);
    SDL_SetAtomicInt(&s_in_progress, 0);
}

int Editor_LightBake_ApplyFromDisk(const char *bsp_path)
{
    int rc;
    int mono_size = 0, face_count = 0;
    int max_faces;
    int *lightofs;
    int i;
    model_t  *mod = cl.worldmodel;
    msurface_t *surf;

    if (!mod) {
        Con_Printf("light_apply: no cl.worldmodel\n");
        return -1;
    }
    if (!s_live_lightdata || !s_live_rgblightdata) {
        Con_Printf("light_apply: live buffer not initialised\n");
        return -1;
    }

    rc = light_relight_loaded_bsp(bsp_path);
    if (rc != 0) return rc;

    lightofs = (int *)malloc(sizeof(int) * MAX_MAP_FACES);
    if (!lightofs) {
        Con_Printf("light_apply: oom\n");
        return -1;
    }

    rc = light_snapshot_result(s_live_lightdata, LIVE_LIGHTING_MAX,
                               s_live_rgblightdata,
                               lightofs, MAX_MAP_FACES,
                               &mono_size, &face_count);
    if (rc != 0) {
        free(lightofs);
        Con_Printf("light_apply: snapshot overflow\n");
        return rc;
    }

    mod->lightdata    = s_live_lightdata;
    mod->rgblightdata = s_live_rgblightdata;
    s_live_lit = 1;

    /* Engine surfaces should be 1:1 with the BSP face array. A drift
     * means the loaded .bsp on disk doesn't match cl.worldmodel -- not
     * fatal, but we should only repoint what overlaps. */
    max_faces = mod->numsurfaces < face_count
              ? mod->numsurfaces : face_count;
    if (mod->numsurfaces != face_count)
    {
        Con_Printf("light_apply: surface count drift (engine=%d, bake=%d); "
                   "applying first %d faces only\n",
                   mod->numsurfaces, face_count, max_faces);
    }

    surf = mod->surfaces;
    for (i = 0; i < max_faces; i++, surf++)
    {
        int ofs = lightofs[i];
        if (ofs < 0 || ofs >= mono_size) {
            surf->samples     = NULL;
            surf->rgb_samples = NULL;
        } else {
            surf->samples     = s_live_lightdata    + ofs;
            surf->rgb_samples = s_live_rgblightdata + ofs * 3;
        }
        surf->dlightframe = r_framecount;
    }

    /* light_apply runs during console processing, before R_RenderView
     * bumps r_framecount, so setting dlightframe = r_framecount lands
     * one behind: the next frame's cache check sees
     * `dlightframe (X) != r_framecount (X+1)` AND cache->dlight is
     * still 0 from the previous render, so D_CacheSurface returns the
     * stale cached pixels instead of re-rendering. The visible
     * symptom is "AO doesn't appear until I walk around" (new
     * surfaces entering the frustum allocate fresh caches). Flushing
     * the entire surface cache nulls every cachespots[*] so the next
     * D_CacheSurface call always falls through to D_SCAlloc +
     * R_DrawSurface, which reads our freshly-repointed surf->samples.
     * Same precedent as host.c:503 on map load. */
    D_FlushCaches();

    free(lightofs);
    Con_Printf("light_apply: applied (%d faces, %d bytes mono / %d bytes lit)\n",
               face_count, mono_size, mono_size * 3);
    return 0;
}
