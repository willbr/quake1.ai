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
// d_part.c: software driver module for drawing particles

#include "quakedef.h"
#include "d_local.h"
#include "r_shared.h"	// xscaleshrink, yscaleshrink, r_pright/r_pup/r_ppn
#include "r_emitter.h"	// emitter_def_t + sampling (pt_emitter draw)
#include <stdint.h>	// uintptr_t (per-particle salt in D_DrawSmokeParticle)


/*
==============
D_EndParticles
==============
*/
void D_EndParticles (void)
{
// not used by software driver
}


/*
==============
D_StartParticles
==============
*/
void D_StartParticles (void)
{
// not used by software driver
}


#if	!id386

/*
==============
D_DrawParticle
==============
*/
void D_DrawParticle (particle_t *pparticle)
{
	vec3_t	local, transformed;
	float	zi;
	byte	*pdest;
	short	*pz;
	int		i, izi, pix, count, u, v;

// transform point
	VectorSubtract (pparticle->org, r_origin, local);

	transformed[0] = DotProduct(local, r_pright);
	transformed[1] = DotProduct(local, r_pup);
	transformed[2] = DotProduct(local, r_ppn);		

	if (transformed[2] < PARTICLE_Z_CLIP)
		return;

// project the point
// FIXME: preadjust xcenter and ycenter
	zi = 1.0 / transformed[2];
	u = (int)(xcenter + zi * transformed[0] + 0.5);
	v = (int)(ycenter - zi * transformed[1] + 0.5);

	if ((v > d_vrectbottom_particle) || 
		(u > d_vrectright_particle) ||
		(v < d_vrecty) ||
		(u < d_vrectx))
	{
		return;
	}

	pz = d_pzbuffer + (d_zwidth * v) + u;
	pdest = d_viewbuffer + d_scantable[v] + u;
	izi = (int)(zi * 0x8000);

	pix = izi >> d_pix_shift;

	if (pix < d_pix_min)
		pix = d_pix_min;
	else if (pix > d_pix_max)
		pix = d_pix_max;

	switch (pix)
	{
	case 1:
		count = 1 << d_y_aspect_shift;

		for ( ; count ; count--, pz += d_zwidth, pdest += screenwidth)
		{
			if (pz[0] <= izi)
			{
				pz[0] = izi;
				pdest[0] = pparticle->color;
			}
		}
		break;

	case 2:
		count = 2 << d_y_aspect_shift;

		for ( ; count ; count--, pz += d_zwidth, pdest += screenwidth)
		{
			if (pz[0] <= izi)
			{
				pz[0] = izi;
				pdest[0] = pparticle->color;
			}

			if (pz[1] <= izi)
			{
				pz[1] = izi;
				pdest[1] = pparticle->color;
			}
		}
		break;

	case 3:
		count = 3 << d_y_aspect_shift;

		for ( ; count ; count--, pz += d_zwidth, pdest += screenwidth)
		{
			if (pz[0] <= izi)
			{
				pz[0] = izi;
				pdest[0] = pparticle->color;
			}

			if (pz[1] <= izi)
			{
				pz[1] = izi;
				pdest[1] = pparticle->color;
			}

			if (pz[2] <= izi)
			{
				pz[2] = izi;
				pdest[2] = pparticle->color;
			}
		}
		break;

	case 4:
		count = 4 << d_y_aspect_shift;

		for ( ; count ; count--, pz += d_zwidth, pdest += screenwidth)
		{
			if (pz[0] <= izi)
			{
				pz[0] = izi;
				pdest[0] = pparticle->color;
			}

			if (pz[1] <= izi)
			{
				pz[1] = izi;
				pdest[1] = pparticle->color;
			}

			if (pz[2] <= izi)
			{
				pz[2] = izi;
				pdest[2] = pparticle->color;
			}

			if (pz[3] <= izi)
			{
				pz[3] = izi;
				pdest[3] = pparticle->color;
			}
		}
		break;

	default:
		count = pix << d_y_aspect_shift;

		for ( ; count ; count--, pz += d_zwidth, pdest += screenwidth)
		{
			for (i=0 ; i<pix ; i++)
			{
				if (pz[i] <= izi)
				{
					pz[i] = izi;
					pdest[i] = pparticle->color;
				}
			}
		}
		break;
	}
}

