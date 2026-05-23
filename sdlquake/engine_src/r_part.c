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

#include "quakedef.h"
#include "r_local.h"
#include "hotreload.h"   // g_game_api + game_api_t (NATIVE_GAME guard)
#include "debug_lines.h" // DebugLines_Add — debug overlay for r_particle_wind_debug
#include <math.h>        // expf

#define MAX_PARTICLES			32768	// default max # of particles at one
										//  time (raised for sim_wind smoke;
										//  smoke also yields to gameplay via
										//  SMOKE_GAMEPLAY_RESERVE below)
#define ABSOLUTE_MIN_PARTICLES	512		// no fewer than this no matter what's
										//  on the command line

// Smoke tunables — declared in r_main.c, registered in R_Init.
extern cvar_t r_smoke_lifetime;
extern cvar_t r_smoke_ramp_min;
extern cvar_t r_smoke_ramp_max;
extern cvar_t r_smoke_emit_div;

// Spark tunables — declared in r_main.c, registered in R_Init.
extern cvar_t r_sparks_count_mul;
extern cvar_t r_sparks_settle_dwell;
extern cvar_t r_sparks_restitution;

// Floor of free particles that R_AddSmokePuff refuses to dip below.
// Smoke is a low-priority visualization re-emitted every frame; gameplay
// FX (rocket trails, gibs, explosions) get clobbered if smoke drains the
// shared pool. When fewer than this many particles are free, smoke skips
// the burst and lets the next frame's gameplay spawns succeed.
#define SMOKE_GAMEPLAY_RESERVE	2048

int		ramp1[8] = {0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61};
int		ramp2[8] = {0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66};
int		ramp3[8] = {0x6d, 0x6b, 6, 5, 4, 3};

// Per-type wind drag coefficients.  Used by R_DrawParticles to lerp
// each active particle's velocity toward the locally-sampled wind
// velocity.  Higher k snaps faster; 0 disables for that type.
// Indexed by ptype_t (see d_iface.h).  pt_grav is excluded (heavy
// blood from gibs should not billow).
static const float wind_drag_k[] = {
    [pt_static]   = 3.0f,
    [pt_smoke]    = 1.5f,	// moderate drag -- puff holds its spawn impulse long enough to billow (~half a second), then drifts with the ambient wind
    [pt_fire]     = 0.4f,   // used by R_RocketTrail; spawns at zero vel,
                            // so any strong drag yanks the trail off the
                            // rocket's path. Keep this low.
    [pt_explode]  = 0.6f,
    [pt_explode2] = 0.6f,
    [pt_blob]     = 0.4f,
    [pt_blob2]    = 0.4f,
    [pt_grav]     = 0.0f,
    [pt_slowgrav] = 2.5f,
};

particle_t	*active_particles, *free_particles;

particle_t	*particles;
int			r_numparticles;

vec3_t			r_pright, r_pup, r_ppn;

// Smoke-cell voxel-haze rasteriser lives in d_part.c (D_DrawSmokeCells).
// R_QueueSmokeCell is defined there alongside the queue + struct.
// D_CompositeSmoke walks vid.buffer at end-of-frame and applies the fog
// colormap to every pixel that got smoke density written into it.
void D_DrawSmokeCells (void);
void D_CompositeSmoke (void);


/*
===============
R_InitParticles
===============
*/
void R_InitParticles (void)
{
	int		i;

	i = COM_CheckParm ("-particles");

	if (i)
	{
		r_numparticles = (int)(Q_atoi(com_argv[i+1]));
		if (r_numparticles < ABSOLUTE_MIN_PARTICLES)
			r_numparticles = ABSOLUTE_MIN_PARTICLES;
	}
	else
	{
		r_numparticles = MAX_PARTICLES;
	}

	particles = (particle_t *)
			Hunk_AllocName (r_numparticles * sizeof(particle_t), "particles");
}

