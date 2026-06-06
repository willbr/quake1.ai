// video_record.c -- MPEG-1 video capture of the software framebuffer.
//
// A development-only screen recorder (Mac / Windows desktop). Every rendered
// frame's 8-bit framebuffer (vid.buffer) is palette-expanded to RGBX and
// encoded as MPEG-1 with the vendored jo_mpeg writer (each frame is an
// independent intra-coded sequence — large files, but trivially seekable and
// zero-dependency).
//
// Threading / parallel encode: the MPEG-1 DCT is far too slow to run inline on
// the render thread (it tanked the game to ~1fps), and at the 4x framebuffer
// (1280x800) a single encode thread only sustains ~15fps — so half the frames
// were dropped and, since the stream is constant-frame-rate, the video played
// ~2x too fast. Because the frames here are independent, we encode them in
// PARALLEL: the render thread (single producer) palette-expands each sampled
// frame into a free slot; a pool of encode workers turn slots into MPEG-1 byte
// buffers concurrently; and one writer thread concatenates those buffers to the
// file in capture order. The producer BLOCKS (never drops) when the in-flight
// window is full, so no frame is lost and playback speed stays correct — with
// enough workers the drain rate exceeds the capture rate, so it rarely blocks.
//
// The engine renders at a variable rate, so a wallclock sampler keyed off
// Sys_FloatTime() emits a constant-frame-rate stream (duplicating the last
// frame when rendering lags, dropping extras when it races). No audio (yet).
//
// Console: `recordvideo [name] [30s|2m|level]` / `stopvideo`. Cvar: record_fps.
// (`record` / `stop` already belong to Quake's demo system, hence the longer
// command names.)

#include <SDL3/SDL.h>
#include "quakedef.h"
#include "video_record.h"
#include "../vendor/jo_mpeg/jo_mpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  define vr_mkdir(p) _mkdir(p)
#else
#  include <sys/types.h>
#  define vr_mkdir(p) mkdir((p), 0755)
#endif

#define VR_EXT          "mpg"
#define VR_MAX_WORKERS  6     // encode worker threads (clamped to cores-2)
#define VR_BUFS         12    // in-flight frame window (must exceed worker count)

// jo_mpeg only supports these frame rates; snap the requested one down.
static int jo_snap_fps(int fps)
{
    if (fps <= 24) return 24;
    if (fps <= 25) return 25;
    if (fps <= 30) return 30;
    if (fps <= 50) return 50;
    return 60;
}

// ---------------------------------------------------------------------------
// Recorder state
// ---------------------------------------------------------------------------
static int            s_recording;
static int            s_w, s_h;        // frame dims, locked for the session
static int            s_fps;           // committed (snapped) capture rate
static double         s_next_capture;  // Sys_FloatTime() of the next CFR sample
static int            s_frames;        // frames captured (== written; no drops)
static char           s_path[256];
static FILE          *s_fp;

// Auto-stop conditions (mirrors perf.c's `profile <n>` / `profile level`):
static double         s_deadline;          // Sys_FloatTime() absolute stop time, 0 = none
static int            s_until_level;       // "recordvideo level": stop when the level ends
static int            s_level_pending;     // started before a level loaded; latch on load
static char           s_level_name[64];    // BSP name snapshot — the end-of-level edge
static int            s_level_intermission;

// --- parallel encode pipeline ----------------------------------------------
// A frame slot moves FREE -> FILLING (producer expands) -> READY (queued) ->
// ENCODING (a worker) -> DONE (encoded, awaiting in-order write) -> FREE.
enum { VR_FREE = 0, VR_FILLING, VR_READY, VR_ENCODING, VR_DONE };
typedef struct {
    unsigned char *rgbx;     // expand target, s_w*s_h*4 (persistent for the session)
    unsigned char *enc;      // encoded MPEG bytes (malloc'd by a worker, freed by writer)
    int            enc_len;
    long long      seq;      // capture order of the frame currently in this slot
    int            state;
} vr_slot_t;