#endif	// !id386


/*
==============
Smoke density buffer + fog colormap (no-dither composite path).

Smoke is no longer rasterised directly into the framebuffer with a hash
dither. Instead each puff/cell writes a smooth (1-d^2)^2 contribution
into a per-pixel byte density buffer (z-tested against the world), and
a single composite pass at the end of R_DrawParticles tints the frame-
buffer by looking up fog_colormap[framebuffer_index][density>>2]. The
colormap is the standard Quake light-table trick: for each of 64
density slots we precompute the nearest palette index to a linear lerp
between the original colour and the smoke tint, so blending costs one
byte LUT per affected pixel. No stipple artefacts, no diagonal hash
stripes, no opaque grey cores.

Buffer sizing matches vid_buffer's max in vid_sdl.c (1280x800). Active
area is vid.width*vid.rowbytes; the rest stays untouched.
==============
*/
#define SMOKE_BUF_MAX  (1280 * 800)
static byte s_smoke_dens[SMOKE_BUF_MAX];
static int  s_smoke_dens_dirty = 0;

static byte s_fog_cmap[256][64];
static int  s_fog_cmap_built = 0;

static int int_clamp_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void build_fog_colormap(void)
{
	// Smoke tint in 0..255 RGB. Slightly cool mid-grey so the smoke
	// reads as atmospheric haze rather than printer-paper white.
	const int smoke_r = 170, smoke_g = 170, smoke_b = 178;

	if (!host_basepal) return;

	for (int c = 0; c < 256; c++) {
		int sr = host_basepal[c*3 + 0];
		int sg = host_basepal[c*3 + 1];
		int sb = host_basepal[c*3 + 2];
		for (int slot = 0; slot < 64; slot++) {
			// slot=0 keeps the original colour; slot=63 is fully smoke.
			int t = slot * 255 / 63;   // 0..255 mix weight
			int r = (sr * (255 - t) + smoke_r * t) / 255;
			int g = (sg * (255 - t) + smoke_g * t) / 255;
			int b = (sb * (255 - t) + smoke_b * t) / 255;
			int best = 0, best_d2 = 0x7fffffff;
			for (int p = 0; p < 256; p++) {
				int pr = host_basepal[p*3 + 0];
				int pg = host_basepal[p*3 + 1];
				int pb = host_basepal[p*3 + 2];
				int dr = r - pr, dg = g - pg, db = b - pb;
				int d2 = dr*dr + dg*dg + db*db;
				if (d2 < best_d2) { best_d2 = d2; best = p; }
			}
			s_fog_cmap[c][slot] = (byte)best;
		}
	}
	s_fog_cmap_built = 1;
}

void D_CompositeSmoke(void)
{
	if (!s_smoke_dens_dirty) return;
	if (!s_fog_cmap_built) build_fog_colormap();
	if (!s_fog_cmap_built) { s_smoke_dens_dirty = 0; return; }

	int w  = (int)vid.width;
	int h  = (int)vid.height;
	int rb = (int)vid.rowbytes;
	for (int y = 0; y < h; y++) {
		byte *dst  = vid.buffer    + y * rb;
		byte *dens = s_smoke_dens + y * rb;
		for (int x = 0; x < w; x++) {
			int d = dens[x];
			if (d == 0) continue;
			int slot = d >> 2;       // 0..255 -> 0..63
			if (slot > 63) slot = 63;
			dst[x] = s_fog_cmap[dst[x]][slot];
			dens[x] = 0;             // clear inline so next frame starts blank
		}
	}
	s_smoke_dens_dirty = 0;
}

