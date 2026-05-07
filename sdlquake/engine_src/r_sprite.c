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
// r_sprite.c

#include "quakedef.h"
#include "r_local.h"

static int				clip_current;
static vec5_t			clip_verts[2][MAXWORKINGVERTS];
static int				sprite_width, sprite_height;

spritedesc_t			r_spritedesc;
	

/*
================
R_RotateSprite
================
*/
void R_RotateSprite (float beamlength)
{
	vec3_t	vec;
	
	if (beamlength == 0.0)
		return;

	VectorScale (r_spritedesc.vpn, -beamlength, vec);
	VectorAdd (r_entorigin, vec, r_entorigin);
	VectorSubtract (modelorg, vec, modelorg);
}


/*
=============
R_ClipSpriteFace

Clips the winding at clip_verts[clip_current] and changes clip_current
Throws out the back side
==============
*/
int R_ClipSpriteFace (int nump, clipplane_t *pclipplane)
{
	int		i, outcount;
	float	dists[MAXWORKINGVERTS+1];
	float	frac, clipdist, *pclipnormal;
	float	*in, *instep, *outstep, *vert2;

	clipdist = pclipplane->dist;
	pclipnormal = pclipplane->normal;
	
// calc dists
	if (clip_current)
	{
		in = clip_verts[1][0];
		outstep = clip_verts[0][0];
		clip_current = 0;
	}
	else
	{
		in = clip_verts[0][0];
		outstep = clip_verts[1][0];
		clip_current = 1;
	}
	
	instep = in;
	for (i=0 ; i<nump ; i++, instep += sizeof (vec5_t) / sizeof (float))
	{
		dists[i] = DotProduct (instep, pclipnormal) - clipdist;
	}
	
// handle wraparound case
	dists[nump] = dists[0];
	Q_memcpy (instep, in, sizeof (vec5_t));


// clip the winding
	instep = in;
	outcount = 0;

	for (i=0 ; i<nump ; i++, instep += sizeof (vec5_t) / sizeof (float))
	{
		if (dists[i] >= 0)
		{
			Q_memcpy (outstep, instep, sizeof (vec5_t));
			outstep += sizeof (vec5_t) / sizeof (float);
			outcount++;
		}

		if (dists[i] == 0 || dists[i+1] == 0)
			continue;

		if ( (dists[i] > 0) == (dists[i+1] > 0) )
			continue;
			
	// split it into a new vertex
		frac = dists[i] / (dists[i] - dists[i+1]);
			
		vert2 = instep + sizeof (vec5_t) / sizeof (float);
		
		outstep[0] = instep[0] + frac*(vert2[0] - instep[0]);
		outstep[1] = instep[1] + frac*(vert2[1] - instep[1]);
		outstep[2] = instep[2] + frac*(vert2[2] - instep[2]);
		outstep[3] = instep[3] + frac*(vert2[3] - instep[3]);
		outstep[4] = instep[4] + frac*(vert2[4] - instep[4]);

		outstep += sizeof (vec5_t) / sizeof (float);
		outcount++;
	}	
	
	return outcount;
}