#ifdef QUAKE2
void R_DarkFieldParticles (entity_t *ent)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;
	vec3_t		org;

	org[0] = ent->origin[0];
	org[1] = ent->origin[1];
	org[2] = ent->origin[2];
	for (i=-16 ; i<16 ; i+=8)
		for (j=-16 ; j<16 ; j+=8)
			for (k=0 ; k<32 ; k+=8)
			{
				if (!free_particles)
					return;
				p = free_particles;
				free_particles = p->next;
				p->next = active_particles;
				active_particles = p;
		
				p->die = cl.time + 0.2 + (rand()&7) * 0.02;
				p->color = 150 + rand()%6;
				p->type = pt_slowgrav;
				
				dir[0] = j*8;
				dir[1] = i*8;
				dir[2] = k*8;
	
				p->org[0] = org[0] + i + (rand()&3);
				p->org[1] = org[1] + j + (rand()&3);
				p->org[2] = org[2] + k + (rand()&3);
	
				VectorNormalize (dir);						
				vel = 50 + (rand()&63);
				VectorScale (dir, vel, p->vel);
			}
}
#endif


/*
===============
R_EntityParticles
===============
*/

#define NUMVERTEXNORMALS	162
extern	float	r_avertexnormals[NUMVERTEXNORMALS][3];
vec3_t	avelocities[NUMVERTEXNORMALS];
float	beamlength = 16;
vec3_t	avelocity = {23, 7, 3};
float	partstep = 0.01;
float	timescale = 0.01;

void R_EntityParticles (entity_t *ent)
{
	int			count;
	int			i;
	particle_t	*p;
	float		angle;
	float		sr, sp, sy, cr, cp, cy;
	vec3_t		forward;
	float		dist;
	
	dist = 64;
	count = 50;

if (!avelocities[0][0])
{
for (i=0 ; i<NUMVERTEXNORMALS*3 ; i++)
avelocities[0][i] = (rand()&255) * 0.01;
}


	for (i=0 ; i<NUMVERTEXNORMALS ; i++)
	{
		angle = cl.time * avelocities[i][0];
		sy = sin(angle);
		cy = cos(angle);
		angle = cl.time * avelocities[i][1];
		sp = sin(angle);
		cp = cos(angle);
		angle = cl.time * avelocities[i][2];
		sr = sin(angle);
		cr = cos(angle);
	
		forward[0] = cp*cy;
		forward[1] = cp*sy;
		forward[2] = -sp;

		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;

		p->die = cl.time + 0.01;
		p->color = 0x6f;
		p->type = pt_explode;
		
		p->org[0] = ent->origin[0] + r_avertexnormals[i][0]*dist + forward[0]*beamlength;			
		p->org[1] = ent->origin[1] + r_avertexnormals[i][1]*dist + forward[1]*beamlength;			
		p->org[2] = ent->origin[2] + r_avertexnormals[i][2]*dist + forward[2]*beamlength;			
	}
}


/*
===============
R_ClearParticles
===============
*/
void R_ClearParticles (void)
{
	int		i;
	
	free_particles = &particles[0];
	active_particles = NULL;

	for (i=0 ;i<r_numparticles ; i++)
		particles[i].next = &particles[i+1];
	particles[r_numparticles-1].next = NULL;
}


void R_ReadPointFile_f (void)
{
	FILE	*f;
	vec3_t	org;
	int		r;
	int		c;
	particle_t	*p;
	char	name[MAX_OSPATH];
	
	sprintf (name,"maps/%s.pts", sv.name);

	COM_FOpenFile (name, &f);
	if (!f)
	{
		Con_Printf ("couldn't open %s\n", name);
		return;
	}
	
	Con_Printf ("Reading %s...\n", name);
	c = 0;
	for ( ;; )
	{
		r = fscanf (f,"%f %f %f\n", &org[0], &org[1], &org[2]);
		if (r != 3)
			break;
		c++;
		
		if (!free_particles)
		{
			Con_Printf ("Not enough free particles\n");
			break;
		}
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;

		p->die = 99999;
		p->color = (-c)&15;
		p->type = pt_static;
		VectorCopy (vec3_origin, p->vel);
		VectorCopy (org, p->org);
	}

	fclose (f);
	Con_Printf ("%i points read\n", c);
}

/*
===============
R_ParseParticleEffect

Parse an effect out of the server message
===============
*/
void R_ParseParticleEffect (void)
{
	vec3_t		org, dir;
	int			i, count, msgcount, color;
	
	for (i=0 ; i<3 ; i++)
		org[i] = MSG_ReadCoord ();
	for (i=0 ; i<3 ; i++)
		dir[i] = MSG_ReadChar () * (1.0/16);
	msgcount = MSG_ReadByte ();
	color = MSG_ReadByte ();

if (msgcount == 255)
	count = 1024;
else
	count = msgcount;
	
	R_RunParticleEffect (org, dir, color, count);
}
	
