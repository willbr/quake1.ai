/*
 * vis_namespace.h -- forced-include header prepended to every vendored
 * vis .c file by build.zig (after qbsp_namespace.h). Renames vis's
 * exported types and functions to vis_-prefixed versions so they don't
 * collide with qbsp's same-named-but-different definitions (qbsp's
 * winding_t / plane_t / portal_t / leaf_t are structurally different
 * from VIS's, and qbsp's portals.c defines its own NewWinding /
 * ClipWinding / CopyWinding / FreeWinding).
 *
 * cmdlib / mathlib / bspfile symbols are already prefixed to qbsp_ by
 * qbsp_namespace.h, which we also force-include — vis's translation
 * units link against qbsp's already-built copies of those modules.
 *
 * Add new entries here when a future link-time collision shows up.
 */

#ifndef VIS_NAMESPACE_H
#define VIS_NAMESPACE_H

/* vis.h type-name clashes with qbsp's bsp5.h (different struct layouts) */
#define winding_t          vis_winding_t
#define plane_t            vis_plane_t
#define portal_t           vis_portal_t
#define leaf_t             vis_leaf_t
#define pstack_s           vis_pstack_s
#define pstack_t           vis_pstack_t
#define threaddata_t       vis_threaddata_t
#define seperating_plane_s vis_seperating_plane_s
#define sep_t              vis_sep_t
#define passage_s          vis_passage_s
#define passage_t          vis_passage_t
#define vstatus_t          vis_vstatus_t

/* vis.c globals */
#define numportals         vis_numportals
#define portalleafs        vis_portalleafs
#define portals            vis_portals
#define leafs              vis_leafs
#define c_portaltest       vis_c_portaltest
#define c_portalpass       vis_c_portalpass
#define c_portalcheck      vis_c_portalcheck
#define showgetleaf        vis_showgetleaf
#define leafon             vis_leafon
#define vismap             vis_vismap
#define vismap_p           vis_vismap_p
#define vismap_end         vis_vismap_end
#define originalvismapsize vis_originalvismapsize
#define uncompressed       vis_uncompressed
#define bitbytes           vis_bitbytes
#define bitlongs           vis_bitlongs
#define numthreads         vis_numthreads
#define fastvis            vis_fastvis
#define verbose            vis_verbose
#define testlevel          vis_testlevel
#define totalvis           vis_totalvis
#define count_sep          vis_count_sep

/* flow.c globals */
#define c_chains           vis_c_chains
#define c_portalskip       vis_c_portalskip
#define c_leafskip         vis_c_leafskip
#define c_vistest          vis_c_vistest
#define c_mighttest        vis_c_mighttest
#define active             vis_active
#define c_leafsee          vis_c_leafsee
#define c_portalsee        vis_c_portalsee
#define portalsee          vis_portalsee

/* Function names that clash with qbsp's portals.c */
#define NewWinding         vis_NewWinding
#define FreeWinding        vis_FreeWinding
#define ClipWinding        vis_ClipWinding
#define CopyWinding        vis_CopyWinding

/* vis-internal functions (no clash today, but prefix for hygiene + so the
 * editor.c side can call them via vis_-prefixed names if needed) */
#define PlaneFromWinding   vis_PlaneFromWinding
#define LeafFlow           vis_LeafFlow
#define BasePortalVis      vis_BasePortalVis
#define PortalFlow         vis_PortalFlow
#define CalcAmbientSounds  vis_CalcAmbientSounds
#define LoadPortals        vis_LoadPortals
#define CalcVis            vis_CalcVis
#define CalcPortalVis      vis_CalcPortalVis
#define CalcPassages       vis_CalcPassages
#define CompressRow        vis_CompressRow
#define ClipToSeperators   vis_ClipToSeperators
#define RecursiveLeafFlow  vis_RecursiveLeafFlow
#define SimpleFlood        vis_SimpleFlood
#define Findpassages       vis_Findpassages
#define CheckStack         vis_CheckStack
#define PlaneCompare       vis_PlaneCompare
#define pw                 vis_pw
#define prl                vis_prl
#define SurfaceBBox        vis_SurfaceBBox

/* vis.c's old main is renamed and never called — vis_compile_in_place is
 * the real entry point. The rename also keeps it from colliding with
 * sys_sdl.c's main(). */
#define main               vis_main_unused

#endif /* VIS_NAMESPACE_H */
