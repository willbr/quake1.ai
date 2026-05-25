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

// ---------------------------------------------------------------------------
// Native entity types from game_types.h. The QC VM (entvars_t / progs.dat)
// is gone; pr_strings / pr_global_struct / pr_edict_size survive as a small
// compatibility shim because engine code still pokes them.
// ---------------------------------------------------------------------------
// common.h (already included by quakedef.h before us) defines link_t and
// entity_state_t is defined inline in quakedef.h. Set guards so game_types.h
// doesn't redefine them.
#define LINK_T_DEFINED
#define ENTITY_STATE_T_DEFINED
#include "game_api.h"    // edict_t forward decl, game_globals_t
#include "game_types.h"  // full edict_t with native C types

extern char           *pr_strings;
extern int             pr_edict_size;
extern game_globals_t  game_globals;
extern unsigned short  pr_crc;

// Minimal globalvars_t stub -- engine code still references pr_global_struct
typedef struct {
	int   pad[28];
	int   self, other, world;
	float time, frametime, force_retouch;
	int   mapname_unused;
	float deathmatch, coop, teamplay, serverflags;
	float total_secrets, total_monsters, found_secrets, killed_monsters;
	float parm1, parm2, parm3, parm4, parm5, parm6, parm7, parm8;
	float parm9, parm10, parm11, parm12, parm13, parm14, parm15, parm16;
	float v_forward[3], v_up[3], v_right[3];
	float trace_allsolid, trace_startsolid, trace_fraction;
	float trace_endpos[3], trace_plane_normal[3];
	float trace_plane_dist;
	int   trace_ent;
	float trace_inopen, trace_inwater;
	int   msg_entity;
} globalvars_t;
extern globalvars_t *pr_global_struct;

void PR_Init(void);
void PR_LoadProgs(void);
void PR_ExecuteProgram(int fnum);   /* never called; stub for legacy refs */

edict_t *ED_Alloc(void);
void     ED_Free(edict_t *ed);
char    *ED_NewString(const char *string);
void     ED_Print(edict_t *ed);
char    *ED_ParseEdict(char *data, edict_t *ent);
void     ED_LoadFromFile(char *data);
void     ED_PrintEdicts(void);
void     ED_PrintNum(int ent);

edict_t *EDICT_NUM(int n);
int      NUM_FOR_EDICT(edict_t *e);

#define NEXT_EDICT(e)    ((edict_t *)((byte *)(e) + pr_edict_size))
/* Entity-link fields are edict_t* now (not int offsets into sv.edicts as in
 * the VM era). PROG_TO_EDICT is a no-op cast; EDICT_TO_PROG produces a byte
 * offset only for the legacy pr_global_struct->self int slot, which the
 * engine writes but doesn't read back meaningfully. */
#define EDICT_TO_PROG(e) ((int)((byte *)(e) - (byte *)sv.edicts))
#define PROG_TO_EDICT(e) ((edict_t *)(e))
#define EDICT_FROM_AREA(l) STRUCT_FROM_LINK(l,edict_t,area)

/* For compat with GetEdictFieldValue calls in debug code */
typedef union { float _float; int _int; char *string; } eval_t;
static inline eval_t *GetEdictFieldValue(edict_t *ed, char *field) {
	(void)ed; (void)field; return (eval_t*)0;
}

/* Dummy to satisfy any remaining references */
void PR_RunError(char *error, ...);
void PR_Profile_f(void);