/*
===============
R_ParticleExplosion

===============
*/
void R_ParticleExplosion (vec3_t org)
{
	int			i, j;
	particle_t	*p;
	
	for (i=0 ; i<1024 ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;

		p->die = cl.time + 5;
		p->color = ramp1[0];
		p->ramp = rand()&3;
		if (i & 1)
		{
			p->type = pt_explode;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
		else
		{
			p->type = pt_explode2;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
	}
}

/*
===============
R_ParticleExplosion2

===============
*/
void R_ParticleExplosion2 (vec3_t org, int colorStart, int colorLength)
{
	int			i, j;
	particle_t	*p;
	int			colorMod = 0;

	for (i=0; i<512; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;

		p->die = cl.time + 0.3;
		p->color = colorStart + (colorMod % colorLength);
		colorMod++;

		p->type = pt_blob;
		for (j=0 ; j<3 ; j++)
		{
			p->org[j] = org[j] + ((rand()%32)-16);
			p->vel[j] = (rand()%512)-256;
		}
	}
}

/*
===============
R_BlobExplosion

===============
*/
void R_BlobExplosion (vec3_t org)
{
	int			i, j;
	particle_t	*p;
	
	for (i=0 ; i<1024 ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;

		p->die = cl.time + 1 + (rand()&8)*0.05;

		if (i & 1)
		{
			p->type = pt_blob;
			p->color = 66 + rand()%6;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
		else
		{
			p->type = pt_blob2;
			p->color = 150 + rand()%6;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()%32)-16);
				p->vel[j] = (rand()%512)-256;
			}
		}
	}
}

/*
===============
R_AddSmokePuff

Smoke-specific particle spawn: pt_static (no gravity), long lifetime,
caller-controlled drift velocity with minimal random jitter. Used by the
sim_wind smoke field; bypasses the svc_particle protocol because the wire
format clamps direction to ±8 units and lifetime to ≤0.4s, both of which
are wrong for drifting smoke.
===============
*/
/*
===============
R_PushSmokeTube

Push every active pt_smoke particle that lies within `radius` perpendicular
distance of the axis-line through `origin` outward, perpendicular to axis,
with magnitude `mag * (1 - perp_dist/radius)`. Used by the game DLL's
Missile_SmokeWake to carve a visible tunnel through a smoke cloud as a
rocket flies; bypasses the wind grid because rockets cross a cloud in a
few hundred ms -- too fast for wind-drag to transport particles visibly.
Direct velocity kick gives the effect immediately.
===============
*/
void R_PushSmokeTube (const float *origin, const float *axis,
                     float mag, float radius)
{
	float alen2 = axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2];
	if (alen2 < 1.0f) return;
	float alen = sqrtf(alen2);
	float ax = axis[0]/alen, ay = axis[1]/alen, az = axis[2]/alen;
	float r2 = radius * radius;

	// Per-particle target speed: SET radial velocity toward this each
	// frame the particle is in range, rather than ADDing. Avoids the
	// "particle picked up 600 u/s over 8 frames then drifts forever"
	// problem -- once the rocket leaves, vel is just whatever the last
	// touch set, which then decays via the normal wind-drag in
	// R_DrawParticles. Knot: still kick the particle outward each frame
	// it's in range, but don't accumulate.
	for (particle_t *p = active_particles; p; p = p->next) {
		if (p->type != pt_smoke) continue;
		float rx = p->org[0] - origin[0];
		float ry = p->org[1] - origin[1];
		float rz = p->org[2] - origin[2];
		float dot = rx*ax + ry*ay + rz*az;
		rx -= dot * ax;
		ry -= dot * ay;
		rz -= dot * az;
		float perp2 = rx*rx + ry*ry + rz*rz;
		if (perp2 >= r2 || perp2 < 0.01f) continue;
		float perp = sqrtf(perp2);
		float falloff = 1.0f - (perp / radius);
		float target = mag * falloff;
		// Set the perpendicular component to `target` outward, preserve
		// the along-axis component of velocity (rocket shouldn't kill
		// natural updraft).
		float vdot = p->vel[0]*ax + p->vel[1]*ay + p->vel[2]*az;
		float axis_vx = vdot * ax;
		float axis_vy = vdot * ay;
		float axis_vz = vdot * az;
		float unit_x = rx / perp;
		float unit_y = ry / perp;
		float unit_z = rz / perp;
		p->vel[0] = axis_vx + unit_x * target;
		p->vel[1] = axis_vy + unit_y * target;
		p->vel[2] = axis_vz + unit_z * target;
	}
}

