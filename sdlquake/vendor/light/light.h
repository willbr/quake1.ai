
#include "cmdlib.h"
#include "mathlib.h"
#include "bspfile.h"
#include "entities.h"
#include "threads.h"

#define	ON_EPSILON	0.1

#define	MAXLIGHTS			1024

void LoadNodes (char *file);
qboolean TestLine (vec3_t start, vec3_t stop);

void LightFace (int surfnum);
void LightLeaf (dleaf_t *leaf);

void MakeTnodes (dmodel_t *bm);

extern	float		scaledist;
extern	float		scalecos;
extern	float		rangescale;

/* Dirt / ambient-occlusion globals -- mirror the light_options_t fields
 * of the same name. Read in LightFace's emit loop. */
extern	qboolean	dirt_enable;
extern	float		dirt_gain;
extern	float		dirt_depth;
extern	int		dirt_samples;
extern	qboolean	dirt_debug;

extern	int		c_culldistplane, c_proper;

byte *GetFileSpace (int size);
extern	byte		*filebase;

extern	vec3_t	bsp_origin;
extern	vec3_t	bsp_xvector;
extern	vec3_t	bsp_yvector;

void TransformSample (vec3_t in, vec3_t out);
void RotateSample (vec3_t in, vec3_t out);

extern	qboolean	extrasamples;

extern	float		minlights[MAX_MAP_FACES];
