// r_caustics.c — underwater caustics for the software renderer. See r_caustics.h.
// A tileable ridge texture is baked once; each frame two scroll offsets animate
// it. In the solid-surface span filler two samples are multiplied (caustic
// networks are where two wave systems constructively interfere) into a 0..63
// level that brightens the texel toward a light blue-white via r_caustic_map.

#include "quakedef.h"
#include "r_caustics.h"
#include <math.h>
#include <limits.h>

extern mleaf_t *r_viewleaf;   // set each frame in R_SetupFrame (r_misc.c)

cvar_t r_caustics           = {"r_caustics",           "1",   false};
cvar_t r_caustics_intensity = {"r_caustics_intensity", "0.7", false};
cvar_t r_caustics_scale     = {"r_caustics_scale",     "2",   false};
cvar_t r_caustics_speed     = {"r_caustics_speed",     "12",  false};

unsigned char r_caustic_tex[CAUSTIC_SIZE * CAUSTIC_SIZE];
unsigned char r_caustic_map[64 * 256];

int r_caustics_active = 0;
int r_caustic_shift = 20;
int r_caustic_ox1, r_caustic_oy1, r_caustic_ox2, r_caustic_oy2;

// caustic highlight color (bright light blue-white; must punch through the
// brown underwater palette shift)
#define CAUSTIC_R 225
#define CAUSTIC_G 238
#define CAUSTIC_B 255

static float cached_intensity = -1.0f;


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


// Tileable (integer sine frequencies) wavy field, squared so it is mostly dark
// with sparse bright ridges; multiplying two scrolled copies then yields the
// sparse bright caustic cells.
static void BuildCausticTex (void)
{
	int x, y;

	for (y = 0; y < CAUSTIC_SIZE; y++)
	{
		for (x = 0; x < CAUSTIC_SIZE; x++)
		{
			float fx = (float)x / CAUSTIC_SIZE * 6.2831853f;
			float fy = (float)y / CAUSTIC_SIZE * 6.2831853f;
			float v  = sinf (fx*2.0f) + sinf (fy*3.0f) + sinf ((fx + fy)*2.0f)
			           + sinf ((fx - fy)*3.0f);                 // [-4,4], tileable
			float n  = (v + 4.0f) * 0.125f;                     // [0,1]
			float r  = n * n; r = r * r;                        // n^4: sparse bright ridges
			int   iv = (int)(r * 63.0f + 0.5f);
			if (iv < 0) iv = 0; else if (iv > 63) iv = 63;
			r_caustic_tex[y*CAUSTIC_SIZE + x] = (unsigned char)iv;
		}
	}
}


static void BuildCausticMap (float intensity)
{
	int level, c;

	if (intensity < 0.0f) intensity = 0.0f; else if (intensity > 1.0f) intensity = 1.0f;

	for (level = 0; level < 64; level++)
	{
		float t = ((float)level / 63.0f) * intensity;   // level 0 = identity
		for (c = 0; c < 256; c++)
		{
			int pr = host_basepal[c*3 + 0];
			int pg = host_basepal[c*3 + 1];
			int pb = host_basepal[c*3 + 2];
			int br = (int)(pr + (CAUSTIC_R - pr) * t + 0.5f);
			int bg = (int)(pg + (CAUSTIC_G - pg) * t + 0.5f);
			int bb = (int)(pb + (CAUSTIC_B - pb) * t + 0.5f);
			r_caustic_map[level*256 + c] = (unsigned char)FindClosestPaletteIndex (br, bg, bb);
		}
	}
}


void R_Caustics_Update (void)
{
	float intensity = r_caustics_intensity.value;
	int   sc;
	float ph;

	if (intensity != cached_intensity)
	{
		cached_intensity = intensity;
		BuildCausticMap (intensity);
	}

	r_caustics_active = (r_caustics.value != 0.0f)
		&& r_viewleaf && (r_viewleaf->contents == CONTENTS_WATER);

	if (!r_caustics_active)
		return;

	sc = (int)r_caustics_scale.value;       // bigger = coarser cells
	if (sc < 0) sc = 0; else if (sc > 12) sc = 12;
	r_caustic_shift = 16 + sc;

	// two layers scroll in different directions, animated by cl.time
	ph = (float)cl.time * r_caustics_speed.value;
	r_caustic_ox1 =  ((int)(ph))        & CAUSTIC_MASK;
	r_caustic_oy1 =  ((int)(ph * 0.6f)) & CAUSTIC_MASK;
	r_caustic_ox2 = (-(int)(ph * 0.8f)) & CAUSTIC_MASK;
	r_caustic_oy2 =  ((int)(ph * 1.1f)) & CAUSTIC_MASK;
}


void R_Caustics_Init (void)
{
	Cvar_RegisterVariable (&r_caustics);
	Cvar_RegisterVariable (&r_caustics_intensity);
	Cvar_RegisterVariable (&r_caustics_scale);
	Cvar_RegisterVariable (&r_caustics_speed);

	BuildCausticTex ();
	BuildCausticMap (r_caustics_intensity.value);
	cached_intensity = r_caustics_intensity.value;
}