// Forward decl from world.c — keeps r_part.c off the full world.h include
// (which pulls server-side types). World-hull traceline only; no entity
// list, no contents-flag handling.
extern qboolean SV_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f,
                                       vec3_t p1, vec3_t p2, trace_t *trace);

/*
===============
R_TraceParticle

Thin wrapper over SV_RecursiveHullCheck on cl.worldmodel->hulls[0]. Used
by R_DrawParticles to test whether a particle's per-frame integration step
crosses a world brush surface. World-only (no entity collision); returns 1
on hit, 0 on clean traversal or when no worldmodel is loaded.
===============
*/
static int R_TraceParticle (const vec3_t start, const vec3_t end, trace_t *trace)
{
	vec3_t p1, p2;
	if (!cl.worldmodel) return 0;

	memset (trace, 0, sizeof(*trace));
	trace->fraction = 1.0f;

	VectorCopy (start, p1);
	VectorCopy (end,   p2);

	SV_RecursiveHullCheck (&cl.worldmodel->hulls[0], 0, 0.0f, 1.0f, p1, p2, trace);
	return (trace->fraction < 1.0f) ? 1 : 0;
}

void R_AddSmokePuff (vec3_t org, vec3_t dir, int color, int count)
{
	int			i, j;
	particle_t	*p;

	// Yield to gameplay particles when the pool is below the reserve.
	// Walks the free list with early-bail at reserve+count nodes.
	{
		int need = SMOKE_GAMEPLAY_RESERVE + count;
		particle_t *probe = free_particles;
		while (need > 0 && probe) { probe = probe->next; need--; }
		if (need > 0) return;
	}

	for (i=0 ; i<count ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;

		// Lifetime and ramp range are tunable via r_smoke_* cvars (F12
		// "Smoke" panel). ramp is the puff's TARGET (peak) size scale;
		// D_DrawSmokeParticle multiplies it by an age-driven growth curve
		// so the puff starts small and reaches `ramp` near the end of
		// its life. birth lets the draw side compute that age.
		{
			float ramp_min = r_smoke_ramp_min.value;
			float ramp_max = r_smoke_ramp_max.value;
			if (ramp_max < ramp_min) ramp_max = ramp_min;
			p->birth = cl.time;
			p->die = cl.time + r_smoke_lifetime.value + (rand() & 31) * 0.04;
			p->color = color;
			p->type = pt_smoke;
			p->ramp = ramp_min + (rand() & 31) * ((ramp_max - ramp_min) / 31.0f);
		}
		for (j=0 ; j<3 ; j++)
		{
			p->org[j] = org[j] + ((rand() & 7) - 4);	// ±4 unit jitter
			p->vel[j] = dir[j] + ((rand() & 15) - 8);	// dir + ±8 noise
		}
	}
}

/*
===============
R_RunParticleEffect

===============
*/
void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count)
{
	int			i, j;
	particle_t	*p;
	
	for (i=0 ; i<count ; i++)
	{
		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;

		if (count == 1024)
		{	// rocket explosion
			p->die = cl.time + 5;
			p->color = ramp1[0];
			p->ramp = rand()&3;
			if (i & 1)
			{
				p->type = pt_explode;
				for (j=0 ; j<3 ; j++)
				{
					p->org[j] = org[j] + ((rand()%32)-16);
					p->vel[j] = (rand()%512)-256;
				}
			}
			else
			{
				p->type = pt_explode2;
				for (j=0 ; j<3 ; j++)
				{
					p->org[j] = org[j] + ((rand()%32)-16);
					p->vel[j] = (rand()%512)-256;
				}
			}
		}
		else
		{
			p->die = cl.time + 0.1*(rand()%5);
			// Bolt cyan-white (244-246) is the only useful chunk inside
			// the 240-247 block — 240-243 is yellow, 247 is red — so the
			// default & ~7 mask poisons it. Special-case the lightning
			// range to a tight 3-wide pick so it matches the bolt skin.
			if (color >= 244 && color <= 246)
				p->color = 244 + (rand() % 3);
			else
				p->color = (color&~7) + (rand()&7);
			p->type = pt_slowgrav;
			for (j=0 ; j<3 ; j++)
			{
				p->org[j] = org[j] + ((rand()&15)-8);
				p->vel[j] = dir[j]*15 + (rand()%300)-150;
			}
		}
	}
}


