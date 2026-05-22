// r_drawflat.h -- coloured flat-shaded debug render mode (`r_drawflat 2`).

#ifndef R_DRAWFLAT_H
#define R_DRAWFLAT_H

#define R_FLAT_SRC_SIZE (256 * 256)

enum {
	R_FLAT_CAT_FLOOR,
	R_FLAT_CAT_CEILING,
	R_FLAT_CAT_WALL_PX,
	R_FLAT_CAT_WALL_NX,
	R_FLAT_CAT_WALL_PY,
	R_FLAT_CAT_WALL_NY,
	R_FLAT_CAT_SLOPE,
	R_FLAT_CAT_BRUSH_ENT,
	R_FLAT_CAT_SKY,
	R_FLAT_CAT_WATER,
	R_FLAT_CAT_LAVA,
	R_FLAT_CAT_SLIME,
	R_FLAT_CAT_ITEM,
	R_FLAT_CAT_MONSTER,
	R_FLAT_CAT_VIEWMODEL,
	R_FLAT_NUM_CATS,
};

extern byte r_drawflat_src[R_FLAT_NUM_CATS][R_FLAT_SRC_SIZE];

void R_DrawFlat_Init (void);
int  R_DrawFlat_SurfCategory (msurface_t *surf);
int  R_DrawFlat_AliasCategory (entity_t *ent);

#endif