static vr_slot_t      s_slots[VR_BUFS];
static SDL_Mutex     *s_mtx;
static SDL_Condition *s_cv_ready;     // a slot became READY (wake workers)
static SDL_Condition *s_cv_free;      // a slot became FREE (wake producer)
static SDL_Condition *s_cv_done;      // a slot became DONE (wake writer)
static SDL_Condition *s_cv_progress;  // writer advanced s_write_seq (wake finisher)
static SDL_Thread    *s_workers[VR_MAX_WORKERS];
static int            s_nworkers;
static SDL_Thread    *s_writer;
static int            s_quit;
static long long      s_seq_next;     // next capture seq to assign (producer)
static long long      s_write_seq;    // next seq the writer must emit
// FIFOs of slot indices (guarded by s_mtx):
static int            s_free_q[VR_BUFS], s_free_head, s_free_tail, s_free_count;
static int            s_ready_q[VR_BUFS], s_ready_head, s_ready_tail, s_ready_count;

static cvar_t record_fps = {"record_fps", "30"};

static void q_push(int *q, int *tail, int *count, int v)
{ q[*tail] = v; *tail = (*tail + 1) % VR_BUFS; (*count)++; }
static int q_pop(int *q, int *head, int *count)
{ int v = q[*head]; *head = (*head + 1) % VR_BUFS; (*count)--; return v; }

// Encode worker: claim READY slots and encode them to private byte buffers in
// parallel; mark DONE for the writer to concatenate in order.
static int SDLCALL vr_encode_worker(void *unused)
{
    (void)unused;
    for (;;) {
        int            idx, w, h, fps, len = 0;
        unsigned char *rgbx, *enc;

        SDL_LockMutex(s_mtx);
        while (s_ready_count == 0 && !s_quit) SDL_WaitCondition(s_cv_ready, s_mtx);
        if (s_ready_count == 0 && s_quit) { SDL_UnlockMutex(s_mtx); break; }
        idx = q_pop(s_ready_q, &s_ready_head, &s_ready_count);
        s_slots[idx].state = VR_ENCODING;
        rgbx = s_slots[idx].rgbx; w = s_w; h = s_h; fps = s_fps;
        SDL_UnlockMutex(s_mtx);

        enc = jo_encode_mpeg_frame(rgbx, w, h, fps, &len);   // the expensive part, lock-free

        SDL_LockMutex(s_mtx);
        s_slots[idx].enc = enc; s_slots[idx].enc_len = len; s_slots[idx].state = VR_DONE;
        SDL_BroadcastCondition(s_cv_done);
        SDL_UnlockMutex(s_mtx);
    }
    return 0;
}

// Writer: emit encoded frames to the file strictly in capture order.
static int SDLCALL vr_writer(void *unused)
{
    (void)unused;
    for (;;) {
        int            idx = -1, i, len;
        unsigned char *enc;

        SDL_LockMutex(s_mtx);
        for (;;) {
            idx = -1;
            for (i = 0; i < VR_BUFS; i++)
                if (s_slots[i].state == VR_DONE && s_slots[i].seq == s_write_seq) { idx = i; break; }
            if (idx >= 0 || s_quit) break;
            SDL_WaitCondition(s_cv_done, s_mtx);
        }
        if (idx < 0) { SDL_UnlockMutex(s_mtx); break; }   // quit + nothing left to write
        enc = s_slots[idx].enc; len = s_slots[idx].enc_len;
        SDL_UnlockMutex(s_mtx);

        if (enc && len > 0 && s_fp) fwrite(enc, 1, (size_t)len, s_fp);
        free(enc);

        SDL_LockMutex(s_mtx);
        s_slots[idx].enc = NULL; s_slots[idx].enc_len = 0; s_slots[idx].state = VR_FREE;
        q_push(s_free_q, &s_free_tail, &s_free_count, idx);
        s_write_seq++;
        SDL_BroadcastCondition(s_cv_free);
        SDL_BroadcastCondition(s_cv_progress);
        SDL_UnlockMutex(s_mtx);
    }
    return 0;
}

