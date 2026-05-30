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
// sv_edict.c -- entity dictionary

#include "quakedef.h"
#include "hotreload.h"

/* In NATIVE_GAME=1 only these are needed; VM types not present */
char			*pr_strings;
globalvars_t	*pr_global_struct;
float			*pr_globals;
int				pr_edict_size;
unsigned short	pr_crc;


cvar_t	nomonsters = {"nomonsters", "0"};
cvar_t	gamecfg = {"gamecfg", "0"};
cvar_t	scratch1 = {"scratch1", "0"};
cvar_t	scratch2 = {"scratch2", "0"};
cvar_t	scratch3 = {"scratch3", "0"};
cvar_t	scratch4 = {"scratch4", "0"};
cvar_t	savedgamecfg = {"savedgamecfg", "0", true};
cvar_t	saved1 = {"saved1", "0", true};
cvar_t	saved2 = {"saved2", "0", true};
cvar_t	saved3 = {"saved3", "0", true};
cvar_t	saved4 = {"saved4", "0", true};


/*
=================
ED_ClearEdict

Sets everything to NULL
=================
*/
void ED_ClearEdict (edict_t *e)
{
	memset (&e->v, 0, sizeof(entvars_t));
	e->free = false;
}

/*
=================
ED_Alloc

Either finds a free edict, or allocates a new one.
Try to avoid reusing an entity that was recently freed, because it
can cause the client to think the entity morphed into something else
instead of being removed and recreated, which can cause interpolated
angles and bad trails.
=================
*/
edict_t *ED_Alloc (void)
{
	int			i;
	edict_t		*e;

	for ( i=svs.maxclients+1 ; i<sv.num_edicts ; i++)
	{
		e = EDICT_NUM(i);
		// the first couple seconds of server time can involve a lot of
		// freeing and allocating, so relax the replacement policy
		if (e->free && ( e->freetime < 2 || sv.time - e->freetime > 0.5 ) )
		{
			ED_ClearEdict (e);
			return e;
		}
	}
	
	if (i == MAX_EDICTS)
		Sys_Error ("ED_Alloc: no free edicts");
		
	sv.num_edicts++;
	e = EDICT_NUM(i);
	ED_ClearEdict (e);

	return e;
}

/*
=================
ED_Free

Marks the edict as free
FIXME: walk all entities and NULL out references to this entity
=================
*/
void ED_Free (edict_t *ed)
{
	SV_UnlinkEdict (ed);		// unlink from world bsp

	ed->free = true;
	ed->v.model = NULL;
	ed->v.takedamage = 0;
	ed->v.modelindex = 0;
	ed->v.colormap = 0;
	ed->v.skin = 0;
	ed->v.frame = 0;
	VectorCopy (vec3_origin, ed->v.origin);
	VectorCopy (vec3_origin, ed->v.angles);
	ed->v.nextthink = -1;
	ed->v.solid = 0;
	
	ed->freetime = sv.time;
}

//===========================================================================


//============================================================================


/*
=============
ED_NewString
=============
*/
char *ED_NewString (const char *string)
{
	char	*new, *new_p;
	int		i,l;
	
	l = strlen(string) + 1;
	new = Hunk_Alloc (l);
	new_p = new;

	for (i=0 ; i< l ; i++)
	{
		if (string[i] == '\\' && i < l-1)
		{
			i++;
			if (string[i] == 'n')
				*new_p++ = '\n';
			else
				*new_p++ = '\\';
		}
		else
			*new_p++ = string[i];
	}
	
	return new;
}



