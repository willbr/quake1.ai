/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_alias.c: routines for setting up to draw alias models

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"	// FIXME: shouldn't be needed (is needed for patch
						// right now, but that should move)
#include "r_drawflat.h"
#include "iqm.h"
#include "iqm_dynamics.h"

#define LIGHT_MIN	5		// lowest light value we'll allow, to avoid the
							//  need for inner-loop light clamping

mtriangle_t		*ptriangles;
affinetridesc_t	r_affinetridesc;

void *			acolormap;	// FIXME: should go away

trivertx_t		*r_apverts;
trivertx_t		*r_apverts_prev;
float			r_framelerp;

// TODO: these probably will go away with optimized rasterization
mdl_t				*pmdl;
vec3_t				r_plightvec;
int					r_ambientlight;
float				r_shadelight;
aliashdr_t			*paliashdr;
finalvert_t			*pfinalverts;
auxvert_t			*pauxverts;
static float		ziscale;
static model_t		*pmodel;

static vec3_t		alias_forward, alias_right, alias_up;

static maliasskindesc_t	*pskindesc;

int				r_amodels_drawn;
int				a_skinwidth;
int				r_anumverts;

float	aliastransform[3][4];

static inline void R_LerpVert (trivertx_t *cur, trivertx_t *prev, vec3_t out)
{
	if (r_framelerp >= 1.0f)
	{
		out[0] = cur->v[0];
		out[1] = cur->v[1];
		out[2] = cur->v[2];
	}
	else
	{
		float inv = 1.0f - r_framelerp;
		out[0] = cur->v[0] * r_framelerp + prev->v[0] * inv;
		out[1] = cur->v[1] * r_framelerp + prev->v[1] * inv;
		out[2] = cur->v[2] * r_framelerp + prev->v[2] * inv;
	}
}

typedef struct {
	int	index0;
	int	index1;
} aedge_t;

static aedge_t	aedges[12] = {
{0, 1}, {1, 2}, {2, 3}, {3, 0},
{4, 5}, {5, 6}, {6, 7}, {7, 4},
{0, 5}, {1, 4}, {2, 7}, {3, 6}
};

#define NUMVERTEXNORMALS	162

float	r_avertexnormals[NUMVERTEXNORMALS][3] = {
#include "anorms.h"
};

void R_AliasTransformAndProjectFinalVerts (finalvert_t *fv,
	stvert_t *pstverts);
void R_AliasSetUpTransform (int trivial_accept);
void R_AliasTransformVector (vec3_t in, vec3_t out);
void R_AliasTransformFinalVert (finalvert_t *fv, auxvert_t *av,
	trivertx_t *pverts, trivertx_t *pverts_prev, stvert_t *pstverts);
void R_AliasProjectFinalVert (finalvert_t *fv, auxvert_t *av);


/*
================
R_AliasCheckBBox
================
*/
qboolean R_AliasCheckBBox (void)
{
	int					i, flags, frame, numv;
	aliashdr_t			*pahdr;
	float				zi, basepts[8][3], v0, v1, frac;
	finalvert_t			*pv0, *pv1, viewpts[16];
	auxvert_t			*pa0, *pa1, viewaux[16];
	qboolean			zclipped, zfullyclipped;
	unsigned			anyclip, allclip;
	int					minz;
	
// expand, rotate, and translate points into worldspace

	currententity->trivial_accept = 0;
	pmodel = currententity->model;
	pahdr = Mod_Extradata (pmodel);
	pmdl = (mdl_t *)((byte *)pahdr + pahdr->model);

	R_AliasSetUpTransform (0);

// construct the base bounding box, unioned across current and previous frames
	frame = currententity->frame;
// TODO: don't repeat this check when drawing?
	if ((frame >= pmdl->numframes) || (frame < 0))
	{
		Con_DPrintf ("No such frame %d %s\n", frame,
				pmodel->name);
		frame = 0;
	}

	{
		int prev_frame = currententity->prev_frame;
		maliasframedesc_t *pf_cur, *pf_prev;
		float bbmin[3], bbmax[3];
		int k;

		if ((prev_frame >= pmdl->numframes) || (prev_frame < 0))
			prev_frame = frame;

		pf_cur  = &pahdr->frames[frame];
		pf_prev = &pahdr->frames[prev_frame];

		for (k = 0; k < 3; k++)
		{
			float a_min = (float)pf_cur->bboxmin.v[k];
			float a_max = (float)pf_cur->bboxmax.v[k];
			float b_min = (float)pf_prev->bboxmin.v[k];
			float b_max = (float)pf_prev->bboxmax.v[k];
			bbmin[k] = (a_min < b_min) ? a_min : b_min;
			bbmax[k] = (a_max > b_max) ? a_max : b_max;
		}

		// x worldspace coordinates
		basepts[0][0] = basepts[1][0] = basepts[2][0] = basepts[3][0] = bbmin[0];
		basepts[4][0] = basepts[5][0] = basepts[6][0] = basepts[7][0] = bbmax[0];

		// y worldspace coordinates
		basepts[0][1] = basepts[3][1] = basepts[5][1] = basepts[6][1] = bbmin[1];
		basepts[1][1] = basepts[2][1] = basepts[4][1] = basepts[7][1] = bbmax[1];

		// z worldspace coordinates
		basepts[0][2] = basepts[1][2] = basepts[4][2] = basepts[5][2] = bbmin[2];
		basepts[2][2] = basepts[3][2] = basepts[6][2] = basepts[7][2] = bbmax[2];
	}

	zclipped = false;
	zfullyclipped = true;

	minz = 9999;
	for (i=0; i<8 ; i++)
	{
		R_AliasTransformVector  (&basepts[i][0], &viewaux[i].fv[0]);

		if (viewaux[i].fv[2] < ALIAS_Z_CLIP_PLANE)
		{
		// we must clip points that are closer than the near clip plane
			viewpts[i].flags = ALIAS_Z_CLIP;
			zclipped = true;
		}
		else
		{
			if (viewaux[i].fv[2] < minz)
				minz = viewaux[i].fv[2];
			viewpts[i].flags = 0;
			zfullyclipped = false;
		}
	}

	
	if (zfullyclipped)
	{
		return false;	// everything was near-z-clipped
	}

	numv = 8;

	if (zclipped)
	{
	// organize points by edges, use edges to get new points (possible trivial
	// reject)
		for (i=0 ; i<12 ; i++)
		{
		// edge endpoints
			pv0 = &viewpts[aedges[i].index0];
			pv1 = &viewpts[aedges[i].index1];
			pa0 = &viewaux[aedges[i].index0];
			pa1 = &viewaux[aedges[i].index1];

		// if one end is clipped and the other isn't, make a new point
			if (pv0->flags ^ pv1->flags)
			{
				frac = (ALIAS_Z_CLIP_PLANE - pa0->fv[2]) /
					   (pa1->fv[2] - pa0->fv[2]);
				viewaux[numv].fv[0] = pa0->fv[0] +
						(pa1->fv[0] - pa0->fv[0]) * frac;
				viewaux[numv].fv[1] = pa0->fv[1] +
						(pa1->fv[1] - pa0->fv[1]) * frac;
				viewaux[numv].fv[2] = ALIAS_Z_CLIP_PLANE;
				viewpts[numv].flags = 0;
				numv++;
			}
		}
	}

// project the vertices that remain after clipping
	anyclip = 0;
	allclip = ALIAS_XY_CLIP_MASK;

// TODO: probably should do this loop in ASM, especially if we use floats
	for (i=0 ; i<numv ; i++)
	{
	// we don't need to bother with vertices that were z-clipped
		if (viewpts[i].flags & ALIAS_Z_CLIP)
			continue;

		zi = 1.0 / viewaux[i].fv[2];

	// FIXME: do with chop mode in ASM, or convert to float
		v0 = (viewaux[i].fv[0] * xscale * zi) + xcenter;
		v1 = (viewaux[i].fv[1] * yscale * zi) + ycenter;

		flags = 0;

		if (v0 < r_refdef.fvrectx)
			flags |= ALIAS_LEFT_CLIP;
		if (v1 < r_refdef.fvrecty)
			flags |= ALIAS_TOP_CLIP;
		if (v0 > r_refdef.fvrectright)
			flags |= ALIAS_RIGHT_CLIP;
		if (v1 > r_refdef.fvrectbottom)
			flags |= ALIAS_BOTTOM_CLIP;

		anyclip |= flags;
		allclip &= flags;
	}

	if (allclip)
		return false;	// trivial reject off one side

	currententity->trivial_accept = !anyclip & !zclipped;

	if (currententity->trivial_accept)
	{
		if (minz > (r_aliastransition + (pmdl->size * r_resfudge)))
		{
			currententity->trivial_accept |= 2;
		}
	}

	return true;
}