/*
================
R_SetupAndDrawSprite
================
*/
void R_SetupAndDrawSprite ()
{
	int			i, nump;
	float		dot, scale, *pv;
	vec5_t		*pverts;
	vec3_t		left, up, right, down, transformed, local;
	emitpoint_t	outverts[MAXWORKINGVERTS+1], *pout;

	dot = DotProduct (r_spritedesc.vpn, modelorg);

// backface cull
	if (dot >= 0)
		return;

// build the sprite poster in worldspace
	VectorScale (r_spritedesc.vright, r_spritedesc.pspriteframe->right, right);
	VectorScale (r_spritedesc.vup, r_spritedesc.pspriteframe->up, up);
	VectorScale (r_spritedesc.vright, r_spritedesc.pspriteframe->left, left);
	VectorScale (r_spritedesc.vup, r_spritedesc.pspriteframe->down, down);

	pverts = clip_verts[0];

	pverts[0][0] = r_entorigin[0] + up[0] + left[0];
	pverts[0][1] = r_entorigin[1] + up[1] + left[1];
	pverts[0][2] = r_entorigin[2] + up[2] + left[2];
	pverts[0][3] = 0;
	pverts[0][4] = 0;

	pverts[1][0] = r_entorigin[0] + up[0] + right[0];
	pverts[1][1] = r_entorigin[1] + up[1] + right[1];
	pverts[1][2] = r_entorigin[2] + up[2] + right[2];
	pverts[1][3] = sprite_width;
	pverts[1][4] = 0;

	pverts[2][0] = r_entorigin[0] + down[0] + right[0];
	pverts[2][1] = r_entorigin[1] + down[1] + right[1];
	pverts[2][2] = r_entorigin[2] + down[2] + right[2];
	pverts[2][3] = sprite_width;
	pverts[2][4] = sprite_height;

	pverts[3][0] = r_entorigin[0] + down[0] + left[0];
	pverts[3][1] = r_entorigin[1] + down[1] + left[1];
	pverts[3][2] = r_entorigin[2] + down[2] + left[2];
	pverts[3][3] = 0;
	pverts[3][4] = sprite_height;

// clip to the frustum in worldspace
	nump = 4;
	clip_current = 0;

	for (i=0 ; i<4 ; i++)
	{
		nump = R_ClipSpriteFace (nump, &view_clipplanes[i]);
		if (nump < 3)
			return;
		if (nump >= MAXWORKINGVERTS)
			Sys_Error("R_SetupAndDrawSprite: too many points");
	}

// transform vertices into viewspace and project
	pv = &clip_verts[clip_current][0][0];
	r_spritedesc.nearzi = -999999;

	for (i=0 ; i<nump ; i++)
	{
		VectorSubtract (pv, r_origin, local);
		TransformVector (local, transformed);

		if (transformed[2] < NEAR_CLIP)
			transformed[2] = NEAR_CLIP;

		pout = &outverts[i];
		pout->zi = 1.0 / transformed[2];
		if (pout->zi > r_spritedesc.nearzi)
			r_spritedesc.nearzi = pout->zi;

		pout->s = pv[3];
		pout->t = pv[4];
		
		scale = xscale * pout->zi;
		pout->u = (xcenter + scale * transformed[0]);

		scale = yscale * pout->zi;
		pout->v = (ycenter - scale * transformed[1]);

		pv += sizeof (vec5_t) / sizeof (*pv);
	}

// draw it
	r_spritedesc.nump = nump;
	r_spritedesc.pverts = outverts;
	D_DrawSprite ();
}


/*
================
R_GetSpriteframe
================
*/
mspriteframe_t *R_GetSpriteframe (msprite_t *psprite)
{
	mspritegroup_t	*pspritegroup;
	mspriteframe_t	*pspriteframe;
	int				i, numframes, frame;
	float			*pintervals, fullinterval, targettime, time;

	frame = currententity->frame;

	if ((frame >= psprite->numframes) || (frame < 0))
	{
		Con_Printf ("R_DrawSprite: no such frame %d\n", frame);
		frame = 0;
	}

	if (psprite->frames[frame].type == SPR_SINGLE)
	{
		pspriteframe = psprite->frames[frame].frameptr;
	}
	else
	{
		pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		pintervals = pspritegroup->intervals;
		numframes = pspritegroup->numframes;
		fullinterval = pintervals[numframes-1];

		time = cl.time + currententity->syncbase;

	// when loading in Mod_LoadSpriteGroup, we guaranteed all interval values
	// are positive, so we don't have to worry about division by 0
		targettime = time - ((int)(time / fullinterval)) * fullinterval;

		for (i=0 ; i<(numframes-1) ; i++)
		{
			if (pintervals[i] > targettime)
				break;
		}

		pspriteframe = pspritegroup->frames[i];
	}

	return pspriteframe;
}


/*
================
R_BlitSpriteScreen

PHASE 6 PATCH: paletted screen-space blit with transparent-pixel skip.
No world-space transform, no perspective, no z-buffer — just copy a
sprite frame's pixels straight into vid.buffer at (sx, sy). Bytes equal
to TRANSPARENT_COLOR (0xFF) are skipped. Caller is responsible for
clipping; this function returns silently if the sprite would extend
past the framebuffer.

Modeled on draw.c:Draw_TransPic (which does the same thing for HUD pics).
================
*/
extern byte vid_palette_id[];	// from sdlquake/platform/vid_sdl.c

