// video_record.c -- MPEG-1 video capture of the software framebuffer.
//
// A development-only screen recorder (Mac / Windows desktop). Every rendered
// frame's 8-bit framebuffer (vid.buffer) is palette-expanded to RGBX and fed
// to a pluggable video_encoder_t; the shipped encoder wraps the vendored
// single-file jo_mpeg MPEG-1 writer (each frame is an independent intra-coded
// sequence — large files, but trivially seekable and zero-dependency).
//
// Threading: MPEG-1 DCT encoding is far too slow to run inline on the render
// thread (it tanked the frame rate to ~1 fps, and the CFR catch-up loop
// *amplified* it into a death spiral). So encoding runs on a dedicated worker
// thread fed by a bounded SPSC frame queue: the render thread only does the
// cheap palette-expand into a free slot and enqueues it. If the encoder can't
// keep up the queue fills and frames are dropped (logged on stop) — the game
// stays smooth either way.
//
// The engine renders at a variable rate (host_frametime is clamped, not
// fixed), so a wallclock sampler keyed off Sys_FloatTime() emits a *constant*
// frame-rate stream: it duplicates the last frame when rendering lags and
// drops extras when it races ahead, so playback runs at real-time speed. No
// audio by design (yet) — the encoder boundary leaves room to add a muxing
// encoder later without touching the capture path.
//
// Console: `recordvideo [name] [30s|2m|level]` / `stopvideo`. Cvar: record_fps.
// (`record` / `stop` already belong to Quake's demo system, hence the longer
// command names.)

#include <SDL3/SDL.h>
#include "quakedef.h"
#include "video_record.h"
#include "../vendor/jo_mpeg/jo_mpeg.h"

#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  define vr_mkdir(p) _mkdir(p)
#else
#  include <sys/types.h>
#  define vr_mkdir(p) mkdir((p), 0755)
#endif

// ---------------------------------------------------------------------------
// Pluggable encoder interface. One concrete impl today (jo_mpeg); the seam
// is here so a future H.264/MP4 (or audio-muxing) encoder slots in without
// the recorder caring.
// ---------------------------------------------------------------------------
typedef struct video_encoder_s {
    int  (*open)(struct video_encoder_s *e, const char *path, int w, int h, int fps);
    void (*write_frame)(struct video_encoder_s *e, const unsigned char *rgbx, int w, int h);
    void (*close)(struct video_encoder_s *e);
    const char *ext;   // file extension, no dot
    FILE       *fp;
    int         fps;   // the rate the encoder actually committed to (may be snapped)
} video_encoder_t;

// jo_mpeg only supports these frame rates; snap the requested one down.
static int jo_snap_fps(int fps)
{
    if (fps <= 24) return 24;
    if (fps <= 25) return 25;
    if (fps <= 30) return 30;
    if (fps <= 50) return 50;
    return 60;
}
static int jo_enc_open(video_encoder_t *e, const char *path, int w, int h, int fps)
{
    (void)w; (void)h;
    e->fp = fopen(path, "wb");
    if (!e->fp) return 0;
    e->fps = jo_snap_fps(fps);
    return 1;
}
static void jo_enc_write(video_encoder_t *e, const unsigned char *rgbx, int w, int h)
{
    if (e->fp) jo_write_mpeg(e->fp, rgbx, w, h, e->fps);
}
static void jo_enc_close(video_encoder_t *e)
{
    if (e->fp) { fclose(e->fp); e->fp = NULL; }
}
static video_encoder_t s_jo_encoder = { jo_enc_open, jo_enc_write, jo_enc_close, "mpg", NULL, 30 };

// ---------------------------------------------------------------------------
// Recorder state
// ---------------------------------------------------------------------------
static video_encoder_t *s_enc;
static int            s_recording;
static int            s_w, s_h;        // frame dims, locked for the session
static int            s_fps;           // committed capture rate
static double         s_next_capture;  // Sys_FloatTime() of the next CFR sample
static int            s_frames;        // frames enqueued (written) this session
static int            s_dropped;       // frames dropped because the queue was full
static char           s_path[256];

// Auto-stop conditions (mirrors perf.c's `profile <n>` / `profile level`):
static double         s_deadline;          // Sys_FloatTime() absolute stop time, 0 = none
static int            s_until_level;       // "recordvideo level": stop when the level ends
static int            s_level_pending;     // started before a level loaded; latch on load
static char           s_level_name[64];    // BSP name snapshot — the end-of-level edge
static int            s_level_intermission;

// Encode worker + bounded SPSC frame queue. Render thread is the single
// producer (head); worker is the single consumer (tail). The two semaphores
// provide both flow control and the release/acquire fences across the slot
// buffers, so no extra mutex is needed.
#define VR_QUEUE_SLOTS 8
static unsigned char *s_slot[VR_QUEUE_SLOTS];  // each holds one s_w*s_h*4 RGBX frame
static size_t         s_slot_cap;              // bytes currently allocated per slot
static int            s_head, s_tail;
static SDL_Semaphore *s_free_sem;              // free slots available to the producer
static SDL_Semaphore *s_ready_sem;             // queued frames (+ one quit wake token)
static SDL_AtomicInt  s_pending;               // queued-but-not-yet-encoded count
static SDL_AtomicInt  s_quit;                  // set on stop to retire the worker
static SDL_Thread    *s_worker;

static cvar_t record_fps = {"record_fps", "30"};

