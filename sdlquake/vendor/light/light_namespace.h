/*
 * light_namespace.h -- forced-include header prepended to every vendored
 * light .c file by build.zig (after qbsp_namespace.h). Renames light's
 * exported symbols to light_-prefixed versions so they don't collide
 * with qbsp's identically-named globals (LIGHT and QBSP both declare
 * `entities`, `num_entities`, `entity_t`, etc.) in the same binary.
 *
 * cmdlib / mathlib / bspfile symbols are already renamed to qbsp_ by
 * qbsp_namespace.h, which we also force-include — the LIGHT translation
 * units link against qbsp's already-built copies of those modules.
 */

#ifndef LIGHT_NAMESPACE_H
#define LIGHT_NAMESPACE_H

/* light.c file-scope globals */
#define scaledist            light_scaledist
#define scalecos             light_scalecos
#define rangescale           light_rangescale
#define filebase             light_filebase
#define file_p               light_file_p
#define file_end             light_file_end
#define bspmodel             light_bspmodel
#define bspfileface          light_bspfileface
#define bsp_origin           light_bsp_origin
#define bsp_xvector          light_bsp_xvector
#define bsp_yvector          light_bsp_yvector
#define extrasamples         light_extrasamples
#define minlights            light_minlights
#define GetFileSpace         light_GetFileSpace
#define LightThread          light_LightThread
#define LightWorld           light_LightWorld

/* entities.c globals + helpers (clash with qbsp's map.c) */
#define entity_s             light_entity_s
#define entity_t             light_entity_t
#define epair_s              light_epair_s
#define epair_t              light_epair_t
#define entities             light_entities
#define num_entities         light_num_entities
#define numlighttargets      light_numlighttargets
#define lighttargets         light_lighttargets
#define LightStyleForTargetname light_LightStyleForTargetname
#define MatchTargets         light_MatchTargets
#define LoadEntities         light_LoadEntities
#define ValueForKey          light_ValueForKey
#define SetKeyValue          light_SetKeyValue
#define FloatForKey          light_FloatForKey
#define GetVectorForKey      light_GetVectorForKey
#define WriteEntitiesToString light_WriteEntitiesToString

/* ltface.c */
#define CastRay              light_CastRay
#define lightinfo_t          light_lightinfo_t
#define CalcFaceVectors      light_CalcFaceVectors
#define CalcFaceExtents      light_CalcFaceExtents
#define CalcPoints           light_CalcPoints
#define c_bad                light_c_bad
#define SingleLightFace      light_SingleLightFace
#define FixMinlight          light_FixMinlight
#define LightFace            light_LightFace
#define c_culldistplane      light_c_culldistplane
#define c_proper             light_c_proper

/* trace.c */
#define tnode_s              light_tnode_s
#define tnode_t              light_tnode_t
#define tnodes               light_tnodes
#define tnode_p              light_tnode_p
#define MakeTnode            light_MakeTnode
#define MakeTnodes           light_MakeTnodes
#define TestLine             light_TestLine
#define tracestack_t         light_tracestack_t

/* threads (stubbed) */
#define numthreads           light_numthreads
#define InitThreads          light_InitThreads
#define RunThreadsOn         light_RunThreadsOn
#define threadfunc_t         light_threadfunc_t

/* light.c's old main is renamed and never called — light_compile_to_memory
 * is the real entry point. */
#define main                 light_main_unused

#endif /* LIGHT_NAMESPACE_H */