/*
================
R_AliasTransformVector
================
*/
void R_AliasTransformVector (vec3_t in, vec3_t out)
{
	out[0] = DotProduct(in, aliastransform[0]) + aliastransform[0][3];
	out[1] = DotProduct(in, aliastransform[1]) + aliastransform[1][3];
	out[2] = DotProduct(in, aliastransform[2]) + aliastransform[2][3];
}


/*
================
R_AliasPreparePoints

General clipped case
================
*/
void R_AliasPreparePoints (void)
{
	int			i;
	stvert_t	*pstverts;
	finalvert_t	*fv;
	auxvert_t	*av;
	mtriangle_t	*ptri;
	finalvert_t	*pfv[3];

	pstverts = (stvert_t *)((byte *)paliashdr + paliashdr->stverts);
	r_anumverts = pmdl->numverts;
 	fv = pfinalverts;
	av = pauxverts;

	for (i=0 ; i<r_anumverts ; i++, fv++, av++, r_apverts++, r_apverts_prev++, pstverts++)
	{
		R_AliasTransformFinalVert (fv, av, r_apverts, r_apverts_prev, pstverts);
		if (av->fv[2] < ALIAS_Z_CLIP_PLANE)
			fv->flags |= ALIAS_Z_CLIP;
		else
		{
			 R_AliasProjectFinalVert (fv, av);

			if (fv->v[0] < r_refdef.aliasvrect.x)
				fv->flags |= ALIAS_LEFT_CLIP;
			if (fv->v[1] < r_refdef.aliasvrect.y)
				fv->flags |= ALIAS_TOP_CLIP;
			if (fv->v[0] > r_refdef.aliasvrectright)
				fv->flags |= ALIAS_RIGHT_CLIP;
			if (fv->v[1] > r_refdef.aliasvrectbottom)
				fv->flags |= ALIAS_BOTTOM_CLIP;	
		}
	}

//
// clip and draw all triangles
//
	r_affinetridesc.numtriangles = 1;

	ptri = (mtriangle_t *)((byte *)paliashdr + paliashdr->triangles);
	for (i=0 ; i<pmdl->numtris ; i++, ptri++)
	{
		pfv[0] = &pfinalverts[ptri->vertindex[0]];
		pfv[1] = &pfinalverts[ptri->vertindex[1]];
		pfv[2] = &pfinalverts[ptri->vertindex[2]];

		if ( pfv[0]->flags & pfv[1]->flags & pfv[2]->flags & (ALIAS_XY_CLIP_MASK | ALIAS_Z_CLIP) )
			continue;		// completely clipped
		
		if ( ! ( (pfv[0]->flags | pfv[1]->flags | pfv[2]->flags) &
			(ALIAS_XY_CLIP_MASK | ALIAS_Z_CLIP) ) )
		{	// totally unclipped
			r_affinetridesc.pfinalverts = pfinalverts;
			r_affinetridesc.ptriangles = ptri;
			D_PolysetDraw ();
		}
		else		
		{	// partially clipped
			R_AliasClipTriangle (ptri);
		}
	}
}


