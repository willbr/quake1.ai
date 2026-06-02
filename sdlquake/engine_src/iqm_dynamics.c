// iqm_dynamics.c -- R4 client-side ponytail dynamics (Verlet point-chain).
// Engine-side, cosmetic, never networked. Simulates in world space (gravity is
// world-down, camera motion injects no force), then writes actor-space skin
// matrices for the simulated (free) ponytail joints. The chain root joint stays
// rigid (its R3-posed skin is left untouched); it is the pinned anchor that the
// rest of the tail hangs and swings from.

#include "quakedef.h"
#include "iqm.h"
#include "iqm_dynamics.h"

cvar_t actor_dynamics       = {"actor_dynamics",       "1"};
cvar_t actor_pony_gravity   = {"actor_pony_gravity",   "320"}; // units/s^2 down
cvar_t actor_pony_damping   = {"actor_pony_damping",   "0.92"};// velocity retain/frame
cvar_t actor_pony_stiffness = {"actor_pony_stiffness", "0.20"};// straighten toward rigid dir [0..1]
cvar_t actor_pony_iters     = {"actor_pony_iters",     "4"};   // constraint relax passes

void IQM_DynamicsInitCvars (void)
{
	Cvar_RegisterVariable (&actor_dynamics);
	Cvar_RegisterVariable (&actor_pony_gravity);
	Cvar_RegisterVariable (&actor_pony_damping);
	Cvar_RegisterVariable (&actor_pony_stiffness);
	Cvar_RegisterVariable (&actor_pony_iters);
}

#define PONY_MAX  8
#define DYN_POOL  16

typedef struct {
	void	*ent;             // owning entity (NULL = free slot)
	float	last;             // cl.time of last sim
	int		n;                // node count
	vec3_t	pos[PONY_MAX];    // world-space node positions
	vec3_t	prev[PONY_MAX];
	int		inited;
} dyn_state_t;

static dyn_state_t s_pool[DYN_POOL];

static dyn_state_t *dyn_get (void *ent)
{
	int		i, lru = 0;
	float	oldest = 1e30f;

	for (i = 0; i < DYN_POOL; i++)
		if (s_pool[i].ent == ent)
			return &s_pool[i];
	for (i = 0; i < DYN_POOL; i++)
		if (!s_pool[i].ent)
		{
			memset (&s_pool[i], 0, sizeof(s_pool[i]));
			s_pool[i].ent = ent;
			return &s_pool[i];
		}
	for (i = 0; i < DYN_POOL; i++)
		if (s_pool[i].last < oldest) { oldest = s_pool[i].last; lru = i; }
	memset (&s_pool[lru], 0, sizeof(s_pool[lru]));
	s_pool[lru].ent = ent;
	return &s_pool[lru];
}

// actor-space point -> world
static void a2w_point (float rot[3][3], float org[3], const vec3_t a, vec3_t w)
{
	w[0] = org[0] + rot[0][0]*a[0] + rot[0][1]*a[1] + rot[0][2]*a[2];
	w[1] = org[1] + rot[1][0]*a[0] + rot[1][1]*a[1] + rot[1][2]*a[2];
	w[2] = org[2] + rot[2][0]*a[0] + rot[2][1]*a[1] + rot[2][2]*a[2];
}
// world vector -> actor space (rot^T), no translation
static void w2a_vec (float rot[3][3], const vec3_t w, vec3_t a)
{
	a[0] = rot[0][0]*w[0] + rot[1][0]*w[1] + rot[2][0]*w[2];
	a[1] = rot[0][1]*w[0] + rot[1][1]*w[1] + rot[2][1]*w[2];
	a[2] = rot[0][2]*w[0] + rot[1][2]*w[1] + rot[2][2]*w[2];
}