/*
===============
R_LavaSplash

===============
*/
void R_LavaSplash (vec3_t org)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;

	for (i=-16 ; i<16 ; i++)
		for (j=-16 ; j<16 ; j++)
			for (k=0 ; k<1 ; k++)
			{
				if (!free_particles)
					return;
				p = free_particles;
				free_particles = p->next;
				p->next = active_particles;
				active_particles = p;
				p->flags = 0;

				p->die = cl.time + 2 + (rand()&31) * 0.02;
				p->color = 224 + (rand()&7);
				p->type = pt_slowgrav;
				
				dir[0] = j*8 + (rand()&7);
				dir[1] = i*8 + (rand()&7);
				dir[2] = 256;
	
				p->org[0] = org[0] + dir[0];
				p->org[1] = org[1] + dir[1];
				p->org[2] = org[2] + (rand()&63);
	
				VectorNormalize (dir);						
				vel = 50 + (rand()&63);
				VectorScale (dir, vel, p->vel);
			}
}

/*
===============
R_WaterSplash

Surface-splash burst for hitscan/projectile water impacts. Uses pt_grav so the
spray rises and falls under full gravity (no wind drag, no slow-grav float).
kind: 0=water (light cyan), 1=slime (green-grey), 2=lava (orange).
===============
*/
void R_WaterSplash (vec3_t org, int kind, int strength_q4)
{
	int			i, j;
	particle_t	*p;
	int			base_color;
	float		s;
	int			count;
	float		vz_lo, vz_hi;
	int			lat_range;

	switch (kind) {
	case 1:  base_color = 176; break; // slime: 176..183 green-grey
	case 2:  base_color = 232; break; // lava: 232..239 orange ramp
	default: base_color = 244; break; // water: light cyan special-case
	}

	if (strength_q4 < 4)   strength_q4 = 4;   // floor at 0.25x
	if (strength_q4 > 128) strength_q4 = 128; // ceil at 8x
	s = strength_q4 * (1.0f/16.0f);

	count     = (int)(28 * s);
	vz_lo     = 180.0f * s;
	vz_hi     = 300.0f * s;
	lat_range = (int)(160 * s); // total spread; halved to ±range/2

	for (i = 0; i < count; i++) {
		if (!free_particles) return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;

		p->die  = cl.time + 0.9f + (rand() & 31) * 0.02f;
		p->type = pt_grav;
		if (base_color == 244)
			p->color = 244 + (rand() % 3);
		else
			p->color = (base_color & ~7) + (rand() & 7);

		for (j = 0; j < 2; j++) {
			p->org[j] = org[j] + ((rand() & 7) - 4);
			p->vel[j] = (rand() % (lat_range + 1)) - (lat_range >> 1);
		}
		p->org[2] = org[2] + (rand() & 3);
		p->vel[2] = vz_lo + (rand() % (int)(vz_hi - vz_lo + 1.0f));
	}
}

/*
===============
R_BloodSpray

Like R_RunParticleEffect for the blood-color ramp, but with pt_grav so droplets
arc and fall instead of floating away on slow-grav + wind drag. dir is the same
impulse vector svc_particle would carry (already scaled by the caller — see
SpawnBlood); we add the usual jitter so the spray fans out.
===============
*/
void R_BloodSpray (vec3_t org, vec3_t dir, int count)
{
	int			i, j;
	particle_t	*p;

	for (i = 0; i < count; i++) {
		if (!free_particles) return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;

		p->die   = cl.time + 0.6f + (rand() & 31) * 0.02f;
		p->type  = pt_grav;
		p->color = (73 & ~7) + (rand() & 7); // blood ramp 72..79

		for (j = 0; j < 3; j++) {
			p->org[j] = org[j] + ((rand() & 15) - 8);
			p->vel[j] = dir[j] * 15 + (rand() % 300) - 150;
		}
	}
}

