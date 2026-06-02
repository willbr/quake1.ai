// iqm_dev.c -- dev console commands for the mod_iqm renderer (R1 test harness)

#include "quakedef.h"
#include "iqm.h"
#include "iqm_dev.h"

static entity_t	dev_actor;
static qboolean	dev_actor_active = false;
static model_t	*dev_actor_world;   // worldmodel at spawn; a map change deactivates
									//  the dev actor so we never read stale iqmdata

/*
=================
Actor_Dump_f
  Loads an .iqm and prints its parsed structure — verifies the IQM reader.
=================
*/
static void Actor_Dump_f (void)
{
	char		name[MAX_QPATH];
	model_t		*mod;
	lm_iqm_t	*iqm;
	int			i;

	if (Cmd_Argc () < 2) { Con_Printf ("usage: actor_dump <file.iqm>\n"); return; }
	Q_strcpy (name, Cmd_Argv (1));
	mod = Mod_ForName (name, false);
	if (!mod) { Con_Printf ("actor_dump: %s not found\n", name); return; }
	if (mod->type != mod_iqm) { Con_Printf ("actor_dump: %s is not an IQM\n", name); return; }

	iqm = mod->iqmdata;
	Con_Printf ("IQM %s: %d meshes, %d joints, %d verts, %d tris\n",
		name, iqm->nummeshes, iqm->numjoints, iqm->numverts, iqm->numtris);
	Con_Printf ("  mins %g %g %g  maxs %g %g %g\n",
		iqm->mins[0], iqm->mins[1], iqm->mins[2],
		iqm->maxs[0], iqm->maxs[1], iqm->maxs[2]);
	Con_Printf ("  anim: %d frames @ %g fps%s\n",
		iqm->numframes, iqm->framerate, iqm->frametrs ? "" : " (static)");
	Con_Printf ("  roles: head %d  chest %d  jaw %d  eyes %d  pony %d\n",
		iqm->head_joint, iqm->chest_joint, iqm->jaw_joint, iqm->num_eye, iqm->num_pony);
	for (i = 0; i < iqm->numjoints; i++)
		Con_Printf ("  joint %d '%s' parent %d  t(%g %g %g)\n", i,
			iqm->joints[i].name, iqm->joints[i].parent,
			iqm->joints[i].translate[0], iqm->joints[i].translate[1],
			iqm->joints[i].translate[2]);
	for (i = 0; i < iqm->nummeshes; i++)
		Con_Printf ("  mesh %d '%s' mat '%s' v[%u+%u] t[%u+%u]\n", i,
			iqm->meshes[i].name, iqm->meshes[i].material,
			iqm->meshes[i].first_vertex, iqm->meshes[i].num_vertexes,
			iqm->meshes[i].first_triangle, iqm->meshes[i].num_triangles);
}

/*
=================
Actor_Spawn_f
  Creates a persistent client-side entity holding the given IQM, placed in front
  of the player (or at explicit x y z). Rendered without any server involvement.
=================
*/
static void Actor_Spawn_f (void)
{
	char	name[MAX_QPATH];
	model_t	*mod;
	vec3_t	fwd, right, up, src;

	if (Cmd_Argc () < 2) { Con_Printf ("usage: actor_spawn <file.iqm> [x y z]\n"); return; }
	if (cls.state != ca_connected || !cl.worldmodel)
	{ Con_Printf ("actor_spawn: load a map first\n"); return; }

	Q_strcpy (name, Cmd_Argv (1));
	mod = Mod_ForName (name, false);
	if (!mod) { Con_Printf ("actor_spawn: %s not found\n", name); return; }
	if (mod->type != mod_iqm) { Con_Printf ("actor_spawn: %s is not an IQM\n", name); return; }

	memset (&dev_actor, 0, sizeof(dev_actor));
	dev_actor.model    = mod;
	dev_actor.colormap = vid.colormap;
	dev_actor.frame    = 0;

	if (Cmd_Argc () >= 5)
	{
		dev_actor.origin[0] = Q_atof (Cmd_Argv (2));
		dev_actor.origin[1] = Q_atof (Cmd_Argv (3));
		dev_actor.origin[2] = Q_atof (Cmd_Argv (4));
	}
	else
	{
		VectorCopy (cl_entities[cl.viewentity].origin, src);
		src[2] += 24;
		AngleVectors (cl.viewangles, fwd, right, up);
		VectorMA (src, 96, fwd, dev_actor.origin);
		dev_actor.angles[YAW] = cl.viewangles[YAW] + 180;	// face the player
	}

	dev_actor_active = true;
	dev_actor_world  = cl.worldmodel;
	Con_Printf ("actor_spawn: %s at %g %g %g\n", name,
		dev_actor.origin[0], dev_actor.origin[1], dev_actor.origin[2]);
}

