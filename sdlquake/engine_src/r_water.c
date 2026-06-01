// r_water.c — depth-graded sheen for liquid surfaces (software renderer).
// See r_water.h. Built like the fog colormap (r_fog.c), but the row is chosen
// by distance (a fresnel proxy for horizontal water) rather than fog density,
// and may be nudged per-pixel by the warp for ripple glints. Surface tint only.

#include "quakedef.h"
#include "r_water.h"
#include <math.h>
#include <limits.h>

extern cvar_t r_drawflat;   // defined in r_main.c; sheen auto-disables under it

cvar_t r_water_sheen       = {"r_water_sheen",       "0.4",   false};
cvar_t r_water_sheen_red   = {"r_water_sheen_red",   "0.80",  false};
cvar_t r_water_sheen_green = {"r_water_sheen_green", "0.90",  false};
cvar_t r_water_sheen_blue  = {"r_water_sheen_blue",  "1.0",   false};
cvar_t r_water_sheen_dist  = {"r_water_sheen_dist",  "0.004", false};
cvar_t r_water_ripple      = {"r_water_ripple",      "0.3",   false};

unsigned char r_water_sheenmap[64*256];
int r_water_sheen_active = 0;
int r_water_ripple_scale = 0;

// Ripple: d_scan computes  off = (((turb - AMP) >> 8) * r_water_ripple_scale) >> 16
// where (turb - AMP) is the centered turbulence in 16.16, range [-8<<16, +8<<16]
// (AMP = 8<<16). We want +-RIPPLE_MAX_ROWS rows at ripple strength 1.0:
//   scale = RIPPLE_MAX_ROWS * (1<<16) / ((8<<16) >> 8) = RIPPLE_MAX_ROWS * 32.
#define RIPPLE_MAX_ROWS 12

static float cached_strength = -1.0f;
static float cached_r = -1.0f, cached_g = -1.0f, cached_b = -1.0f;


static int FindClosestPaletteIndex (int r, int g, int b)
{
	int		i, best_i = 0;
	long	best_d = LONG_MAX;

	for (i = 0; i < 256; i++)
	{
		long dr = host_basepal[i*3 + 0] - r;
		long dg = host_basepal[i*3 + 1] - g;
		long db = host_basepal[i*3 + 2] - b;
		long d  = dr*dr + dg*dg + db*db;
		if (d < best_d) { best_d = d; best_i = i; }
	}
	return best_i;
}


static void BuildSheenmap (float strength, float sr, float sg, float sb)
{
	int level, c;
	int shr, shg, shb;

	shr = (int)(sr * 255.0f + 0.5f);
	shg = (int)(sg * 255.0f + 0.5f);
	shb = (int)(sb * 255.0f + 0.5f);
	if (shr < 0) shr = 0; else if (shr > 255) shr = 255;
	if (shg < 0) shg = 0; else if (shg > 255) shg = 255;
	if (shb < 0) shb = 0; else if (shb > 255) shb = 255;

	if (strength < 0.0f) strength = 0.0f; else if (strength > 1.0f) strength = 1.0f;

	for (level = 0; level < 64; level++)
	{
		float t = ((float)level / 63.0f) * strength;   // row 0 = identity (t = 0)
		for (c = 0; c < 256; c++)
		{
			int pr = host_basepal[c*3 + 0];
			int pg = host_basepal[c*3 + 1];
			int pb = host_basepal[c*3 + 2];
			int br = (int)(pr + (shr - pr) * t + 0.5f);
			int bg = (int)(pg + (shg - pg) * t + 0.5f);
			int bb = (int)(pb + (shb - pb) * t + 0.5f);
			r_water_sheenmap[level*256 + c] = (unsigned char)FindClosestPaletteIndex (br, bg, bb);
		}
	}
}


void R_Water_Update (void)
{
	float s  = r_water_sheen.value;
	float sr = r_water_sheen_red.value;
	float sg = r_water_sheen_green.value;
	float sb = r_water_sheen_blue.value;
	float rip;

	if (s != cached_strength || sr != cached_r || sg != cached_g || sb != cached_b)
	{
		cached_strength = s; cached_r = sr; cached_g = sg; cached_b = sb;
		BuildSheenmap (s, sr, sg, sb);
	}

	r_water_sheen_active = (s > 0.00001f) && (r_drawflat.value == 0.0f);

	rip = r_water_ripple.value;
	if (rip < 0.0f) rip = 0.0f;
	r_water_ripple_scale = (int)(rip * (float)(RIPPLE_MAX_ROWS * 32));
}


void R_Water_Init (void)
{
	Cvar_RegisterVariable (&r_water_sheen);
	Cvar_RegisterVariable (&r_water_sheen_red);
	Cvar_RegisterVariable (&r_water_sheen_green);
	Cvar_RegisterVariable (&r_water_sheen_blue);
	Cvar_RegisterVariable (&r_water_sheen_dist);
	Cvar_RegisterVariable (&r_water_ripple);

	BuildSheenmap (r_water_sheen.value, r_water_sheen_red.value,
	               r_water_sheen_green.value, r_water_sheen_blue.value);
	cached_strength = r_water_sheen.value;
	cached_r = r_water_sheen_red.value;
	cached_g = r_water_sheen_green.value;
	cached_b = r_water_sheen_blue.value;

	R_Water_Update ();
}


// Floor row + Bayer threshold from Euclidean eye->liquid distance. Mirrors
// R_Fog_GetRows but (a) takes distance, not 1/z, so the gradient is
// rotation-invariant (doesn't swim as you turn), and (b) returns a row index
// (we add the per-pixel ripple offset before indexing the LUT), not a pointer.
void R_Water_GetRows (float dist, int *base_lo, int *thresh4)
{
	float f, row_f, frac;
	int   lo;

	if (dist < 0.0f) dist = 0.0f;
	f = 1.0f - expf (-r_water_sheen_dist.value * dist);   // near -> 0, far -> 1
	if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;

	row_f = f * 63.0f;
	lo = (int)row_f;
	if (lo > 62) lo = 62;                // so base+1 stays <= 63
	frac = row_f - (float)lo;

	*base_lo = lo;
	*thresh4 = (int)(frac * 4.0f);       // 0..3
	if (*thresh4 > 3) *thresh4 = 3;
}