/*
===============
R_SparkBurst

Hemispherical spark burst at `origin`, biased along `normal` (so sparks
fan away from the surface they spawn on). Used by TE_SPARKBURST (lightning
gun impacts). Each spark: pt_spark, PARTFL_BOUNCE | PARTFL_RAMP_HOLD,
cyan-white birth colour, lifetime ~0.8-1.2 s, speed 200-500 u/s.

count is multiplied by r_sparks_count_mul.value (clamped >= 0) so the
cvar gates spawns uniformly regardless of caller. Origin is nudged 1 unit
along normal so the first integration step doesn't trace-immediately-back
into the spawn surface.
===============
*/
void R_SparkBurst (vec3_t origin, vec3_t normal, int count)
{
	int		i, j, scaled_count;
	particle_t	*p;
	float		mul, speed, vlen;
	vec3_t		v, base;

	mul = r_sparks_count_mul.value;
	if (mul <= 0.0f) return;
	scaled_count = (int)(count * mul);
	if (scaled_count <= 0) return;

	// 1-unit spawn offset along the surface normal.
	base[0] = origin[0] + normal[0];
	base[1] = origin[1] + normal[1];
	base[2] = origin[2] + normal[2];

	for (i = 0; i < scaled_count; i++) {
		if (!free_particles) return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		// Hemisphere-oriented direction: sample inside unit cube, reject
		// the obviously-bad zero vector, flip into +normal hemisphere.
		do {
			v[0] = ((rand() & 1023) - 512) * (1.0f / 512.0f);
			v[1] = ((rand() & 1023) - 512) * (1.0f / 512.0f);
			v[2] = ((rand() & 1023) - 512) * (1.0f / 512.0f);
			vlen = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
		} while (vlen < 0.01f);

		// Flip into the hemisphere defined by `normal`.
		if (v[0]*normal[0] + v[1]*normal[1] + v[2]*normal[2] < 0.0f) {
			v[0] = -v[0]; v[1] = -v[1]; v[2] = -v[2];
		}

		// Normalise then scale to a random speed in [200, 500].
		vlen = 1.0f / sqrtf (vlen);
		speed = 200.0f + (rand() & 255) * (300.0f / 256.0f);
		for (j = 0; j < 3; j++) p->vel[j] = v[j] * vlen * speed;

		VectorCopy (base, p->org);
		p->color = 244 + (rand() % 3);		// cyan-white core: 244..246
		p->ramp = 0;
		p->birth = cl.time;
		p->die = cl.time + 0.8f + (rand() & 31) * (0.4f / 31.0f);	// 0.8-1.2 s
		p->type = pt_spark;
		p->flags = PARTFL_BOUNCE | PARTFL_RAMP_HOLD;
	}
}

/*
===============
R_TeleportSplash

===============
*/
void R_TeleportSplash (vec3_t org)
{
	int			i, j, k;
	particle_t	*p;
	float		vel;
	vec3_t		dir;

	for (i=-16 ; i<16 ; i+=4)
		for (j=-16 ; j<16 ; j+=4)
			for (k=-24 ; k<32 ; k+=4)
			{
				if (!free_particles)
					return;
				p = free_particles;
				free_particles = p->next;
				p->next = active_particles;
				active_particles = p;
				p->flags = 0;

				p->die = cl.time + 0.2 + (rand()&7) * 0.02;
				p->color = 7 + (rand()&7);
				p->type = pt_slowgrav;
				
				dir[0] = j*8;
				dir[1] = i*8;
				dir[2] = k*8;
	
				p->org[0] = org[0] + i + (rand()&3);
				p->org[1] = org[1] + j + (rand()&3);
				p->org[2] = org[2] + k + (rand()&3);
	
				VectorNormalize (dir);						
				vel = 50 + (rand()&63);
				VectorScale (dir, vel, p->vel);
			}
}