static void Actor_Clear_f (void)
{
	dev_actor_active = false;
	Con_Printf ("actor cleared\n");
}

/*
=================
Actor_Roundtrip_f
  Load an IQM, serialize it back out with lm_write_iqm, re-parse the bytes, and
  verify the geometry + skeleton survive the round trip. Tests the editor's IQM
  writer (E1 pipeline backend) without any UI. Animation is not written yet, so
  the re-parsed model is static (compared on geometry/joints only).
=================
*/
static void Actor_Roundtrip_f (void)
{
	char		name[MAX_QPATH];
	model_t		*mod;
	lm_iqm_t	*src, *rt;
	void		*buf;
	size_t		len;
	lm_result_t	r;
	int			i, ok;

	if (Cmd_Argc () < 2) { Con_Printf ("usage: actor_roundtrip <file.iqm>\n"); return; }
	Q_strcpy (name, Cmd_Argv (1));
	mod = Mod_ForName (name, false);
	if (!mod || mod->type != mod_iqm) { Con_Printf ("actor_roundtrip: %s is not an IQM\n", name); return; }
	src = mod->iqmdata;

	r = lm_write_iqm (src, &buf, &len);
	if (r != LM_OK) { Con_Printf ("actor_roundtrip: write failed (%d)\n", (int)r); return; }

	r = lm_load_iqm (buf, len, 0, &rt);
	if (r != LM_OK) { Con_Printf ("actor_roundtrip: re-parse failed (%d), wrote %d bytes\n", (int)r, (int)len); free (buf); return; }

	ok = (src->nummeshes == rt->nummeshes && src->numjoints == rt->numjoints &&
	      src->numverts  == rt->numverts  && src->numtris   == rt->numtris);
	for (i = 0; ok && i < src->numjoints; i++)
		if (src->joints[i].parent != rt->joints[i].parent || Q_strcmp (src->joints[i].name, rt->joints[i].name))
			ok = 0;
	for (i = 0; ok && i < src->numverts; i++)
		if (src->verts[i].pos[0] != rt->verts[i].pos[0] || src->verts[i].pos[1] != rt->verts[i].pos[1] ||
		    src->verts[i].pos[2] != rt->verts[i].pos[2] || src->verts[i].bone   != rt->verts[i].bone)
			ok = 0;

	Con_Printf ("actor_roundtrip %s: wrote %d bytes; src(m%d j%d v%d t%d) -> rt(m%d j%d v%d t%d) %s\n",
		name, (int)len,
		src->nummeshes, src->numjoints, src->numverts, src->numtris,
		rt->nummeshes,  rt->numjoints,  rt->numverts,  rt->numtris,
		ok ? "MATCH" : "MISMATCH");

	lm_iqm_free (rt);
	free (buf);
}

void IQMDev_Init (void)
{
	Cmd_AddCommand ("actor_dump",      Actor_Dump_f);
	Cmd_AddCommand ("actor_spawn",     Actor_Spawn_f);
	Cmd_AddCommand ("actor_clear",     Actor_Clear_f);
	Cmd_AddCommand ("actor_roundtrip", Actor_Roundtrip_f);
}

void IQMDev_AddToScene (void)
{
	if (!dev_actor_active || !dev_actor.model)
		return;
	if (cl.worldmodel != dev_actor_world)	// map changed -> iqmdata is stale
	{
		dev_actor_active = false;
		return;
	}
	if (cl_numvisedicts >= MAX_VISEDICTS)
		return;
	cl_visedicts[cl_numvisedicts++] = &dev_actor;
}
