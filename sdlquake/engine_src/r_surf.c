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
// r_surf.c: surface-related refresh code

#include "quakedef.h"
#include "r_local.h"
#include "r_drawflat.h"

drawsurf_t	r_drawsurf;

int				lightleft, sourcesstep, blocksize, sourcetstep;
int				lightdelta, lightdeltastep;
int				lightright, lightleftstep, lightrightstep, blockdivshift;
unsigned		blockdivmask;
void			*prowdestbase;
unsigned char	*pbasesource;
int				surfrowbytes;	// used by ASM files
unsigned		*r_lightptr;
int				r_stepback;
int				r_lightwidth;
int				r_numhblocks, r_numvblocks;
unsigned char	*r_source, *r_sourcemax;

void R_DrawSurfaceBlock8_mip0 (void);
void R_DrawSurfaceBlock8_mip1 (void);
void R_DrawSurfaceBlock8_mip2 (void);
void R_DrawSurfaceBlock8_mip3 (void);

static void	(*surfmiptable[4])(void) = {
	R_DrawSurfaceBlock8_mip0,
	R_DrawSurfaceBlock8_mip1,
	R_DrawSurfaceBlock8_mip2,
	R_DrawSurfaceBlock8_mip3
};



unsigned		blocklights[18*18];
unsigned		blocklights_rgb[18*18*3];

/* 4x4 Bayer ordered dither table, shared by mono and RGB block writers.
   Values 0..15; callers shift to scale to the appropriate quantisation step.
   Indexed as r_bayer4x4[(row & 3) * 4 + (col & 3)]. */
const unsigned char r_bayer4x4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

/*
===============
R_AddDynamicLights
===============
*/
void R_AddDynamicLights (void)
{
	msurface_t *surf;
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;

	surf = r_drawsurf.surf;
	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	tex = surf->texinfo;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if ( !DLIGHTBITS_TESTBIT(&surf->dlightbits, lnum) )
			continue;		// not lit by this light

		rad = cl_dlights[lnum].radius;
		dist = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) -
				surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];
		
		for (t = 0 ; t<tmax ; t++)
		{
			td = local[1] - t*16;
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
#ifdef QUAKE2
				{
					unsigned temp;
					temp = (rad - dist)*256;
					i = t*smax + s;
					if (!cl_dlights[lnum].dark)
						blocklights[i] += temp;
					else
					{
						if (blocklights[i] > temp)
							blocklights[i] -= temp;
						else
							blocklights[i] = 0;
					}
				}
#else
					blocklights[t*smax + s] += (rad - dist)*256;
#endif
			}
		}
	}
}