void IQM_SolveDynamics (struct entity_s *ent, lm_iqm_t *iqm,
		float curwld[][3][4], float bindinv[][3][4], float skin[][3][4],
		float a2w_rot[3][3], float a2w_org[3], float now)
{
	dyn_state_t	*st;
	int			N, i, it, iters;
	float		dt, damp, grav, stiff;
	vec3_t		rigidW[PONY_MAX];   // rigid posed node positions (world)
	vec3_t		restdir[PONY_MAX];  // world dir node[i-1]->node[i] (rigid)
	float		rest[PONY_MAX];

	if (!iqm || iqm->num_pony < 2 || actor_dynamics.value == 0)
		return;
	N = iqm->num_pony;
	if (N > PONY_MAX) N = PONY_MAX;

	// rigid posed positions (actor->world) for every chain joint; these track
	// the head (curwld already includes the R3 head look-at).
	for (i = 0; i < N; i++)
	{
		vec3_t a;
		int pj = iqm->pony_joint[i];
		a[0] = curwld[pj][0][3]; a[1] = curwld[pj][1][3]; a[2] = curwld[pj][2][3];
		a2w_point (a2w_rot, a2w_org, a, rigidW[i]);
	}
	for (i = 1; i < N; i++)
	{
		VectorSubtract (rigidW[i], rigidW[i-1], restdir[i]);
		rest[i] = VectorLength (restdir[i]);
		if (rest[i] < 0.01f) rest[i] = 0.01f;
	}

	st = dyn_get ((void *)ent);
	st->n = N;

	if (!st->inited)
	{
		for (i = 0; i < N; i++) { VectorCopy (rigidW[i], st->pos[i]); VectorCopy (rigidW[i], st->prev[i]); }
		st->inited = 1;
		st->last = now;
	}

	dt = now - st->last;
	st->last = now;
	if (dt < 0.0f) dt = 0.0f;
	if (dt > 0.1f) dt = 0.1f;        // clamp huge gaps (pause / teleport / first frame)

	damp  = actor_pony_damping.value;
	grav  = actor_pony_gravity.value;
	stiff = actor_pony_stiffness.value;
	iters = (int)actor_pony_iters.value; if (iters < 1) iters = 1;

	// node 0 is pinned to the rigid posed root (follows the head rigidly)
	VectorCopy (rigidW[0], st->pos[0]);
	VectorCopy (rigidW[0], st->prev[0]);

	// Verlet integrate the free nodes
	for (i = 1; i < N; i++)
	{
		vec3_t vel, np;
		VectorSubtract (st->pos[i], st->prev[i], vel);
		VectorScale (vel, damp, vel);
		np[0] = st->pos[i][0] + vel[0];
		np[1] = st->pos[i][1] + vel[1];
		np[2] = st->pos[i][2] + vel[2] - grav * dt * dt;
		VectorCopy (st->pos[i], st->prev[i]);
		VectorCopy (np, st->pos[i]);
	}

	// constraints: light straighten toward the rigid direction, then hold the
	// rest length to the parent (child-only correction; parent is anchor-ward)
	for (it = 0; it < iters; it++)
	{
		for (i = 1; i < N; i++)
		{
			vec3_t d; float len, diff;
			if (stiff > 0.0f)
			{
				vec3_t want;
				want[0] = st->pos[i-1][0] + restdir[i][0];
				want[1] = st->pos[i-1][1] + restdir[i][1];
				want[2] = st->pos[i-1][2] + restdir[i][2];
				st->pos[i][0] += (want[0] - st->pos[i][0]) * stiff * 0.5f;
				st->pos[i][1] += (want[1] - st->pos[i][1]) * stiff * 0.5f;
				st->pos[i][2] += (want[2] - st->pos[i][2]) * stiff * 0.5f;
			}
			VectorSubtract (st->pos[i], st->pos[i-1], d);
			len = VectorLength (d);
			if (len < 0.0001f) { d[0]=0; d[1]=0; d[2]=-1; len = 1; }
			diff = (len - rest[i]) / len;
			st->pos[i][0] -= d[0] * diff;
			st->pos[i][1] -= d[1] * diff;
			st->pos[i][2] -= d[2] * diff;
		}
	}

	// write skin matrices for the FREE joints (1..N-1). The root joint (0) keeps
	// its rigid R3 skin. Each segment's long (+Z) bind axis is aimed along the
	// simulated chain direction; +X/+Y fill an orthonormal basis.
	for (i = 1; i < N; i++)
	{
		int		pj = iqm->pony_joint[i];
		vec3_t	fwdw, zax, xax, yax, ref, posw, posa;
		float	posed[3][4];

		VectorSubtract (st->pos[i], st->pos[i-1], fwdw);   // world chain dir
		w2a_vec (a2w_rot, fwdw, zax);                      // -> actor space
		if (VectorNormalize (zax) < 0.0001f) { zax[0]=0; zax[1]=0; zax[2]=-1; }

		ref[0]=1; ref[1]=0; ref[2]=0;
		if (zax[0] > 0.99f || zax[0] < -0.99f) { ref[0]=0; ref[1]=1; ref[2]=0; }
		CrossProduct (ref, zax, xax); VectorNormalize (xax);
		CrossProduct (zax, xax, yax); VectorNormalize (yax);

		// node position world->actor (relative to entity origin)
		VectorSubtract (st->pos[i], a2w_org, posw);
		w2a_vec (a2w_rot, posw, posa);

		posed[0][0]=xax[0]; posed[0][1]=yax[0]; posed[0][2]=zax[0]; posed[0][3]=posa[0];
		posed[1][0]=xax[1]; posed[1][1]=yax[1]; posed[1][2]=zax[1]; posed[1][3]=posa[1];
		posed[2][0]=xax[2]; posed[2][1]=yax[2]; posed[2][2]=zax[2]; posed[2][3]=posa[2];

		R_ConcatTransforms (posed, bindinv[pj], skin[pj]);
	}
}