/*
==============
draw_smoke_blob

Shared rasteriser for both smoke renderings: voxel-haze cells
(D_DrawSmokeCells) and hero pt_smoke particles (D_DrawSmokeParticle).
Stamps an ellipse of (pix x rows) screen pixels centred on (cu, cv),
z-tested against izi (caller's view-space inverse depth). Inside the
ellipse the density falls off radially via (1-d^2)^2 and accumulates
(saturating) into s_smoke_dens; the composite pass later turns that
density into a per-pixel fog tint. No screen-space hash or dither.
salt + color are vestigial and ignored — kept for now to keep the call
sites unchanged.
==============
*/
static void draw_smoke_blob (int cu, int cv, int pix, int rows,
                             int izi, float density, unsigned salt, byte color)
{
	(void)salt; (void)color;
	if (pix < 2 || rows < 2) return;

	// Floor the blob density. Cells just over sim_wind's threshold pass in
	// values near 0.05 and produce a barely-visible tint, which reads as
	// "holes" inside the cloud where you can see geometry through the haze.
	// Clamping to 0.3 keeps the per-cell contrast (cells with 1.0 still
	// dominate the centre) while making sure every cell that draws gets
	// past the threshold of visibility.
	if (density < 0.30f) density = 0.30f;

	int u = cu - pix  / 2;
	int v = cv - rows / 2;

	int u0 = (u < d_vrectx)               ? d_vrectx - u   : 0;
	int u1 = (u + pix > r_refdef.vrectright)  ? r_refdef.vrectright  - u : pix;
	int v0 = (v < d_vrecty)               ? d_vrecty - v   : 0;
	int v1 = (v + rows > r_refdef.vrectbottom) ? r_refdef.vrectbottom - v : rows;
	if (u0 >= u1 || v0 >= v1) return;

	float rx     = pix  * 0.5f;
	float ry     = rows * 0.5f;
	float inv_rx2 = 1.0f / (rx * rx);
	float inv_ry2 = 1.0f / (ry * ry);

	short *pz    = d_pzbuffer    + d_zwidth * (v + v0) + u;
	byte  *sdens = s_smoke_dens  + (v + v0) * vid.rowbytes + u;

	for (int row = v0; row < v1; row++, pz += d_zwidth, sdens += vid.rowbytes)
	{
		float dy = ((float)row + 0.5f) - ry;
		float dy2_n = (dy * dy) * inv_ry2;
		for (int i = u0; i < u1; i++)
		{
			float dx = ((float)i + 0.5f) - rx;
			float d2 = dx * dx * inv_rx2 + dy2_n;
			if (d2 >= 1.0f) continue;
			float k = 1.0f - d2;
			int add = (int)(density * k * k * 255.0f);
			if (add <= 0) continue;
			if (pz[i] <= izi) {
				int s = (int)sdens[i] + add;
				sdens[i] = (s > 255) ? 255 : (byte)s;
			}
		}
	}
	s_smoke_dens_dirty = 1;
}

