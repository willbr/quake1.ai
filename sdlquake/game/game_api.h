// game_api.h -- ABI between the engine and the hot-reloadable game DLL.
// Bump GAME_API_VERSION whenever any struct layout changes.

#ifndef GAME_API_H
#define GAME_API_H

#define GAME_API_VERSION 8

// Forward declarations (full definitions in game_types.h)
typedef struct edict_s edict_t;
typedef float vec3_t[3];

// ---------------------------------------------------------------------------
// Shared mutable globals — owned by the game DLL, pointed to by both sides.
// Engine writes time/frametime/self/other before calling any game entry point.
// Game writes trace_* and v_forward/up/right after computing them.
// ---------------------------------------------------------------------------
typedef struct {
    float    time;
    float    frametime;
    float    force_retouch;  // decremented each frame; triggers full entity retouch
    float    deathmatch;
    float    coop;
    float    teamplay;
    edict_t *self;
    edict_t *other;
    edict_t *world;
    edict_t *activator;    // entity that fired a trigger
    edict_t *damage_attacker;
    edict_t *msg_entity;   // destination for MSG_ONE writes

    // makevectors output
    vec3_t   v_forward, v_up, v_right;

    // traceline output
    float    trace_allsolid;
    float    trace_startsolid;
    float    trace_fraction;
    vec3_t   trace_endpos;
    vec3_t   trace_plane_normal;
    float    trace_plane_dist;
    edict_t *trace_ent;
    float    trace_inopen;
    float    trace_inwater;

    // spawn parms (carry across level changes)
    float    parm[16];

    // server state
    float    serverflags;
    float    total_secrets,   found_secrets;
    float    total_monsters,  killed_monsters;
    float    movedist;
    float    gameover;
    float    framecount;
    edict_t *newmis;         // set by launch_spike after spawning

    // map name (set at level load; points to persistent sv.name, not a temp buffer)
    const char *mapname;
} game_globals_t;