// Producer (render thread): palette-expand the current framebuffer into a free
// slot and queue it. Blocks if the in-flight window is full (backpressure)
// rather than dropping, so the stream stays at the target rate.
static void vr_enqueue_frame(void)
{
    int            idx, w = s_w, h = s_h, rb = (int)vid.rowbytes, x, y;
    unsigned char *base;

    SDL_LockMutex(s_mtx);
    while (s_free_count == 0 && !s_quit) SDL_WaitCondition(s_cv_free, s_mtx);
    if (s_quit) { SDL_UnlockMutex(s_mtx); return; }
    idx = q_pop(s_free_q, &s_free_head, &s_free_count);
    s_slots[idx].seq = s_seq_next++;
    s_slots[idx].state = VR_FILLING;
    SDL_UnlockMutex(s_mtx);

    base = s_slots[idx].rgbx;
    for (y = 0; y < h; y++) {
        const byte    *src = vid.buffer + (size_t)y * rb;
        unsigned char *dst = base       + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            unsigned c = d_8to24table[src[x]];   // 0xAARRGGBB
            dst[x*4 + 0] = (unsigned char)(c >> 16); // R
            dst[x*4 + 1] = (unsigned char)(c >>  8); // G
            dst[x*4 + 2] = (unsigned char)(c >>  0); // B
            dst[x*4 + 3] = 0;                         // X (ignored)
        }
    }

    SDL_LockMutex(s_mtx);
    s_slots[idx].state = VR_READY;
    q_push(s_ready_q, &s_ready_tail, &s_ready_count, idx);
    SDL_SignalCondition(s_cv_ready);
    SDL_UnlockMutex(s_mtx);
    s_frames++;
}

// Spin up the pipeline (buffers, sync, workers + writer). Returns 0 on failure.
static int vr_session_start(void)
{
    size_t need = (size_t)s_w * (size_t)s_h * 4;
    int    cores, i;

    for (i = 0; i < VR_BUFS; i++) {
        s_slots[i].rgbx = (unsigned char *)malloc(need);
        if (!s_slots[i].rgbx) return 0;
        s_slots[i].enc = NULL; s_slots[i].enc_len = 0;
        s_slots[i].seq = -1;   s_slots[i].state = VR_FREE;
    }

    s_free_head = s_free_tail = s_free_count = 0;
    for (i = 0; i < VR_BUFS; i++) q_push(s_free_q, &s_free_tail, &s_free_count, i);
    s_ready_head = s_ready_tail = s_ready_count = 0;
    s_seq_next = s_write_seq = 0;
    s_quit = 0;
    s_frames = 0;

    s_mtx         = SDL_CreateMutex();
    s_cv_ready    = SDL_CreateCondition();
    s_cv_free     = SDL_CreateCondition();
    s_cv_done     = SDL_CreateCondition();
    s_cv_progress = SDL_CreateCondition();
    if (!s_mtx || !s_cv_ready || !s_cv_free || !s_cv_done || !s_cv_progress) return 0;

    cores = SDL_GetNumLogicalCPUCores();
    s_nworkers = cores - 2;
    if (s_nworkers < 2) s_nworkers = 2;
    if (s_nworkers > VR_MAX_WORKERS) s_nworkers = VR_MAX_WORKERS;

    s_writer = SDL_CreateThread(vr_writer, "video_write", NULL);
    if (!s_writer) return 0;
    for (i = 0; i < s_nworkers; i++) {
        s_workers[i] = SDL_CreateThread(vr_encode_worker, "video_encode", NULL);
        if (!s_workers[i]) return 0;
    }
    return 1;
}

// Drain the pipeline, retire all threads, close the file, free everything.
static void vr_session_finish(void)
{
    int i;

    if (s_mtx) {
        SDL_LockMutex(s_mtx);
        // Wait for the writer to emit every captured frame before we tear down,
        // so the tail of the recording isn't lost. (Only meaningful if the
        // writer is actually running.)
        if (s_writer)
            while (s_write_seq < s_seq_next) SDL_WaitCondition(s_cv_progress, s_mtx);
        s_quit = 1;
        SDL_BroadcastCondition(s_cv_ready);
        SDL_BroadcastCondition(s_cv_done);
        SDL_BroadcastCondition(s_cv_free);
        SDL_UnlockMutex(s_mtx);
    }

    for (i = 0; i < VR_MAX_WORKERS; i++)
        if (s_workers[i]) { SDL_WaitThread(s_workers[i], NULL); s_workers[i] = NULL; }
    if (s_writer) { SDL_WaitThread(s_writer, NULL); s_writer = NULL; }

    if (s_fp) { fclose(s_fp); s_fp = NULL; }

    for (i = 0; i < VR_BUFS; i++) {
        free(s_slots[i].rgbx); s_slots[i].rgbx = NULL;
        free(s_slots[i].enc);  s_slots[i].enc  = NULL;
    }
    if (s_cv_ready)    { SDL_DestroyCondition(s_cv_ready);    s_cv_ready = NULL; }
    if (s_cv_free)     { SDL_DestroyCondition(s_cv_free);     s_cv_free = NULL; }
    if (s_cv_done)     { SDL_DestroyCondition(s_cv_done);     s_cv_done = NULL; }
    if (s_cv_progress) { SDL_DestroyCondition(s_cv_progress); s_cv_progress = NULL; }
    if (s_mtx)         { SDL_DestroyMutex(s_mtx);             s_mtx = NULL; }
    s_recording = 0;
}

