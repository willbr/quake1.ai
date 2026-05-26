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

// Particle sliding — declared in r_main.c, registered in R_Init.
extern cvar_t r_particle_slide_speed;
extern cvar_t r_particle_slide_debug;

// Rain — declared in r_main.c, registered in R_Init.
extern cvar_t r_rain;
extern cvar_t r_rain_debug;

// Cached sky-surface bboxes, rebuilt when cl.worldmodel changes.
// Each entry is the XY bbox of one SURF_DRAWSKY surface (any orientation)
// plus its average z altitude. Used by R_RainSpawn to check whether sky
// is nearby the player.
#define R_RAIN_MAX_SKY  256
typedef struct {
	float xmin, xmax, ymin, ymax;
	float z;     // average z (kept for legacy/debug; not used by spawn)
	float zmax;  // highest vertex z — where rain spawns from
} r_sky_bbox_t;
static r_sky_bbox_t r_sky_bboxes[R_RAIN_MAX_SKY];
static int           r_sky_bbox_count = 0;
static model_t      *r_sky_bbox_model = NULL;	// worldmodel pointer this list was built for

static void R_RainBuildSkyList (void)
{
	int i, j;
	model_t *m = cl.worldmodel;
	r_sky_bbox_count = 0;
	r_sky_bbox_model = m;
	if (!m) return;
	for (i = 0; i < m->numsurfaces && r_sky_bbox_count < R_RAIN_MAX_SKY; i++) {
		msurface_t *s = &m->surfaces[i];
		if (!(s->flags & SURF_DRAWSKY)) continue;
		// Reject anything not facing downward enough to be a real sky
		// ceiling — slipgates also use sky-prefixed textures (so they
		// get SURF_DRAWSKY) but their planes face horizontally.
		//
		// Effective outward normal is plane->normal, negated when the
		// SURF_PLANEBACK flag is set. We want outward_nz <= -0.5
		// (faces downward into the room) to keep this surface.
		if (!s->plane) continue;
		{
			float nz = s->plane->normal[2];
			if (s->flags & SURF_PLANEBACK) nz = -nz;
			if (nz > -0.5f) continue;
		}
		float xmin =  1e30f, xmax = -1e30f;
		float ymin =  1e30f, ymax = -1e30f;
		float zsum = 0.0f, zmax = -1e30f;
		int   zcount = 0;
		for (j = 0; j < s->numedges; j++) {
			int eidx = m->surfedges[s->firstedge + j];
			int vidx;
			if (eidx > 0) vidx = m->edges[ eidx].v[0];
			else          vidx = m->edges[-eidx].v[1];
			float *v = m->vertexes[vidx].position;
			if (v[0] < xmin) xmin = v[0];
			if (v[0] > xmax) xmax = v[0];
			if (v[1] < ymin) ymin = v[1];
			if (v[1] > ymax) ymax = v[1];
			if (v[2] > zmax) zmax = v[2];
			zsum += v[2]; zcount++;
		}
		if (zcount == 0) continue;
		// Reject small surfaces: slipgate door-tops are downward-facing
		// sky-textured but only ~64x32 units. Real outdoor sky ceilings
		// are hundreds of units on a side. Require min(width_x, width_y)
		// >= 128 so we keep real sky and drop slipgate tops.
		if ((xmax - xmin) < 128.0f || (ymax - ymin) < 128.0f) continue;
		r_sky_bboxes[r_sky_bbox_count].xmin = xmin;
		r_sky_bboxes[r_sky_bbox_count].xmax = xmax;
		r_sky_bboxes[r_sky_bbox_count].ymin = ymin;
		r_sky_bboxes[r_sky_bbox_count].ymax = ymax;
		r_sky_bboxes[r_sky_bbox_count].z    = zsum / (float)zcount;
		r_sky_bboxes[r_sky_bbox_count].zmax = zmax;
		r_sky_bbox_count++;
	}
	if (r_rain_debug.value >= 1)
		Con_Printf ("r_rain: cached %d sky surfaces (any orientation)\n", r_sky_bbox_count);
}

// Cached horizontal liquid surfaces (water/slime/lava floors of pools),
// rebuilt when cl.worldmodel changes. R_WaterSplash uses this to find the
// surface under a splash point so it can sample the actual texture colour
// (rather than using a hardcoded palette range that's typically too blue
// for the muted greens/browns of id1 water textures).
#define R_SPLASH_MAX_LIQ_SURFS  1024
typedef struct {
	msurface_t *surf;
	float xmin, xmax, ymin, ymax;
} r_liq_surf_t;
static r_liq_surf_t r_liq_surfs[R_SPLASH_MAX_LIQ_SURFS];
static int          r_liq_surf_count = 0;
static model_t     *r_liq_surf_model = NULL;

static void R_BuildLiquidSurfList (void)
{
	int i, j;
	model_t *m = cl.worldmodel;
	r_liq_surf_count = 0;
	r_liq_surf_model = m;
	if (!m) return;
	for (i = 0; i < m->numsurfaces && r_liq_surf_count < R_SPLASH_MAX_LIQ_SURFS; i++) {
		msurface_t *s = &m->surfaces[i];
		if (!(s->flags & SURF_DRAWTURB)) continue;
		if (!s->plane) continue;
		// Restrict to roughly-horizontal faces — vertical waterfall sheets
		// would mis-match an XY bbox test against a splash point on a
		// neighbouring pool.
		float nz = s->plane->normal[2];
		if (s->flags & SURF_PLANEBACK) nz = -nz;
		if (fabsf(nz) < 0.5f) continue;
		float xmin =  1e30f, xmax = -1e30f;
		float ymin =  1e30f, ymax = -1e30f;
		int   any = 0;
		for (j = 0; j < s->numedges; j++) {
			int eidx = m->surfedges[s->firstedge + j];
			int vidx = eidx > 0 ? m->edges[eidx].v[0] : m->edges[-eidx].v[1];
			float *v = m->vertexes[vidx].position;
			if (v[0] < xmin) xmin = v[0];
			if (v[0] > xmax) xmax = v[0];
			if (v[1] < ymin) ymin = v[1];
			if (v[1] > ymax) ymax = v[1];
			any = 1;
		}
		if (!any) continue;
		r_liq_surfs[r_liq_surf_count].surf = s;
		r_liq_surfs[r_liq_surf_count].xmin = xmin;
		r_liq_surfs[r_liq_surf_count].xmax = xmax;
		r_liq_surfs[r_liq_surf_count].ymin = ymin;
		r_liq_surfs[r_liq_surf_count].ymax = ymax;
		r_liq_surf_count++;
	}
}