/*
================
R_AliasSetUpTransform
================
*/
void R_AliasSetUpTransform (int trivial_accept)
{
	int				i;
	float			rotationmatrix[3][4], t2matrix[3][4];
	static float	tmatrix[3][4];
	static float	viewmatrix[3][4];
	vec3_t			angles;

// TODO: should really be stored with the entity instead of being reconstructed
// TODO: should use a look-up table
// TODO: could cache lazily, stored in the entity

	angles[ROLL] = currententity->angles[ROLL];
	angles[PITCH] = -currententity->angles[PITCH];
	angles[YAW] = currententity->angles[YAW];
	AngleVectors (angles, alias_forward, alias_right, alias_up);

	tmatrix[0][0] = pmdl->scale[0];
	tmatrix[1][1] = pmdl->scale[1];
	tmatrix[2][2] = pmdl->scale[2];

	tmatrix[0][3] = pmdl->scale_origin[0];
	tmatrix[1][3] = pmdl->scale_origin[1];
	tmatrix[2][3] = pmdl->scale_origin[2];

// TODO: can do this with simple matrix rearrangement

	for (i=0 ; i<3 ; i++)
	{
		t2matrix[i][0] = alias_forward[i];
		t2matrix[i][1] = -alias_right[i];
		t2matrix[i][2] = alias_up[i];
	}

	t2matrix[0][3] = -modelorg[0];
	t2matrix[1][3] = -modelorg[1];
	t2matrix[2][3] = -modelorg[2];

// FIXME: can do more efficiently than full concatenation
	R_ConcatTransforms (t2matrix, tmatrix, rotationmatrix);

// TODO: should be global, set when vright, etc., set
	VectorCopy (vright, viewmatrix[0]);
	VectorCopy (vup, viewmatrix[1]);
	VectorInverse (viewmatrix[1]);
	VectorCopy (vpn, viewmatrix[2]);

//	viewmatrix[0][3] = 0;
//	viewmatrix[1][3] = 0;
//	viewmatrix[2][3] = 0;

	R_ConcatTransforms (viewmatrix, rotationmatrix, aliastransform);

// do the scaling up of x and y to screen coordinates as part of the transform
// for the unclipped case (it would mess up clipping in the clipped case).
// Also scale down z, so 1/z is scaled 31 bits for free, and scale down x and y
// correspondingly so the projected x and y come out right
// FIXME: make this work for clipped case too?
	if (trivial_accept)
	{
		for (i=0 ; i<4 ; i++)
		{
			aliastransform[0][i] *= aliasxscale *
					(1.0 / ((float)0x8000 * 0x10000));
			aliastransform[1][i] *= aliasyscale *
					(1.0 / ((float)0x8000 * 0x10000));
			aliastransform[2][i] *= 1.0 / ((float)0x8000 * 0x10000);

		}
	}
}


/*
================
R_AliasTransformFinalVert
================
*/
void R_AliasTransformFinalVert (finalvert_t *fv, auxvert_t *av,
	trivertx_t *pverts, trivertx_t *pverts_prev, stvert_t *pstverts)
{
	int		temp;
	float	lightcos, *plightnormal;
	vec3_t	v;

	R_LerpVert (pverts, pverts_prev, v);

	av->fv[0] = DotProduct(v, aliastransform[0]) +
			aliastransform[0][3];
	av->fv[1] = DotProduct(v, aliastransform[1]) +
			aliastransform[1][3];
	av->fv[2] = DotProduct(v, aliastransform[2]) +
			aliastransform[2][3];

	fv->v[2] = pstverts->s;
	fv->v[3] = pstverts->t;

	fv->flags = pstverts->onseam;

// lighting (use current frame's normal index — not lerped, see spec)
	plightnormal = r_avertexnormals[pverts->lightnormalindex];
	lightcos = DotProduct (plightnormal, r_plightvec);
	temp = r_ambientlight;

	if (lightcos < 0)
	{
		temp += (int)(r_shadelight * lightcos);

	// clamp; because we limited the minimum ambient and shading light, we
	// don't have to clamp low light, just bright
		if (temp < 0)
			temp = 0;
	}

	fv->v[4] = temp;
}


#if	!id386

/*
================
R_AliasTransformAndProjectFinalVerts
================
*/
void R_AliasTransformAndProjectFinalVerts (finalvert_t *fv, stvert_t *pstverts)
{
	int			i, temp;
	float		lightcos, *plightnormal, zi;
	trivertx_t	*pverts;
	trivertx_t	*pverts_prev;
	vec3_t		v;

	pverts = r_apverts;
	pverts_prev = r_apverts_prev;

	for (i=0 ; i<r_anumverts ; i++, fv++, pverts++, pverts_prev++, pstverts++)
	{
		R_LerpVert (pverts, pverts_prev, v);

	// transform and project
		zi = 1.0 / (DotProduct(v, aliastransform[2]) +
				aliastransform[2][3]);

	// x, y, and z are scaled down by 1/2**31 in the transform, so 1/z is
	// scaled up by 1/2**31, and the scaling cancels out for x and y in the
	// projection
		fv->v[5] = zi;

		fv->v[0] = ((DotProduct(v, aliastransform[0]) +
				aliastransform[0][3]) * zi) + aliasxcenter;
		fv->v[1] = ((DotProduct(v, aliastransform[1]) +
				aliastransform[1][3]) * zi) + aliasycenter;

		fv->v[2] = pstverts->s;
		fv->v[3] = pstverts->t;
		fv->flags = pstverts->onseam;

	// lighting
		plightnormal = r_avertexnormals[pverts->lightnormalindex];
		lightcos = DotProduct (plightnormal, r_plightvec);
		temp = r_ambientlight;

		if (lightcos < 0)
		{
			temp += (int)(r_shadelight * lightcos);

		// clamp; because we limited the minimum ambient and shading light, we
		// don't have to clamp low light, just bright
			if (temp < 0)
				temp = 0;
		}

		fv->v[4] = temp;
	}
}

#endif


