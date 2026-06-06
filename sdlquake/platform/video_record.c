// video_record.c -- MPEG-1 video capture of the software framebuffer.
//
// A development-only screen recorder (Mac / Windows desktop). Every rendered
// frame's 8-bit framebuffer (vid.buffer) is palette-expanded to RGBX and fed
// to a pluggable video_encoder_t; the shipped encoder wraps the vendored
// single-file jo_mpeg MPEG-1 writer (each frame is an independent intra-coded
// sequence — large files, but trivially seekable and zero-dependency).
//
// The engine renders at a variable rate (host_frametime is clamped, not
// fixed), so a wallclock sampler keyed off Sys_FloatTime() emits a *constant*
// frame-rate stream: it duplicates the last frame when rendering lags and
// drops extras when it races ahead, so playback always runs at real-time
// speed. No audio by design (yet) — the encoder boundary leaves room to add a
// muxing encoder later without touching the capture path.
//
// Console: `recordvideo [name]` / `stopvideo`. Cvar: record_fps.
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
static unsigned char *s_rgbx;          // w*h*4 expand scratch
static size_t         s_rgbx_cap;
static int            s_frames;        // frames written this session
static char           s_path[256];

// Auto-stop conditions (mirrors perf.c's `profile <n>` / `profile level`):
static double         s_deadline;          // Sys_FloatTime() absolute stop time, 0 = none
static int            s_until_level;       // "recordvideo level": stop when the level ends
static int            s_level_pending;     // started before a level loaded; latch on load
static char           s_level_name[64];    // BSP name snapshot — the end-of-level edge
static int            s_level_intermission;

static cvar_t record_fps = {"record_fps", "30"};

// Palette-expand the current framebuffer into s_rgbx (RGBX). Returns 0 if the
// framebuffer is gone or its size no longer matches the locked session dims.
static int vr_expand_frame(void)
{
    int w = (int)vid.width, h = (int)vid.height, rb = (int)vid.rowbytes;
    int x, y;

    if (!vid.buffer) return 0;
    if (w != s_w || h != s_h) return 0;

    for (y = 0; y < h; y++) {
        const byte    *src = vid.buffer + (size_t)y * rb;
        unsigned char *dst = s_rgbx     + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            unsigned c = d_8to24table[src[x]];   // 0xAARRGGBB
            dst[x*4 + 0] = (unsigned char)(c >> 16); // R
            dst[x*4 + 1] = (unsigned char)(c >>  8); // G
            dst[x*4 + 2] = (unsigned char)(c >>  0); // B
            dst[x*4 + 3] = 0;                         // X (ignored)
        }
    }
    return 1;
}

static void vr_close(void)
{
    if (s_enc) s_enc->close(s_enc);
    s_recording = 0;
}

static void vr_autostop(void)
{
    vr_close();
    Con_Printf("recordvideo: finished, wrote %s (%d frames @ %d fps)\n",
               s_path, s_frames, s_fps);
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
// Per-frame capture (called from VID_Update). Wallclock CFR sampler.
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

    now = Sys_FloatTime();
    dt  = 1.0 / (double)s_fps;
    if (now < s_next_capture) return;          // not time for the next frame yet

    // A long hitch (e.g. level load) leaves us far behind — resync rather than
    // emitting a flood of duplicate frames to "catch up".
    if (now - s_next_capture > 1.0) s_next_capture = now;

    if (!vr_expand_frame()) {
        Con_Printf("recordvideo: framebuffer changed size (%dx%d -> %dx%d); stopping\n",
                   s_w, s_h, (int)vid.width, (int)vid.height);
        vr_close();
        Con_Printf("stopvideo: wrote %s (%d frames)\n", s_path, s_frames);
        return;
    }

    // Emit one frame per elapsed 1/fps interval; duplicate the current frame if
    // rendering fell behind. Cap the catch-up at ~1s so we never spin.
    guard = s_fps;
    while (now >= s_next_capture && guard-- > 0) {
        s_enc->write_frame(s_enc, s_rgbx, s_w, s_h);
        s_frames++;
        s_next_capture += dt;
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
static void Video_Record_f(void)
{
    int    w, h, fps;
    size_t need;
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

    // (Re)size the expand scratch to the locked dimensions.
    need = (size_t)w * (size_t)h * 4;
    if (need > s_rgbx_cap) {
        unsigned char *p = (unsigned char *)realloc(s_rgbx, need);
        if (!p) { Con_Printf("recordvideo: out of memory\n"); return; }
        s_rgbx = p;
        s_rgbx_cap = need;
    }

    if (!s_enc->open(s_enc, s_path, w, h, fps)) {
        Con_Printf("recordvideo: cannot open %s for writing\n", s_path);
        return;
    }

    s_w = w; s_h = h;
    s_fps = s_enc->fps;          // the rate the encoder actually committed to
    s_frames = 0;
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
    vr_close();
    Con_Printf("stopvideo: wrote %s (%d frames @ %d fps)\n", s_path, s_frames, s_fps);
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
    if (s_recording) vr_close();   // flush the file on quit-while-recording
}