/*
====================
ED_ParseEdict

Parses an edict out of the given string, returning the new position
ed should be a properly initialized empty edict.
Used for initial level load and for savegames.
====================
*/
/* Native field table: maps keyname string → offset+type in entvars_t */
typedef enum { FT_FLOAT, FT_VECTOR, FT_STRING } native_ftype_t;
typedef struct { const char *name; size_t off; native_ftype_t type; } native_field_t;
#define NF_FLOAT(nm)  { #nm, offsetof(entvars_t, nm), FT_FLOAT }
#define NF_VEC(nm)    { #nm, offsetof(entvars_t, nm), FT_VECTOR }
#define NF_STR(nm)    { #nm, offsetof(entvars_t, nm), FT_STRING }
static const native_field_t s_nfields[] = {
	NF_STR(classname), NF_STR(model), NF_STR(netname), NF_STR(message),
	NF_STR(target), NF_STR(targetname), NF_STR(killtarget),
	NF_STR(noise), NF_STR(noise1), NF_STR(noise2), NF_STR(noise3), NF_STR(noise4),
	NF_STR(weaponmodel), NF_STR(wad), NF_STR(map), NF_STR(mdl), NF_STR(deathtype),
	NF_VEC(origin), NF_VEC(oldorigin), NF_VEC(velocity), NF_VEC(angles),
	NF_VEC(avelocity), NF_VEC(punchangle), NF_VEC(absmin), NF_VEC(absmax),
	NF_VEC(mins), NF_VEC(maxs), NF_VEC(size), NF_VEC(movedir), NF_VEC(view_ofs),
	NF_VEC(v_angle), NF_VEC(dest), NF_VEC(dest1), NF_VEC(dest2),
	NF_VEC(mangle), NF_VEC(pos1), NF_VEC(pos2), NF_VEC(finaldest), NF_VEC(finalangle),
	NF_FLOAT(ltime), NF_FLOAT(movetype), NF_FLOAT(solid),
	NF_FLOAT(waterlevel), NF_FLOAT(watertype),
	NF_FLOAT(health), NF_FLOAT(max_health), NF_FLOAT(frags), NF_FLOAT(deadflag),
	NF_FLOAT(takedamage), NF_FLOAT(dmg_take), NF_FLOAT(dmg_save),
	NF_FLOAT(armortype), NF_FLOAT(armorvalue),
	NF_FLOAT(items), NF_FLOAT(weapon), NF_FLOAT(weaponframe), NF_FLOAT(currentammo),
	NF_FLOAT(ammo_shells), NF_FLOAT(ammo_nails), NF_FLOAT(ammo_rockets), NF_FLOAT(ammo_cells),
	NF_FLOAT(modelindex), NF_FLOAT(frame), NF_FLOAT(skin), NF_FLOAT(effects),
	NF_FLOAT(colormap), NF_FLOAT(team), NF_FLOAT(spawnflags), NF_FLOAT(worldtype),
	NF_FLOAT(nextthink), NF_FLOAT(button0), NF_FLOAT(button1), NF_FLOAT(button2),
	NF_FLOAT(button3), NF_FLOAT(button4), NF_FLOAT(button5),
	NF_FLOAT(impulse), NF_FLOAT(fixangle), NF_FLOAT(idealpitch), NF_FLOAT(ideal_yaw),
	NF_FLOAT(yaw_speed), NF_FLOAT(teleport_time), NF_FLOAT(attack_finished),
	NF_FLOAT(pain_finished), NF_FLOAT(invincible_finished), NF_FLOAT(invisible_finished),
	NF_FLOAT(super_damage_finished), NF_FLOAT(radsuit_finished),
	NF_FLOAT(invincible_time), NF_FLOAT(invincible_sound),
	NF_FLOAT(invisible_time), NF_FLOAT(invisible_sound),
	NF_FLOAT(super_time), NF_FLOAT(super_sound), NF_FLOAT(rad_time), NF_FLOAT(fly_sound),
	NF_FLOAT(axhitme), NF_FLOAT(show_hostile), NF_FLOAT(jump_flag), NF_FLOAT(swim_flag),
	NF_FLOAT(air_finished), NF_FLOAT(bubble_count), NF_FLOAT(walkframe), NF_FLOAT(flags),
	NF_FLOAT(speed), NF_FLOAT(lefty), NF_FLOAT(search_time), NF_FLOAT(attack_state),
	NF_FLOAT(pausetime), NF_FLOAT(wait), NF_FLOAT(delay), NF_FLOAT(state),
	NF_FLOAT(height), NF_FLOAT(lip), NF_FLOAT(aflag), NF_FLOAT(dmg),
	NF_FLOAT(t_length), NF_FLOAT(t_width), NF_FLOAT(light_lev), NF_FLOAT(style),
	NF_FLOAT(cnt), NF_FLOAT(count), NF_FLOAT(sounds), NF_FLOAT(volume),
	NF_FLOAT(distance), NF_FLOAT(waitmin), NF_FLOAT(waitmax),
	NF_FLOAT(dmgtime), NF_FLOAT(healamount), NF_FLOAT(healtype), NF_FLOAT(hit_z),
	NF_FLOAT(decal_on_bounce),
	{NULL, 0, FT_FLOAT}
};