/*
================
R_AliasProjectFinalVert
================
*/
void R_AliasProjectFinalVert (finalvert_t *fv, auxvert_t *av)
{
	float	zi;

// project points
	zi = 1.0 / av->fv[2];

	fv->v[5] = zi * ziscale;

	fv->v[0] = (av->fv[0] * aliasxscale * zi) + aliasxcenter;
	fv->v[1] = (av->fv[1] * aliasyscale * zi) + aliasycenter;
}


/*
================
R_AliasPrepareUnclippedPoints
================
*/
void R_AliasPrepareUnclippedPoints (void)
{
	stvert_t	*pstverts;
	finalvert_t	*fv;

	pstverts = (stvert_t *)((byte *)paliashdr + paliashdr->stverts);
	r_anumverts = pmdl->numverts;
// FIXME: just use pfinalverts directly?
	fv = pfinalverts;

	R_AliasTransformAndProjectFinalVerts (fv, pstverts);

	if (r_affinetridesc.drawtype)
		D_PolysetDrawFinalVerts (fv, r_anumverts);

	r_affinetridesc.pfinalverts = pfinalverts;
	r_affinetridesc.ptriangles = (mtriangle_t *)
			((byte *)paliashdr + paliashdr->triangles);
	r_affinetridesc.numtriangles = pmdl->numtris;

	D_PolysetDraw ();
}

/*
===============
R_AliasSetupSkin
===============
*/
void R_AliasSetupSkin (void)
{
	int					skinnum;
	int					i, numskins;
	maliasskingroup_t	*paliasskingroup;
	float				*pskinintervals, fullskininterval;
	float				skintargettime, skintime;

	skinnum = currententity->skinnum;
	if ((skinnum >= pmdl->numskins) || (skinnum < 0))
	{
		Con_DPrintf ("R_AliasSetupSkin: no such skin # %d\n", skinnum);
		skinnum = 0;
	}

	pskindesc = ((maliasskindesc_t *)
			((byte *)paliashdr + paliashdr->skindesc)) + skinnum;
	a_skinwidth = pmdl->skinwidth;

	if (pskindesc->type == ALIAS_SKIN_GROUP)
	{
		paliasskingroup = (maliasskingroup_t *)((byte *)paliashdr +
				pskindesc->skin);
		pskinintervals = (float *)
				((byte *)paliashdr + paliasskingroup->intervals);
		numskins = paliasskingroup->numskins;
		fullskininterval = pskinintervals[numskins-1];
	
		skintime = cl.time + currententity->syncbase;
	
	// when loading in Mod_LoadAliasSkinGroup, we guaranteed all interval
	// values are positive, so we don't have to worry about division by 0
		skintargettime = skintime -
				((int)(skintime / fullskininterval)) * fullskininterval;
	
		for (i=0 ; i<(numskins-1) ; i++)
		{
			if (pskinintervals[i] > skintargettime)
				break;
		}
	
		pskindesc = &paliasskingroup->skindescs[i];
	}

	r_affinetridesc.pskindesc = pskindesc;
	r_affinetridesc.pskin = (void *)((byte *)paliashdr + pskindesc->skin);
	r_affinetridesc.skinwidth = a_skinwidth;
	r_affinetridesc.seamfixupX16 =  (a_skinwidth >> 1) << 16;
	r_affinetridesc.skinheight = pmdl->skinheight;
}

/*
================
R_AliasSetupLighting
================
*/
void R_AliasSetupLighting (alight_t *plighting)
{

// guarantee that no vertex will ever be lit below LIGHT_MIN, so we don't have
// to clamp off the bottom
	r_ambientlight = plighting->ambientlight;

	if (r_ambientlight < LIGHT_MIN)
		r_ambientlight = LIGHT_MIN;

	r_ambientlight = (255 - r_ambientlight) << VID_CBITS;

	if (r_ambientlight < LIGHT_MIN)
		r_ambientlight = LIGHT_MIN;

	r_shadelight = plighting->shadelight;

	if (r_shadelight < 0)
		r_shadelight = 0;

	r_shadelight *= VID_GRADES;

// fullbright: draw alias models (monsters, items, and the view-model gun, which
// all funnel through here) at the brightest flat shade, mirroring the world
// surfaces' r_fullbright path. LIGHT_MIN is the post-transform brightest end of
// the ambient range; zeroing shadelight removes the per-vertex normal falloff.
	if (r_fullbright.value)
	{
		r_ambientlight = LIGHT_MIN;
		r_shadelight = 0;
	}

// rotate the lighting vector into the model's frame of reference
	r_plightvec[0] = DotProduct (plighting->plightvec, alias_forward);
	r_plightvec[1] = -DotProduct (plighting->plightvec, alias_right);
	r_plightvec[2] = DotProduct (plighting->plightvec, alias_up);
}