void R_BlitSpriteScreen (int sx, int sy, mspriteframe_t *frame, byte palette_id)
{
	int		w, h, u, v;
	int		u_start, u_end, v_start, v_end;
	byte	*dest, *source, *pal_dest, *src_row, tbyte;

	if (r_pixbytes != 1)
		return;		// 16-bit color path not supported for viewmodel sprites yet

	w = frame->width;
	h = frame->height;
	if (w <= 0 || h <= 0)
		return;

	// Per-pixel clip to the 3D viewport (vrect), not the full vid rect.
	// The viewmodel must not write into the sbar zone, because this routine
	// also stamps vid_palette_id with the model's palette tag — and Sbar_Draw
	// only overwrites vid.buffer, never vid_palette_id, so any pixel we tag
	// in the sbar zone gets re-coloured through the wrong palette LUT in
	// VID_Update even after the sbar overdraws it. Clipping at vrect is also
	// the correct Doom-style visual: peak weapon bob makes the gun's bottom
	// rows disappear at the sbar top edge, exactly like Doom hiding the gun
	// behind the status bar.
	{
		int x_min = r_refdef.vrect.x;
		int y_min = r_refdef.vrect.y;
		int x_max = r_refdef.vrect.x + r_refdef.vrect.width;
		int y_max = r_refdef.vrect.y + r_refdef.vrect.height;
		u_start = (sx < x_min) ? x_min - sx : 0;
		u_end   = (sx + w > x_max) ? x_max - sx : w;
		v_start = (sy < y_min) ? y_min - sy : 0;
		v_end   = (sy + h > y_max) ? y_max - sy : h;
	}
	if (u_start >= u_end || v_start >= v_end)
		return;		// fully off-screen

	source = (byte *)&frame->pixels[0];

	for (v = v_start; v < v_end; v++)
	{
		dest     = vid.buffer     + (sy + v) * vid.rowbytes + (sx + u_start);
		pal_dest = vid_palette_id + (sy + v) * vid.width    + (sx + u_start);
		src_row  = source         +  v       * w            +       u_start;
		for (u = 0; u < u_end - u_start; u++)
		{
			if ((tbyte = src_row[u]) != TRANSPARENT_COLOR)
			{
				dest[u]     = tbyte;
				pal_dest[u] = palette_id;
			}
		}
	}
}


/*
================
R_DrawViewModelSprite

PHASE 6 PATCH: route a held-weapon entity whose model is a sprite (.spr)
through screen-space blit instead of the world-space sprite renderer.
Anchors the sprite bottom-center; the status bar overlays its lower
portion the same way it does the 3D MDL viewmodel.
================
*/
// Doom-style weapon bob — replicates A_WeaponReady's psp->sx/sy update from
// p_pspr.c. Horizontal axis sways with cosine; vertical axis dips with |sine|
// (Doom's WEAPONTOP+|sin| means the weapon only ever falls below its rest
// position, never rising above it). Magnitude scales with horizontal speed.
//
// Numbers chosen to match Doom's feel at 320×200: full cycle ≈ 1.83 s
// (Doom: angle = 128*leveltime mod FINEANGLES at 35 Hz → 1.829 s/cycle),
// max amplitude 16 px (Doom MAXBOB), saturating around 320 u/s ≈ Quake run
// speed.
#define DOOM_BOB_PERIOD     1.83f
#define DOOM_BOB_MAX        16.0f
#define DOOM_BOB_REF_SPEED  320.0f

// Bob phase accumulator. Advanced only when !cl.paused so the gun freezes
// during pause/console; r_doom_bob_last_time is updated unconditionally so
// resuming doesn't integrate the entire pause duration on the first frame.
static float  r_doom_bob_phase     = 0.0f;
static double r_doom_bob_last_time = 0.0;

static float R_DoomViewBobAmount (void)
{
	float vx = cl.velocity[0];
	float vy = cl.velocity[1];
	float speed = sqrt (vx*vx + vy*vy);
	float bob = speed * (DOOM_BOB_MAX / DOOM_BOB_REF_SPEED);
	if (bob > DOOM_BOB_MAX) bob = DOOM_BOB_MAX;
	return bob;
}