// Returns the liquid surface under `org` (within ~6u of its plane and
// inside its XY bbox), or NULL if none. Used by R_WaterSplash to grab
// the surface's texture for per-particle colour sampling.
static msurface_t *R_FindLiquidSurface (vec3_t org)
{
	int i;
	if (r_liq_surf_model != cl.worldmodel) R_BuildLiquidSurfList();
	for (i = 0; i < r_liq_surf_count; i++) {
		r_liq_surf_t *e = &r_liq_surfs[i];
		if (org[0] < e->xmin - 1.0f || org[0] > e->xmax + 1.0f) continue;
		if (org[1] < e->ymin - 1.0f || org[1] > e->ymax + 1.0f) continue;
		msurface_t *s = e->surf;
		float dist = DotProduct(org, s->plane->normal) - s->plane->dist;
		if (s->flags & SURF_PLANEBACK) dist = -dist;
		if (fabsf(dist) > 6.0f) continue;
		return s;
	}
	return NULL;
}

// Floor of free particles that R_AddSmokePuff refuses to dip below.
// Smoke is a low-priority visualization re-emitted every frame; gameplay
// FX (rocket trails, gibs, explosions) get clobbered if smoke drains the
// shared pool. When fewer than this many particles are free, smoke skips
// the burst and lets the next frame's gameplay spawns succeed.
#define SMOKE_GAMEPLAY_RESERVE	2048

int		ramp1[8] = {0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61};
int		ramp2[8] = {0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66};
int		ramp3[8] = {0x6d, 0x6b, 6, 5, 4, 3};

// Blood fade ramp: dark red (248 = #8b0000) walks monotonically
// darker through saturated reds, ending at pure black (0). All entries
// are red-only (G=B=0) so the fade stays chromatic — no detour through
// neutral grays. Used by pt_blood; advanced once per frame in R_DrawParticles.
int ramp_blood[8] = {248, 79, 77, 75, 73, 71, 67, 0};

