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
// d_init.c: rasterization driver initialization

#include "quakedef.h"
#include "d_local.h"

#define NUM_MIPS	4

cvar_t	d_subdiv16 = {"d_subdiv16", "1"};
cvar_t	d_mipcap = {"d_mipcap", "0"};
// d_mipscale biases the per-surface mip ladder; thresholds in d_scalemip[]
// are multiplied by this value. Default is 1.3 (raised from id's 1.0): at
// 320x200 with nearest-neighbour sampling, mip 3 is still the floor and
// distant texels are undersampled — biasing transitions to coarser mips
// earlier reduces the resulting per-pixel shimmer as the camera turns,
// at the cost of slightly blurrier mid-range surfaces. Tune live in the
// imgui cvar panel.
cvar_t	d_mipscale = {"d_mipscale", "1.3"};
// r_mipdither toggles the Bayer-blend between adjacent mips for surfaces
// sitting in a transition band around d_scalemip[]. 0 = behaviour
// identical to bare per-surface mip selection.
cvar_t	r_mipdither      = {"r_mipdither", "1"};
// Half-width of the dither transition band as a fraction of the
// d_scalemip[] threshold value. 0.3 = band spans [0.7*t, 1.3*t].
// Bands cannot overlap because d_scalemip[] thresholds are ~2x apart.
cvar_t	r_mipdither_band = {"r_mipdither_band", "0.3"};

surfcache_t		*d_initial_rover;
qboolean		d_roverwrapped;
int				d_minmip;
float			d_scalemip[NUM_MIPS-1];

static float	basemip[NUM_MIPS-1] = {1.0, 0.5*0.8, 0.25*0.8};

extern int			d_aflatcolor;

void (*d_drawspans) (espan_t *pspan);


/*
===============
D_Init
===============
*/
void D_Init (void)
{

	r_skydirect = 1;

	Cvar_RegisterVariable (&d_subdiv16);
	Cvar_RegisterVariable (&d_mipcap);
	Cvar_RegisterVariable (&d_mipscale);
	Cvar_RegisterVariable (&r_mipdither);
	Cvar_RegisterVariable (&r_mipdither_band);

	r_drawpolys = false;
	r_worldpolysbacktofront = false;
	r_recursiveaffinetriangles = true;
	r_pixbytes = 1;
	r_aliasuvscale = 1.0;
}


/*
===============
D_CopyRects
===============
*/
void D_CopyRects (vrect_t *prects, int transparent)
{

// this function is only required if the CPU doesn't have direct access to the
// back buffer, and there's some driver interface function that the driver
// doesn't support and requires Quake to do in software (such as drawing the
// console); Quake will then draw into wherever the driver points vid.buffer
// and will call this function before swapping buffers

	UNUSED(prects);
	UNUSED(transparent);
}


/*
===============
D_EnableBackBufferAccess
===============
*/
void D_EnableBackBufferAccess (void)
{
	VID_LockBuffer ();
}


/*
===============
D_TurnZOn
===============
*/
void D_TurnZOn (void)
{
// not needed for software version
}


/*
===============
D_DisableBackBufferAccess
===============
*/
void D_DisableBackBufferAccess (void)
{
	VID_UnlockBuffer ();
}


/*
===============
D_SetupFrame
===============
*/
void D_SetupFrame (void)
{
	int		i;

	if (r_dowarp)
		d_viewbuffer = r_warpbuffer;
	else
		d_viewbuffer = (void *)(byte *)vid.buffer;

	if (r_dowarp)
		screenwidth = WARP_WIDTH;
	else
		screenwidth = vid.rowbytes;

	d_roverwrapped = false;
	d_initial_rover = sc_rover;

	d_minmip = d_mipcap.value;
	if (d_minmip > 3)
		d_minmip = 3;
	else if (d_minmip < 0)
		d_minmip = 0;

	for (i=0 ; i<(NUM_MIPS-1) ; i++)
		d_scalemip[i] = basemip[i] * d_mipscale.value;

#if	id386
				if (d_subdiv16.value)
					d_drawspans = D_DrawSpans16;
				else
					d_drawspans = D_DrawSpans8;
#else
				d_drawspans = D_DrawSpans8;
#endif

	d_aflatcolor = 0;
}


/*
===============
D_UpdateRects
===============
*/
void D_UpdateRects (vrect_t *prect)
{

// the software driver draws these directly to the vid buffer

	UNUSED(prect);
}