/*
=================
R_AliasSetupFrame

set r_apverts
=================
*/
void R_AliasSetupFrame (void)
{
	int				frame, prev_frame;
	int				i, prev_i, numframes;
	maliasgroup_t	*paliasgroup;
	float			*pintervals, fullinterval, targettime, time;

	frame = currententity->frame;
	if ((frame >= pmdl->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_AliasSetupFrame: no such frame %d\n", frame);
		frame = 0;
	}

	prev_frame = currententity->prev_frame;
	if ((prev_frame >= pmdl->numframes) || (prev_frame < 0))
		prev_frame = frame;

	if (!r_lerpmodels.value || cl_nolerp.value || frame == prev_frame)
	{
		r_framelerp = 1.0f;
	}
	else
	{
		r_framelerp = (cl.time - currententity->frame_start_time) / 0.1f;
		if (r_framelerp > 1.0f) r_framelerp = 1.0f;
		if (r_framelerp < 0.0f) r_framelerp = 0.0f;
	}

	if (paliashdr->frames[frame].type == ALIAS_SINGLE)
	{
		r_apverts = (trivertx_t *)
				((byte *)paliashdr + paliashdr->frames[frame].frame);

		if (paliashdr->frames[prev_frame].type == ALIAS_SINGLE)
		{
			r_apverts_prev = (trivertx_t *)
					((byte *)paliashdr + paliashdr->frames[prev_frame].frame);
		}
		else
		{
			// mixed single/group transition — don't lerp
			r_apverts_prev = r_apverts;
			r_framelerp = 1.0f;
		}
		return;
	}

	// ALIAS_GROUP path
	paliasgroup = (maliasgroup_t *)
				((byte *)paliashdr + paliashdr->frames[frame].frame);
	pintervals = (float *)((byte *)paliashdr + paliasgroup->intervals);
	numframes = paliasgroup->numframes;
	fullinterval = pintervals[numframes-1];

	time = cl.time + currententity->syncbase;

	// when loading in Mod_LoadAliasGroup, we guaranteed all interval values
	// are positive, so we don't have to worry about division by 0
	targettime = time - ((int)(time / fullinterval)) * fullinterval;

	for (i=0 ; i<(numframes-1) ; i++)
	{
		if (pintervals[i] > targettime)
			break;
	}

	r_apverts = (trivertx_t *)
				((byte *)paliashdr + paliasgroup->frames[i].frame);

	prev_i = (i == 0) ? (numframes - 1) : (i - 1);
	r_apverts_prev = (trivertx_t *)
				((byte *)paliashdr + paliasgroup->frames[prev_i].frame);

	if (r_lerpmodels.value && !cl_nolerp.value)
	{
		float t_lo = (i == 0) ? 0.0f : pintervals[i-1];
		float t_hi = pintervals[i];
		float span = t_hi - t_lo;
		if (span > 0.0f)
		{
			r_framelerp = (targettime - t_lo) / span;
			if (r_framelerp > 1.0f) r_framelerp = 1.0f;
			if (r_framelerp < 0.0f) r_framelerp = 0.0f;
		}
		else
		{
			r_framelerp = 1.0f;
		}
	}
	else
	{
		r_framelerp = 1.0f;
	}
}


/*
================
R_AliasDrawModel
================
*/
void R_AliasDrawModel (alight_t *plighting)
{
	finalvert_t		finalverts[MAXALIASVERTS +
						((CACHE_SIZE - 1) / sizeof(finalvert_t)) + 1];
	auxvert_t		auxverts[MAXALIASVERTS];

	r_amodels_drawn++;

// cache align
	pfinalverts = (finalvert_t *)
			(((size_t)&finalverts[0] + CACHE_SIZE - 1) & ~((size_t)(CACHE_SIZE - 1)));
	pauxverts = &auxverts[0];

	paliashdr = (aliashdr_t *)Mod_Extradata (currententity->model);
	pmdl = (mdl_t *)((byte *)paliashdr + paliashdr->model);

	R_AliasSetupSkin ();
	R_AliasSetUpTransform (currententity->trivial_accept);
	R_AliasSetupLighting (plighting);
	R_AliasSetupFrame ();

	// r_drawflat 2: replace the skin with a category-coloured uniform
	// buffer. acolormap shading still applies, so each model reads as a
	// lit flat colour (item/monster/etc).
	if (r_drawflat.value == 2)
	{
		int cat = R_DrawFlat_AliasCategory (currententity);
		r_affinetridesc.pskin = r_drawflat_src[cat];
		// skinwidth/skinheight untouched — uniform source means any (s,t)
		// sample returns the same byte.
	}

	if (!currententity->colormap)
		Sys_Error ("R_AliasDrawModel: !currententity->colormap");

	r_affinetridesc.drawtype = (currententity->trivial_accept == 3) &&
			r_recursiveaffinetriangles;

	if (r_affinetridesc.drawtype)
	{
		D_PolysetUpdateTables ();		// FIXME: precalc...
	}
	else
	{
#if	id386
		D_Aff8Patch (currententity->colormap);
#endif
	}

	acolormap = currententity->colormap;

	if (currententity != &cl.viewent)
		ziscale = (float)0x8000 * (float)0x10000;
	else
		ziscale = (float)0x8000 * (float)0x10000 * 3.0;

	if (currententity->trivial_accept)
		R_AliasPrepareUnclippedPoints ();
	else
		R_AliasPreparePoints ();
}


// R3 procedural-face cvars (head look-at, eye gaze, breathing). Registered from R_Init.
cvar_t	actor_lookat       = {"actor_lookat",       "1"};
cvar_t	actor_gaze_yaw     = {"actor_gaze_yaw",     "50"};   // head yaw clamp (deg)
cvar_t	actor_gaze_pitch   = {"actor_gaze_pitch",   "30"};   // head pitch clamp (deg)
cvar_t	actor_eye_yaw      = {"actor_eye_yaw",      "25"};   // eye yaw clamp (deg)
cvar_t	actor_eye_pitch    = {"actor_eye_pitch",    "20"};   // eye pitch clamp (deg)
cvar_t	actor_breathe_rate = {"actor_breathe_rate", "0.25"}; // breaths/sec
cvar_t	actor_breathe_amp  = {"actor_breathe_amp",  "0.04"}; // chest scale +/- fraction
cvar_t	actor_clip         = {"actor_clip",         "0"};    // selected anim clip index (multi-clip)

void R_IQMInitCvars (void)
{
	Cvar_RegisterVariable (&actor_lookat);
	Cvar_RegisterVariable (&actor_gaze_yaw);
	Cvar_RegisterVariable (&actor_gaze_pitch);
	Cvar_RegisterVariable (&actor_eye_yaw);
	Cvar_RegisterVariable (&actor_eye_pitch);
	Cvar_RegisterVariable (&actor_breathe_rate);
	Cvar_RegisterVariable (&actor_breathe_amp);
	Cvar_RegisterVariable (&actor_clip);
	IQM_DynamicsInitCvars ();
}

#define MAX_IQM_JOINTS 128

// quaternion (x,y,z,w) -> 3x4 rotation (column-vector convention: out = M*p)
static void IQM_QuatMat (const float q[4], float m[3][4])
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float n = x*x + y*y + z*z + w*w;
	float s = (n > 0.0f) ? 2.0f / n : 0.0f;
	float xs = x*s, ys = y*s, zs = z*s;
	float wx = w*xs, wy = w*ys, wz = w*zs;
	float xx = x*xs, xy = x*ys, xz = x*zs;
	float yy = y*ys, yz = y*zs, zz = z*zs;
	m[0][0] = 1.0f-(yy+zz); m[0][1] = xy-wz;       m[0][2] = xz+wy;       m[0][3] = 0;
	m[1][0] = xy+wz;        m[1][1] = 1.0f-(xx+zz);m[1][2] = yz-wx;       m[1][3] = 0;
	m[2][0] = xz-wy;        m[2][1] = yz+wx;       m[2][2] = 1.0f-(xx+yy);m[2][3] = 0;
}