/*
==============
D_DrawSmokeParticle

Hero pt_smoke puff renderer with age-driven curves.

A pt_smoke particle has a lifetime t = (cl.time - birth) / (die - birth)
in [0, 1]. The visual is "puff out + dissipate": small + dense at birth,
swelling to ~1.5x its peak ramp by the end while fading toward zero
density. The draw side writes density into s_smoke_dens; the composite
pass turns that into a fog-coloured framebuffer tint, so a fading puff
looks like real disipating smoke rather than a vanishing dot.
==============
*/
void D_DrawSmokeParticle (particle_t *pparticle)
{
	vec3_t	local, transformed;

	VectorSubtract (pparticle->org, r_origin, local);
	transformed[0] = DotProduct(local, r_pright);
	transformed[1] = DotProduct(local, r_pup);
	transformed[2] = DotProduct(local, r_ppn);
	if (transformed[2] < PARTICLE_Z_CLIP)
		return;

	float zi = 1.0f / transformed[2];
	int   u  = (int)(xcenter + zi * transformed[0] + 0.5f);
	int   v  = (int)(ycenter - zi * transformed[1] + 0.5f);
	int   izi = (int)(zi * 0x8000);

	// Normalised age. Guard against zero-duration particles (shouldn't
	// happen but cheap insurance). Old particles spawned before birth
	// was added (e.g. legacy SV_Smoke callers that bypass R_AddSmokePuff)
	// surface with birth == 0 → treat them as fully aged.
	float life = pparticle->die - pparticle->birth;
	float t    = (life > 0.001f) ? (cl.time - pparticle->birth) / life : 1.0f;
	if (t < 0) t = 0; else if (t > 1) t = 1;

	// Size curve: grows from 0.35x of `ramp` at birth to 1.5x by death.
	// Uses sqrt for fast initial expansion (a puff "pops" out) then a
	// slow drift to maximum, matching the look of real exhaled smoke.
	float size_scale = 0.35f + 1.15f * sqrtf(t);

	// Density curve: hold full for the first half of life so puffs read
	// as thick at the source, then ease down to 0 by death.
	float density;
	if (t < 0.5f) {
		density = 1.0f;
	} else {
		float tt = (t - 0.5f) / 0.5f;
		density = 1.0f - tt * tt;
	}

	float ramp_eff = pparticle->ramp * size_scale;
	int pix = ((int)(izi * ramp_eff)) >> d_pix_shift;
	{
		extern cvar_t r_smoke_size_min, r_smoke_size_max;
		int smoke_min = d_pix_max * (int)r_smoke_size_min.value;
		int smoke_max = d_pix_max * (int)r_smoke_size_max.value;
		if (smoke_max < smoke_min) smoke_max = smoke_min;
		if (pix < smoke_min) pix = smoke_min;
		if (pix > smoke_max) pix = smoke_max;
	}
	int rows = pix << d_y_aspect_shift;

	draw_smoke_blob(u, v, pix, rows, izi, density, 0u, (byte)pparticle->color);
}


/*
==============
draw_fire_blob

Solid-colour sibling of draw_smoke_blob: stamps a filled ellipse of (pix x
rows) pixels in `color`, z-tested and z-written against izi, straight into the
framebuffer (no fog-density accumulation). Backs the ADSR fire plume so each
ember is a small growing/shrinking blob rather than a 1px dot.
==============
*/
static void draw_fire_blob (int cu, int cv, int pix, int rows, int izi, byte color)
{
	if (pix < 1 || rows < 1) return;

	int u = cu - pix  / 2;
	int v = cv - rows / 2;

	int u0 = (u < d_vrectx)                    ? d_vrectx - u             : 0;
	int u1 = (u + pix  > r_refdef.vrectright)   ? r_refdef.vrectright  - u : pix;
	int v0 = (v < d_vrecty)                    ? d_vrecty - v             : 0;
	int v1 = (v + rows > r_refdef.vrectbottom)  ? r_refdef.vrectbottom - v : rows;
	if (u0 >= u1 || v0 >= v1) return;

	float rx = pix * 0.5f, ry = rows * 0.5f;
	float inv_rx2 = 1.0f / (rx * rx);
	float inv_ry2 = 1.0f / (ry * ry);

	short *pz    = d_pzbuffer   + d_zwidth * (v + v0) + u;
	byte  *pdest = d_viewbuffer + d_scantable[v + v0] + u;

	for (int row = v0; row < v1; row++, pz += d_zwidth, pdest += screenwidth)
	{
		float dy   = ((float)row + 0.5f) - ry;
		float dy2n = (dy * dy) * inv_ry2;
		for (int i = u0; i < u1; i++)
		{
			float dx = ((float)i + 0.5f) - rx;
			if (dx * dx * inv_rx2 + dy2n >= 1.0f) continue;
			if (pz[i] <= izi) { pz[i] = izi; pdest[i] = color; }
		}
	}
}