char *ED_ParseEdict (char *data, edict_t *ent)
{
	qboolean	anglehack;
	qboolean	init;
	char		keyname[256];
	int			n;
	int			j;
	float		v[3];
	char		*d;

	init = false;

	if (ent != sv.edicts)
		memset (&ent->v, 0, sizeof(entvars_t));

	while (1)
	{
		data = COM_Parse (data);
		if (com_token[0] == '}') break;
		if (!data) Sys_Error ("ED_ParseEntity: EOF without closing brace");

		if (!strcmp(com_token, "angle")) { strcpy(com_token, "angles"); anglehack = true; }
		else anglehack = false;
		if (!strcmp(com_token, "light")) strcpy(com_token, "light_lev");

		strcpy (keyname, com_token);
		n = strlen(keyname);
		while (n && keyname[n-1] == ' ') { keyname[n-1] = 0; n--; }

		data = COM_Parse (data);
		if (!data) Sys_Error ("ED_ParseEntity: EOF without closing brace");
		if (com_token[0] == '}') Sys_Error ("ED_ParseEntity: closing brace without data");

		init = true;
		if (keyname[0] == '_') continue;

		for (j = 0; s_nfields[j].name; j++) {
			if (!strcmp(s_nfields[j].name, keyname)) break;
		}
		if (!s_nfields[j].name) {
			Con_Printf ("'%s' is not a field\n", keyname);
			continue;
		}

		if (anglehack) {
			char temp[32];
			strcpy(temp, com_token);
			sprintf(com_token, "0 %s 0", temp);
		}

		d = (char *)&ent->v + s_nfields[j].off;
		switch (s_nfields[j].type) {
		case FT_STRING:
			*(const char **)d = ED_NewString(com_token);
			break;
		case FT_FLOAT:
			*(float *)d = (float)atof(com_token);
			break;
		case FT_VECTOR: {
			char *tok = com_token;
			for (j = 0; j < 3; j++) {
				while (*tok == ' ') tok++;
				v[j] = (float)atof(tok);
				while (*tok && *tok != ' ') tok++;
			}
			((float*)d)[0] = v[0];
			((float*)d)[1] = v[1];
			((float*)d)[2] = v[2];
			break;
		}
		}
	}

	if (!init)
		ent->free = true;

	return data;
}


/*
================
ED_LoadFromFile

The entities are directly placed in the array, rather than allocated with
ED_Alloc, because otherwise an error loading the map would have entity
number references out of order.

Creates a server's entity / program execution context by
parsing textual entity definitions out of an ent file.

Used for both fresh maps and savegame loads.  A fresh map would also need
to call ED_CallSpawnFunctions () to let the objects initialize themselves.
================
*/
void ED_LoadFromFile (char *data)
{
	edict_t		*ent;
	int			inhibit;

	ent = NULL;
	inhibit = 0;
	pr_global_struct->time = sv.time;

// parse ents
	while (1)
	{
// parse the opening brace
		data = COM_Parse (data);
		if (!data)
			break;
		if (com_token[0] != '{')
			Sys_Error ("ED_LoadFromFile: found %s when expecting {",com_token);

		if (!ent)
			ent = EDICT_NUM(0);
		else
			ent = ED_Alloc ();
		data = ED_ParseEdict (data, ent);

// remove things from different skill levels or deathmatch
		if (deathmatch.value)
		{
			if (((int)ent->v.spawnflags & SPAWNFLAG_NOT_DEATHMATCH))
			{
				ED_Free (ent);
				inhibit++;
				continue;
			}
		}
		else if ((current_skill == 0 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_EASY))
				|| (current_skill == 1 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_MEDIUM))
				|| (current_skill >= 2 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_HARD)) )
		{
			ED_Free (ent);
			inhibit++;
			continue;
		}