static void vr_report(const char *verb)
{
    Con_Printf("%s wrote %s (%d frames @ %d fps)\n", verb, s_path, s_frames, s_fps);
}

static void vr_autostop(void)
{
    vr_session_finish();
    vr_report("recordvideo: finished,");
}

// Current level's BSP name — changes on map load, empty at menu/disconnect; a
// reliable end-of-level edge (mirrors perf.c's capture_level_name).
static void vr_level_name(char *out, size_t cap)
{
    if (cls.state == ca_connected && cl.worldmodel && cl.worldmodel->name[0])
        snprintf(out, cap, "%s", cl.worldmodel->name);
    else
        out[0] = 0;
}

// Classify a `recordvideo` argument: 0 = filename, 1 = duration (seconds into
// *out_secs), 2 = "level". A duration is `level`, or a number with an 's'
// (seconds) / 'm' (minutes) suffix — the leading-digit requirement keeps
// filenames like "boom" from reading as minutes.
static int vr_parse_duration(const char *a, double *out_secs)
{
    int len = (int)strlen(a);
    if (!Q_strcasecmp((char *)a, "level")) return 2;
    if (len >= 2 && a[0] >= '0' && a[0] <= '9') {
        char last = a[len-1];
        if (last == 's' || last == 'S') { *out_secs = atof(a);        return 1; }
        if (last == 'm' || last == 'M') { *out_secs = atof(a) * 60.0; return 1; }
    }
    return 0;
}