// local 3x4 from TRS[10] (0..2 translate, 3..6 quat xyzw, 7..9 scale)
static void IQM_LocalMat (const float trs[10], float m[3][4])
{
	float r[3][4];
	IQM_QuatMat (&trs[3], r);
	m[0][0]=r[0][0]*trs[7]; m[0][1]=r[0][1]*trs[8]; m[0][2]=r[0][2]*trs[9]; m[0][3]=trs[0];
	m[1][0]=r[1][0]*trs[7]; m[1][1]=r[1][1]*trs[8]; m[1][2]=r[1][2]*trs[9]; m[1][3]=trs[1];
	m[2][0]=r[2][0]*trs[7]; m[2][1]=r[2][1]*trs[8]; m[2][2]=r[2][2]*trs[9]; m[2][3]=trs[2];
}

// inverse of a rotation(+unit-scale)+translation 3x4 (transpose rotation; exact
// for R2's unit-scale bind. Non-uniform scale (breathing) is revisited in R3.)
static void IQM_Invert34 (const float in[3][4], float out[3][4])
{
	out[0][0]=in[0][0]; out[0][1]=in[1][0]; out[0][2]=in[2][0];
	out[1][0]=in[0][1]; out[1][1]=in[1][1]; out[1][2]=in[2][1];
	out[2][0]=in[0][2]; out[2][1]=in[1][2]; out[2][2]=in[2][2];
	out[0][3]=-(out[0][0]*in[0][3] + out[0][1]*in[1][3] + out[0][2]*in[2][3]);
	out[1][3]=-(out[1][0]*in[0][3] + out[1][1]*in[1][3] + out[1][2]*in[2][3]);
	out[2][3]=-(out[2][0]*in[0][3] + out[2][1]*in[1][3] + out[2][2]*in[2][3]);
}

// Build a local 3x4 that aims the joint's +X axis at tgt_actor (actor space),
// with yaw/pitch clamped (degrees). Keeps the joint's base translation (trs[0..2]);
// replaces its rotation. parent = curwld[pp] (rotation assumed unit-scale, like
// IQM_Invert34's caveat). pp < 0 means root.
static void R_IQMLookAtLocal (const float *trs, int pp,
		float curwld[][3][4], const vec3_t tgt_actor,
		float maxyaw_deg, float maxpitch_deg, float local[3][4])
{
	vec3_t	org, dir, dirp;
	float	yaw, pitch, hyp, c1, s1, c2, s2, maxyaw, maxpitch;

	if (pp >= 0)
	{
		org[0] = DotProduct (trs, curwld[pp][0]) + curwld[pp][0][3];
		org[1] = DotProduct (trs, curwld[pp][1]) + curwld[pp][1][3];
		org[2] = DotProduct (trs, curwld[pp][2]) + curwld[pp][2][3];
	}
	else
	{
		org[0] = trs[0]; org[1] = trs[1]; org[2] = trs[2];
	}

	VectorSubtract (tgt_actor, org, dir);
	if (pp >= 0)
	{	// into parent space: dirp = Rot(curwld[pp])^T * dir
		dirp[0] = dir[0]*curwld[pp][0][0] + dir[1]*curwld[pp][1][0] + dir[2]*curwld[pp][2][0];
		dirp[1] = dir[0]*curwld[pp][0][1] + dir[1]*curwld[pp][1][1] + dir[2]*curwld[pp][2][1];
		dirp[2] = dir[0]*curwld[pp][0][2] + dir[1]*curwld[pp][1][2] + dir[2]*curwld[pp][2][2];
	}
	else
	{
		VectorCopy (dir, dirp);
	}

	yaw   = atan2 (dirp[1], dirp[0]);
	hyp   = sqrt (dirp[0]*dirp[0] + dirp[1]*dirp[1]);
	pitch = atan2 (dirp[2], hyp);

	maxyaw   = maxyaw_deg   * (M_PI / 180.0);
	maxpitch = maxpitch_deg * (M_PI / 180.0);
	if (yaw   >  maxyaw)   yaw   =  maxyaw;
	if (yaw   < -maxyaw)   yaw   = -maxyaw;
	if (pitch >  maxpitch) pitch =  maxpitch;
	if (pitch < -maxpitch) pitch = -maxpitch;

	c1 = cos (yaw);   s1 = sin (yaw);
	c2 = cos (pitch); s2 = sin (pitch);
	// M = Rz(yaw) * Ry(-pitch); maps +X -> (c1*c2, s1*c2, s2)
	local[0][0] = c1*c2; local[0][1] = -s1; local[0][2] = -c1*s2; local[0][3] = trs[0];
	local[1][0] = s1*c2; local[1][1] =  c1; local[1][2] = -s1*s2; local[1][3] = trs[1];
	local[2][0] = s2;    local[2][1] =   0; local[2][2] =    c2;  local[2][3] = trs[2];
}

static int R_IQMJointIsEye (lm_iqm_t *iqm, int jj)
{
	int e;
	for (e = 0; e < iqm->num_eye; e++)
		if (iqm->eye_joint[e] == jj)
			return 1;
	return 0;
}