// Encode worker: pull queued frames in order and hand each to the encoder.
// Wakes once per enqueued frame plus once on quit; drains everything pending
// before exiting (so the tail of the recording isn't lost).
static int SDLCALL vr_worker(void *unused)
{
    (void)unused;
    for (;;) {
        SDL_WaitSemaphore(s_ready_sem);
        if (SDL_GetAtomicInt(&s_pending) > 0) {
            s_enc->write_frame(s_enc, s_slot[s_tail], s_w, s_h);
            s_tail = (s_tail + 1) % VR_QUEUE_SLOTS;
            SDL_AddAtomicInt(&s_pending, -1);
            SDL_SignalSemaphore(s_free_sem);
        } else {
            break;   // woke with nothing pending => quit token, queue drained
        }
    }
    return 0;
}

// Palette-expand the current framebuffer into a free queue slot and publish it.
// Returns without enqueuing (counting a drop) if the encoder is saturated.
// Caller guarantees vid dims still match the locked session dims.
static void vr_enqueue_frame(void)
{
    int            w = s_w, h = s_h, rb = (int)vid.rowbytes, x, y;
    unsigned char *base;

    if (!SDL_TryWaitSemaphore(s_free_sem)) { s_dropped++; return; }  // queue full → drop

    base = s_slot[s_head];
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
    s_head = (s_head + 1) % VR_QUEUE_SLOTS;
    SDL_AddAtomicInt(&s_pending, 1);
    SDL_SignalSemaphore(s_ready_sem);
    s_frames++;
}

// Spin up the encode worker + queue for a session. Returns 0 on failure.
static int vr_session_start(void)
{
    size_t need = (size_t)s_w * (size_t)s_h * 4;
    int    i;

    if (need > s_slot_cap) {
        for (i = 0; i < VR_QUEUE_SLOTS; i++) {
            unsigned char *p = (unsigned char *)realloc(s_slot[i], need);
            if (!p) return 0;
            s_slot[i] = p;
        }
        s_slot_cap = need;
    }

    s_head = s_tail = 0;
    s_frames = s_dropped = 0;
    SDL_SetAtomicInt(&s_pending, 0);
    SDL_SetAtomicInt(&s_quit, 0);
    s_free_sem  = SDL_CreateSemaphore(VR_QUEUE_SLOTS);
    s_ready_sem = SDL_CreateSemaphore(0);
    if (!s_free_sem || !s_ready_sem) return 0;
    s_worker = SDL_CreateThread(vr_worker, "video_encode", NULL);
    return s_worker != NULL;
}

// Drain the queue, retire the worker, close the file, tear down the queue.
static void vr_session_finish(void)
{
    if (s_worker) {
        SDL_SetAtomicInt(&s_quit, 1);
        SDL_SignalSemaphore(s_ready_sem);   // wake the worker to observe quit + drain
        SDL_WaitThread(s_worker, NULL);
        s_worker = NULL;
    }
    if (s_enc) s_enc->close(s_enc);
    if (s_ready_sem) { SDL_DestroySemaphore(s_ready_sem); s_ready_sem = NULL; }
    if (s_free_sem)  { SDL_DestroySemaphore(s_free_sem);  s_free_sem  = NULL; }
    s_recording = 0;
    // s_slot[] kept allocated for reuse by the next session; freed at shutdown.
}

static void vr_report(const char *verb)
{
    if (s_dropped)
        Con_Printf("%s: wrote %s (%d frames @ %d fps, %d dropped — encoder fell behind)\n",
                   verb, s_path, s_frames, s_fps, s_dropped);
    else
        Con_Printf("%s: wrote %s (%d frames @ %d fps)\n", verb, s_path, s_frames, s_fps);
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
static int vr_auto_path(char *out, size_t outsz, const char *ext)
{
    int i;
    (void)vr_mkdir("videos");
    for (i = 0; i <= 9999; i++) {
        struct stat st;
        snprintf(out, outsz, "videos/clip_%04d.%s", i, ext);
        if (stat(out, &st) != 0) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Per-frame capture (called from VID_Update). Wallclock CFR sampler — cheap:
// just palette-expand + enqueue; the worker thread does the encoding.
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

    s_enc = &s_jo_encoder;
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
                snprintf(s_path, sizeof(s_path), "videos/%s.%s", name, s_enc->ext);
        } else if (!vr_auto_path(s_path, sizeof(s_path), s_enc->ext)) {
            Con_Printf("recordvideo: videos/ is full (10000 slots)\n");
            return;
        }
    }

    if (!s_enc->open(s_enc, s_path, w, h, fps)) {
        Con_Printf("recordvideo: cannot open %s for writing\n", s_path);
        return;
    }

    s_w = w; s_h = h;
    s_fps = s_enc->fps;          // the rate the encoder actually committed to

    if (!vr_session_start()) {
        Con_Printf("recordvideo: cannot start encode worker\n");
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
        Con_Printf("recordvideo: %dx%d @ %d fps (snapped from %d) -> %s\n", w, h, s_fps, fps, s_path);
    else
        Con_Printf("recordvideo: %dx%d @ %d fps -> %s\n", w, h, s_fps, s_path);
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
    int i;
    if (s_recording) vr_session_finish();   // drain + flush the file on quit-while-recording
    for (i = 0; i < VR_QUEUE_SLOTS; i++) {
        free(s_slot[i]);
        s_slot[i] = NULL;
    }
    s_slot_cap = 0;
}