/*
==============
D_DrawFireParticle

pt_fireblob renderer: an ADSR (grow-then-shrink) size envelope keyed off the
fire ramp (0 at birth -> 6 at death). A fast attack to a peak early in life,
then a long release, so each ember puffs up and shrinks away rather than
holding a constant size. Colour is the ramp3 fire colour the physics step
already animates; r_fire_size sets the peak size.
==============
*/
void D_DrawFireParticle (particle_t *pparticle)
{
	vec3_t	local, transformed;

	VectorSubtract (pparticle->org, r_origin, local);
	transformed[0] = DotProduct(local, r_pright);
	transformed[1] = DotProduct(local, r_pup);
	transformed[2] = DotProduct(local, r_ppn);
	if (transformed[2] < PARTICLE_Z_CLIP)
		return;

	float zi  = 1.0f / transformed[2];
	int   u   = (int)(xcenter + zi * transformed[0] + 0.5f);
	int   v   = (int)(ycenter - zi * transformed[1] + 0.5f);
	int   izi = (int)(zi * 0x8000);

	// ADSR size: a in [0,1] over the ramp life; fast attack to a peak at
	// a=0.25, linear release to ~0 by death. Floor at 0.25 so it never
	// vanishes to sub-pixel before its colour has faded.
	float a = pparticle->ramp * (1.0f / 6.0f);
	if (a < 0) a = 0; else if (a > 1) a = 1;
	float env = (a < 0.25f) ? (a / 0.25f) : (1.0f - (a - 0.25f) / 0.75f);
	float size_scale = 0.25f + 0.75f * env;

	extern cvar_t r_fire_size;
	float ramp_eff = r_fire_size.value * size_scale;
	int pix = ((int)(izi * ramp_eff)) >> d_pix_shift;
	if (pix < 1) pix = 1;
	if (pix > d_pix_max * 3) pix = d_pix_max * 3;
	int rows = pix << d_y_aspect_shift;

	draw_fire_blob (u, v, pix, rows, izi, (byte)pparticle->color);
}


/*
==============
Smoke-cell voxel-haze (Phase 8).

sim_wind calls eng->Draw_SmokeCell(pos, world_size, density) once per
filled smoke cell per frame. Requests are buffered and rasterised at the
end of R_DrawParticles, so the framebuffer + z-buffer are valid and the
puffs depth-test against world geometry. Each entry is a world-space
cube of side world_size; we draw it as a screen-aligned square sized to
its projected extent and stipple by a hashed threshold against density,
so a thin cell shows ~25 % of pixels and a saturated cell shows ~100 %.
Bounded; overflow silently drops late cells (the field is overcomplete
by design).
==============
*/
typedef struct {
	vec3_t	org;
	float	world_size;
	float	density;
} smoke_cell_t;

#define MAX_SMOKE_CELLS  4096
static smoke_cell_t s_smoke_cells[MAX_SMOKE_CELLS];
static int          s_smoke_cell_count = 0;

void R_QueueSmokeCell (const float *org, float world_size, float density)
{
	if (s_smoke_cell_count >= MAX_SMOKE_CELLS) return;
	smoke_cell_t *c = &s_smoke_cells[s_smoke_cell_count++];
	c->org[0] = org[0]; c->org[1] = org[1]; c->org[2] = org[2];
	c->world_size = world_size;
	c->density    = density;
}

