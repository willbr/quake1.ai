/* sv_bridge.h -- accessor functions that give hotreload.c correct sv/svs field offsets.
 *
 * hotreload.c cannot include quakedef.h (would conflict with its minimal type stubs),
 * so all sv.xxx / svs.xxx field accesses go through these bridge functions instead.
 * sv_bridge.c includes quakedef.h and has the real struct layouts.
 *
 * Only used when NATIVE_GAME=1.
 */
#ifndef SV_BRIDGE_H
#define SV_BRIDGE_H

struct edict_s;
struct sizebuf_s;

/* --- sv fields --- */
struct sizebuf_s *svb_datagram(void);
struct sizebuf_s *svb_reliable_datagram(void);
struct sizebuf_s *svb_signon(void);
int               svb_state(void);     /* ss_loading=0, ss_active=1 */
int               svb_active(void);    /* sv.active (qboolean) */
const char       *svb_name(void);      /* sv.name (current map) */
struct edict_s   *svb_edicts(void);
int               svb_num_edicts(void);
const char       *svb_model_precache(int i);
void              svb_set_model_precache(int i, const char *s);
/* Fills mins/maxs from sv.models[i].  Returns 1 on success, 0 if no such model. */
int               svb_model_bounds(int i, float *mins, float *maxs);
/* Loads name via Mod_ForName and stores in sv.models[i].  Mirrors PF_precache_model
 * for non-inline BSP/MDL/SPR files (e.g. maps/b_explob.bsp) so SV_SetModel can pull
 * mins/maxs back out — without this, such entities get a zero-size bbox. */
void              svb_load_model(int i, const char *name);
const char       *svb_sound_precache(int i);
void              svb_set_sound_precache(int i, const char *s);
void              svb_set_lightstyle(int i, const char *s);

/* --- svs fields --- */
int               svb_maxclients(void);
int               svb_changelevel_issued(void);
void              svb_set_changelevel_issued(int v);

/* --- svs.clients[i] fields --- */
int               svb_client_active(int i);
int               svb_client_spawned(int i);
struct edict_s   *svb_client_edict(int i);
struct sizebuf_s *svb_client_message(int i);
float            *svb_client_spawn_parms(int i);

/* --- compound operations that need the real client_t layout --- */
void              svb_sv_sprint(struct edict_s *e, int level, const char *msg);
void              svb_sv_centerprint(struct edict_s *e, const char *msg);
void              svb_sv_stuffcmd(struct edict_s *e, const char *cmd);
void              svb_sv_lightstyle(int style, const char *val);

#endif /* SV_BRIDGE_H */
