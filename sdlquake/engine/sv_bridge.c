/* sv_bridge.c -- thin accessors giving hotreload.c correct sv/svs struct offsets.
 *
 * This file includes quakedef.h so it compiles with the real server_t and
 * server_static_t layouts from server.h.  hotreload.c must NOT include
 * quakedef.h (its minimal hand-rolled type stubs would conflict), so any
 * access to sv.xxx or svs.clients[i].xxx goes through these functions.
 */

#include "quakedef.h"   /* server_t sv, server_static_t svs, client_t, sizebuf_t, MSG_* */
#include "sv_bridge.h"

/* --- sv fields --- */

sizebuf_t *svb_datagram(void)           { return &sv.datagram; }
sizebuf_t *svb_reliable_datagram(void)  { return &sv.reliable_datagram; }
sizebuf_t *svb_signon(void)             { return &sv.signon; }
int        svb_state(void)              { return (int)sv.state; }
edict_t   *svb_edicts(void)             { return sv.edicts; }
int        svb_num_edicts(void)         { return sv.num_edicts; }

const char *svb_model_precache(int i)   { return sv.model_precache[i]; }
void svb_set_model_precache(int i, const char *s) { sv.model_precache[i] = (char *)s; }

int svb_model_bounds(int i, float *mins, float *maxs)
{
    model_t *mod;
    if (i < 0 || i >= MAX_MODELS) return 0;
    mod = sv.models[i];
    if (!mod) return 0;
    mins[0] = mod->mins[0]; mins[1] = mod->mins[1]; mins[2] = mod->mins[2];
    maxs[0] = mod->maxs[0]; maxs[1] = mod->maxs[1]; maxs[2] = mod->maxs[2];
    return 1;
}

void svb_load_model(int i, const char *name)
{
    model_t *m;
    if (i < 0 || i >= MAX_MODELS) return;
    if (sv.models[i] && cl.model_precache[i]) return;   /* fully synced */
    m = Mod_ForName((char *)name, false);
    if (!sv.models[i])           sv.models[i]         = m;
    /* Mid-game precaches (e.g. editor-spawned monsters mid-session) need
     * to mirror cl.model_precache too — otherwise the client renderer
     * looks up modelindex N and finds NULL, so the entity is invisible.
     * Signon will memset+refill cl.model_precache afterwards on a fresh
     * map load, so writing early is safe. */
    if (!cl.model_precache[i])   cl.model_precache[i] = m;
}

const char *svb_sound_precache(int i)   { return sv.sound_precache[i]; }
void svb_set_sound_precache(int i, const char *s) { sv.sound_precache[i] = (char *)s; }

void svb_set_lightstyle(int i, const char *s) {
    if (i >= 0 && i < MAX_LIGHTSTYLES)
        sv.lightstyles[i] = (char *)s;
}

/* --- svs fields --- */

int  svb_maxclients(void)              { return svs.maxclients; }
int  svb_changelevel_issued(void)      { return (int)svs.changelevel_issued; }
void svb_set_changelevel_issued(int v) { svs.changelevel_issued = (qboolean)v; }

/* --- svs.clients[i] fields --- */

int        svb_client_active(int i)    { return (int)svs.clients[i].active; }
int        svb_client_spawned(int i)   { return (int)svs.clients[i].spawned; }
edict_t   *svb_client_edict(int i)     { return svs.clients[i].edict; }
sizebuf_t *svb_client_message(int i)   { return &svs.clients[i].message; }
float     *svb_client_spawn_parms(int i) { return svs.clients[i].spawn_parms; }

/* --- compound operations using real client_t layout --- */

void svb_sv_sprint(edict_t *e, int level, const char *msg)
{
    int i;
    (void)level;
    for (i = 0; i < svs.maxclients; i++) {
        if (svs.clients[i].edict == e && svs.clients[i].active) {
            MSG_WriteByte(&svs.clients[i].message, 8); /* svc_print */
            MSG_WriteString(&svs.clients[i].message, (char *)msg);
            return;
        }
    }
}

void svb_sv_centerprint(edict_t *e, const char *msg)
{
    int i;
    for (i = 0; i < svs.maxclients; i++) {
        if (svs.clients[i].edict == e && svs.clients[i].active) {
            MSG_WriteByte(&svs.clients[i].message, 26); /* svc_centerprint */
            MSG_WriteString(&svs.clients[i].message, (char *)msg);
            return;
        }
    }
}

void svb_sv_stuffcmd(edict_t *e, const char *cmd)
{
    int i;
    for (i = 0; i < svs.maxclients; i++) {
        if (svs.clients[i].edict == e && svs.clients[i].active) {
            MSG_WriteByte(&svs.clients[i].message, 9); /* svc_stufftext */
            MSG_WriteString(&svs.clients[i].message, (char *)cmd);
            return;
        }
    }
}

void svb_sv_lightstyle(int style, const char *val)
{
    int j;
    if (style >= 0 && style < MAX_LIGHTSTYLES)
        sv.lightstyles[style] = (char *)val;
    if (sv.state != ss_active) return;
    for (j = 0; j < svs.maxclients; j++) {
        client_t *cl = &svs.clients[j];
        if (cl->active || cl->spawned) {
            MSG_WriteChar(&cl->message, svc_lightstyle);
            MSG_WriteChar(&cl->message, style);
            MSG_WriteString(&cl->message, (char *)val);
        }
    }
}