void D_DrawSmokeCells (void)
{
	extern cvar_t r_smoke_cell_mode;
	if (r_smoke_cell_mode.value == 0.0f) { s_smoke_cell_count = 0; return; }

	// Palette index for the haze. 8 = mid-grey in id1; matches sim_wind's
	// existing R_AddSmokePuff color so the hero puffs blend in.
	const byte smoke_color = 8;

	// Each cell rasterises 1.5x its world side so neighbours overlap, and
	// inside the rect the density falls off radially in screen space:
	// solid out to 50 % of the radius, smoothstep to 0 by 100 %. The dither
	// hash also salts on the cell's world origin, so two overlapping cells
	// stipple different pixels — together they cover the gap that hard
	// per-cell rects left in v0.
	const float OVERSIZE = 2.0f;

	for (int n = 0; n < s_smoke_cell_count; n++)
	{
		smoke_cell_t *c = &s_smoke_cells[n];
		vec3_t local, transformed;
		VectorSubtract(c->org, r_origin, local);
		transformed[0] = DotProduct(local, r_pright);
		transformed[1] = DotProduct(local, r_pup);
		transformed[2] = DotProduct(local, r_ppn);
		if (transformed[2] < PARTICLE_Z_CLIP) continue;

		float zi = 1.0f / transformed[2];
		int cu = (int)(xcenter + zi * transformed[0] + 0.5f);
		int cv = (int)(ycenter - zi * transformed[1] + 0.5f);
		int izi = (int)(zi * 0x8000);

		// r_pright/r_pup are scaled by x/yscaleshrink, so the cell width
		// in world units projects to (world_size * xscaleshrink / depth).
		float draw_size = c->world_size * OVERSIZE;
		int pix  = (int)(draw_size * xscaleshrink * zi + 0.5f);
		int rows = (int)(draw_size * yscaleshrink * zi + 0.5f);

		// Salt off the cell's world origin, quantised to the grid so it
		// stays stable across frames (cells don't move but cheap insurance).
		unsigned salt = ((unsigned)(int)(c->org[0] * 0.5f) * 2246822519u) ^
		                ((unsigned)(int)(c->org[1] * 0.5f) * 3266489917u) ^
		                ((unsigned)(int)(c->org[2] * 0.5f) * 668265263u);

		draw_smoke_blob(cu, cv, pix, rows, izi, c->density, salt, smoke_color);
	}

	s_smoke_cell_count = 0;
}


/*
==============
D_DrawEmitterParticle

Data-driven emitter particle (pt_emitter). Samples the def's palette ramp and
size envelope at the particle's normalized age, then draws via the style's
existing rasteriser:
  STYLE_DOT   -> classic distance-scaled splat (set color, reuse D_DrawParticle)
  STYLE_BLOB  -> solid ellipse (draw_fire_blob), size from the envelope
  STYLE_SMOKE -> fog-density ellipse (draw_smoke_blob), size from the envelope
==============
*/
void D_DrawEmitterParticle (particle_t *pparticle)
{
	emitter_def_t *d = R_EmitterGetDef(pparticle->def);
	vec3_t local, transformed;
	float life, t, zi, ramp_eff;
	int u, v, izi, pix, rows;

	if (!d) return;

	// normalized age
	life = pparticle->die - pparticle->birth;
	t    = (life > 0.001f) ? (cl.time - pparticle->birth) / life : 1.0f;
	if (t < 0) t = 0; else if (t > 1) t = 1;

	// color from ramp (write into the particle so the DOT path picks it up)
	pparticle->color = R_EmitterRampColor(d, t);

	if (d->style == STYLE_DOT) {
		D_DrawParticle(pparticle);
		return;
	}

	// project for blob/smoke
	VectorSubtract (pparticle->org, r_origin, local);
	transformed[0] = DotProduct(local, r_pright);
	transformed[1] = DotProduct(local, r_pup);
	transformed[2] = DotProduct(local, r_ppn);
	if (transformed[2] < PARTICLE_Z_CLIP) return;

	zi  = 1.0f / transformed[2];
	u   = (int)(xcenter + zi * transformed[0] + 0.5f);
	v   = (int)(ycenter - zi * transformed[1] + 0.5f);
	izi = (int)(zi * 0x8000);

	ramp_eff = R_EmitterSizeEnv(d, t);
	pix = ((int)(izi * ramp_eff)) >> d_pix_shift;
	if (pix < 1) pix = 1;
	if (pix > d_pix_max * 3) pix = d_pix_max * 3;
	rows = pix << d_y_aspect_shift;

	if (d->style == STYLE_SMOKE) {
		float tt = (t - 0.5f) / 0.5f;
		float density = (t < 0.5f) ? 1.0f : (1.0f - tt * tt);
		draw_smoke_blob(u, v, pix, rows, izi, density, 0u, (byte)pparticle->color);
	} else { // STYLE_BLOB
		draw_fire_blob(u, v, pix, rows, izi, (byte)pparticle->color);
	}
}