// videos/clip_NNNN.mpg, first free slot. Returns 0 if the dir is full.
static int vr_auto_path(char *out, size_t outsz)
{
    int i;
    (void)vr_mkdir("videos");
    for (i = 0; i <= 9999; i++) {
        struct stat st;
        snprintf(out, outsz, "videos/clip_%04d." VR_EXT, i);
        if (stat(out, &st) != 0) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Per-frame capture (called from VID_Update). Wallclock CFR sampler — the
// render thread only palette-expands + enqueues; workers do the encoding.
// ---------------------------------------------------------------------------
void Video_Record_CaptureFrame(void)
{
    double now, dt;
    int    guard;

    if (!s_recording) return;

    // "recordvideo level" issued before a level loaded — wait for one, latch
    // onto it, then start the capture clock fresh so the deferral doesn't
    // count as elapsed time.
    if (s_until_level && s_level_pending) {
        char now_name[64];
        vr_level_name(now_name, sizeof(now_name));
        if (!now_name[0]) return;   // no level yet; nothing to capture
        snprintf(s_level_name, sizeof(s_level_name), "%s", now_name);
        s_level_intermission = cl.intermission;
        s_level_pending = 0;
        s_next_capture = Sys_FloatTime();
        Con_Printf("recordvideo: recording until %s ends\n", s_level_name);
    }

    // Auto-stop conditions.
    if (s_deadline > 0.0 && Sys_FloatTime() >= s_deadline) { vr_autostop(); return; }
    if (s_until_level && !s_level_pending) {
        char now_name[64];
        vr_level_name(now_name, sizeof(now_name));
        if (strcmp(now_name, s_level_name) != 0)          { vr_autostop(); return; }
        if (cl.intermission && !s_level_intermission)     { vr_autostop(); return; }
    }

    // Framebuffer must still match the locked session dimensions.
    if (!vid.buffer || (int)vid.width != s_w || (int)vid.height != s_h) {
        Con_Printf("recordvideo: framebuffer changed size (%dx%d -> %dx%d); stopping\n",
                   s_w, s_h, (int)vid.width, (int)vid.height);
        vr_session_finish();
        vr_report("recordvideo: stopped,");
        return;
    }

    now = Sys_FloatTime();
    dt  = 1.0 / (double)s_fps;
    if (now < s_next_capture) return;          // not time for the next frame yet

    // A long hitch (e.g. level load) leaves us far behind — resync rather than
    // enqueuing a flood of duplicate frames to "catch up".
    if (now - s_next_capture > 1.0) s_next_capture = now;

    // Enqueue one frame per elapsed 1/fps interval (duplicating the current
    // frame if rendering fell behind). Cap the catch-up at ~1s so we never spin.
    guard = s_fps;
    while (now >= s_next_capture && guard-- > 0) {
        vr_enqueue_frame();
        s_next_capture += dt;
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
static void Video_Record_f(void)
{
    int    w, h, fps;
    int    mode_time = 0, mode_level = 0;
    double dur_secs = 0.0;

    if (s_recording) {
        Con_Printf("recordvideo: already recording %s (use stopvideo)\n", s_path);
        return;
    }
    if (!vid.buffer || vid.width <= 0 || vid.height <= 0) {
        Con_Printf("recordvideo: no framebuffer\n");
        return;
    }

    w   = (int)vid.width;
    h   = (int)vid.height;
    fps = (int)record_fps.value;
    if (fps <= 0) fps = 30;

    // Parse args: an optional name and an optional duration token (`30s`,
    // `2m`, or `level`). e.g. `recordvideo`, `recordvideo myclip`,
    // `recordvideo 30s`, `recordvideo myclip 2m`, `recordvideo level`.
    {
        int         i;
        const char *name = NULL;
        for (i = 1; i < Cmd_Argc(); i++) {
            const char *a = Cmd_Argv(i);
            double      secs = 0.0;
            int         k = vr_parse_duration(a, &secs);
            if      (k == 2) mode_level = 1;
            else if (k == 1) { mode_time = 1; dur_secs = secs; }
            else             name = a;
        }
        if (mode_time && dur_secs <= 0.0) {
            Con_Printf("recordvideo: bad duration\n");
            return;
        }

        // Resolve output path.
        if (name) {
            (void)vr_mkdir("videos");
            if (strchr(name, '.'))
                snprintf(s_path, sizeof(s_path), "videos/%s", name);
            else
                snprintf(s_path, sizeof(s_path), "videos/%s." VR_EXT, name);
        } else if (!vr_auto_path(s_path, sizeof(s_path))) {
            Con_Printf("recordvideo: videos/ is full (10000 slots)\n");
            return;
        }
    }

    s_fp = fopen(s_path, "wb");
    if (!s_fp) {
        Con_Printf("recordvideo: cannot open %s for writing\n", s_path);
        return;
    }

    s_w = w; s_h = h;
    s_fps = jo_snap_fps(fps);

    if (!vr_session_start()) {
        Con_Printf("recordvideo: cannot start encode pipeline\n");
        vr_session_finish();     // tears down whatever partially came up + closes file
        return;
    }

    s_next_capture = Sys_FloatTime();
    s_recording = 1;

    // Stop-condition setup.
    s_deadline = 0.0;
    s_until_level = 0;
    s_level_pending = 0;
    if (mode_level) {
        s_until_level = 1;
        vr_level_name(s_level_name, sizeof(s_level_name));
        s_level_intermission = cl.intermission;
        s_level_pending = (s_level_name[0] == 0);   // defer until a level loads
    } else if (mode_time) {
        s_deadline = Sys_FloatTime() + dur_secs;
    }

    if (s_fps != fps)
        Con_Printf("recordvideo: %dx%d @ %d fps (snapped from %d), %d encode threads -> %s\n",
                   w, h, s_fps, fps, s_nworkers, s_path);
    else
        Con_Printf("recordvideo: %dx%d @ %d fps, %d encode threads -> %s\n",
                   w, h, s_fps, s_nworkers, s_path);
    if (mode_level && s_level_pending)
        Con_Printf("  (will record the next level until it ends; stopvideo to cancel)\n");
    else if (mode_level)
        Con_Printf("  (recording until %s ends; stopvideo to finish early)\n", s_level_name);
    else if (mode_time)
        Con_Printf("  (recording for %g s; stopvideo to finish early)\n", dur_secs);
    else
        Con_Printf("  (stopvideo to finish)\n");
}

static void Video_Stop_f(void)
{
    if (!s_recording) {
        Con_Printf("stopvideo: not recording\n");
        return;
    }
    vr_session_finish();
    vr_report("stopvideo:");
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Video_Record_Init(void)
{
    Cvar_RegisterVariable(&record_fps);
    Cmd_AddCommand("recordvideo", Video_Record_f);
    Cmd_AddCommand("stopvideo", Video_Stop_f);
}

void Video_Record_Shutdown(void)
{
    if (s_recording) vr_session_finish();   // drain + flush the file on quit-while-recording
}
