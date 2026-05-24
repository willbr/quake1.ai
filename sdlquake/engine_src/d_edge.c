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
// d_edge.c

#include "quakedef.h"
#include "d_local.h"
#include "r_drawflat.h"

static int	miplevel;

float		scale_for_mip;
int			screenwidth;
int			ubasestep, errorterm, erroradjustup, erroradjustdown;
int			vstartscan;

// FIXME: should go away
extern void			R_RotateBmodel (void);
extern void			R_TransformFrustum (void);

vec3_t		transformed_modelorg;

/*
==============
D_DrawPoly

==============
*/
void D_DrawPoly (void)
{
// this driver takes spans, not polygons
}


/*
=============
D_MipLevelForScale
=============
*/
int D_MipLevelForScale (float scale)
{
	int		lmiplevel;

	if (scale >= d_scalemip[0] )
		lmiplevel = 0;
	else if (scale >= d_scalemip[1] )
		lmiplevel = 1;
	else if (scale >= d_scalemip[2] )
		lmiplevel = 2;
	else
		lmiplevel = 3;

	if (lmiplevel < d_minmip)
		lmiplevel = d_minmip;

	return lmiplevel;
}


/*
==============
D_DrawSolidSurface
==============
*/

// FIXME: clean this up

void D_DrawSolidSurface (surf_t *surf, int color)
{
	espan_t	*span;
	byte	*pdest;
	int		u, u2, pix;
	
	pix = (color<<24) | (color<<16) | (color<<8) | color;
	for (span=surf->spans ; span ; span=span->pnext)
	{
		pdest = (byte *)d_viewbuffer + screenwidth*span->v;
		u = span->u;
		u2 = span->u + span->count - 1;
		((byte *)pdest)[u] = pix;

		if (u2 - u < 8)
		{
			for (u++ ; u <= u2 ; u++)
				((byte *)pdest)[u] = pix;
		}
		else
		{
			for (u++ ; u & 3 ; u++)
				((byte *)pdest)[u] = pix;

			u2 -= 4;
			for ( ; u <= u2 ; u+=4)
				*(int *)((byte *)pdest + u) = pix;
			u2 += 4;
			for ( ; u <= u2 ; u++)
				((byte *)pdest)[u] = pix;
		}
	}
}


/*
==============
D_CalcGradients
==============
*/
void D_CalcGradients (msurface_t *pface)
{
	mplane_t	*pplane;
	float		mipscale;
	vec3_t		p_temp1;
	vec3_t		p_saxis, p_taxis;
	float		t;

	pplane = pface->plane;

	mipscale = 1.0 / (float)(1 << miplevel);

	TransformVector (pface->texinfo->vecs[0], p_saxis);
	TransformVector (pface->texinfo->vecs[1], p_taxis);

	t = xscaleinv * mipscale;
	d_sdivzstepu = p_saxis[0] * t;
	d_tdivzstepu = p_taxis[0] * t;

	t = yscaleinv * mipscale;
	d_sdivzstepv = -p_saxis[1] * t;
	d_tdivzstepv = -p_taxis[1] * t;

	d_sdivzorigin = p_saxis[2] * mipscale - xcenter * d_sdivzstepu -
			ycenter * d_sdivzstepv;
	d_tdivzorigin = p_taxis[2] * mipscale - xcenter * d_tdivzstepu -
			ycenter * d_tdivzstepv;

	VectorScale (transformed_modelorg, mipscale, p_temp1);

	t = 0x10000*mipscale;
	sadjust = ((fixed16_t)(DotProduct (p_temp1, p_saxis) * 0x10000 + 0.5)) -
			((pface->texturemins[0] << 16) >> miplevel)
			+ pface->texinfo->vecs[0][3]*t;
	tadjust = ((fixed16_t)(DotProduct (p_temp1, p_taxis) * 0x10000 + 0.5)) -
			((pface->texturemins[1] << 16) >> miplevel)
			+ pface->texinfo->vecs[1][3]*t;

//
// -1 (-epsilon) so we never wander off the edge of the texture
//
	bbextents = ((pface->extents[0] << 16) >> miplevel) - 1;
	bbextentt = ((pface->extents[1] << 16) >> miplevel) - 1;
}