// ---------------------------------------------------------------------------
// engine_api_t — functions the engine exposes to the game DLL.
// ---------------------------------------------------------------------------
typedef struct engine_api_s {
    void   (*Con_Print)(const char *msg);
    void   (*Cvar_SetValue)(const char *name, float value);
    float  (*Cvar_VariableValue)(const char *name);
    double (*Sys_FloatTime)(void);

    // Entity management
    edict_t    *(*ED_Alloc)(void);
    void        (*ED_Free)(edict_t *e);
    edict_t    *(*ED_Find)(edict_t *start, const char *field, const char *value);
    edict_t    *(*ED_FindRadius)(vec3_t origin, float radius);
    edict_t    *(*ED_Next)(edict_t *e);
    edict_t    *(*ED_CheckClient)(void);
    int         (*ED_GetNum)(edict_t *e);   // entity index (for network writes)

    // Spatial / physics
    void  (*SV_SetOrigin)(edict_t *e, vec3_t origin);
    void  (*SV_SetModel)(edict_t *e, const char *model);
    void  (*SV_SetSize)(edict_t *e, vec3_t mins, vec3_t maxs);
    int   (*SV_DropToFloor)(edict_t *e);
    int   (*SV_WalkMove)(edict_t *e, float yaw, float dist);
    void  (*SV_ChangeYaw)(edict_t *e);
    void  (*SV_Traceline)(vec3_t start, vec3_t end, int nomonsters, edict_t *skip);
    int   (*SV_CheckBottom)(edict_t *e);
    int   (*SV_PointContents)(vec3_t point);
    void  (*SV_MoveToGoal)(edict_t *e, float step);
    void  (*SV_Aim)(edict_t *e, float speed, vec3_t out);

    // Output / messaging
    void  (*SV_BPrint)(int level, const char *msg);
    void  (*SV_SPrint)(edict_t *client, int level, const char *msg);
    void  (*SV_CenterPrint)(edict_t *client, const char *msg);
    void  (*SV_StuffCmd)(edict_t *client, const char *cmd);
    void  (*Cbuf_AddText)(const char *cmd);
    void  (*Con_DPrintf)(const char *msg);
    void  (*Host_Error)(const char *msg);

    // Network message writing (dest = MSG_BROADCAST/ONE/ALL/INIT)
    void  (*MSG_WriteByte)(int dest, int val);
    void  (*MSG_WriteChar)(int dest, int val);
    void  (*MSG_WriteShort)(int dest, int val);
    void  (*MSG_WriteLong)(int dest, int val);
    void  (*MSG_WriteAngle)(int dest, float val);
    void  (*MSG_WriteCoord)(int dest, float val);
    void  (*MSG_WriteString)(int dest, const char *s);
    void  (*MSG_WriteEntity)(int dest, edict_t *e);

    // Audio
    void  (*SV_StartSound)(edict_t *e, int channel, const char *sample,
                           float vol, float attenuation);
    void  (*SV_AmbientSound)(vec3_t origin, const char *sample,
                             float vol, float attenuation);
    void  (*SV_LightStyle)(int style, const char *pattern);

    // Math
    void   (*MakeVectors)(vec3_t angles);
    void   (*VectorNormalize)(vec3_t v, vec3_t out);
    float  (*VectorLength)(vec3_t v);
    float  (*VectorToYaw)(vec3_t v);
    void   (*VectorToAngles)(vec3_t v, vec3_t out);
    float  (*Random)(void);
    float  (*FAbsF)(float f);

    // String conversion (return static buffer, use immediately)
    const char *(*FToS)(float f);
    const char *(*VToS)(vec3_t v);

    // Precache (call during entity spawn or level init only)
    const char *(*PrecacheModel)(const char *path);
    const char *(*PrecacheSound)(const char *path);
    const char *(*PrecacheFile)(const char *path);

    // Misc
    void  (*SV_ChangeLevel)(const char *map);
    void  (*SV_Particle)(vec3_t origin, vec3_t dir, float color, float count);
    void  (*SV_MakeStatic)(edict_t *e);
    void  (*SV_SetSpawnParms)(edict_t *client);

    // imgui dev panels (no-op if imgui inactive)
    void  (*ImguiAI_Clear)(void);
    void  (*ImguiAI_Push)(int edict_num, int state, float alert_level,
                          const float last_known_pos[3], int target_edict);
    int   (*ImguiAI_Active)(void);   // returns 1 if the panel is visible
    void  (*ImguiNav_Set)(const void *pts_xy, int np,
                          const void *edges_ushort_pairs, int ne);
    void  (*ImguiNav_SetPath)(const void *path_xy_floats, int n);

    // Cvar registration (must be called during init, before console is used)
    void  (*Cvar_Register)(const char *name, const char *default_val);
} engine_api_t;

// ---------------------------------------------------------------------------
// game_api_t — functions the game DLL exposes to the engine.
// ---------------------------------------------------------------------------
typedef struct game_api_s {
    int   version;   // must equal GAME_API_VERSION

    void  (*init)(engine_api_t *engine, game_globals_t *globals);
    void  (*shutdown)(void);

    // Per server frame (called after physics)
    void  (*start_frame)(void);

    // Entity lifecycle
    void  (*entity_spawn)(edict_t *e, const char *classname);
    void  (*entity_think)(edict_t *e);
    void  (*entity_touch)(edict_t *e, edict_t *other);

    // Client lifecycle
    void  (*client_connect)(edict_t *client);
    void  (*client_disconnect)(edict_t *client);
    void  (*put_client_in_server)(edict_t *client);
    void  (*client_prethink)(edict_t *client);
    void  (*client_postthink)(edict_t *client);
    void  (*client_kill)(edict_t *client);

    // Level transitions
    void  (*set_new_parms)(void);
    void  (*set_change_parms)(edict_t *client);

    // Editor introspection — return a NULL-terminated array of classname
    // strings the DLL knows how to spawn. Pointers borrow into the DLL's
    // static spawn table; valid for the DLL's lifetime. *out_count receives
    // the entry count (excluding the trailing NULL).
    const char *const *(*list_spawn_classes)(int *out_count);
} game_api_t;

typedef game_api_t *(*Game_GetAPI_fn)(void);

#endif // GAME_API_H
