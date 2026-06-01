// iqm_dev.c -- dev console commands for the mod_iqm renderer (R1 test harness)

#include "quakedef.h"
#include "iqm.h"
#include "iqm_dev.h"

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

void IQMDev_Init (void)
{
	Cmd_AddCommand ("actor_dump", Actor_Dump_f);
}

void IQMDev_AddToScene (void)
{
	// implemented in Task 6 (persistent dev actor injection)
}