/*
==============
D_DrawSurfaces
==============
*/
void D_DrawSurfaces (void)
{
	surf_t			*s;
	msurface_t		*pface;
	surfcache_t		*pcurrentcache;
	vec3_t			world_transformed_modelorg;
	vec3_t			local_modelorg;

	currententity = &cl_entities[0];
	TransformVector (modelorg, transformed_modelorg);
	VectorCopy (transformed_modelorg, world_transformed_modelorg);

// TODO: could preset a lot of this at mode set time
	// r_drawflat 1: original id "hash s->data" debug mode. Mode 2 (lit
	// coloured flat shading) falls through to the normal textured path
	// below, where R_DrawSurface picks up the per-category source override.
	if (r_drawflat.value == 1)
	{
		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			d_zistepu = s->d_zistepu;
			d_zistepv = s->d_zistepv;
			d_ziorigin = s->d_ziorigin;

			D_DrawSolidSurface (s, (int)((uintptr_t)s->data & 0xFF));
			D_DrawZSpans (s->spans);
		}
	}
	else
	{
		for (s = &surfaces[1] ; s<surface_p ; s++)
		{
			if (!s->spans)
				continue;

			r_drawnpolycount++;

			d_zistepu = s->d_zistepu;
			d_zistepv = s->d_zistepv;
			d_ziorigin = s->d_ziorigin;

			if (s->flags & SURF_DRAWSKY)
			{
				if (r_drawflat.value == 2)
				{
					// Flat-shaded sky: one solid colour instead of the
					// scrolling sky panorama. Reads as "outside / out of
					// bounds" in nav mode.
					extern byte r_drawflat_src[][256 * 256];
					int sky_color = r_drawflat_src[R_FLAT_CAT_SKY][0];
					D_DrawSolidSurface (s, sky_color);
					D_DrawZSpans (s->spans);
				}
				else
				{
					if (!r_skymade)
					{
						R_MakeSky ();
					}

					D_DrawSkyScans8 (s->spans);
					D_DrawZSpans (s->spans);
				}
			}
			else if (s->flags & SURF_DRAWBACKGROUND)
			{
			// set up a gradient for the background surface that places it
			// effectively at infinity distance from the viewpoint
				d_zistepu = 0;
				d_zistepv = 0;
				d_ziorigin = -0.9;

				D_DrawSolidSurface (s, (int)r_clearcolor.value & 0xFF);
				D_DrawZSpans (s->spans);
			}
			else if (s->flags & SURF_DRAWTURB)
			{
				pface = s->data;
				miplevel = 0;
				cacheblock = (pixel_t *)
						((byte *)pface->texinfo->texture +
						pface->texinfo->texture->offsets[0]);
				cachewidth = 64;

				if (s->insubmodel)
				{
				// FIXME: we don't want to do all this for every polygon!
				// TODO: store once at start of frame
					currententity = s->entity;	//FIXME: make this passed in to
												// R_RotateBmodel ()
					VectorSubtract (r_origin, currententity->origin,
							local_modelorg);
					TransformVector (local_modelorg, transformed_modelorg);

					R_RotateBmodel ();	// FIXME: don't mess with the frustum,
										// make entity passed in
				}

				// r_drawflat 2: override cacheblock with category source.
				// Turbulent8 still warps, but a uniform source warps to a
				// uniform output, so water/lava/slime read as solid blocks.
				if (r_drawflat.value == 2)
				{
					int cat = R_DrawFlat_SurfCategory (pface);
					cacheblock = (pixel_t *)r_drawflat_src[cat];
					cachewidth = 64;
				}

				D_CalcGradients (pface);
				Turbulent8 (s->spans);
				D_DrawZSpans (s->spans);

				if (s->insubmodel)
				{
				//
				// restore the old drawing state
				// FIXME: we don't want to do this every time!
				// TODO: speed up
				//
					currententity = &cl_entities[0];
					VectorCopy (world_transformed_modelorg,
								transformed_modelorg);
					VectorCopy (base_vpn, vpn);
					VectorCopy (base_vup, vup);
					VectorCopy (base_vright, vright);
					VectorCopy (base_modelorg, modelorg);
					R_TransformFrustum ();
				}
			}
			else
			{
				if (s->insubmodel)
				{
				// FIXME: we don't want to do all this for every polygon!
				// TODO: store once at start of frame
					currententity = s->entity;	//FIXME: make this passed in to
												// R_RotateBmodel ()
					VectorSubtract (r_origin, currententity->origin, local_modelorg);
					TransformVector (local_modelorg, transformed_modelorg);

					R_RotateBmodel ();	// FIXME: don't mess with the frustum,
										// make entity passed in
				}

				pface = s->data;
				{
					float mipscale = s->nearzi * scale_for_mip
						* pface->texinfo->mipadjust;
					int raw_mip = D_MipLevelForScale (mipscale);
					int prev    = pface->last_miplevel;
					int bucket  = 0;

					// Existing mip-level hysteresis: hold prev mip while scale
					// sits within a 15% deadband of the boundary.
					if (prev >= 0 && prev != raw_mip)
					{
						int b = (prev < raw_mip) ? prev : raw_mip;
						if (b >= 0 && b < MIPLEVELS - 1)
						{
							float t = d_scalemip[b];
							if (mipscale > t * 0.85f
							 && mipscale < t * 1.15f)
								raw_mip = prev;
						}
					}

					miplevel = raw_mip;

					// Mip-dither bucket selection. Two boundary bands can apply:
					// the one below raw_mip (between raw_mip and raw_mip+1) and
					// the one above (between raw_mip-1 and raw_mip). They are
					// independent `if` blocks because raw_mip+1 existing tells
					// us nothing about raw_mip-1 existing - both can be true.
					// At default cvar settings the mipscale bands themselves
					// cannot overlap (thresholds are ~2x apart in mipscale,
					// band is 2*r_mipdither_band = 0.6), so only one inner
					// mipscale-in-band test fires. If a user dials the band
					// width up enough to overlap, the upper-band assignment
					// runs second and wins, biasing toward the sharper mip.
					// Dlit surfaces rebuild cache every frame; skip dither to keep that
					// path cheap. Coloured dlight intensity masks mip-step pop anyway.
					if (r_mipdither.value
						&& r_mipdither_band.value > 0.0f
						&& pface->dlightframe != r_framecount)
					{
						float band = r_mipdither_band.value;

						// Lower boundary: dither toward raw_mip+1 (coarser).
						if (raw_mip + 1 <= MIPLEVELS - 1)
						{
							float t_lo = d_scalemip[raw_mip];
							float hi   = t_lo * (1.0f + band);
							float lo   = t_lo * (1.0f - band);
							if (mipscale < hi && mipscale > lo)
							{
								float t   = (hi - mipscale) / (hi - lo);
								float tn  = t * (NUM_DITHER_BUCKETS - 1);
								int   q;
								// Bucket hysteresis: hold prev_bucket while we're within 0.75 of it
								// (vs. the natural round() boundary at 0.5). Only applies when the
								// previous frame's surface was on the same (miplevel, raw_mip) pair.
								if (pface->last_miplevel == raw_mip
									&& pface->last_bucket   >= 0
									&& pface->last_bucket   <= NUM_DITHER_BUCKETS - 1
									&& fabsf(tn - (float)pface->last_bucket) < 0.75f)
								{
									q = pface->last_bucket;
								}
								else
								{
									q = (int)(tn + 0.5f);
									if (q < 0) q = 0;
									if (q > NUM_DITHER_BUCKETS - 1)
										q = NUM_DITHER_BUCKETS - 1;
								}
								miplevel = raw_mip;
								bucket   = q;
							}
						}

						// Upper boundary: dither toward raw_mip-1 (sharper).
						// d_minmip is the floor (higher number = stricter
						// floor); raw_mip-1 must be at-or-above it AND >= 0.
						if (raw_mip - 1 >= d_minmip && raw_mip >= 1)
						{
							float t_hi = d_scalemip[raw_mip - 1];
							float hi   = t_hi * (1.0f + band);
							float lo   = t_hi * (1.0f - band);
							if (mipscale < hi && mipscale > lo)
							{
								float t   = (hi - mipscale) / (hi - lo);
								float tn  = t * (NUM_DITHER_BUCKETS - 1);
								int   q;
								if (pface->last_miplevel == raw_mip - 1
									&& pface->last_bucket   >= 0
									&& pface->last_bucket   <= NUM_DITHER_BUCKETS - 1
									&& fabsf(tn - (float)pface->last_bucket) < 0.75f)
								{
									q = pface->last_bucket;
								}
								else
								{
									q = (int)(tn + 0.5f);
									if (q < 0) q = 0;
									if (q > NUM_DITHER_BUCKETS - 1)
										q = NUM_DITHER_BUCKETS - 1;
								}
								miplevel = raw_mip - 1;
								bucket   = q;
							}
						}
					}

					pface->last_miplevel = (signed char)miplevel;
					pface->last_bucket   = (signed char)bucket;

					pcurrentcache = D_CacheSurface (pface, miplevel, bucket);
				}

				cacheblock = (pixel_t *)pcurrentcache->data;
				cachewidth = pcurrentcache->width;

				D_CalcGradients (pface);

				(*d_drawspans) (s->spans);

				D_DrawZSpans (s->spans);

				if (s->insubmodel)
				{
				//
				// restore the old drawing state
				// FIXME: we don't want to do this every time!
				// TODO: speed up
				//
					currententity = &cl_entities[0];
					VectorCopy (world_transformed_modelorg,
								transformed_modelorg);
					VectorCopy (base_vpn, vpn);
					VectorCopy (base_vup, vup);
					VectorCopy (base_vright, vright);
					VectorCopy (base_modelorg, modelorg);
					R_TransformFrustum ();
				}
			}
		}
	}
}