/*
===============
R_AddDynamicLights_RGB

Mono R_AddDynamicLights, three channels. The dlight->color modulates each
channel's contribution; default {1,1,1} reproduces mono behaviour.
===============
*/
void R_AddDynamicLights_RGB (void)
{
	msurface_t	*surf;
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;

	surf = r_drawsurf.surf;
	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	tex  = surf->texinfo;

	for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
	{
		if (!DLIGHTBITS_TESTBIT(&surf->dlightbits, lnum))
			continue;

		rad      = cl_dlights[lnum].radius;
		dist     = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) -
				   surf->plane->dist;
		rad     -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight) continue;
		minlight = rad - minlight;

		for (i = 0; i < 3; i++)
			impact[i] = cl_dlights[lnum].origin[i] - surf->plane->normal[i]*dist;

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];
		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		for (t = 0; t < tmax; t++)
		{
			td = local[1] - t*16;
			if (td < 0) td = -td;
			for (s = 0; s < smax; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0) sd = -sd;
				dist = (sd > td) ? sd + (td>>1) : td + (sd>>1);
				if (dist < minlight)
				{
					unsigned add = (unsigned)((rad - dist) * 256);
					int      idx = (t*smax + s) * 3;
					/* paint_light_preview implies coloured dlights — the
					 * whole point of the preview is showing _color edits. */
					extern cvar_t paint_light_preview;
					if (r_colored_dlights.value || paint_light_preview.value) {
						blocklights_rgb[idx + 0] += (unsigned)(add * cl_dlights[lnum].color[0]);
						blocklights_rgb[idx + 1] += (unsigned)(add * cl_dlights[lnum].color[1]);
						blocklights_rgb[idx + 2] += (unsigned)(add * cl_dlights[lnum].color[2]);
					} else {
						blocklights_rgb[idx + 0] += add;
						blocklights_rgb[idx + 1] += add;
						blocklights_rgb[idx + 2] += add;
					}
				}
			}
		}
	}
}

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap (void)
{
	int			smax, tmax;
	int			t;
	int			i, size;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	msurface_t	*surf;

	surf = r_drawsurf.surf;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax;
	lightmap = surf->samples;

	if (r_fullbright.value || !cl.worldmodel->lightdata)
	{
		for (i=0 ; i<size ; i++)
			blocklights[i] = 0;
		return;
	}

// clear to ambient
	for (i=0 ; i<size ; i++)
		blocklights[i] = r_refdef.ambientlight<<8;


// add all the lightmaps
	if (lightmap)
		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
			 maps++)
		{
			scale = r_drawsurf.lightadj[maps];	// 8.8 fraction		
			for (i=0 ; i<size ; i++)
				blocklights[i] += lightmap[i] * scale;
			lightmap += size;	// skip to next lightmap
		}

// add all the dynamic lights
	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights ();

	/* Stain (decal) layer is NOT applied here. Mono lightmap collapses RGB
	   deltas into a single luminance channel, which is why decals used to
	   show as colourless dark patches. Chroma now lives in the post-cache
	   overlay (R_OverlayStain), run from R_DrawSurface after the writer
	   fills the cache — so decals carry colour even on the mono path and
	   even when r_fullbright is on. */

// bound, invert, and shift
	for (i=0 ; i<size ; i++)
	{
		t = (255*256 - (int)blocklights[i]) >> (8 - VID_CBITS);

		if (t < (1 << 6))
			t = (1 << 6);

		blocklights[i] = t;
	}
}


/*
===============
R_BuildLightMap_RGB

Three-channel sibling of R_BuildLightMap. Reads from surf->rgb_samples
(must be non-NULL; caller's responsibility), writes blocklights_rgb in
6.8 fixed-point, range 0..16384 per channel. Unlike the mono path we
do NOT invert: the RGB block writer is multiplicative (light_int *
basepal[tex]), so high blocklights means bright output.
===============
*/
void R_BuildLightMap_RGB (void)
{
	int			smax, tmax;
	int			i, size;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	msurface_t	*surf;
	unsigned	amb;

	surf = r_drawsurf.surf;
	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax * tmax;
	lightmap = surf->rgb_samples;

	if (r_fullbright.value || !cl.worldmodel->lightdata) {
		for (i = 0; i < size * 3; i++) blocklights_rgb[i] = 0;
		return;
	}

	amb = r_refdef.ambientlight << 8;
	for (i = 0; i < size; i++) {
		blocklights_rgb[i*3 + 0] = amb;
		blocklights_rgb[i*3 + 1] = amb;
		blocklights_rgb[i*3 + 2] = amb;
	}

	if (lightmap)
		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			scale = r_drawsurf.lightadj[maps];	// 8.8 fixed
			for (i = 0; i < size; i++) {
				blocklights_rgb[i*3 + 0] += lightmap[i*3 + 0] * scale;
				blocklights_rgb[i*3 + 1] += lightmap[i*3 + 1] * scale;
				blocklights_rgb[i*3 + 2] += lightmap[i*3 + 2] * scale;
			}
			lightmap += size * 3;
		}

	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights_RGB ();

	/* Stain (decal) layer is applied as a post-pass in R_OverlayStain after
	   the writer fills the cache, not here. See the matching note in the
	   mono R_BuildLightMap. Keeps decal chroma identical across mono / RGB
	   / fullbright paths. */

	/* RGB path is multiplicative (light_int * basepal[tex]), not LUT-indexed
	   like mono — so we do NOT invert. Shift one bit less than the natural
	   8.8->6.8 rescale to compensate for the gamma curve baked into mono's
	   vid.colormap (mono mid-tones come out brighter than linear; RGB
	   multiplies linearly). The 64<<8 cap saturates fullbright cleanly. */
	for (i = 0; i < size * 3; i++) {
		unsigned v = blocklights_rgb[i] >> (7 - VID_CBITS);
		if (v > (64u << 8)) v = (64u << 8);
		blocklights_rgb[i] = v;
	}
}