/*
================
R_IQMSetUpTransform

Like R_AliasSetUpTransform, but identity model scale: IQM verts are float
model-units already in actor space at bind pose.
================
*/
static void R_IQMSetUpTransform (void)
{
	int				i;
	float			rotationmatrix[3][4];
	static float	viewmatrix[3][4];
	vec3_t			angles;

	angles[ROLL]  = currententity->angles[ROLL];
	angles[PITCH] = -currententity->angles[PITCH];
	angles[YAW]   = currententity->angles[YAW];
	AngleVectors (angles, alias_forward, alias_right, alias_up);

	for (i = 0; i < 3; i++)
	{
		rotationmatrix[i][0] = alias_forward[i];
		rotationmatrix[i][1] = -alias_right[i];
		rotationmatrix[i][2] = alias_up[i];
	}
	rotationmatrix[0][3] = -modelorg[0];
	rotationmatrix[1][3] = -modelorg[1];
	rotationmatrix[2][3] = -modelorg[2];

	VectorCopy (vright, viewmatrix[0]);
	VectorCopy (vup,    viewmatrix[1]);
	VectorInverse (viewmatrix[1]);
	VectorCopy (vpn,    viewmatrix[2]);
	// viewmatrix is static => [*][3] stay zero-initialised

	R_ConcatTransforms (viewmatrix, rotationmatrix, aliastransform);
}

