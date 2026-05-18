// r_fog.c — distance fog for the software renderer.
//
// Builds a 64-row palette LUT where row N maps every palette index to the
// nearest palette index of lerp(color, fog_color, N/63). Rasterizers pick a row
// per sub-span from 1/z and route the texel write through the LUT.

#include "quakedef.h"
#include "r_fog.h"
#include <math.h>
#include <limits.h>

cvar_t r_fog_density = {"r_fog_density", "0",    false};
cvar_t r_fog_red     = {"r_fog_red",     "0.5",  false};
cvar_t r_fog_green   = {"r_fog_green",   "0.5",  false};
cvar_t r_fog_blue    = {"r_fog_blue",    "0.55", false};

int            r_fog_active = 0;
int            r_fog_color_index = 0;
unsigned char *r_fog_row = NULL;

static unsigned char r_fog_colormap[64 * 256];

static float cached_density = -1.0f;
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


static void BuildFogColormap (float fr, float fg, float fb)
{
	int level, c;
	int fog_r, fog_g, fog_b;

	fog_r = (int)(fr * 255.0f + 0.5f);
	fog_g = (int)(fg * 255.0f + 0.5f);
	fog_b = (int)(fb * 255.0f + 0.5f);
	if (fog_r < 0) fog_r = 0; else if (fog_r > 255) fog_r = 255;
	if (fog_g < 0) fog_g = 0; else if (fog_g > 255) fog_g = 255;
	if (fog_b < 0) fog_b = 0; else if (fog_b > 255) fog_b = 255;

	r_fog_color_index = FindClosestPaletteIndex (fog_r, fog_g, fog_b);

	for (level = 0; level < 64; level++)
	{
		float t = (float)level / 63.0f;
		for (c = 0; c < 256; c++)
		{
			int pr = host_basepal[c*3 + 0];
			int pg = host_basepal[c*3 + 1];
			int pb = host_basepal[c*3 + 2];
			int br = (int)(pr + (fog_r - pr) * t + 0.5f);
			int bg = (int)(pg + (fog_g - pg) * t + 0.5f);
			int bb = (int)(pb + (fog_b - pb) * t + 0.5f);
			r_fog_colormap[level*256 + c] = (unsigned char)FindClosestPaletteIndex (br, bg, bb);
		}
	}
}


void R_Fog_Update (void)
{
	float d  = r_fog_density.value;
	float fr = r_fog_red.value;
	float fg = r_fog_green.value;
	float fb = r_fog_blue.value;

	if (fr != cached_r || fg != cached_g || fb != cached_b)
	{
		cached_r = fr; cached_g = fg; cached_b = fb;
		BuildFogColormap (fr, fg, fb);
	}
	cached_density = d;
	r_fog_active = (d > 0.00001f) ? 1 : 0;
}


void R_Fog_Init (void)
{
	Cvar_RegisterVariable (&r_fog_density);
	Cvar_RegisterVariable (&r_fog_red);
	Cvar_RegisterVariable (&r_fog_green);
	Cvar_RegisterVariable (&r_fog_blue);
	BuildFogColormap (r_fog_red.value, r_fog_green.value, r_fog_blue.value);
	cached_r = r_fog_red.value;
	cached_g = r_fog_green.value;
	cached_b = r_fog_blue.value;
	cached_density = r_fog_density.value;
	r_fog_active = (cached_density > 0.00001f) ? 1 : 0;
}


// Parses worldspawn "fog" key: "density r g b" (all floats).
void R_Fog_NewMap (void)
{
	char *data;

	Cvar_SetValue ("r_fog_density", 0);

	if (!cl.worldmodel || !cl.worldmodel->entities)
	{
		R_Fog_Update ();
		return;
	}

	data = COM_Parse (cl.worldmodel->entities);
	if (!data || com_token[0] != '{')
	{
		R_Fog_Update ();
		return;
	}

	while (1)
	{
		char keybuf[64];

		data = COM_Parse (data);
		if (!data || com_token[0] == '}')
			break;
		strncpy (keybuf, com_token, sizeof(keybuf) - 1);
		keybuf[sizeof(keybuf) - 1] = 0;

		data = COM_Parse (data);
		if (!data)
			break;

		if (!strcmp (keybuf, "fog"))
		{
			float density = 0, fr = 0.5f, fg = 0.5f, fb = 0.55f;
			if (sscanf (com_token, "%f %f %f %f", &density, &fr, &fg, &fb) >= 1)
			{
				Cvar_SetValue ("r_fog_density", density);
				Cvar_SetValue ("r_fog_red",     fr);
				Cvar_SetValue ("r_fog_green",   fg);
				Cvar_SetValue ("r_fog_blue",    fb);
			}
		}
	}

	R_Fog_Update ();
}


unsigned char *R_Fog_RowFromZi (float zi)
{
	float depth, f;
	int   idx;

	if (zi <= 1e-6f)
	{
		idx = 63;
	}
	else
	{
		depth = 1.0f / zi;
		if (depth < 0.0f) depth = 0.0f;
		f = 1.0f - expf (-r_fog_density.value * depth);
		idx = (int)(f * 63.0f + 0.5f);
		if (idx < 0) idx = 0;
		else if (idx > 63) idx = 63;
	}
	return r_fog_colormap + idx * 256;
}


// 2x2 Bayer matrix, normalized to [0,3]. Indexed by ((y&1)<<1)|(x&1).
//   (0, 2)
//   (3, 1)
const unsigned char r_fog_bayer2x2[4] = { 0, 2, 3, 1 };


void R_Fog_GetRows (float zi, unsigned char **row_lo,
                    unsigned char **row_hi, int *thresh4)
{
	float depth, f, row_f, frac;
	int   idx_lo;

	if (zi <= 1e-6f)
	{
		*row_lo  = r_fog_colormap + 63 * 256;
		*row_hi  = *row_lo;
		*thresh4 = 0;
		return;
	}

	depth = 1.0f / zi;
	if (depth < 0.0f) depth = 0.0f;
	f = 1.0f - expf (-r_fog_density.value * depth);
	if (f < 0.0f) f = 0.0f;
	else if (f > 1.0f) f = 1.0f;

	row_f  = f * 63.0f;
	idx_lo = (int)row_f;
	if (idx_lo > 62) idx_lo = 62;
	frac   = row_f - (float)idx_lo;

	*row_lo  = r_fog_colormap + idx_lo * 256;
	*row_hi  = r_fog_colormap + (idx_lo + 1) * 256;
	*thresh4 = (int)(frac * 4.0f);   /* 0..3; ==4 only if frac==1 which is clamped */
	if (*thresh4 > 3) *thresh4 = 3;
}
