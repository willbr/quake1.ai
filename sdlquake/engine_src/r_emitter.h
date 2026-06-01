// r_emitter.h -- data-driven particle effects (the "particle editor" runtime).
//
// An emitter_def_t describes ONE effect as data. The registry (s_defs in
// r_emitter.c) holds up to EMIT_MAX_DEFS of them, loaded from id1/particles/
// *.pcl at startup and mutated live by the editor. Spawning an effect either
// bursts particles immediately or parks a live-emitter instance that emits at
// a rate until it expires. Particles spawned are type pt_emitter, carrying a
// `def` index; R_DrawParticles integrates them and D_DrawEmitterParticle draws
// them, both reading the def.
//
// Caller must have already included "quakedef.h" (defines byte, vec3_t).

#ifndef R_EMITTER_H
#define R_EMITTER_H

typedef enum { EMIT_BURST = 0, EMIT_CONTINUOUS = 1 } emit_mode_t;
typedef enum { SHAPE_POINT = 0, SHAPE_SPHERE = 1, SHAPE_CONE = 2, SHAPE_BOX = 3 } emit_shape_t;
typedef enum { DIR_ALONG_SHAPE = 0, DIR_INHERIT = 1, DIR_UP = 2 } emit_dirmode_t;
typedef enum { STYLE_DOT = 0, STYLE_BLOB = 1, STYLE_SMOKE = 2 } emit_style_t;

#define EMIT_MAX_RAMP   8
#define EMIT_MAX_DEFS   128
#define EMIT_MAX_LIVE   64
#define EMIT_NAME_LEN   32

typedef struct {
    int             used;               // 0 = free registry slot
    char            name[EMIT_NAME_LEN];// lookup key == .pcl filename stem

    int             mode;               // emit_mode_t
    int             count;              // burst total
    float           rate;               // particles/sec (continuous)
    float           duration;           // sec; 0 = loop until stopped

    int             shape;              // emit_shape_t
    vec3_t          origin_offset;
    float           shape_size;         // sphere radius / box half-extent
    float           cone_angle;         // half-angle, deg (SHAPE_CONE)

    float           speed, speed_jitter;
    int             dir_mode;           // emit_dirmode_t
    float           spread;             // deg
    float           radial_bias;        // outward-from-origin velocity add

    float           gravity_scale;      // 1.0 ~= a classic falling particle; <0 rises
    float           drag;               // per-sec linear velocity decay

    float           life_min, life_max; // sec

    int             style;              // emit_style_t
    float           size_start, size_peak, size_end;  // blob/smoke size scale

    int             ramp_count;
    float           ramp_frac[EMIT_MAX_RAMP];   // 0..1 ascending
    byte            ramp_pal [EMIT_MAX_RAMP];   // palette index per stop
} emitter_def_t;

// ---- lifecycle ----------------------------------------------------------
void  R_EmitterInit (void);            // called from R_InitParticles
void  R_UpdateEmitters (void);         // called each frame before R_DrawParticles

// ---- registry access ----------------------------------------------------
int             R_EmitterCount (void);              // number of used slots
emitter_def_t  *R_EmitterGetDef (int idx);          // NULL if idx invalid/unused
int             R_EmitterFind (const char *name);   // slot idx or -1
int             R_EmitterNew  (const char *name);   // alloc slot with defaults, idx or -1
void            R_EmitterDelete (int idx);

// ---- spawning ------------------------------------------------------------
// Returns a live-instance handle (>=0) for continuous effects, or -1 for
// bursts / on failure. Handle is used only to stop a looping preview.
int   R_SpawnEffectIdx (int idx, vec3_t org, vec3_t dir);
void  R_SpawnParticleEffectByName (const char *name, vec3_t org, vec3_t dir); // engine_api hook
void  R_EmitterStopHandle (int handle);
void  R_EmitterStopAll (void);

// ---- color/size sampling (shared by r_part.c integrate + d_part.c draw) --
byte  R_EmitterRampColor (const emitter_def_t *d, float t);  // t in [0,1]
float R_EmitterSizeEnv  (const emitter_def_t *d, float t);   // size scale at age t

// ---- persistence ---------------------------------------------------------
void  R_EmitterLoadAll (void);         // scan id1/particles/*.pcl into s_defs
int   R_EmitterSave (int idx);         // write s_defs[idx] -> id1/particles/<name>.pcl; 1 ok

#endif // R_EMITTER_H