void R_DrawViewModelSprite (entity_t *e)
{
	msprite_t      *psprite;
	mspriteframe_t *frame;
	int             frame_idx;
	int             sx, sy;
	int             vp_x, vp_y, vp_w, vp_h;
	float           bob;

	if (!e->model || e->model->type != mod_sprite)
		return;

	// PHASE 6: skip the screen-space viewmodel sprite when the menu or console
	// is up. R_BlitSpriteScreen tags vid_palette_id with the model's palette
	// slot (1 = Doom for v_doom*.spr). Menu / console / chat overdraws write
	// Quake-palette indices into vid.buffer at the same pixels but never touch
	// vid_palette_id, so VID_Update dispatches the new pixels through the wrong
	// LUT. Easiest fix: don't paint the viewmodel sprite while an overlay is up
	// -- the player isn't actively aiming, and the overlay paints over a clean
	// (palette_id == 0) region.
	if (key_dest != key_game)
		return;
	if (scr_con_current > 0)
		return;

	psprite = e->model->cache.data;
	if (!psprite)
		return;

	frame_idx = e->frame;
	if (frame_idx < 0 || frame_idx >= psprite->numframes)
		frame_idx = 0;

	if (psprite->frames[frame_idx].type != SPR_SINGLE)
		return;		// animation-group viewmodels not supported (no Phase 6 weapon needs it)

	frame = psprite->frames[frame_idx].frameptr;
	if (!frame)
		return;

	// Anchor inside the 3D viewport so the rest position lines up with the
	// status bar (Sbar_Draw owns everything below vrect bottom).
	vp_x = r_refdef.vrect.x;
	vp_y = r_refdef.vrect.y;
	vp_w = r_refdef.vrect.width;
	vp_h = r_refdef.vrect.height;

	// Rest position: bottom-centered, gun bottom flush with the sbar top.
	// Doom's |sin| pulls the gun further down on the bob (sy increases),
	// briefly into sbar territory; Sbar_Draw runs after the viewmodel and
	// overdraws the sbar zone every frame (see vid.numpages bump in vid_sdl.c
	// — the original optimization that skipped redrawing the sbar relied on
	// VRAM-page persistence we don't have under SDL).
	sx = vp_x + (vp_w - frame->width) / 2;
	sy = vp_y +  vp_h - frame->height;

	// Heal `r_doom_bob_last_time` after a level reload (or any other time
	// warp where cl.time runs backward) so the next delta isn't a large
	// negative spike that snaps the phase.
	if (cl.time < r_doom_bob_last_time)
		r_doom_bob_last_time = cl.time;
	if (!cl.paused)
		r_doom_bob_phase += (float)((cl.time - r_doom_bob_last_time)
		                            * (2.0 * M_PI / DOOM_BOB_PERIOD));
	r_doom_bob_last_time = cl.time;

	bob = R_DoomViewBobAmount ();
	if (bob > 0.0f)
	{
		sx += (int)(bob * cos (r_doom_bob_phase));
		sy += (int)(bob * fabs (sin (r_doom_bob_phase)));
	}

	R_BlitSpriteScreen (sx, sy, frame, e->model->palette_id);
}


