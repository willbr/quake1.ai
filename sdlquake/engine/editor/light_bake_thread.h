/*
 * light_bake_thread.h -- background light-bake worker for the editor.
 *
 * editor_compile_full populates qbsp's globals and runs the bake once
 * on the main thread (current behaviour, ~2 s for start.bsp). After
 * that, the user can poke lights in the inspector and request
 * incremental re-bakes via Editor_LightBake_Trigger; the worker runs
 * on an SDL_Thread, the main thread polls every frame, and the result
 * lands into a side buffer that cl.worldmodel->lightdata /
 * rgblightdata are pointed at. Surface samples pointers + dlightframe
 * are also fixed up so the renderer rebuilds caches with the new bake.
 */

#ifndef LIGHT_BAKE_THREAD_H
#define LIGHT_BAKE_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

void Editor_LightBake_Init(void);
void Editor_LightBake_Shutdown(void);

/*
 * Returns 1 if a bake is currently in flight, 0 otherwise. UI can show
 * a spinner / disable the trigger button while in_progress is set.
 */
int  Editor_LightBake_InProgress(void);

/*
 * Queue a re-bake. Returns:
 *   0   -- queued / spawned successfully
 *   1   -- a bake is already in flight; the caller may try again later
 *   -1  -- not ready (qbsp's globals aren't populated; editor_compile_full
 *          hasn't run yet)
 */
int  Editor_LightBake_Trigger(void);

/*
 * Per-frame poll from Editor_PreRender. If the worker has finished,
 * copies its result into the side buffer, repoints cl.worldmodel's
 * lightdata + rgblightdata, walks surfaces fixing samples/rgb_samples
 * from the worker's lightofs table, and bumps dlightframe on every
 * surface to invalidate the surface cache. Cheap when no bake is
 * pending (single atomic read + early return).
 */
void Editor_LightBake_Poll(void);

/*
 * Synchronous bake-and-apply against a .bsp on disk, no editor session
 * required. Loads bsp_path into LIGHT's globals (LoadBSPFile), runs
 * LightWorld using the current persistent options, snapshots the result
 * into the live side buffer, and repoints cl.worldmodel + surfaces so
 * the rendered scene picks up the new lightmap on the next frame.
 *
 * This is the "I just want to see my lighting changes on the running
 * map" path; it bypasses qbsp + the brush-decompile-from-BSP gap that
 * editor_compile_full hits. Returns 0 on success.
 */
int  Editor_LightBake_ApplyFromDisk(const char *bsp_path);

#ifdef __cplusplus
}
#endif

#endif