/*
================
R_IQMDrawModel

Bind-pose rigid render: transform each actor-space vertex to view space,
project, and rasterize each mesh's triangles with a flat per-mesh skin colour
through the shared D_PolysetDraw rasterizer. R1 skips any triangle touching the
near plane or screen edge (no clipping) and uses solid-colour synth skins.
================
*/
void R_IQMDrawModel (alight_t *plighting)
{
	finalvert_t			finalverts[MAXALIASVERTS +
							((CACHE_SIZE - 1) / sizeof(finalvert_t)) + 1];
	finalvert_t			*fvbase;
	lm_iqm_t			*iqm;
	int					i, mi, t, light;
	static const byte	meshcolor[8] = { 0x6d, 0x52, 0x0b, 0xf3, 0x40, 0x20, 0xfe, 0x10 };
	static byte			meshskin[16];
	static float		skin[MAX_IQM_JOINTS][3][4];
	static float		bindinv[MAX_IQM_JOINTS][3][4];
	static float		bindwld[MAX_IQM_JOINTS][3][4];
	static float		curwld[MAX_IQM_JOINTS][3][4];
	float				local[3][4], trs[10];
	int					animated, jj, ff, pp, cc;
	qboolean			lookat;
	vec3_t				tgt_actor, delta;

	r_amodels_drawn++;

	iqm = currententity->model->iqmdata;
	if (!iqm)
		return;
	if (iqm->numverts > MAXALIASVERTS)
		return;

	fvbase = (finalvert_t *)
		(((size_t)&finalverts[0] + CACHE_SIZE - 1) & ~((size_t)(CACHE_SIZE - 1)));

	R_IQMSetUpTransform ();
	R_AliasSetupLighting (plighting);	// sets r_ambientlight / r_shadelight / r_plightvec
	light = r_ambientlight;				// R1: flat-lit
	ziscale = (float)0x8000 * (float)0x10000;

	acolormap = currententity->colormap;
	if (!acolormap)
		acolormap = vid.colormap;

	// Animation (R2): build a per-joint skin matrix for this frame. At bind pose
	// skin == identity, so static models (frametrs == NULL) skip this entirely.
	animated = (iqm->frametrs != 0 && iqm->numjoints <= MAX_IQM_JOINTS);
	if (animated)
	{
		for (jj = 0; jj < iqm->numjoints; jj++)
		{
			trs[0] = iqm->joints[jj].translate[0];
			trs[1] = iqm->joints[jj].translate[1];
			trs[2] = iqm->joints[jj].translate[2];
			trs[3] = iqm->joints[jj].rotate[0];
			trs[4] = iqm->joints[jj].rotate[1];
			trs[5] = iqm->joints[jj].rotate[2];
			trs[6] = iqm->joints[jj].rotate[3];
			trs[7] = iqm->joints[jj].scale[0];
			trs[8] = iqm->joints[jj].scale[1];
			trs[9] = iqm->joints[jj].scale[2];
			IQM_LocalMat (trs, local);
			pp = iqm->joints[jj].parent;
			if (pp >= 0)
				R_ConcatTransforms (bindwld[pp], local, bindwld[jj]);
			else
				memcpy (bindwld[jj], local, sizeof(local));
			IQM_Invert34 (bindwld[jj], bindinv[jj]);
		}

		// R3: player position (r_origin) expressed in actor space, for look-at.
		// world->actor = Rot(entity)^T * (world - entity_origin); the entity
		// rotation columns are (alias_forward, -alias_right, alias_up).
		lookat = (actor_lookat.value != 0);
		VectorSubtract (r_origin, currententity->origin, delta);
		tgt_actor[0] =  DotProduct (delta, alias_forward);
		tgt_actor[1] = -DotProduct (delta, alias_right);
		tgt_actor[2] =  DotProduct (delta, alias_up);

		// Pick the playing frame. Multi-clip: actor_clip selects a clip and we
		// loop within its frame range; otherwise loop the whole frame set (R2).
		if (iqm->numclips > 0)
		{
			int				ci2 = (int)actor_clip.value;
			lm_iqm_clip_t	*clp;
			if (ci2 < 0) ci2 = 0;
			if (ci2 >= iqm->numclips) ci2 = iqm->numclips - 1;
			clp = &iqm->clips[ci2];
			if (clp->num_frames > 0)
			{
				ff = (int)(cl.time * clp->framerate) % clp->num_frames;
				if (ff < 0) ff += clp->num_frames;
				ff += clp->first_frame;
			}
			else
				ff = 0;
		}
		else
		{
			ff = (int)(cl.time * iqm->framerate);
			ff %= iqm->numframes;
			if (ff < 0)
				ff += iqm->numframes;
		}

		for (jj = 0; jj < iqm->numjoints; jj++)
		{
			float *src = iqm->frametrs + ((size_t)ff * iqm->numjoints + jj) * 10;
			for (cc = 0; cc < 10; cc++)
				trs[cc] = src[cc];
			pp = iqm->joints[jj].parent;

			if (lookat && jj == iqm->head_joint)
				R_IQMLookAtLocal (trs, pp, curwld, tgt_actor,
					actor_gaze_yaw.value, actor_gaze_pitch.value, local);
			else if (lookat && R_IQMJointIsEye (iqm, jj))
				R_IQMLookAtLocal (trs, pp, curwld, tgt_actor,
					actor_eye_yaw.value, actor_eye_pitch.value, local);
			else
				IQM_LocalMat (trs, local);

			if (pp >= 0)
				R_ConcatTransforms (curwld[pp], local, curwld[jj]);
			else
				memcpy (curwld[jj], local, sizeof(local));
			R_ConcatTransforms (curwld[jj], bindinv[jj], skin[jj]);
		}

		// R3 breathing: scale ONLY the chest mesh about its posed origin, by
		// post-multiplying its skin matrix. Non-propagating (children use their
		// own skin matrices), matching the design's "uniform, non-propagating
		// mesh scale" decision. v' = org + s*(v - org) on the posed vertex.
		if (iqm->chest_joint >= 0 && iqm->chest_joint < iqm->numjoints &&
			actor_breathe_amp.value != 0.0f)
		{
			int		ci = iqm->chest_joint;
			float	s  = 1.0f + actor_breathe_amp.value *
						sin (cl.time * actor_breathe_rate.value * 2.0f * M_PI);
			float	org[3];
			int		rr;

			org[0] = curwld[ci][0][3];
			org[1] = curwld[ci][1][3];
			org[2] = curwld[ci][2][3];
			for (rr = 0; rr < 3; rr++)
			{
				skin[ci][rr][0] *= s;
				skin[ci][rr][1] *= s;
				skin[ci][rr][2] *= s;
				skin[ci][rr][3] = s * skin[ci][rr][3] + (1.0f - s) * org[rr];
			}
		}

		// R4: ponytail dynamics. Build the actor->world rotation (entity columns
		// alias_forward / -alias_right / alias_up) + origin, solve the chain, and
		// write the simulated (free) ponytail joints' skin matrices.
		if (iqm->num_pony >= 2)
		{
			float	a2w_rot[3][3];
			vec3_t	a2w_org;
			int		rr2;

			for (rr2 = 0; rr2 < 3; rr2++)
			{
				a2w_rot[rr2][0] =  alias_forward[rr2];
				a2w_rot[rr2][1] = -alias_right[rr2];
				a2w_rot[rr2][2] =  alias_up[rr2];
			}
			VectorCopy (currententity->origin, a2w_org);
			IQM_SolveDynamics (currententity, iqm, curwld, bindinv, skin,
				a2w_rot, a2w_org, cl.time);
		}
	}

	for (i = 0; i < iqm->numverts; i++)
	{
		finalvert_t	*fv = &fvbase[i];
		float		*p = iqm->verts[i].pos;
		float		posed[3], vx, vy, vz, zi;
		int			bone;

		if (animated)
		{
			bone = iqm->verts[i].bone;
			if (bone >= iqm->numjoints)
				bone = 0;
			posed[0] = DotProduct (p, skin[bone][0]) + skin[bone][0][3];
			posed[1] = DotProduct (p, skin[bone][1]) + skin[bone][1][3];
			posed[2] = DotProduct (p, skin[bone][2]) + skin[bone][2][3];
		}
		else
		{
			posed[0] = p[0]; posed[1] = p[1]; posed[2] = p[2];
		}

		vx = DotProduct (posed, aliastransform[0]) + aliastransform[0][3];
		vy = DotProduct (posed, aliastransform[1]) + aliastransform[1][3];
		vz = DotProduct (posed, aliastransform[2]) + aliastransform[2][3];

		fv->flags = 0;
		if (vz < ALIAS_Z_CLIP_PLANE)
		{
			fv->flags = ALIAS_Z_CLIP;
			continue;
		}

		zi = 1.0f / vz;
		fv->v[5] = (int)(zi * ziscale);
		fv->v[0] = (int)((vx * aliasxscale * zi) + aliasxcenter);
		fv->v[1] = (int)((vy * aliasyscale * zi) + aliasycenter);
		fv->v[2] = 0;		// solid-colour skin: always sample texel (0,0)
		fv->v[3] = 0;
		fv->v[4] = light;

		if (fv->v[0] < r_refdef.vrect.x ||
			fv->v[0] >= r_refdef.vrect.x + r_refdef.vrect.width ||
			fv->v[1] < r_refdef.vrect.y ||
			fv->v[1] >= r_refdef.vrect.y + r_refdef.vrect.height)
			fv->flags |= ALIAS_XY_CLIP_MASK;
	}

	r_affinetridesc.drawtype     = 0;
	r_affinetridesc.seamfixupX16 = 0;
	r_affinetridesc.pfinalverts  = fvbase;
	r_affinetridesc.skinwidth    = 4;
	r_affinetridesc.skinheight   = 4;

	for (mi = 0; mi < iqm->nummeshes; mi++)
	{
		lm_iqm_mesh_t	*me = &iqm->meshes[mi];
		mtriangle_t		tri;

		memset (meshskin, meshcolor[mi & 7], sizeof(meshskin));
		r_affinetridesc.pskin = meshskin;
		D_PolysetUpdateTables ();

		tri.facesfront = 1;
		r_affinetridesc.ptriangles   = &tri;
		r_affinetridesc.numtriangles = 1;

		for (t = 0; t < (int)me->num_triangles; t++)
		{
			unsigned	*idx = &iqm->tris[(me->first_triangle + t) * 3];
			finalvert_t	*a = &fvbase[idx[0]];
			finalvert_t	*b = &fvbase[idx[1]];
			finalvert_t	*c = &fvbase[idx[2]];

			if ((a->flags | b->flags | c->flags) & (ALIAS_XY_CLIP_MASK | ALIAS_Z_CLIP))
				continue;	// R1: skip clipped triangles

			tri.vertindex[0] = (int)idx[0];
			tri.vertindex[1] = (int)idx[1];
			tri.vertindex[2] = (int)idx[2];
			D_PolysetDraw ();
		}
	}
}

