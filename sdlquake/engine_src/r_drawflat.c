/*
r_drawflat.c -- coloured flat-shaded debug render mode (`r_drawflat 2`)

Replaces texture sampling with a per-surface uniform colour chosen from
geometric/entity category. The lightmap blend still runs, so lit flat
shading falls out for free.

Category selection is centralised here. The fill is delivered via
static 256*256 uniform source buffers, one per category, swapped into
`r_source` (R_DrawSurface) or `r_affinetridesc.pskin` (R_AliasDrawModel).

Buffer size 256*256 covers the largest possible Quake texture, so the
existing UV-wrap math is a no-op against a uniform source.
*/

#include "quakedef.h"
#include "r_local.h"
#include "r_drawflat.h"

byte r_drawflat_src[R_FLAT_NUM_CATS][R_FLAT_SRC_SIZE];

// Palette index per category. Tunable starting defaults — picked for
// visual distinctness on the stock Quake palette.
static const byte r_flat_palette[R_FLAT_NUM_CATS] = {
	[R_FLAT_CAT_FLOOR]      = 47,	// cyan
	[R_FLAT_CAT_CEILING]    = 144,	// dark blue
	[R_FLAT_CAT_WALL_PX]    = 79,	// light green
	[R_FLAT_CAT_WALL_NX]    = 95,	// dark green
	[R_FLAT_CAT_WALL_PY]    = 207,	// light red
	[R_FLAT_CAT_WALL_NY]    = 220,	// dark red
	[R_FLAT_CAT_SLOPE]      = 7,	// mid gray
	[R_FLAT_CAT_BRUSH_ENT]  = 251,	// gold
	[R_FLAT_CAT_SKY]        = 50,	// light blue
	[R_FLAT_CAT_WATER]      = 35,	// blue
	[R_FLAT_CAT_LAVA]       = 240,	// orange
	[R_FLAT_CAT_SLIME]      = 56,	// green
	[R_FLAT_CAT_ITEM]       = 15,	// white
	[R_FLAT_CAT_MONSTER]    = 196,	// red
	[R_FLAT_CAT_VIEWMODEL]  = 11,	// pale gray — "your" weapon
};

void R_DrawFlat_Init (void)
{
	int i;
	for (i = 0; i < R_FLAT_NUM_CATS; i++)
		memset (r_drawflat_src[i], r_flat_palette[i], R_FLAT_SRC_SIZE);
}

int R_DrawFlat_SurfCategory (msurface_t *surf)
{
	const char *texname;
	vec3_t n;
	float ax, ay, az;

	// Sky / liquids first so they survive even inside brush submodel
	// entities (rare, but a func_water is legal).
	if (surf->flags & SURF_DRAWSKY)
		return R_FLAT_CAT_SKY;

	if ((surf->flags & SURF_DRAWTURB) && surf->texinfo && surf->texinfo->texture)
	{
		texname = surf->texinfo->texture->name;
		if (texname[0] == '*')
		{
			if (!strncmp (texname + 1, "lava", 4))  return R_FLAT_CAT_LAVA;
			if (!strncmp (texname + 1, "slime", 5)) return R_FLAT_CAT_SLIME;
			return R_FLAT_CAT_WATER;
		}
	}

	if (currententity && currententity != &cl_entities[0])
		return R_FLAT_CAT_BRUSH_ENT;

	if (!surf->plane)
		return R_FLAT_CAT_SLOPE;

	VectorCopy (surf->plane->normal, n);
	if (surf->flags & SURF_PLANEBACK)
		VectorInverse (n);

	ax = fabs (n[0]);
	ay = fabs (n[1]);
	az = fabs (n[2]);

	if (az >= 0.7f)
		return (n[2] > 0.0f) ? R_FLAT_CAT_FLOOR : R_FLAT_CAT_CEILING;
	if (ax >= 0.7f && ax >= ay)
		return (n[0] > 0.0f) ? R_FLAT_CAT_WALL_PX : R_FLAT_CAT_WALL_NX;
	if (ay >= 0.7f)
		return (n[1] > 0.0f) ? R_FLAT_CAT_WALL_PY : R_FLAT_CAT_WALL_NY;

	return R_FLAT_CAT_SLOPE;
}

// Classify by model basename. Quake stores model name as the full path
// like "progs/g_shot.mdl"; we look at the part after the last slash.
static const char *model_basename (const char *path)
{
	const char *p = strrchr (path, '/');
	return p ? p + 1 : path;
}

int R_DrawFlat_AliasCategory (entity_t *ent)
{
	const char *name, *base;
	int i;
	static const char * const monster_names[] = {
		"soldier.mdl", "dog.mdl", "ogre.mdl", "knight.mdl", "hknight.mdl",
		"wizard.mdl", "demon.mdl", "shambler.mdl", "zombie.mdl",
		"shalrath.mdl", "enforcer.mdl", "fish.mdl", "boss.mdl",
		"oldone.mdl", "tarbaby.mdl",
		NULL,
	};

	if (ent == &cl.viewent)
		return R_FLAT_CAT_VIEWMODEL;

	if (!ent->model)
		return R_FLAT_CAT_ITEM;	// shouldn't happen; pick a benign default

	name = ent->model->name;
	base = model_basename (name);

	// Item pickups: progs/g_*, progs/w_*, progs/b_*, progs/m_*.
	// g_ = guns/items, w_ = world weapons, b_ = backpack/backpack-like,
	// m_ = mega/armor.
	if (base[0] && base[1] == '_' &&
	    (base[0] == 'g' || base[0] == 'w' || base[0] == 'b' || base[0] == 'm'))
		return R_FLAT_CAT_ITEM;

	for (i = 0; monster_names[i]; i++)
		if (!strcmp (base, monster_names[i]))
			return R_FLAT_CAT_MONSTER;

	// Unclassified alias model — gibs, debris, projectiles, etc. Use the
	// monster colour so anything dynamic-looking is still highlighted.
	return R_FLAT_CAT_MONSTER;
}