//
// immediately call spawn function
//
		if (!ent->v.classname)
		{
			Con_Printf ("No classname for:\n");
			ED_Print (ent);
			ED_Free (ent);
			continue;
		}

	// look for the spawn function
		pr_global_struct->self = EDICT_TO_PROG(ent);
		if (g_game_api)
			g_game_api->entity_spawn(ent, ent->v.classname);
	}

	Con_DPrintf ("%i entities inhibited\n", inhibit);
}


/*
===============
PR_LoadProgs
===============
*/
void PR_LoadProgs (void)
{
	int		i;

	// For NATIVE_GAME=1, skip loading progs.dat.
	// Set pr_edict_size to the native edict_t layout and set up a
	// minimal pr_global_struct stub (some engine code still reads it).
	// Always reallocate: Host_ClearMemory (called just above us in
	// SV_SpawnServer) ran Hunk_FreeToLowMark, so any prior pointer is now
	// stale and would alias whatever sv.edicts is about to allocate.
	pr_edict_size = sizeof(edict_t);
	pr_global_struct = (globalvars_t *)Hunk_AllocName(sizeof(globalvars_t), "pr_globals");
	pr_strings       = Hunk_AllocName(1, "pr_strings");
}


/*
===============
PR_Init
===============
*/
void PR_Init (void)
{
	Cmd_AddCommand ("edicts", ED_PrintEdicts);
	Cmd_AddCommand ("profile", PR_Profile_f);
	Cvar_RegisterVariable (&nomonsters);
	Cvar_RegisterVariable (&gamecfg);
	Cvar_RegisterVariable (&scratch1);
	Cvar_RegisterVariable (&scratch2);
	Cvar_RegisterVariable (&scratch3);
	Cvar_RegisterVariable (&scratch4);
	Cvar_RegisterVariable (&savedgamecfg);
	Cvar_RegisterVariable (&saved1);
	Cvar_RegisterVariable (&saved2);
	Cvar_RegisterVariable (&saved3);
	Cvar_RegisterVariable (&saved4);
}



edict_t *EDICT_NUM(int n)
{
	if (n < 0 || n >= sv.max_edicts)
		Sys_Error ("EDICT_NUM: bad number %i", n);
	return (edict_t *)((byte *)sv.edicts+ (n)*pr_edict_size);
}

int NUM_FOR_EDICT(edict_t *e)
{
	ptrdiff_t off;
	int       b;

	off = (byte *)e - (byte *)sv.edicts;
	b   = (int)(off / pr_edict_size);

	if (b < 0 || b >= sv.num_edicts)
	{
		Con_Printf ("NUM_FOR_EDICT: bad pointer\n"
			"  e             = %p\n"
			"  sv.edicts     = %p\n"
			"  sv.num_edicts = %d  (max=%d, edict_size=%d)\n"
			"  byte offset   = %lld  -> index %d (out of range)\n",
			(void *)e, (void *)sv.edicts,
			sv.num_edicts, sv.max_edicts, pr_edict_size,
			(long long)off, b);
		// Force a debugger-catchable crash: __builtin_trap on clang
		// becomes UD2 on x86, which the OS reports as an access
		// violation / illegal instruction with a real call stack
		// (vs. Sys_Error -> exit(1) which loses the frames).
		__builtin_trap ();
	}
	return b;
}

/* ---------------------------------------------------------------------------
 * Stubs for VM-era functions that are compiled away with NATIVE_GAME=1 but
 * still referenced from engine code that we haven't fully guarded yet.
 * Savegame support for NATIVE_GAME is not implemented.
 * --------------------------------------------------------------------------- */
void ED_Print (edict_t *ed)
{
	if (!ed) return;
	Con_Printf ("<edict %d: classname=%s>\n",
		NUM_FOR_EDICT(ed),
		ed->v.classname ? ed->v.classname : "(null)");
}
void ED_PrintEdicts (void) { Con_Printf ("<ED_PrintEdicts: NATIVE_GAME stub>\n"); }
void ED_WriteGlobals (FILE *f) { (void)f; }
void ED_Write (FILE *f, edict_t *ed) { (void)f; (void)ed; }
void ED_ParseGlobals (char *data) { (void)data; }
void PR_Profile_f (void) { Con_Printf ("<PR_Profile_f: no VM in NATIVE_GAME=1>\n"); }
void PR_RunError (char *error, ...) { Sys_Error(error); }