/*
================
R_DrawSprite
================
*/
void R_DrawSprite (void)
{
	int				i;
	msprite_t		*psprite;
	vec3_t			tvec;
	float			dot, angle, sr, cr;

	psprite = currententity->model->cache.data;

	r_spritedesc.pspriteframe = R_GetSpriteframe (psprite);

	sprite_width = r_spritedesc.pspriteframe->width;
	sprite_height = r_spritedesc.pspriteframe->height;

// TODO: make this caller-selectable
	if (psprite->type == SPR_FACING_UPRIGHT)
	{
	// generate the sprite's axes, with vup straight up in worldspace, and
	// r_spritedesc.vright perpendicular to modelorg.
	// This will not work if the view direction is very close to straight up or
	// down, because the cross product will be between two nearly parallel
	// vectors and starts to approach an undefined state, so we don't draw if
	// the two vectors are less than 1 degree apart
		tvec[0] = -modelorg[0];
		tvec[1] = -modelorg[1];
		tvec[2] = -modelorg[2];
		VectorNormalize (tvec);
		dot = tvec[2];	// same as DotProduct (tvec, r_spritedesc.vup) because
						//  r_spritedesc.vup is 0, 0, 1
		if ((dot > 0.999848) || (dot < -0.999848))	// cos(1 degree) = 0.999848
			return;
		r_spritedesc.vup[0] = 0;
		r_spritedesc.vup[1] = 0;
		r_spritedesc.vup[2] = 1;
		r_spritedesc.vright[0] = tvec[1];
								// CrossProduct(r_spritedesc.vup, -modelorg,
		r_spritedesc.vright[1] = -tvec[0];
								//              r_spritedesc.vright)
		r_spritedesc.vright[2] = 0;
		VectorNormalize (r_spritedesc.vright);
		r_spritedesc.vpn[0] = -r_spritedesc.vright[1];
		r_spritedesc.vpn[1] = r_spritedesc.vright[0];
		r_spritedesc.vpn[2] = 0;
					// CrossProduct (r_spritedesc.vright, r_spritedesc.vup,
					//  r_spritedesc.vpn)
	}
	else if (psprite->type == SPR_VP_PARALLEL)
	{
	// generate the sprite's axes, completely parallel to the viewplane. There
	// are no problem situations, because the sprite is always in the same
	// position relative to the viewer
		for (i=0 ; i<3 ; i++)
		{
			r_spritedesc.vup[i] = vup[i];
			r_spritedesc.vright[i] = vright[i];
			r_spritedesc.vpn[i] = vpn[i];
		}
	}
	else if (psprite->type == SPR_VP_PARALLEL_UPRIGHT)
	{
	// generate the sprite's axes, with vup straight up in worldspace, and
	// r_spritedesc.vright parallel to the viewplane.
	// This will not work if the view direction is very close to straight up or
	// down, because the cross product will be between two nearly parallel
	// vectors and starts to approach an undefined state, so we don't draw if
	// the two vectors are less than 1 degree apart
		dot = vpn[2];	// same as DotProduct (vpn, r_spritedesc.vup) because
						//  r_spritedesc.vup is 0, 0, 1
		if ((dot > 0.999848) || (dot < -0.999848))	// cos(1 degree) = 0.999848
			return;
		r_spritedesc.vup[0] = 0;
		r_spritedesc.vup[1] = 0;
		r_spritedesc.vup[2] = 1;
		r_spritedesc.vright[0] = vpn[1];
										// CrossProduct (r_spritedesc.vup, vpn,
		r_spritedesc.vright[1] = -vpn[0];	//  r_spritedesc.vright)
		r_spritedesc.vright[2] = 0;
		VectorNormalize (r_spritedesc.vright);
		r_spritedesc.vpn[0] = -r_spritedesc.vright[1];
		r_spritedesc.vpn[1] = r_spritedesc.vright[0];
		r_spritedesc.vpn[2] = 0;
					// CrossProduct (r_spritedesc.vright, r_spritedesc.vup,
					//  r_spritedesc.vpn)
	}
	else if (psprite->type == SPR_ORIENTED)
	{
	// generate the sprite's axes, according to the sprite's world orientation
		AngleVectors (currententity->angles, r_spritedesc.vpn,
					  r_spritedesc.vright, r_spritedesc.vup);
	}
	else if (psprite->type == SPR_VP_PARALLEL_ORIENTED)
	{
	// generate the sprite's axes, parallel to the viewplane, but rotated in
	// that plane around the center according to the sprite entity's roll
	// angle. So vpn stays the same, but vright and vup rotate
		angle = currententity->angles[ROLL] * (M_PI*2 / 360);
		sr = sin(angle);
		cr = cos(angle);

		for (i=0 ; i<3 ; i++)
		{
			r_spritedesc.vpn[i] = vpn[i];
			r_spritedesc.vright[i] = vright[i] * cr + vup[i] * sr;
			r_spritedesc.vup[i] = vright[i] * -sr + vup[i] * cr;
		}
	}
	else
	{
		Sys_Error ("R_DrawSprite: Bad sprite type %d", psprite->type);
	}

	R_RotateSprite (psprite->beamlength);

	R_SetupAndDrawSprite ();
}