void R_RocketTrail (vec3_t start, vec3_t end, int type)
{
	vec3_t		vec;
	float		len;
	int			j;
	particle_t	*p;
	int			dec;
	static int	tracercount;

	VectorSubtract (end, start, vec);
	len = VectorNormalize (vec);
	if (type < 128)
		dec = 3;
	else
	{
		dec = 1;
		type -= 128;
	}

	while (len > 0)
	{
		len -= dec;

		if (!free_particles)
			return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;

		VectorCopy (vec3_origin, p->vel);
		p->die = cl.time + 2;

		switch (type)
		{
			case 0:	// rocket trail
				p->ramp = (rand()&3);
				p->color = ramp3[(int)p->ramp];
				p->type = pt_fire;
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				break;

			case 1:	// smoke smoke
				p->ramp = (rand()&3) + 2;
				p->color = ramp3[(int)p->ramp];
				p->type = pt_fire;
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				break;

			case 2:	// blood
				p->type = pt_grav;
				p->color = 67 + (rand()&3);
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				break;

			case 3:
			case 5:	// tracer
				p->die = cl.time + 0.5;
				p->type = pt_static;
				if (type == 3)
					p->color = 52 + ((tracercount&4)<<1);
				else
					p->color = 230 + ((tracercount&4)<<1);
			
				tracercount++;

				VectorCopy (start, p->org);
				if (tracercount & 1)
				{
					p->vel[0] = 30*vec[1];
					p->vel[1] = 30*-vec[0];
				}
				else
				{
					p->vel[0] = 30*-vec[1];
					p->vel[1] = 30*vec[0];
				}
				break;

			case 4:	// slight blood
				p->type = pt_grav;
				p->color = 67 + (rand()&3);
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()%6)-3);
				len -= 3;
				break;

			case 6:	// voor trail
				p->color = 9*16 + 8 + (rand()&3);
				p->type = pt_static;
				p->die = cl.time + 0.3;
				for (j=0 ; j<3 ; j++)
					p->org[j] = start[j] + ((rand()&15)-8);
				break;
		}
		

		VectorAdd (start, vec, start);
	}
}


/*
===============
R_DrawParticles
===============
*/
extern	cvar_t	sv_gravity;

