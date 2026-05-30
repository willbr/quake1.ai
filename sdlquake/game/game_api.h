// game_api.h -- ABI between the engine and the hot-reloadable game DLL.
// Bump GAME_API_VERSION whenever any struct layout changes.

#ifndef GAME_API_H
#define GAME_API_H

#define GAME_API_VERSION 36

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

    // VFS file loader — searches PAK files; caller frees the buffer with free().
    // Returns NULL if the file is not found. out_size receives the file length.
    void *(*LoadFile)(const char *path, int *out_size);

    // Debug line overlay — drawn into vid.buffer this frame only. Color is
    // a Quake palette index (0-255). ztest != 0 occludes the line against
    // the world depth buffer so only the un-walled portion is visible.
    // Call every frame to keep the line visible.
    void  (*SV_DebugLine)(vec3_t start, vec3_t end, int color, int ztest);

    // Current render-camera origin (r_origin). Used by overlays to cull
    // submission by distance; far edges are skipped before the cross-DLL
    // SV_DebugLine call even fires.
    void  (*Get_ViewOrigin)(vec3_t out);

    // Bbox sweep — like SV_Traceline but with non-zero mins/maxs so the
    // trace accounts for entity volume. Required for navmesh validation
    // where the player's 32x32x56 body must actually fit through any gap
    // the bake claims is traversable. Writes the same trace_* globals.
    void  (*SV_TraceMove)(vec3_t start, vec3_t mins, vec3_t maxs,
                          vec3_t end, int nomonsters, edict_t *skip);

    // Spawn a growing blood pool on the floor beneath origin. Called from
    // game.dll on monster death and from the gib settle path. No-op if no
    // floor within 64 units, or if r_decals/r_decals_bloodpool is 0.
    //   radius_max ≤ 0 → use r_decals_bloodpool_radius cvar default.
    //   owner          → if non-NULL, pool freezes (stops growing) when this
    //                    edict is freed, has its classname pointer changed,
    //                    or moves more than r_decals_bloodpool_owner_drift
    //                    units. Already-painted luxels stay permanent.
    void  (*spawn_blood_pool)(const vec3_t origin, float radius_max,
                              edict_t *owner);

    // Seed a gib's blood budget. The engine decrements the budget by 1.0 on
    // each bounce-decal stain and 2.0 on the (one-shot) settle pool that
    // fires when the gib comes to rest. When the budget hits zero the gib
    // still bounces but stops painting. settle_pool_radius ≤ 0 disables the
    // settle pool for this gib (it'll only paint splat/drip decals).
    void  (*set_gib_blood_budget)(edict_t *gib, float budget,
                                  float settle_pool_radius);

    // Sample the lightmap at a world-space point (Phase 8 / M5).
    // Returns 0..255; 0 if no lightmap is available.
    int   (*Sample_Lightmap)(vec3_t pos);

    // Lightmap delta system (cached preview + gust extinguish).
    // Applies a per-texel additive contribution into the live rgb
    // lightmap; owner: 1=editor preview, 2=gameplay (gust).
    // ClearOwner drops every override matching the tag and rebuilds
    // the live buffer from baked + Σ(surviving overrides).
    void  (*Lightmap_AddDelta)(const vec3_t pos, float radius,
                               const vec3_t color, int owner);
    void  (*Lightmap_ClearOwner)(int owner);

    // Smoke particle puff — like SV_Particle but uses pt_static (no gravity),
    // long lifetime (~1.5–2.8s), and a tame velocity (caller's dir + ±8 noise).
    // Bypasses the svc_particle wire format, which would otherwise clamp dir
    // to ±8 units and lifetime to ≤0.4s. Single-player only.
    void  (*SV_Smoke)(vec3_t origin, vec3_t dir, float color, float count);

    // Queue one voxel-sized smoke billboard for this frame. Drained during
    // R_DrawParticles. world_size is the cell side length in world units;
    // density (0..1) modulates a screen-space dither hash. Use for cell-
    // aligned haze rendering driven by sim_wind's smoke grid.
    void  (*Draw_SmokeCell)(vec3_t origin, float world_size, float density);

    // Push pt_smoke particles outward from a moving axis (rocket tunnel
    // effect). For each pt_smoke whose perpendicular distance to the
    // axis-line through `origin` is < radius, add an impulse velocity
    // pointing radially away from the axis, scaled by mag * falloff.
    // Bypasses the wind grid -- the rocket flies through too fast for
    // wind-drag-mediated displacement to catch up. Cheap O(N_particles).
    void  (*Particles_PushTube)(const vec3_t origin, const vec3_t axis,
                                 float mag, float radius);

    // Scoped per-frame profiler — push/pop pairs nest under the engine's
    // PERF_SCOPE tree. `name` must outlive the next frame (use string
    // literals). Safe to call any time on the main thread; a no-op when
    // not in a tracked frame.
    void  (*Perf_PushScope)(const char *name);
    void  (*Perf_PopScope) (void);

    // Fire plume for a burning entity (Phase 8 / M8). Sibling of SV_Smoke:
    // spawns engine pt_fire particles that rise (orange ramp3 -> grey, ~1.2s).
    // The svc_particle/pt_grav path can only fall and sprays debris, so it
    // can't read as flame. `dir` is a base velocity (per-particle ±3 swirl
    // added); count is particles per call. Single-player only.
    void  (*SV_Fire)(vec3_t origin, vec3_t dir, float count);

    // Stamp a decal at `pos` (M8 F2). `type` is a decal_type_t value (see the
    // DECAL_* mirror in game_defs.h). The engine self-finds the surface near
    // `pos`; single-player, main thread. Used for the oil floor stain and the
    // scorch a fire leaves behind.
    void  (*SV_Decal)(vec3_t pos, int type);
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

    // Per-render-frame debug overlay submission. Called from host.c right
    // before SCR_UpdateScreen, independent of sv.paused / Editor_IsPaused —
    // so navmesh / AI / stim overlays remain visible while the editor or
    // pause has frozen the sim. Implementations should re-submit lines via
    // SV_DebugLine each frame (DebugLines_Add clears after every draw).
    void  (*debug_draw_overlays)(void);

    // Console-command targets (registered engine-side by Host_InitCommands,
    // dispatched here so static door helpers stay inside game.dll). Slots
    // may be NULL in older DLLs — engine handlers must null-check.
    void  (*open_all_doors)(void);
    void  (*open_all_secret_doors)(void);

    // Debug: deterministically inflict damage on an edict (MCP-only). Uses
    // world as inflictor + attacker so corpse-overdamage / gib branches
    // fire the same way as a normal hit, without simulating a weapon trace.
    void  (*mcp_damage)(edict_t *targ, float damage);

    // Sample the wind voxel grid at a world position. Writes wind
    // velocity (units/sec) into out_vel. Outside the grid bounds or
    // before the grid is initialised, writes (0,0,0). Implemented in
    // sim_wind.c. Used by r_part.c to drift visual particles.
    void  (*Wind_SampleVelocity)(const vec3_t pos, vec3_t out_vel);

    // Editor inspector: query the sim-side AI brain for an edict.
    // Returns 1 and fills the out_* pointers when a brain is registered,
    // 0 otherwise (also leaves the outputs untouched in that case).
    //   out_state:           AI_IDLE/SUSPICIOUS/SEARCHING/COMBAT (int cast of ai_state_t)
    //   out_alert:           0..1 alert_level
    //   out_last_known_pos:  vec3, brain's last_known_pos
    //   out_target_edict:    edict number, -1 if none
    //   out_next_tick_dt:    next_tick_time - g->time (seconds until next AI tick)
    //   out_stuck_ticks:     consecutive stuck-tick count
    // Any out_* pointer may be NULL to skip that field.
    int   (*ai_inspect)(edict_t *e,
                        int *out_state,
                        float *out_alert,
                        float *out_last_known_pos,
                        int *out_target_edict,
                        float *out_next_tick_dt,
                        int *out_stuck_ticks);

    // Player bot pathfinding bridge. Wraps Sim_Nav_PathTo so the
    // engine-side bot module (engine_src/bot.c) can drive routes
    // without linking against the DLL's sim. Returns waypoint count
    // (0 if no path or navmesh not yet baked).
    //   out_kinds (nullable): each waypoint's entry-edge kind
    //                         (NAV_EDGE_*; see sim_nav.c).
    //   player_items:         IT_* bitmask; locked edges whose
    //                         requires_items aren't a subset are skipped.
    int   (*nav_path)(const float *from, const float *to,
                      float *out_waypoints_xyz,
                      unsigned char *out_kinds,
                      unsigned int player_items,
                      int max_waypoints);

    // Debug-only nav edge query for the MCP nav_edges_near tool. Fills
    // up to `max_records` snapshots of edges within `radius` of the
    // 3-float `center` point. Returns the number written; sets
    // `*truncated_out` to 1 if more edges matched than fit (when
    // truncated_out is non-NULL). The record layout is fixed by
    // sim_nav_edge_record_t in sim.h — engine-side code should match
    // it via an extern struct rather than redeclaring (see
    // hotreload.c::MCP_NavEdgesNear).
    int   (*nav_edges_near)(const float *center, float radius,
                            void *out_records, int max_records,
                            int *truncated_out);

    // Debug command: delete the cached .nav file for `mapname` and
    // trigger a fresh bake. Invoked by the engine-side console
    // command `nav_rebake` (see host_cmd.c).
    void  (*nav_rebake)(const char *mapname);

    // Dev/test: run a path query from->to using the live serverflags sigil
    // set; returns waypoint count (0 = no path). Console: `nav_testpath`.
    int   (*nav_test_path)(const float *from, const float *to);
} game_api_t;

typedef game_api_t *(*Game_GetAPI_fn)(void);

#endif // GAME_API_H