/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base)
{
	int		reletive;
	int		count;

	if (currententity->frame)
	{
		if (base->alternate_anims)
			base = base->alternate_anims;
	}
	
	if (!base->anim_total)
		return base;

	reletive = (int)(cl.time*10) % base->anim_total;

	count = 0;	
	while (base->anim_min > reletive || base->anim_max <= reletive)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}


/*
===============
R_OverlayStain

Blend the per-luxel stain RGB delta onto the rendered palette indices
in the surface cache. Runs after the writer (mono or RGB) has filled
r_drawsurf.surfdat, so the base lighting math is untouched and decal
chroma appears uniformly across mono / RGB / fullbright paths.

Per-pixel: bilinear-interpolate the stain's 4 corner luxels, scale by
r_decals_intensity, add to basepal_*[base_idx], clamp 0..255, quantise
6 bits per channel, look up nearest palette in rgbtable[].
===============
*/
static void R_OverlayStain (void)
{
	msurface_t *surf;
	stain_t    *st;
	byte       *dest;
	int         w, h;
	int         mip, shift, mask;
	int         ks;
	int         px, py;

	surf = r_drawsurf.surf;
	if (!surf->stain) return;
	st = surf->stain;
	ks = (int)(r_decals_intensity.value * 256.0f);
	if (ks == 0) return;

	dest  = (byte *)r_drawsurf.surfdat;
	w     = r_drawsurf.surfwidth;
	h     = r_drawsurf.surfheight;
	mip   = r_drawsurf.surfmip;
	shift = 4 - mip;
	if (shift < 1) shift = 1;
	mask  = (1 << shift) - 1;

	for (py = 0; py < h; py++)
	{
		int lv0 = py >> shift;
		int lv1 = lv0 + 1;
		int fy  = py & mask;

		if (lv0 >= st->tmax) lv0 = st->tmax - 1;
		if (lv1 >= st->tmax) lv1 = st->tmax - 1;

		for (px = 0; px < w; px++)
		{
			int lu0 = px >> shift;
			int lu1 = lu0 + 1;
			int fx  = px & mask;
			int i00, i01, i10, i11;
			int sr, sg, sb;
			int sr_t, sg_t, sb_t, sr_b, sg_b, sb_b;
			byte base_idx;
			int r, g, b;

			if (lu0 >= st->smax) lu0 = st->smax - 1;
			if (lu1 >= st->smax) lu1 = st->smax - 1;

			i00 = (lv0 * st->smax + lu0) * 3;
			i01 = (lv0 * st->smax + lu1) * 3;
			i10 = (lv1 * st->smax + lu0) * 3;
			i11 = (lv1 * st->smax + lu1) * 3;

			/* Quick reject: skip pixels whose 4 corner luxels are all zero
			   stain. Common case — a 3x3 decal kernel only stains ~9 luxels
			   on a much larger surface, so most pixels short-circuit here. */
			if ((st->rgb[i00] | st->rgb[i00+1] | st->rgb[i00+2]
			   | st->rgb[i01] | st->rgb[i01+1] | st->rgb[i01+2]
			   | st->rgb[i10] | st->rgb[i10+1] | st->rgb[i10+2]
			   | st->rgb[i11] | st->rgb[i11+1] | st->rgb[i11+2]) == 0)
				continue;

			/* Bilinear interp of stain RGB across the 4 corner luxels. */
			sr_t = (int)st->rgb[i00+0] + ((((int)st->rgb[i01+0] - (int)st->rgb[i00+0]) * fx) >> shift);
			sg_t = (int)st->rgb[i00+1] + ((((int)st->rgb[i01+1] - (int)st->rgb[i00+1]) * fx) >> shift);
			sb_t = (int)st->rgb[i00+2] + ((((int)st->rgb[i01+2] - (int)st->rgb[i00+2]) * fx) >> shift);
			sr_b = (int)st->rgb[i10+0] + ((((int)st->rgb[i11+0] - (int)st->rgb[i10+0]) * fx) >> shift);
			sg_b = (int)st->rgb[i10+1] + ((((int)st->rgb[i11+1] - (int)st->rgb[i10+1]) * fx) >> shift);
			sb_b = (int)st->rgb[i10+2] + ((((int)st->rgb[i11+2] - (int)st->rgb[i10+2]) * fx) >> shift);
			sr   = sr_t + (((sr_b - sr_t) * fy) >> shift);
			sg   = sg_t + (((sg_b - sg_t) * fy) >> shift);
			sb   = sb_t + (((sb_b - sb_t) * fy) >> shift);

			/* Scale stain * intensity into a 0..255 channel delta.
			   ks = intensity * 256, so (s * ks) >> 8 = s * intensity. */
			sr = (sr * ks) >> 8;
			sg = (sg * ks) >> 8;
			sb = (sb * ks) >> 8;

			base_idx = dest[py * w + px];
			r = (int)basepal_r[base_idx] + sr;
			g = (int)basepal_g[base_idx] + sg;
			b = (int)basepal_b[base_idx] + sb;
			if (r < 0)   r = 0;   else if (r > 255) r = 255;
			if (g < 0)   g = 0;   else if (g > 255) g = 255;
			if (b < 0)   b = 0;   else if (b > 255) b = 255;

			dest[py * w + px] = rgbtable[((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2)];
		}
	}
	st->last_touched_frame = r_framecount;
}


/*
===============
R_DrawSurface
===============
*/
void R_DrawSurface (void)
{
	unsigned char	*basetptr;
	int				smax, tmax, twidth;
	int				u;
	int				soffset, basetoffset, texwidth;
	int				horzblockstep;
	unsigned char	*pcolumndest;
	void			(*pblockdrawer)(void);
	texture_t		*mt;

	/* paint_light_preview forces the RGB lightmap path so an archived
	 * r_coloredlight 0 in config.cfg can't strand the editor preview in
	 * the mono path (which drops dlight chroma entirely). Still gated on
	 * rgb_samples — without .lit data there's no RGB lightmap to write into. */
	extern cvar_t paint_light_preview;
	qboolean use_rgb =
		r_pixbytes == 1 &&
		(r_coloredlight.value || paint_light_preview.value) &&
		!r_fullbright.value &&
		r_drawsurf.surf->rgb_samples != NULL;

	if (use_rgb) R_BuildLightMap_RGB ();
	else         R_BuildLightMap ();

	surfrowbytes = r_drawsurf.rowbytes;

	mt = r_drawsurf.texture;

	r_source = (byte *)mt + mt->offsets[r_drawsurf.surfmip];

	// r_lightmap: replace texel sampling with a uniform mid-gray so the
	// lightmap (including decal stains) is visible against a neutral base.
	// 256*256 covers the largest possible texture; uniform fill means UV
	// wrap is a no-op (every sample returns the same byte).
	if (r_lightmap.value) {
		static byte r_lightmap_const[256 * 256];
		static qboolean r_lightmap_const_init = false;
		if (!r_lightmap_const_init) {
			memset (r_lightmap_const, 15, sizeof(r_lightmap_const));
			r_lightmap_const_init = true;
		}
		r_source = r_lightmap_const;
	}

	// r_drawflat 2: replace texel sampling with a category-coloured uniform
	// buffer. Same UV-wrap-no-op trick as r_lightmap. Lightmap blend still
	// runs, producing lit flat shading.
	if (r_drawflat.value == 2) {
		int cat = R_DrawFlat_SurfCategory (r_drawsurf.surf);
		r_source = r_drawflat_src[cat];
	}

	// Mip-dither blend: when surf_bucket > 0, build a pre-blended scratch
	// texture mixing mip N and mip N+1 via Bayer ordered dither, then point
	// r_source at it. The per-mip block writers (surfmiptable[] and
	// surfmiptable_rgb[]) are unchanged - they apply the lightmap to the
	// dithered palette indices as if it were a normal texture.
	//
	// Guards:
	//  - bucket > 0:           bucket 0 is the pure single-mip path
	//  - surfmip < MIPLEVELS-1: need mip N+1 to blend toward
	//  - mip-N size fits scratch: scratch is sized for id1's 256x256
	//    cap; oversized mod textures silently fall back to bucket-0
	//    rendering rather than stomping memory
	//  - !r_lightmap, !r_drawflat==2: don't clobber the debug overrides
	//    that set r_source themselves a few lines above
	{
		static byte r_mipdither_scratch[256 * 256];

		if (r_drawsurf.surf_bucket > 0
			&& r_drawsurf.surfmip < MIPLEVELS - 1
			&& (size_t)(mt->width  >> r_drawsurf.surfmip)
			 * (size_t)(mt->height >> r_drawsurf.surfmip)
			   <= sizeof(r_mipdither_scratch)
			&& !r_lightmap.value
			&& r_drawflat.value != 2)
		{
			int mip       = r_drawsurf.surfmip;
			int w         = mt->width  >> mip;
			int h         = mt->height >> mip;
			int w_next    = mt->width  >> (mip + 1);
			byte *src     = (byte *)mt + mt->offsets[mip];
			byte *src_next = (byte *)mt + mt->offsets[mip + 1];
			int threshold = (r_drawsurf.surf_bucket * 16) / NUM_DITHER_BUCKETS;
			int s, t;

			for (t = 0; t < h; t++)
			{
				int t_next = t >> 1;
				for (s = 0; s < w; s++)
				{
					int b = r_bayer4x4[((t & 3) << 2) | (s & 3)];
					r_mipdither_scratch[t * w + s] = (b < threshold)
						? src_next[t_next * w_next + (s >> 1)]
						: src[t * w + s];
				}
			}
			r_source = r_mipdither_scratch;
		}
	}

// the fractional light values should range from 0 to (VID_GRADES - 1) << 16
// from a source range of 0 - 255

	texwidth = mt->width >> r_drawsurf.surfmip;

	blocksize = 16 >> r_drawsurf.surfmip;
	blockdivshift = 4 - r_drawsurf.surfmip;
	blockdivmask = (1 << blockdivshift) - 1;
	
	r_lightwidth = (r_drawsurf.surf->extents[0]>>4)+1;

	r_numhblocks = r_drawsurf.surfwidth >> blockdivshift;
	r_numvblocks = r_drawsurf.surfheight >> blockdivshift;

//==============================

	static void (*surfmiptable_rgb[4])(void) = {
		R_DrawSurfaceBlock8_mip0_rgb,
		R_DrawSurfaceBlock8_mip1_rgb,
		R_DrawSurfaceBlock8_mip2_rgb,
		R_DrawSurfaceBlock8_mip3_rgb
	};

	if (r_pixbytes == 1)
	{
		pblockdrawer = use_rgb
			? surfmiptable_rgb[r_drawsurf.surfmip]
			: surfmiptable[r_drawsurf.surfmip];
	// TODO: only needs to be set when there is a display settings change
		horzblockstep = blocksize;
	}
	else
	{
		pblockdrawer = R_DrawSurfaceBlock16;
	// TODO: only needs to be set when there is a display settings change
		horzblockstep = blocksize << 1;
	}

	smax = mt->width >> r_drawsurf.surfmip;
	twidth = texwidth;
	tmax = mt->height >> r_drawsurf.surfmip;
	sourcetstep = texwidth;
	r_stepback = tmax * twidth;

	r_sourcemax = r_source + (tmax * smax);

	soffset = r_drawsurf.surf->texturemins[0];
	basetoffset = r_drawsurf.surf->texturemins[1];

// << 16 components are to guarantee positive values for %
	soffset = ((soffset >> r_drawsurf.surfmip) + (smax << 16)) % smax;
	basetptr = &r_source[((((basetoffset >> r_drawsurf.surfmip) 
		+ (tmax << 16)) % tmax) * twidth)];

	pcolumndest = r_drawsurf.surfdat;

	for (u=0 ; u<r_numhblocks; u++)
	{
		r_lightptr = blocklights + u;

		prowdestbase = pcolumndest;

		pbasesource = basetptr + soffset;

		(*pblockdrawer)();

		soffset = soffset + blocksize;
		if (soffset >= smax)
			soffset = 0;

		pcolumndest += horzblockstep;
	}

	R_OverlayStain ();
}


//=============================================================================

#if	!id386

/*
================
R_DrawSurfaceBlock8_mip0
================
*/
void R_DrawSurfaceBlock8_mip0 (void)
{
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;
	int				dither_on = (r_lightmap_dither.value != 0.0f);

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> 4;
		lightrightstep = (r_lightptr[1] - lightright) >> 4;

		for (i=0 ; i<16 ; i++)
		{
			const unsigned char *brow = dither_on ? &r_bayer4x4[(i & 3) << 2] : 0;
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> 4;

			light = lightright;

			for (b=15; b>=0; b--)
			{
				unsigned d = brow ? ((unsigned)brow[b & 3] << 4) : 0u;
				unsigned row = ((unsigned)light + d) & 0xFF00u;
				if (row > (63u << 8)) row = 63u << 8;
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)[row + pix];
				light += lightstep;
			}

			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock8_mip1
================
*/
void R_DrawSurfaceBlock8_mip1 (void)
{
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;
	int				dither_on = (r_lightmap_dither.value != 0.0f);

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> 3;
		lightrightstep = (r_lightptr[1] - lightright) >> 3;

		for (i=0 ; i<8 ; i++)
		{
			const unsigned char *brow = dither_on ? &r_bayer4x4[(i & 3) << 2] : 0;
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> 3;

			light = lightright;

			for (b=7; b>=0; b--)
			{
				unsigned d = brow ? ((unsigned)brow[b & 3] << 4) : 0u;
				unsigned row = ((unsigned)light + d) & 0xFF00u;
				if (row > (63u << 8)) row = 63u << 8;
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)[row + pix];
				light += lightstep;
			}

			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock8_mip2
================
*/
void R_DrawSurfaceBlock8_mip2 (void)
{
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;
	int				dither_on = (r_lightmap_dither.value != 0.0f);

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> 2;
		lightrightstep = (r_lightptr[1] - lightright) >> 2;

		for (i=0 ; i<4 ; i++)
		{
			const unsigned char *brow = dither_on ? &r_bayer4x4[(i & 3) << 2] : 0;
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> 2;

			light = lightright;

			for (b=3; b>=0; b--)
			{
				unsigned d = brow ? ((unsigned)brow[b & 3] << 4) : 0u;
				unsigned row = ((unsigned)light + d) & 0xFF00u;
				if (row > (63u << 8)) row = 63u << 8;
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)[row + pix];
				light += lightstep;
			}

			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock8_mip3
================
*/
void R_DrawSurfaceBlock8_mip3 (void)
{
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;
	int				dither_on = (r_lightmap_dither.value != 0.0f);

	psource = pbasesource;
	prowdest = prowdestbase;

	for (v=0 ; v<r_numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = r_lightptr[0];
		lightright = r_lightptr[1];
		r_lightptr += r_lightwidth;
		lightleftstep = (r_lightptr[0] - lightleft) >> 1;
		lightrightstep = (r_lightptr[1] - lightright) >> 1;

		for (i=0 ; i<2 ; i++)
		{
			const unsigned char *brow = dither_on ? &r_bayer4x4[(i & 3) << 2] : 0;
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> 1;

			light = lightright;

			for (b=1; b>=0; b--)
			{
				unsigned d = brow ? ((unsigned)brow[b & 3] << 4) : 0u;
				unsigned row = ((unsigned)light + d) & 0xFF00u;
				if (row > (63u << 8)) row = 63u << 8;
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)[row + pix];
				light += lightstep;
			}

			psource += sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += surfrowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}
}


/*
================
R_DrawSurfaceBlock16

FIXME: make this work
================
*/
void R_DrawSurfaceBlock16 (void)
{
	int				k;
	unsigned char	*psource;
	int				lighttemp, lightstep, light;
	unsigned short	*prowdest;

	prowdest = (unsigned short *)prowdestbase;

	for (k=0 ; k<blocksize ; k++)
	{
		unsigned short	*pdest;
		unsigned char	pix;
		int				b;

		psource = pbasesource;
		lighttemp = lightright - lightleft;
		lightstep = lighttemp >> blockdivshift;

		light = lightleft;
		pdest = prowdest;

		for (b=0; b<blocksize; b++)
		{
			pix = *psource;
			*pdest = vid.colormap16[(light & 0xFF00) + pix];
			psource += sourcesstep;
			pdest++;
			light += lightstep;
		}

		pbasesource += sourcetstep;
		lightright += lightrightstep;
		lightleft += lightleftstep;
		prowdest = (unsigned short *)((size_t)prowdest + surfrowbytes);
	}

	prowdestbase = prowdest;
}

#endif


//============================================================================

/*
================
R_GenTurbTile
================
*/
void R_GenTurbTile (pixel_t *pbasetex, void *pdest)
{
	int		*turb;
	int		i, j, s, t;
	byte	*pd;
	
	turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));
	pd = (byte *)pdest;

	for (i=0 ; i<TILE_SIZE ; i++)
	{
		for (j=0 ; j<TILE_SIZE ; j++)
		{	
			s = (((j << 16) + turb[i & (CYCLE-1)]) >> 16) & 63;
			t = (((i << 16) + turb[j & (CYCLE-1)]) >> 16) & 63;
			*pd++ = *(pbasetex + (t<<6) + s);
		}
	}
}


/*
================
R_GenTurbTile16
================
*/
void R_GenTurbTile16 (pixel_t *pbasetex, void *pdest)
{
	int				*turb;
	int				i, j, s, t;
	unsigned short	*pd;

	turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));
	pd = (unsigned short *)pdest;

	for (i=0 ; i<TILE_SIZE ; i++)
	{
		for (j=0 ; j<TILE_SIZE ; j++)
		{	
			s = (((j << 16) + turb[i & (CYCLE-1)]) >> 16) & 63;
			t = (((i << 16) + turb[j & (CYCLE-1)]) >> 16) & 63;
			*pd++ = d_8to16table[*(pbasetex + (t<<6) + s)];
		}
	}
}


/*
================
R_GenTile
================
*/
void R_GenTile (msurface_t *psurf, void *pdest)
{
	if (psurf->flags & SURF_DRAWTURB)
	{
		if (r_pixbytes == 1)
		{
			R_GenTurbTile ((pixel_t *)
				((byte *)psurf->texinfo->texture + psurf->texinfo->texture->offsets[0]), pdest);
		}
		else
		{
			R_GenTurbTile16 ((pixel_t *)
				((byte *)psurf->texinfo->texture + psurf->texinfo->texture->offsets[0]), pdest);
		}
	}
	else if (psurf->flags & SURF_DRAWSKY)
	{
		if (r_pixbytes == 1)
		{
			R_GenSkyTile (pdest);
		}
		else
		{
			R_GenSkyTile16 (pdest);
		}
	}
	else
	{
		Sys_Error ("Unknown tile type");
	}
}