void R_DrawParticles (void)
{
	particle_t		*p, *kill;
	float			grav;
	int				i;
	float			time2, time3;
	float			time1;
	float			dvel;
	float			frametime;
	
#ifdef GLQUAKE
	vec3_t			up, right;
	float			scale;

    GL_Bind(particletexture);
	glEnable (GL_BLEND);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glBegin (GL_TRIANGLES);

	VectorScale (vup, 1.5, up);
	VectorScale (vright, 1.5, right);
#else
	D_StartParticles ();

	VectorScale (vright, xscaleshrink, r_pright);
	VectorScale (vup, yscaleshrink, r_pup);
	VectorCopy (vpn, r_ppn);
#endif
	frametime = cl.time - cl.oldtime;
	time3 = frametime * 15;
	time2 = frametime * 10; // 15;
	time1 = frametime * 5;
	grav = frametime * sv_gravity.value * 0.05;
	dvel = 4*frametime;
	
	for ( ;; ) 
	{
		kill = active_particles;
		if (kill && kill->die < cl.time)
		{
			active_particles = kill->next;
			kill->next = free_particles;
			free_particles = kill;
			continue;
		}
		break;
	}

	for (p=active_particles ; p ; p=p->next)
	{
		for ( ;; )
		{
			kill = p->next;
			if (kill && kill->die < cl.time)
			{
				p->next = kill->next;
				kill->next = free_particles;
				free_particles = kill;
				continue;
			}
			break;
		}

#ifdef GLQUAKE
		// hack a scale up to keep particles from disapearing
		scale = (p->org[0] - r_origin[0])*vpn[0] + (p->org[1] - r_origin[1])*vpn[1]
			+ (p->org[2] - r_origin[2])*vpn[2];
		if (scale < 20)
			scale = 1;
		else
			scale = 1 + scale * 0.004;
		glColor3ubv ((byte *)&d_8to24table[(int)p->color]);
		glTexCoord2f (0,0);
		glVertex3fv (p->org);
		glTexCoord2f (1,0);
		glVertex3f (p->org[0] + up[0]*scale, p->org[1] + up[1]*scale, p->org[2] + up[2]*scale);
		glTexCoord2f (0,1);
		glVertex3f (p->org[0] + right[0]*scale, p->org[1] + right[1]*scale, p->org[2] + right[2]*scale);
#else
		if (p->type == pt_smoke)
			D_DrawSmokeParticle (p);
		else
			D_DrawParticle (p);
#endif
		// Wind nudge — drift particle velocity toward locally-sampled
		// wind velocity.  Pre-integration so the kick takes effect
		// this same frame.  Guarded so still-air or no-DLL behaviour
		// is byte-identical to legacy r_part.c.
#if NATIVE_GAME
		int wind_debug_color = -1;
		if (!r_particle_wind_disable.value) {
			vec3_t wind = {0, 0, 0};
			if (g_game_api && g_game_api->Wind_SampleVelocity)
				g_game_api->Wind_SampleVelocity(p->org, wind);
			float wlen2 = wind[0]*wind[0] + wind[1]*wind[1] + wind[2]*wind[2];
			if (wlen2 > 1.0f) {
				float k = wind_drag_k[p->type] * r_particle_wind_scale.value;
				if (k > 0) {
					float a = 1.0f - expf(-k * frametime);
					p->vel[0] += (wind[0] - p->vel[0]) * a;
					p->vel[1] += (wind[1] - p->vel[1]) * a;
					p->vel[2] += (wind[2] - p->vel[2]) * a;

					if (r_particle_wind_debug.value >= 1) {
						vec3_t tip = {
							p->org[0] + wind[0] * 0.1f,
							p->org[1] + wind[1] * 0.1f,
							p->org[2] + wind[2] * 0.1f,
						};
						// Colour 15 = white in id1 palette; 1 = ztest on.
						DebugLines_Add(p->org, tip, 15, 1);

						if (r_particle_wind_debug.value >= 2) {
							// Drag bucket: cyan=light, green=mid, red=heavy.
							// Applied after the physics switch so ramp-animated
							// types (pt_fire/explode/explode2) don't overwrite it.
							if      (k >= 3.0f) wind_debug_color = 192;
							else if (k >= 1.5f) wind_debug_color = 110;
							else                wind_debug_color =  79;
						}
					}
				}
			}
		}
#endif
		p->org[0] += p->vel[0]*frametime;
		p->org[1] += p->vel[1]*frametime;
		p->org[2] += p->vel[2]*frametime;
		
		switch (p->type)
		{
		case pt_static:
		case pt_smoke:
			break;
		case pt_spark:
			if (p->flags & PARTFL_RAMP_HOLD) {
				// In-flight cyan-white flicker — no cool-down yet.
				p->color = 244 + (rand() % 3);
			} else if (!(p->flags & PARTFL_DWELL)) {
				p->ramp += time2;	// same advance rate as pt_explode
				if (p->ramp >= 8) {
					p->color = ramp1[7];		// 0x61 — dark ember
					p->flags |= PARTFL_DWELL;
					p->die = cl.time + r_sparks_settle_dwell.value;
				} else {
					p->color = ramp1[(int)p->ramp];
				}
			}
			if (!(p->flags & PARTFL_STUCK))
				p->vel[2] -= grav;	// light gravity (matches pt_slowgrav)
			break;
		case pt_fire:
			p->ramp += time1;
			if (p->ramp >= 6)
				p->die = -1;
			else
				p->color = ramp3[(int)p->ramp];
			p->vel[2] += grav;
			break;

		case pt_explode:
			p->ramp += time2;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = ramp1[(int)p->ramp];
			for (i=0 ; i<3 ; i++)
				p->vel[i] += p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_explode2:
			p->ramp += time3;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = ramp2[(int)p->ramp];
			for (i=0 ; i<3 ; i++)
				p->vel[i] -= p->vel[i]*frametime;
			p->vel[2] -= grav;
			break;

		case pt_blob:
			for (i=0 ; i<3 ; i++)
				p->vel[i] += p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_blob2:
			for (i=0 ; i<2 ; i++)
				p->vel[i] -= p->vel[i]*dvel;
			p->vel[2] -= grav;
			break;

		case pt_grav:
			// Blood-from-gib trails use pt_grav; pt_slowgrav is the 5% drift
			// used for everything else. Original WinQuake fell through here
			// and made blood float; the QUAKE2 branch fixed it. Always apply
			// full gravity so gib blood actually falls.
			p->vel[2] -= grav * 20;
			break;
		case pt_slowgrav:
			p->vel[2] -= grav;
			break;
		}

#if NATIVE_GAME
		// Apply wind-debug bucket colour after the type switch so it
		// wins against ramp3/ramp1/ramp2 reassignments above.
		if (wind_debug_color >= 0) p->color = wind_debug_color;
#endif
	}

#ifdef GLQUAKE
	glEnd ();
	glDisable (GL_BLEND);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
#else
	D_EndParticles ();
	D_DrawSmokeCells ();	// voxel-haze pass (Phase 8)
	D_CompositeSmoke ();	// fog-tint composite over the framebuffer
#endif
}