// Per-type wind drag coefficients.  Used by R_DrawParticles to lerp
// each active particle's velocity toward the locally-sampled wind
// velocity.  Higher k snaps faster; 0 disables for that type.
// Indexed by ptype_t (see d_iface.h).  pt_grav is excluded (heavy
// blood from gibs should not billow).
static const float wind_drag_k[] = {
    [pt_static]   = 3.0f,
    [pt_smoke]    = 1.5f,	// moderate drag -- puff holds its spawn impulse long enough to billow (~half a second), then drifts with the ambient wind
    [pt_spark]    = 0.0f,	// lightning-gun sparks bounce off geometry; ambient wind drift would fight that, so disable
    [pt_blood]    = 0.0f,	// heavy gib droplets; gravity dominates, ambient wind drift would float them oddly
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
		p->flags = PARTFL_BOUNCE;

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
extern int SV_HullPointContents (hull_t *hull, int num, vec3_t p);

/*
===============
R_TraceParticle

Traces start→end against world brushes AND all visible brush entities
(doors, platforms, buttons). Returns the closest hit; returns 1 on any
hit, 0 on clean traversal or when no worldmodel is loaded.

Brush-entity local space: subtract entity->origin from start/end before
the hull check, then add it back to trace->endpos. Stock Quake doors and
platforms never rotate, so this is exact. Rotating bmodels (if a mod ever
adds them) would degrade to axis-aligned-bbox behaviour — visible glitch,
not a crash.
===============
*/
static int R_TraceParticle (const vec3_t start, const vec3_t end, trace_t *trace)
{
	vec3_t p1, p2;
	trace_t best;
	int i;

	if (!cl.worldmodel) return 0;

	memset (&best, 0, sizeof(best));
	best.fraction = 1.0f;

	// 1. World brushes.
	VectorCopy (start, p1);
	VectorCopy (end,   p2);
	SV_RecursiveHullCheck (&cl.worldmodel->hulls[0],
	                       cl.worldmodel->hulls[0].firstclipnode,
	                       0.0f, 1.0f, p1, p2, &best);

	// 2. Visible brush entities (doors, platforms, buttons). Stock Quake
	//    doors/plats don't rotate, so a simple origin subtract is enough
	//    to put start/end into the entity's local space. The normal stays
	//    in world space; the endpos needs translating back. If a rotating
	//    bmodel ever shows up, blood will land on its axis-aligned bbox
	//    rather than its true surface — visible glitch, not a crash.
	for (i = 0; i < cl_numvisedicts; i++) {
		entity_t *ent = cl_visedicts[i];
		if (!ent || !ent->model) continue;
		if (ent->model->type != mod_brush) continue;
		// World model has been linked into cl_visedicts in some configs;
		// skip it to avoid double-tracing.
		if (ent == &cl_entities[0]) continue;
		if (ent->model == cl.worldmodel) continue;

		vec3_t lp1, lp2;
		lp1[0] = start[0] - ent->origin[0];
		lp1[1] = start[1] - ent->origin[1];
		lp1[2] = start[2] - ent->origin[2];
		lp2[0] = end[0]   - ent->origin[0];
		lp2[1] = end[1]   - ent->origin[1];
		lp2[2] = end[2]   - ent->origin[2];

		trace_t local;
		memset (&local, 0, sizeof(local));
		local.fraction = 1.0f;

		SV_RecursiveHullCheck (&ent->model->hulls[0],
		                       ent->model->hulls[0].firstclipnode,
		                       0.0f, 1.0f, lp1, lp2, &local);

		if (local.fraction < best.fraction) {
			best = local;
			best.endpos[0] += ent->origin[0];
			best.endpos[1] += ent->origin[1];
			best.endpos[2] += ent->origin[2];
			// best.plane.normal stays as-is (no rotation).
		}
	}

	*trace = best;
	return (best.fraction < 1.0f) ? 1 : 0;
}

/*
================
Slide debug visualisation

Persistent record of binary-search probes from R_FindWallBottom so the
user can see what the search actually did at stick time.  Each frame,
SlideDebug_DrawAll re-submits unexpired entries to the engine's
DebugLines overlay; entries expire after ~10 s.  Drained only when
r_particle_slide_debug.value > 0.
================
*/
typedef struct {
	vec3_t	start;
	vec3_t	end;
	int		color;
	float	expire_time;
} slide_debug_entry_t;

#define SLIDE_DEBUG_MAX 2048
static slide_debug_entry_t	s_slide_debug[SLIDE_DEBUG_MAX];
static int					s_slide_debug_count;

static void SlideDebug_Push (const vec3_t a, const vec3_t b, int color, float ttl)
{
	slide_debug_entry_t *e;
	if (!r_particle_slide_debug.value) return;
	if (s_slide_debug_count >= SLIDE_DEBUG_MAX) return;
	e = &s_slide_debug[s_slide_debug_count++];
	e->start[0] = a[0]; e->start[1] = a[1]; e->start[2] = a[2];
	e->end[0]   = b[0]; e->end[1]   = b[1]; e->end[2]   = b[2];
	e->color    = color;
	e->expire_time = cl.time + ttl;
}

static void SlideDebug_DrawAll (void)
{
	int	i, w = 0;
	if (!r_particle_slide_debug.value) {
		s_slide_debug_count = 0;
		return;
	}
	for (i = 0; i < s_slide_debug_count; i++) {
		if (cl.time >= s_slide_debug[i].expire_time) continue;
		if (w != i) s_slide_debug[w] = s_slide_debug[i];
		DebugLines_Add (s_slide_debug[w].start, s_slide_debug[w].end,
		                s_slide_debug[w].color, 1);
		w++;
	}
	s_slide_debug_count = w;
}

/*
================
R_FindWallBottom

Binary-search the vertical extent of the wall the particle just stuck to.
Returns the lowest Z where the wall surface is still present (the Z just above
the wall's bottom edge).  At stick time the wall is known present at impact[2];
the search probes downward by 256 u, then bisects to ~2 u precision.

`n` is the contact normal (points away from the wall). The probe is a 1-unit
trace through the wall plane at a given Z: HIT means the wall is there, MISS
means it's gone (air below the wall's bottom, a hole, etc.).

When r_particle_slide_debug is on, each probe is recorded for ~10 s with a
4-u-long horizontal line at the probe Z, colour-coded green (HIT) or red
(MISS), centred along the wall normal.  Long enough to be visible even
when the actual probe geometry is 1 u.
================
*/
// Horizontal probe spans 4 u total (2 u outside the wall, 2 u inside)
// so R_TraceParticle is guaranteed to register a hit when the wall
// surface is present at this Z.  Walked down in 4 u steps; first MISS
// is the wall's bottom edge.
#define SLIDE_PROBE_OUT		2.0f
#define SLIDE_PROBE_IN		2.0f
#define SLIDE_PROBE_NORMAL_DOT	0.9f
#define SLIDE_STEP			4.0f
#define SLIDE_SEARCH_DEPTH	256.0f

/*
================
R_FindWallBottom

Find the bottom Z extent of the wall the particle just stuck to.

Two-phase search:
  1. Coarse exponential walk down (4, 8, 16, 32, 64, 128, 256 u) until
     the first MISS — finds the "zone" where the wall ends.
  2. Bisect inside that zone (between last HIT z and first MISS z) until
     converged to SLIDE_STEP precision.

Probes are horizontal traces perpendicular to the wall, HITting only
with a matching contact normal (rejects floors, ceilings).  Walking
down catches the FIRST gap in the surface, so a doorway opening
correctly terminates the search before reaching any coplanar wall below.

Cost: 1–7 coarse + ≤6 bisect probes per droplet (vs. 64 for a fixed-step
linear walk).  Caveat: a gap small enough to fit between coarse step
positions can be skipped — for typical Quake door / lintel heights (≥16
u) the 4 u initial step catches them.
================
*/
static void slide_debug_record_probe (const vec3_t impact, const vec3_t n, float z, int hit)
{
	vec3_t va, vb;
	int color = hit ? 79 : 251;	// 79 = green, 251 = bright red
	va[0] = impact[0] + n[0] * SLIDE_PROBE_OUT;
	va[1] = impact[1] + n[1] * SLIDE_PROBE_OUT;
	va[2] = z;
	vb[0] = impact[0] - n[0] * SLIDE_PROBE_IN;
	vb[1] = impact[1] - n[1] * SLIDE_PROBE_IN;
	vb[2] = z;
	SlideDebug_Push (va, vb, color, 10.0f);
}

static int slide_probe_at_z (const vec3_t impact, const vec3_t n, float z)
{
	trace_t tr;
	vec3_t a, b;
	int hit;
	a[0] = impact[0] + n[0] * SLIDE_PROBE_OUT;
	a[1] = impact[1] + n[1] * SLIDE_PROBE_OUT;
	a[2] = z;
	b[0] = impact[0] - n[0] * SLIDE_PROBE_IN;
	b[1] = impact[1] - n[1] * SLIDE_PROBE_IN;
	b[2] = z;
	if (R_TraceParticle (a, b, &tr)) {
		float dot = tr.plane.normal[0]*n[0]
		          + tr.plane.normal[1]*n[1]
		          + tr.plane.normal[2]*n[2];
		hit = dot >= SLIDE_PROBE_NORMAL_DOT;
	} else {
		hit = 0;
	}
	slide_debug_record_probe (impact, n, z, hit);
	return hit;
}

static float R_FindWallBottom (const vec3_t impact, const vec3_t n)
{
	float last_hit_z = impact[2];
	float first_miss_z;
	float step;
	float z;
	int   found_miss = 0;

	// Phase 1: coarse exponential walk — 4, 8, 16, 32, ... up to depth.
	for (step = SLIDE_STEP; step <= SLIDE_SEARCH_DEPTH; step *= 2.0f) {
		z = impact[2] - step;
		if (slide_probe_at_z (impact, n, z)) {
			last_hit_z = z;
		} else {
			first_miss_z = z;
			found_miss = 1;
			break;
		}
	}
	if (!found_miss) return impact[2] - SLIDE_SEARCH_DEPTH;

	// Phase 2: bisect between last HIT and first MISS to refine.
	while (last_hit_z - first_miss_z > SLIDE_STEP) {
		z = (last_hit_z + first_miss_z) * 0.5f;
		if (slide_probe_at_z (impact, n, z))
			last_hit_z = z;
		else
			first_miss_z = z;
	}
	return last_hit_z;
}

/*
================
R_SlideRelease

Called when a sliding droplet's Z reaches its stashed wall_bottom_z.  One
short downward trace decides what happened:

  - Surface immediately below (within 4 u): snap to it and stop sliding.
  - Open air below: release for free-fall.  Clear STUCK | WALL_STICK and
    zero vel — the per-type gravity in R_DrawParticles' type switch will
    accelerate the droplet downward.  PARTFL_STICK_ON_HIT is still set on
    the particle, so the next collision re-runs the existing stick branch
    and the droplet re-sticks on whatever it lands on.
================
*/
static void R_SlideRelease (particle_t *p)
{
	trace_t	tr;
	vec3_t	below;

	below[0] = p->org[0];
	below[1] = p->org[1];
	below[2] = p->org[2] - 4.0f;

	if (R_TraceParticle (p->org, below, &tr)) {
		// Surface immediately below — snap and stop sliding.
		p->org[0] = tr.endpos[0] + tr.plane.normal[0] * 0.5f;
		p->org[1] = tr.endpos[1] + tr.plane.normal[1] * 0.5f;
		p->org[2] = tr.endpos[2] + tr.plane.normal[2] * 0.5f;
		p->vel[0] = p->vel[1] = p->vel[2] = 0;
		p->flags &= ~PARTFL_WALL_STICK;
		// STUCK stays set: droplet is at rest on the new surface.
		// Paint the final spatter decal where it landed and retire
		// the sprite — the decal carries the persistent visual.
		if (p->type == pt_blood) {
			R_SpawnBloodSpatter (tr.endpos, tr.plane.normal);
			if (p->die > cl.time + 0.3f) p->die = cl.time + 0.3f;
		}
	} else {
		// Open air below — release for free-fall.
		p->vel[0] = p->vel[1] = p->vel[2] = 0;
		p->flags &= ~(PARTFL_STUCK | PARTFL_WALL_STICK);
	}
}

/*
===============
R_RainSpawn

Spawn (r_rain.value * 8) rain droplets per frame. For each slot, pick
a random cached sky surface, sample random XY in its bbox, and spawn at
(x, y, surface_zmax - 16). Skips if the point is inside SOLID or if the
surface centre is more than 4096 XY units from the player.

Steps each frame:
  1. Rebuild the sky-surface cache if the worldmodel changed.
  2. For each of n spawn attempts:
       - pick a random cached sky surface
       - range-cull: skip if its centre is > 4096 XY from the player
       - sample random XY within the surface bbox
       - spawn at (x, y, surface_zmax - 16)
       - skip if the point lands inside SOLID geometry
       - emit a pt_grav droplet with PARTFL_STICK_ON_HIT

r_rain_debug 1 logs every spawn/skip with position.
===============
*/
static void R_RainSpawn (void)
{
	int    n, i;
	int    debug = (r_rain_debug.value >= 1);
	vec3_t origin;

	if (!cl.worldmodel) return;

	if (r_sky_bbox_model != cl.worldmodel)
		R_RainBuildSkyList ();

	if (r_sky_bbox_count == 0) {
		if (debug) Con_Printf ("r_rain: no sky surfaces in current map\n");
		return;
	}

	VectorCopy (r_origin, origin);

	n = (int)(r_rain.value * 8.0f);
	if (n <= 0) return;
	if (n > 64) n = 64;

	for (i = 0; i < n; i++) {
		if (!free_particles) return;

		// Pick a random sky surface, sample XY in its bbox.
		int si = rand() % r_sky_bbox_count;
		r_sky_bbox_t *b = &r_sky_bboxes[si];

		// Range cull: skip surfaces whose centre is too far from the player.
		float bcx = 0.5f * (b->xmin + b->xmax);
		float bcy = 0.5f * (b->ymin + b->ymax);
		float dx = bcx - origin[0];
		float dy = bcy - origin[1];
		if (dx*dx + dy*dy > 4096.0f * 4096.0f) {
			if (debug) Con_Printf ("rain skip: sky bbox %d too far (centre %.0f,%.0f)\n", si, bcx, bcy);
			continue;
		}

		float u = (rand() & 1023) * (1.0f / 1023.0f);
		float v = (rand() & 1023) * (1.0f / 1023.0f);
		vec3_t spawn;
		spawn[0] = b->xmin + (b->xmax - b->xmin) * u;
		spawn[1] = b->ymin + (b->ymax - b->ymin) * v;
		spawn[2] = b->zmax - 16.0f;

		// Skip if spawn is inside SOLID (e.g. brush face inside another brush).
		if (SV_HullPointContents (&cl.worldmodel->hulls[0], 0, spawn) == CONTENTS_SOLID) {
			if (debug) Con_Printf ("rain skip: in SOLID at (%.0f %.0f %.0f) under sky bbox %d\n",
			                       spawn[0], spawn[1], spawn[2], si);
			continue;
		}

		// Spawn the droplet.
		particle_t *p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		VectorCopy (spawn, p->org);
		p->vel[0] = ((rand() & 1023) - 512) * (1.0f / 1023.0f) * 50.0f;
		p->vel[1] = ((rand() & 1023) - 512) * (1.0f / 1023.0f) * 50.0f;
		p->vel[2] = -700.0f;
		p->color  = 11;
		p->type   = pt_grav;
		p->ramp   = 0;
		p->birth  = cl.time;
		p->die    = cl.time + 1.5f;
		p->flags  = PARTFL_STICK_ON_HIT;

		if (debug) Con_Printf ("rain spawn at (%.0f %.0f %.0f) under sky bbox %d (zmax=%.0f)\n",
		                       spawn[0], spawn[1], spawn[2], si, b->zmax);
	}
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
			p->flags = PARTFL_BOUNCE;
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
			p->type = pt_grav;
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
kind: 0=water, 1=slime, 2=lava — fallback palette ranges if no liquid surface
is found at `org`; otherwise the actual surface texture is sampled per particle
so cyan splashes don't read incongruously against muted brown/green water.

Droplets also remember `org[2]` as their settle plane (p->birth + PARTFL_LIQUID_SURF):
when gravity pulls them back through that height they pin to the surface for a
short dwell instead of plummeting through the (non-solid) water plane.
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
	msurface_t	*liq_surf;
	texture_t	*liq_tex = NULL;
	byte		*liq_pixels = NULL;
	int			liq_w = 0, liq_h = 0;
	float		liq_s0 = 0.0f, liq_t0 = 0.0f;

	switch (kind) {
	case 1:  base_color = 176; break; // slime: 176..183 green-grey
	case 2:  base_color = 232; break; // lava: 232..239 orange ramp
	default: base_color = 244; break; // water: light cyan special-case
	}

	// Foam/highlight palette index per kind. A fraction of droplets pick a
	// bright accent instead of the sampled texture colour, so the spray
	// reads as splash and doesn't camouflage into the (typically muted)
	// surface hue. Picked from the bright end of each kind's natural ramp:
	// cyan-white for water, light green for slime, bright orange for lava.
	int highlight_base;
	switch (kind) {
	case 1:  highlight_base = 180; break; // slime: brighter end of 176..183 ramp
	case 2:  highlight_base = 236; break; // lava: brighter end of 232..239 ramp
	default: highlight_base = 244; break; // water: light cyan (id1 foam idiom)
	}

	liq_surf = R_FindLiquidSurface (org);
	if (liq_surf && liq_surf->texinfo && liq_surf->texinfo->texture &&
	    liq_surf->texinfo->texture->offsets[0]) {
		mtexinfo_t *tex = liq_surf->texinfo;
		liq_tex    = tex->texture;
		liq_w      = (int)liq_tex->width;
		liq_h      = (int)liq_tex->height;
		liq_pixels = (byte *)liq_tex + liq_tex->offsets[0];
		liq_s0     = DotProduct(org, tex->vecs[0]) + tex->vecs[0][3];
		liq_t0     = DotProduct(org, tex->vecs[1]) + tex->vecs[1][3];
	}

	if (strength_q4 < 4)   strength_q4 = 4;   // floor at 0.25x
	if (strength_q4 > 128) strength_q4 = 128; // ceil at 8x
	s = strength_q4 * (1.0f/16.0f);

	// Velocity/spread cap: a person-sized splash (s = 6x) wants lots more
	// droplets but not absurd 1800 ups arcs — clamp the velocity scale at
	// 4x so big splashes read as "denser ring" rather than "fountain to
	// the ceiling". Particle count keeps scaling with s.
	float vs = s > 4.0f ? 4.0f : s;
	count     = (int)(28 * s);
	// Speed magnitude range — directions are picked per-particle from an
	// upper hemisphere below.
	vz_lo     = 180.0f * vs;
	vz_hi     = 300.0f * vs;
	// XY spawn radius scales with the full strength so big splashes
	// emerge from a body-sized footprint instead of all spawning at a
	// single ±4u point and then fanning out from there — that "single
	// point with rays" look reads as a bullet hit, not a person plunking
	// in. At s=1 (bullet) we get ±4u, at s=6 (player) we get ±24u which
	// matches the player bbox half-width.
	int   spawn_r     = (int)(4.0f * s);
	if (spawn_r < 2) spawn_r = 2;
	int   spawn_range = spawn_r * 2 + 1;

	for (i = 0; i < count; i++) {
		if (!free_particles) return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = PARTFL_STICK_ON_HIT | PARTFL_LIQUID_SURF;

		p->die  = cl.time + 0.9f + (rand() & 31) * 0.02f;
		p->type = pt_grav;
		// pt_grav ignores `birth` (only pt_smoke uses it for puff curve);
		// repurpose it here as the settle plane for the integrator's
		// PARTFL_LIQUID_SURF check.
		p->birth = org[2];
		// ~25% foam highlight — bright, kind-specific. The rest sample the
		// actual surface texture so the splash still belongs to the pool
		// it came from.
		if ((rand() & 3) == 0) {
			p->color = highlight_base + (rand() & 3);
		} else if (liq_pixels) {
			// Sample the actual surface texture with ±4 texel jitter so
			// neighbouring droplets pick up natural texture variation
			// rather than reading as a single flat hue.
			int s_i = (int)(liq_s0 + ((rand() & 7) - 4));
			int t_i = (int)(liq_t0 + ((rand() & 7) - 4));
			s_i = ((s_i % liq_w) + liq_w) % liq_w;
			t_i = ((t_i % liq_h) + liq_h) % liq_h;
			p->color = liq_pixels[t_i * liq_w + s_i];
		} else if (base_color == 244) {
			p->color = 244 + (rand() % 3);
		} else {
			p->color = (base_color & ~7) + (rand() & 7);
		}

		for (j = 0; j < 2; j++)
			p->org[j] = org[j] + (rand() % spawn_range) - spawn_r;
		p->org[2] = org[2] + (rand() & 3);

		// Velocity direction: pick uniformly within the upper hemisphere
		// with a polar angle 20°–80° off vertical, so droplets fan out in
		// a 360° dome around the impact rather than rocketing straight
		// up. The narrow vertical cone of the old box-shaped distribution
		// stacked all particles overhead, where a tall enough room let
		// them embed in the ceiling. With this dome, lateral motion is
		// always comparable to vertical, and the per-particle PARTFL_LIQUID_SURF
		// settle pins them back on the water surface when they fall.
		{
			float speed = vz_lo + (float)(rand() % (int)(vz_hi - vz_lo + 1.0f));
			float theta = ((float)(rand() & 4095) / 4096.0f) * 6.2831853f;
			// phi in [0.35, 1.40] rad ≈ [20°, 80°] off vertical.
			float phi   = 0.35f + ((float)(rand() & 4095) / 4096.0f) * 1.05f;
			float sp    = (float)sin(phi);
			float cp    = (float)cos(phi);
			p->vel[0] = speed * sp * (float)cos(theta);
			p->vel[1] = speed * sp * (float)sin(theta);
			p->vel[2] = speed * cp;
		}
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
		p->flags = PARTFL_STICK_ON_HIT;

		// Lifetime is ramp-driven (see pt_blood case in R_DrawParticles);
		// the explicit die is just a safety cap in case ramp advance gets
		// frame-rate-starved.
		p->die   = cl.time + 20.0f;
		p->type  = pt_blood;
		p->ramp  = 0;
		p->color = ramp_blood[0];	// 73 — bright fresh blood
		// Per-droplet rate multiplier in [0.5, 2.0]. pt_blood overloads
		// `birth` as its ramp-rate scale (pt_smoke is the only other user
		// of `birth`). Staggers when droplets vanish so the carpet of
		// blood thins gradually instead of disappearing as a wavefront.
		p->birth = 0.5f + (rand() & 31) * (1.5f / 31.0f);

		for (j = 0; j < 3; j++) {
			p->org[j] = org[j] + ((rand() & 15) - 8);
			p->vel[j] = dir[j] * 15 + (rand() % 300) - 150;
		}
	}
}

/*
===============
R_DebugBloodDroplet

Spawn one pt_blood droplet with exact origin and velocity — no random
spread, no spawn jitter.  Used by TE_DEBUGBLOOD (g_test_shotgunblood)
so the debug particle visibly travels in the direction it was fired
instead of being lost in R_BloodSpray's ±150 per-axis randomisation.
===============
*/
void R_DebugBloodDroplet (vec3_t org, vec3_t vel)
{
	particle_t *p;
	if (!free_particles) return;
	p = free_particles;
	free_particles = p->next;
	p->next = active_particles;
	active_particles = p;

	p->flags = PARTFL_STICK_ON_HIT;
	p->die   = cl.time + 20.0f;
	p->type  = pt_blood;
	p->ramp  = 0;
	p->color = ramp_blood[0];
	p->birth = 1.0f;	// nominal ramp rate
	p->org[0] = org[0]; p->org[1] = org[1]; p->org[2] = org[2];
	p->vel[0] = vel[0]; p->vel[1] = vel[1]; p->vel[2] = vel[2];
}

/*
===============
R_SpawnGunshotChips

Buckshot-scatter pellet marks deposited next to the central DECAL_BULLET
stain. Two 1×1 dark stain cells (DECAL_SPIKE — same kernel as nail-impact
marks) jittered ±2u from the trace endpos so the cluster reads as a
scatter pattern, not a single point. R_SpawnDecal handles the surface
lookup; jittered positions still snap onto the same wall via the BSP
search inside R_SpawnDecal.
===============
*/
void R_SpawnGunshotChips (vec3_t pos)
{
	int		i, j;
	const int	count = 2;

	for (i = 0; i < count; i++) {
		vec3_t jpos;
		for (j = 0; j < 3; j++)
			jpos[j] = pos[j] + ((rand() & 7) - 3);	// -3..+4, ≈±2u jitter
		R_SpawnDecal (jpos, DECAL_SPIKE);
	}
}

/*
===============
R_SpawnShell

Brass shell casing ejected from a fired shotgun. Single pt_grav particle
with PARTFL_BOUNCE (one bounce, then stick), brass-coloured (palette 192..194),
~3.5 s lifetime.
===============
*/
void R_SpawnShell (vec3_t origin, vec3_t vel)
{
	particle_t	*p;

	if (!free_particles) return;
	p = free_particles;
	free_particles = p->next;
	p->next = active_particles;
	active_particles = p;
	p->flags = PARTFL_BOUNCE;

	p->die   = cl.time + 120.0f;	// shells litter the floor for 2 min
	p->type  = pt_grav;
	p->color = 104 + (rand() % 3);	// dark orange shell hull: 104..106 (red-orange, less crimson than lava ramp)
	p->ramp  = 0;

	VectorCopy (origin, p->org);
	VectorCopy (vel, p->vel);
}

/*
===============
R_SparkBurst

Hemispherical spark burst at `origin`, biased along `normal` (so sparks
fan away from the surface they spawn on). Used by TE_SPARKBURST (lightning
gun impacts and nailgun pings). Each spark: pt_spark, PARTFL_BOUNCE |
PARTFL_RAMP_HOLD, cyan-white birth colour, lifetime 0.025-0.05 s in-flight
(post-bounce die capped at 0.05 s, k=40 velocity drag → terminal travel
~31 u). Sparks read as an instantaneous snap at the impact rather than
crawling embers crossing the room. Birth speed 750-1250 u/s.

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

		// Wide-cone direction: bias along the surface normal but with
		// enough perturbation that sparks fan out across the hemisphere
		// instead of firing as a tight beam straight along the normal.
		// Normal weight 2 + ±1.8 perturbation gives an outward-only fan
		// (along-component min = 0.2, always away from the surface) with
		// avg half-angle ~30° and peak ~80°.
		v[0] = normal[0] * 2.0f + ((rand() & 1023) - 512) * (1.8f / 512.0f);
		v[1] = normal[1] * 2.0f + ((rand() & 1023) - 512) * (1.8f / 512.0f);
		v[2] = normal[2] * 2.0f + ((rand() & 1023) - 512) * (1.8f / 512.0f);
		vlen = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];

		// Normalise then scale to a random speed in [750, 1250].
		// Combined with k=40 drag, terminal travel ≈ 1250/40 ≈ 31 u.
		vlen = 1.0f / sqrtf (vlen);
		speed = 750.0f + (rand() & 255) * (500.0f / 256.0f);
		for (j = 0; j < 3; j++) p->vel[j] = v[j] * vlen * speed;

		VectorCopy (base, p->org);
		p->color = 244 + (rand() % 3);		// cyan-white core: 244..246
		p->ramp = 0;
		p->birth = cl.time;
		p->die = cl.time + 0.025f + (rand() & 31) * (0.025f / 31.0f);	// 0.025-0.05 s
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

	// Persistent slide-debug overlay: replay any unexpired probe records
	// from past R_FindWallBottom calls. Drops entries when the cvar is off.
	SlideDebug_DrawAll ();

	// Rain spawner — gated by r_rain cvar; new drops integrate this frame.
	if (r_rain.value > 0.0f)
		R_RainSpawn ();

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
		/* Wall-stuck sliding blood: the decal trail painted by
		   R_SpawnBloodSpatter per cell crossing conveys the slide
		   visually. Drawing the particle sprite on top would just
		   duplicate the same red dot and clutter the wall. */
		if (p->type == pt_blood &&
		    (p->flags & (PARTFL_STUCK | PARTFL_WALL_STICK))
		     == (PARTFL_STUCK | PARTFL_WALL_STICK))
		{
			/* skip draw */
		}
		else if (p->type == pt_smoke)
			D_DrawSmokeParticle (p);
		else
			D_DrawParticle (p);
#endif
		// Wind nudge — drift particle velocity toward locally-sampled
		// wind velocity.  Pre-integration so the kick takes effect
		// this same frame.  Guarded so still-air or no-DLL behaviour
		// is byte-identical to legacy r_part.c.
		int wind_debug_color = -1;
		if (!r_particle_wind_disable.value && !(p->flags & PARTFL_STUCK)) {
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
		// Stuck particles freeze position and skip integration entirely.
		// They still tick through the type switch below so the ramp/dwell
		// timer (and p->die) progresses normally.
		if (p->flags & PARTFL_STUCK) {
			// no position update
		} else if (p->flags & (PARTFL_BOUNCE | PARTFL_STICK_ON_HIT)) {
			vec3_t newpos;
			trace_t tr;
			newpos[0] = p->org[0] + p->vel[0] * frametime;
			newpos[1] = p->org[1] + p->vel[1] * frametime;
			newpos[2] = p->org[2] + p->vel[2] * frametime;

			// Liquid-surface settle: splash droplets pin to their origin
			// water plane (p->birth) when gravity drags them back through
			// it, then dwell briefly. R_TraceParticle only sees solids, so
			// without this they'd fall through the water and either land
			// on whatever's at the pool's bottom or fly off the world.
			if ((p->flags & PARTFL_LIQUID_SURF) &&
			    p->org[2] >= p->birth && newpos[2] <= p->birth) {
				p->org[0] = newpos[0];
				p->org[1] = newpos[1];
				p->org[2] = p->birth;
				p->vel[0] = p->vel[1] = p->vel[2] = 0;
				p->flags |= PARTFL_STUCK;
				if (p->die < cl.time + 0.45f) p->die = cl.time + 0.45f;
			} else if (R_TraceParticle (p->org, newpos, &tr)) {
				vec3_t n;
				VectorCopy (tr.plane.normal, n);

				if ((p->flags & PARTFL_STICK_ON_HIT) ||
				    (p->flags & PARTFL_BOUNCED)) {
					// Stick: park at impact, freeze velocity, mark stuck.
					p->org[0] = tr.endpos[0] + n[0] * 0.5f;
					p->org[1] = tr.endpos[1] + n[1] * 0.5f;
					p->org[2] = tr.endpos[2] + n[2] * 0.5f;
					p->vel[0] = p->vel[1] = p->vel[2] = 0;
					p->flags |= PARTFL_STUCK;
					// Permanent decal: each stuck blood droplet paints one cell.
					// Use tr.endpos (the impact-surface point), not p->org — p->org
					// is offset by 0.5*normal so the particle sprite doesn't z-fight
					// with the wall; we want the decal to project onto the wall plane.
					if (p->type == pt_blood) {
						R_SpawnBloodSpatter (tr.endpos, n);
					}
					// Wall-ish contact (|n.z|<0.7) → also flag for the
					// slide step below so blood/water droplets droop
					// downward instead of freezing flat on the wall.
					// Binary-search the wall's bottom Z once and stash
					// it in vel[2] so the per-frame slide step is a
					// cheap compare instead of a trace.
					if (fabs(n[2]) < 0.7f) {
						p->flags |= PARTFL_WALL_STICK;
						// Per-droplet slide-rate multiplier in [0.5, 2.0]
						// so droplets disperse instead of moving in lockstep.
						p->vel[0] = 0.5f + (rand() & 31) * (1.5f / 31.0f);
						p->vel[2] = R_FindWallBottom (p->org, n);
					}
					// Floor-stuck blood: the high-res spatter decal painted
					// above is the persistent visual. Retire the sprite
					// quickly so we're not rendering a 1-pixel dot on top of
					// the decal for ~16 s. Wall-stuck droplets keep their
					// full lifetime — the slide trail still needs them.
					if (p->type == pt_blood
					    && !(p->flags & PARTFL_WALL_STICK)
					    && p->die > cl.time + 0.3f) {
						p->die = cl.time + 0.3f;
					}
				} else {
					// First bounce: inline reflection at r_sparks_restitution.
					// Clamp so the cvar can't be set super-elastic (>1) or
					// negative (which would push through the wall).
					float r = r_sparks_restitution.value;
					if (r < 0.0f) r = 0.0f;
					else if (r > 1.0f) r = 1.0f;
					float d = p->vel[0]*n[0] + p->vel[1]*n[1] + p->vel[2]*n[2];
					p->vel[0] = (p->vel[0] - 2.0f * d * n[0]) * r;
					p->vel[1] = (p->vel[1] - 2.0f * d * n[1]) * r;
					p->vel[2] = (p->vel[2] - 2.0f * d * n[2]) * r;
					p->org[0] = tr.endpos[0] + n[0] * 0.5f;
					p->org[1] = tr.endpos[1] + n[1] * 0.5f;
					p->org[2] = tr.endpos[2] + n[2] * 0.5f;
					p->flags |= PARTFL_BOUNCED;
					// For pt_spark: cooldown ramp starts now from white-hot.
					// Tight post-bounce cap so a fast spark doesn't fly far
					// after its first hit (post-bounce speed at restitution
					// 0.5 is still ~1000 ups). 0.05 s × ~1000 ups = ~50 u
					// max travel after bounce — stays local to the impact.
					if (p->type == pt_spark) {
						p->flags &= ~PARTFL_RAMP_HOLD;
						p->ramp = 0;
						if (p->die > cl.time + 0.05f)
							p->die = cl.time + 0.05f;
					}
				}
			} else {
				VectorCopy (newpos, p->org);
			}
		} else {
			// Non-collidable: original behaviour, unchanged.
			p->org[0] += p->vel[0]*frametime;
			p->org[1] += p->vel[1]*frametime;
			p->org[2] += p->vel[2]*frametime;
		}

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
					// Don't let the dwell cvar extend a spark past its
					// existing die cap (set on bounce above). Only ever
					// shortens the on-screen ember, never lengthens it.
					float dwell_die = cl.time + r_sparks_settle_dwell.value;
					if (dwell_die < p->die)
						p->die = dwell_die;
				} else {
					p->color = ramp1[(int)p->ramp];
				}
			}
			if (!(p->flags & PARTFL_STUCK)) {
				p->vel[2] -= grav;	// light gravity (matches pt_slowgrav)
				// In-flight drag so a spark (750-1250 ups birth) bleeds
				// momentum within its 0.025-0.05 s window — keeps the
				// burst snappy and local rather than letting initial
				// velocity carry sparks far before die fires. Terminal
				// travel ≈ v0/k ≈ 1250/40 = ~31 u.
				float decay = 1.0f - 40.0f * frametime;
				if (decay < 0.0f) decay = 0.0f;
				p->vel[0] *= decay;
				p->vel[1] *= decay;
				p->vel[2] *= decay;
			}
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
			p->vel[2] -= grav * 20;
			break;

		case pt_explode2:
			p->ramp += time3;
			if (p->ramp >=8)
				p->die = -1;
			else
				p->color = ramp2[(int)p->ramp];
			for (i=0 ; i<3 ; i++)
				p->vel[i] -= p->vel[i]*frametime;
			p->vel[2] -= grav * 20;
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

		case pt_blood:
			// Heavy droplet — full gravity (matches the original pt_grav
			// blood path).  Stuck droplets keep gravity off so they don't
			// "press into" the wall they splatted on.
			if (!(p->flags & PARTFL_STUCK)) {
				p->vel[2] -= grav * 20;
				// Liquid interaction: floating blood reads better than
				// blood teleporting through the surface and falling forever
				// out of sight.  Water/slime = float, lava = vaporise.
				if (cl.worldmodel) {
					int c = SV_HullPointContents (&cl.worldmodel->hulls[0], 0, p->org);
					if (c == CONTENTS_WATER || c == CONTENTS_SLIME) {
						p->vel[0] = p->vel[1] = p->vel[2] = 0;
						p->flags |= PARTFL_STUCK;
					} else if (c == CONTENTS_LAVA) {
						p->die = -1;
					}
				}
			}
			// Walk the colour ramp toward black. Per-droplet rate stored
			// in `birth` (set by R_BloodSpray); range [0.5, 2.0] gives
			// staggered lifetimes between ~8 s and ~32 s, averaging ~16 s.
			p->ramp += time1 * 0.1f * p->birth;
			if (p->ramp >= 8)
				p->die = -1;
			else
				p->color = ramp_blood[(int)p->ramp];
			break;
		case pt_grav:
			// Blood-from-gib trails use pt_grav; pt_slowgrav is the 5% drift
			// used for everything else. Original WinQuake fell through here
			// and made blood float; the QUAKE2 branch fixed it. Always apply
			// full gravity so gib blood actually falls. Underwater: cut
			// gravity to ~1/4 and apply heavy drag so brass shells (which
			// use pt_grav + PARTFL_BOUNCE) sink slowly and lose lateral
			// velocity quickly instead of falling like they're still in air.
			// Stuck droplets skip gravity so vel[2] stays valid as the
			// stashed wall_bottom_z (water-splash droplets on walls slide).
			if (p->flags & PARTFL_STUCK) break;
			if (cl.worldmodel) {
				int c = SV_HullPointContents (&cl.worldmodel->hulls[0], 0, p->org);
				if (c == CONTENTS_WATER || c == CONTENTS_SLIME || c == CONTENTS_LAVA) {
					p->vel[2] -= grav * 5;
					float drag = 1.0f - 6.0f * frametime;
					if (drag < 0.0f) drag = 0.0f;
					p->vel[0] *= drag;
					p->vel[1] *= drag;
					p->vel[2] *= drag;
					break;
				}
			}
			p->vel[2] -= grav * 20;
			break;
		case pt_slowgrav:
			p->vel[2] -= grav;
			break;
		}

		// Sliding wall droplets: blood/water flagged WALL_STICK at stick
		// time creep downward at the cvar's speed × the droplet's
		// per-droplet rate multiplier (vel[0], rolled [0.5, 2.0] at
		// stick time so droplets disperse).  When org[2] reaches
		// vel[2] (wall_bottom_z, found via R_FindWallBottom at stick
		// time), R_SlideRelease decides whether to snap to a floor or
		// free-fall into open air.
		if ((p->flags & (PARTFL_STUCK | PARTFL_WALL_STICK))
		    == (PARTFL_STUCK | PARTFL_WALL_STICK)) {
			float slide = r_particle_slide_speed.value;
			if (slide < 0.0f) slide = 0.0f;
			else if (slide > 32.0f) slide = 32.0f;
			float dz = slide * p->vel[0] * frametime;
			if (dz > 0.0f) {
				float new_z = p->org[2] - dz;
				if (new_z <= p->vel[2]) {
					R_SlideRelease (p);
				} else {
					/* Drag trail: blood droplets leave a streak of spatter
					   dots as they slide. Throttle to one paint per stain
					   cell crossed in Z so a fast slide doesn't paint the
					   same cell every frame. STAIN_CELL_SIZE = 1<<STAIN_CELL_SHIFT. */
					const float cell_size = (float)(1 << STAIN_CELL_SHIFT);
					int old_cell = (int)floor (p->org[2] / cell_size);
					int new_cell = (int)floor (new_z      / cell_size);
					p->org[2] = new_z;
					if (p->type == pt_blood && new_cell != old_cell) {
						/* NULL normal: R_PointOnSurface_World finds the
						   wall on its own — droplet org is offset by
						   0.5*wall_normal from the impact plane, so a
						   nearby coplanar surface is always within tolerance. */
						R_SpawnBloodSpatter (p->org, NULL);
					}
				}
			}
			if (r_particle_slide_debug.value) {
				// Small cyan 3-axis crosshair at wall_bottom_z under
				// the droplet — shows where the slide will release.
				vec3_t c = { p->org[0], p->org[1], p->vel[2] };
				vec3_t ca, cb;
				ca[0] = c[0] - 3; ca[1] = c[1]; ca[2] = c[2];
				cb[0] = c[0] + 3; cb[1] = c[1]; cb[2] = c[2];
				DebugLines_Add (ca, cb, 244, 1);
				ca[0] = c[0]; ca[1] = c[1] - 3; ca[2] = c[2];
				cb[0] = c[0]; cb[1] = c[1] + 3; cb[2] = c[2];
				DebugLines_Add (ca, cb, 244, 1);
				ca[0] = c[0]; ca[1] = c[1]; ca[2] = c[2] - 3;
				cb[0] = c[0]; cb[1] = c[1]; cb[2] = c[2] + 3;
				DebugLines_Add (ca, cb, 244, 1);
			}
		}

		// Apply wind-debug bucket colour after the type switch so it
		// wins against ramp3/ramp1/ramp2 reassignments above.
		if (wind_debug_color >= 0) p->color = wind_debug_color;
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

