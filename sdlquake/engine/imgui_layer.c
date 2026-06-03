// imgui_layer.c -- Dear ImGui dev overlay, pure C.
//
// Toggle with F3.  Renders after the game frame at native window resolution.
// Panels: Perf (FPS/ms + frametime sparkline + per-frame flame graph),
// Cvars (filterable, editable), Entities (table), AI, Console, Debug Render.

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "imgui_bridge.h"
#include "imgui_debug_panel.h"
#include "imgui_layer.h"
#include "imgui_support.h"
#include "perf.h"

static SDL_Window    *s_window  = NULL;
static SDL_GPUDevice *s_gpu     = NULL;
static int            s_open    = 0;
static int            s_inited  = 0;

// Set during PrepareGPU. Drives whether RenderGPU emits draw calls inside the
// swapchain render pass — skip both if nothing to display so the cost of
// NewFrame/Render goes to zero when dev overlay is closed.
static int            s_should_render = 0;

int imgui_ai_panel_open = 1;

// ---------------------------------------------------------------------------
// Responsive layout
//
// Recomputed each frame from the current display size so the F3 overlay
// tracks window resizes. Panels apply their rect with IG_Cond_Always, which
// means a user drag snaps back next frame — fine for a dev overlay.
//
//        col1 (FPS+AI)     col2 (Cvars)        col3 (Entities/DebugRender)
//        +----------+      +------------+      +-----------+
//        |   FPS    |      |            |      |           |
//        +----------+      |   Cvars    |      | Entities  |
//        |          |      |            |      |           |
//        |   AI     |      |            |      |           |
//        +----------+      +------------+      +-----------+
//        |   Console (left+middle)      | Debug Rdr |
//        +------------------------------+-----------+
//        |          Profile (full width over game HUD)         |
//        +-----------------------------------------------------+
// ---------------------------------------------------------------------------

typedef struct { int x, y, w, h; } rect_t;
static rect_t s_fps, s_ai, s_cvars, s_entities, s_console, s_debug, s_profile;

static void compute_layout(void)
{
    float fw, fh;
    IG_GetDisplaySize(&fw, &fh);
    int W = (int)fw, H = (int)fh;
    if (W < 800) W = 800;
    if (H < 600) H = 600;

    const int M      = 10;
    const int FPS_W = 360, FPS_H = 180;
    // Default Profile height — accommodates title + combo + status +
    // 30 px histogram + a ~4-level-deep flame graph + ~10 rows of the
    // aggregate table. Applied as FirstUseEver so user resizing sticks
    // across the session.
    const int PROFILE_H = 440;

    int col3_w = W * 22 / 100; if (col3_w < 280) col3_w = 280;
    int col1_w = W * 27 / 100; if (col1_w < 360) col1_w = 360;
    int col2_min = 240;
    if (col1_w + col3_w + col2_min + M*4 > W) {
        // Window narrow enough to overflow — shrink left/right proportionally.
        int over = col1_w + col3_w + col2_min + M*4 - W;
        int half = over / 2;
        col1_w -= half;        if (col1_w < 240) col1_w = 240;
        col3_w -= over - half; if (col3_w < 240) col3_w = 240;
    }
    int col2_w = W - col1_w - col3_w - M*4;
    if (col2_w < col2_min) col2_w = col2_min;

    int profile_y = H - PROFILE_H - M;          // pinned to the very bottom
    int bottom_h  = H * 24 / 100; if (bottom_h < 180) bottom_h = 180;
    int top_y     = M + FPS_H + M;
    int top_h     = profile_y - top_y - bottom_h - M*2;
    if (top_h < 160) top_h = 160;
    int top_full_h = top_y + top_h - M;

    s_fps.x = M; s_fps.y = M; s_fps.w = FPS_W; s_fps.h = FPS_H;
    s_ai.x  = M; s_ai.y  = top_y; s_ai.w = col1_w; s_ai.h = top_h;

    s_cvars.x    = M*2 + col1_w;   s_cvars.y    = M;
    s_cvars.w    = col2_w;         s_cvars.h    = top_full_h;

    s_entities.x = W - M - col3_w; s_entities.y = M;
    s_entities.w = col3_w;         s_entities.h = top_full_h;

    int bottom_y = top_y + top_h + M;
    s_console.x  = M;              s_console.y  = bottom_y;
    s_console.w  = W - col3_w - M*3; s_console.h = bottom_h;

    s_debug.x    = W - M - col3_w; s_debug.y    = bottom_y;
    s_debug.w    = col3_w;         s_debug.h    = bottom_h;

    // Profile spans the full width over the game HUD.
    s_profile.x = M; s_profile.y = profile_y;
    s_profile.w = W - M*2; s_profile.h = PROFILE_H;
}

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

// Hash a string into one of a few distinguishable colors. Used to colour
// flame-graph bars consistently per scope name.
static unsigned int scope_color(const char *name)
{
    unsigned int h = 5381;
    for (const char *p = name; *p; p++) h = (h * 33) ^ (unsigned char)*p;
    // 8 hand-picked muted hues, opaque, ImGui ABGR.
    static const unsigned int palette[8] = {
        0xFF5577AA, 0xFF55AA77, 0xFF7755AA, 0xFFAA7755,
        0xFFAA5577, 0xFF77AA55, 0xFF55AAAA, 0xFFAA55AA,
    };
    return palette[h & 7];
}

// FPS window: headline numbers + sparklines, top-left, compact.
static void draw_fps(void)
{
    char buf[128];

    IG_SetNextWindowPos ((float)s_fps.x, (float)s_fps.y, IG_Cond_Always);
    IG_SetNextWindowSize((float)s_fps.w, (float)s_fps.h, IG_Cond_Always);
    if (!IG_Begin("FPS", NULL, IG_WF_NoCollapse | IG_WF_NoSavedSettings))
    { IG_End(); return; }

    float fps       = Perf_SmoothedFps();
    float ms        = Perf_LastFrameMs();
    float imgui_fps = IG_GetFramerate();
    const char *tag = Perf_IsReplaying() ? "  [replay]"
                    : Perf_IsPaused()    ? "  [paused]"
                    :                      "";
    snprintf(buf, sizeof(buf),
             "%.1f fps  %.2f ms   (imgui ema: %.1f)%s",
             fps, ms, imgui_fps, tag);
    IG_TextUnformatted(buf);

    static float ft_buf[256];
    int n = Perf_GetFrametimeHistoryMs(ft_buf, 256);
    static float ft_oldest_first[256];
    for (int i = 0; i < n; i++) ft_oldest_first[i] = ft_buf[n - 1 - i];

    snprintf(buf, sizeof(buf), "%.0ffps", fps);
    IG_PlotLines("##fps", ft_oldest_first, n, 0,
                 buf, 5.0f, 50.0f, -1, 50);

    snprintf(buf, sizeof(buf), "%.1fms", ms);
    IG_PlotLines("##ms", ft_oldest_first, n, 0,
                 buf, 0.0f, 33.0f, -1, 50);

    IG_End();
}

// Refresh the dropdown of available captures from `profiles/`. Returns the
// SDL_malloc'd outer array (caller frees) and writes count + the items
// array used by IG_Combo. items[0] is always "Live"; remaining entries are
// pointers into `glob`'s strings, so don't free those individually.
static char **refresh_capture_list(int *out_count,
                                   const char **items, int items_cap)
{
    int count = 0;
    char **glob = SDL_GlobDirectory("profiles", "perf_*.json",
                                    SDL_GLOB_CASEINSENSITIVE, &count);
    int items_n = 0;
    items[items_n++] = "Live";
    if (glob) {
        // Filter out summary JSONs; we only replay traces.
        for (int i = 0; i < count && items_n < items_cap; i++) {
            const char *name = glob[i];
            if (!name) continue;
            int len = (int)strlen(name);
            if (len > 13 &&
                strcmp(name + len - 13, "_summary.json") == 0)
                continue;
            items[items_n++] = name;
        }
    }
    *out_count = items_n;
    return glob;
}

// Profile window: flame graph + capture status + replay scrubber, bottom
// strip full width. Sits over the in-game HUD so the flame graph has the
// widest possible canvas — bars get more horizontal pixels and more labels
// fit inline.
static void draw_profile(void)
{
    char buf[512];

    // FirstUseEver so the user can resize / move the Profile window — the
    // flame graph often wants more vertical room than the default for
    // deep scope stacks. Other dev panels still snap (Cond_Always) since
    // they're tiled and resize would just produce overlap.
    IG_SetNextWindowPos ((float)s_profile.x, (float)s_profile.y, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize((float)s_profile.w, (float)s_profile.h, IG_Cond_FirstUseEver);
    if (!IG_Begin("Profile", NULL, IG_WF_NoCollapse))
    { IG_End(); return; }

    // ---- Source selector + capture status -------------------------------
    enum { MAX_CAPTURES = 32 };
    const char *items[MAX_CAPTURES];
    int n_items = 0;
    char **glob = refresh_capture_list(&n_items, items, MAX_CAPTURES);

    // Figure out which item matches the current replay state.
    int current = 0;
    if (Perf_IsReplaying()) {
        const char *p = Perf_ReplayPath();
        // Skip the "profiles/" prefix for the comparison.
        const char *name = p;
        const char *slash = strrchr(p, '/');
        if (slash) name = slash + 1;
        for (int i = 1; i < n_items; i++) {
            if (strcmp(items[i], name) == 0) { current = i; break; }
        }
    }

    IG_SetNextItemWidth(280);
    int prev = current;
    if (IG_Combo("Source", &current, items, n_items)) {
        if (current == 0) {
            Perf_UnloadCapture();
        } else if (current != prev) {
            char path[512];
            snprintf(path, sizeof(path), "profiles/%s", items[current]);
            Perf_LoadCapture(path);
        }
    }
    if (glob) SDL_free(glob);

    // Capture-in-progress / last-saved status.
    if (Perf_IsCapturing()) {
        snprintf(buf, sizeof(buf), "capturing... %d %s remaining",
                 Perf_CaptureRemaining(), Perf_CaptureUnit());
    } else if (Perf_IsReplaying()) {
        int idx = Perf_ReplayFrameIdx();
        int n   = Perf_ReplayFrameCount();
        snprintf(buf, sizeof(buf),
                 "replay: %s   frame %d / %d",
                 Perf_ReplayPath(), idx + 1, n);
    } else {
        const char *p = Perf_LastCapturePath();
        if (p) snprintf(buf, sizeof(buf), "last capture: %s", p);
        else   snprintf(buf, sizeof(buf),
                        "type 'profile 120' in the console to capture");
    }
    IG_TextUnformatted(buf);

    // Frametime histogram-as-scrubber when replaying. Each frame in the
    // capture becomes a vertical bar coloured by ms (green < 60fps budget,
    // yellow < 30fps, red beyond). Click / drag jumps to that frame; the
    // currently-selected frame gets a white outline. This makes it
    // trivial to spot and jump to the worst frame in the capture.
    if (Perf_IsReplaying() && Perf_ReplayFrameCount() > 0) {
        int   n_frames = Perf_ReplayFrameCount();
        float max_ms   = Perf_ReplayMaxMs();
        if (max_ms < 1.0f) max_ms = 1.0f;
        // Headroom so the worst bar doesn't kiss the ceiling.
        float scale_max = max_ms * 1.05f;

        float canvas_w = IG_GetContentRegionAvailX();
        if (canvas_w < 64) canvas_w = 64;
        float canvas_h = 30.0f;
        IG_BeginCanvas("##scrub", canvas_w, canvas_h);

        // Backdrop + 16.67ms (60fps) and 33.33ms (30fps) reference lines.
        IG_CanvasRectFilled(0, 0, canvas_w, canvas_h, 0xFF1A1A1A);
        if (scale_max > 16.67f) {
            float y60 = canvas_h - (16.67f / scale_max) * canvas_h;
            IG_CanvasLine(0, y60, canvas_w, y60, 0x60FFFFFF);
        }
        if (scale_max > 33.33f) {
            float y30 = canvas_h - (33.33f / scale_max) * canvas_h;
            IG_CanvasLine(0, y30, canvas_w, y30, 0x40FFFFFF);
        }

        int cur = Perf_ReplayFrameIdx();
        float bar_w = canvas_w / (float)n_frames;
        if (bar_w < 1.0f) bar_w = 1.0f;
        for (int i = 0; i < n_frames; i++) {
            float ms = Perf_ReplayFrameMs(i);
            float h  = (ms / scale_max) * canvas_h;
            float x0 = (i * canvas_w) / n_frames;
            float x1 = x0 + bar_w;
            if (x1 > canvas_w) x1 = canvas_w;
            float y0 = canvas_h - h;

            unsigned int col;
            if      (ms < 16.67f) col = 0xFF4FAF50;   // green: at or above 60fps
            else if (ms < 33.33f) col = 0xFF60D0E0;   // yellow-ish
            else                  col = 0xFF4444FF;   // red: sub-30fps spike

            IG_CanvasRectFilled(x0, y0, x1, canvas_h, col);
            if (i == cur) {
                IG_CanvasRect(x0, 0, x1, canvas_h, 0xFFFFFFFF);
            }
        }

        // Hover tooltip + click/drag selection.
        float mx = -1, my = -1;
        IG_CanvasMousePos(&mx, &my);
        if (mx >= 0 && my >= 0) {
            int hover_idx = (int)((mx / canvas_w) * n_frames);
            if (hover_idx < 0)         hover_idx = 0;
            if (hover_idx >= n_frames) hover_idx = n_frames - 1;
            if (IG_IsItemActive()) {
                Perf_SetReplayFrame(hover_idx);
            }
            // Tooltip with the precise hover frame's ms.
            IG_BeginTooltip();
            snprintf(buf, sizeof(buf),
                     "frame %d / %d\n%.2f ms (%.0f fps)",
                     hover_idx + 1, n_frames,
                     Perf_ReplayFrameMs(hover_idx),
                     1000.0f / Perf_ReplayFrameMs(hover_idx));
            IG_TextUnformatted(buf);
            IG_EndTooltip();
        }
        IG_EndCanvas();
    }

    int node_count = Perf_LastFrameNodeCount();
    if (node_count == 0) {
        IG_TextUnformatted("(no scopes — instrument with PERF_SCOPE)");
        IG_End();
        return;
    }

    float frame_ms = Perf_LastFrameMs();
    if (frame_ms < 0.001f) frame_ms = 1.0f;

    int max_depth = 0;
    for (int i = 0; i < node_count; i++) {
        int d = Perf_ScopeDepth(Perf_LastFrameNode(i));
        if (d > max_depth) max_depth = d;
    }

    float canvas_w = IG_GetContentRegionAvailX();
    if (canvas_w < 64) canvas_w = 64;
    float row_h    = 14.0f;
    float canvas_h = (max_depth + 1) * row_h + 4;

    IG_BeginCanvas("##flame", canvas_w, canvas_h);

    float mx = -1, my = -1;
    IG_CanvasMousePos(&mx, &my);
    const perf_scope_view_t *hovered = NULL;
    float hovered_x0 = 0, hovered_x1 = 0, hovered_y0 = 0, hovered_y1 = 0;

    for (int i = 0; i < node_count; i++) {
        const perf_scope_view_t *s = Perf_LastFrameNode(i);
        float start = Perf_ScopeStartMs(s);
        float dur   = Perf_ScopeMs(s);
        int   d     = Perf_ScopeDepth(s);

        float x0 = (start / frame_ms) * canvas_w;
        float x1 = ((start + dur) / frame_ms) * canvas_w;
        if (x1 < x0 + 1) x1 = x0 + 1;   // 1 px minimum so slivers are visible
        float y0 = d * row_h + 2;
        float y1 = y0 + row_h - 2;

        unsigned int col = scope_color(Perf_ScopeName(s));
        IG_CanvasRectFilled(x0, y0, x1, y1, col);

        if (x1 - x0 >= 36) {
            char label[64];
            snprintf(label, sizeof(label), "%s %.2f",
                     Perf_ScopeName(s), dur);
            IG_CanvasText(x0 + 3, y0 + 1, 0xFFFFFFFF, label);
        }

        // Widen narrow bars' hit region (±1.5 px) for hover.
        float hx0 = x0, hx1 = x1;
        if (hx1 - hx0 < 3) {
            float mid = 0.5f * (hx0 + hx1);
            hx0 = mid - 1.5f;
            hx1 = mid + 1.5f;
        }
        if (mx >= hx0 && mx <= hx1 && my >= y0 && my <= y1) {
            hovered    = s;
            hovered_x0 = x0; hovered_x1 = x1;
            hovered_y0 = y0; hovered_y1 = y1;
        }
    }
    if (hovered) {
        IG_CanvasRect(hovered_x0, hovered_y0, hovered_x1, hovered_y1, 0xFFFFFFFF);
    }
    IG_EndCanvas();

    if (hovered && IG_IsItemHoveredEx()) {
        IG_BeginTooltip();
        snprintf(buf, sizeof(buf), "%s\n%.3f ms (start %.2f ms, depth %d)",
                 Perf_ScopeName(hovered), Perf_ScopeMs(hovered),
                 Perf_ScopeStartMs(hovered), Perf_ScopeDepth(hovered));
        IG_TextUnformatted(buf);
        IG_EndTooltip();
    }

    // ---- Aggregate table -----------------------------------------------
    // Per-scope totals over the whole capture (replay mode) or the live
    // frame ring. Lets you spot consistently-expensive scopes that hide
    // when looking at one frame at a time.
    static int s_agg_sort = (int)PERF_AGG_SORT_TOTAL;
    static const char *sort_labels[] = { "total", "avg", "max", "calls", "name" };
    IG_Spacing();
    IG_SetNextItemWidth(120);
    IG_Combo("Sort by", &s_agg_sort, sort_labels,
             (int)(sizeof(sort_labels)/sizeof(sort_labels[0])));

    Perf_AggregateUpdate((perf_agg_sort_t)s_agg_sort);
    int n_rows  = Perf_AggregateRowCount();
    int n_frm   = Perf_AggregateFrameCount();
    float win_ms = Perf_AggregateWindowMs();

    IG_SameLine(0, 16);
    snprintf(buf, sizeof(buf),
             "%d scopes  over %d frames  (%.1f ms total)",
             n_rows, n_frm, win_ms);
    IG_TextUnformatted(buf);

    if (IG_BeginTable("##perf_agg", 6,
            IG_TF_Borders | IG_TF_RowBg | IG_TF_ScrollY | IG_TF_Resizable,
            0, 0))
    {
        IG_TableSetupScrollFreeze(0, 1);
        IG_TableSetupColumn("scope",      IG_TCF_WidthStretch, 0);
        IG_TableSetupColumn("calls",      IG_TCF_WidthFixed,  70);
        IG_TableSetupColumn("total ms",   IG_TCF_WidthFixed,  90);
        IG_TableSetupColumn("avg ms",     IG_TCF_WidthFixed,  80);
        IG_TableSetupColumn("max ms",     IG_TCF_WidthFixed,  80);
        IG_TableSetupColumn("% window",   IG_TCF_WidthFixed,  80);
        IG_TableHeadersRow();

        float inv_win = (win_ms > 0.001f) ? (100.0f / win_ms) : 0.0f;
        for (int i = 0; i < n_rows; i++) {
            const perf_agg_row_t *r = Perf_AggregateRow(i);
            if (!r) continue;
            IG_TableNextRow();
            IG_TableSetColumnIndex(0);
            IG_TextUnformatted(r->name ? r->name : "?");
            IG_TableSetColumnIndex(1);
            snprintf(buf, sizeof(buf), "%d", r->calls);
            IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(2);
            snprintf(buf, sizeof(buf), "%.3f", r->total_ms);
            IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(3);
            snprintf(buf, sizeof(buf), "%.4f", r->avg_ms);
            IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(4);
            snprintf(buf, sizeof(buf), "%.3f", r->max_ms);
            IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(5);
            snprintf(buf, sizeof(buf), "%.1f%%", r->total_ms * inv_win);
            IG_TextUnformatted(buf);
        }
        IG_EndTable();
    }

    IG_End();
}

static const char *ai_state_name(int s) {
    // Values must match ai_state_t in sim.h: IDLE=0, SUSPICIOUS=1, SEARCHING=2, COMBAT=3
    switch (s) {
        case 0: return "idle";
        case 1: return "suspicious";
        case 2: return "searching";
        case 3: return "combat";
        default: return "?";
    }
}

static void draw_ai(void)
{
    if (!imgui_ai_panel_open) return;
    if (!IG_Begin("AI", &imgui_ai_panel_open, IG_WF_None)) { IG_End(); return; }

    int n = ImguiSupport_AI_Count();
    char buf[160];
    snprintf(buf, sizeof(buf), "%d brains", n);
    IG_TextUnformatted(buf);

    if (IG_BeginTable("##ai", 4,
            IG_TF_Borders | IG_TF_RowBg | IG_TF_ScrollY, 0, 120))
    {
        IG_TableSetupColumn("ent",     IG_TCF_WidthFixed, 40);
        IG_TableSetupColumn("state",   IG_TCF_WidthFixed, 70);
        IG_TableSetupColumn("alert",   IG_TCF_WidthFixed, 60);
        IG_TableSetupColumn("target",  IG_TCF_WidthFixed, 60);
        IG_TableHeadersRow();
        for (int i = 0; i < n; i++) {
            const imgui_ai_row_t *r = ImguiSupport_AI_Row(i);
            if (!r) continue;
            IG_TableNextRow();
            IG_TableSetColumnIndex(0);
            snprintf(buf, sizeof(buf), "%d", r->edict_num);
            IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(1);
            IG_TextUnformatted(ai_state_name(r->state));
            IG_TableSetColumnIndex(2);
            snprintf(buf, sizeof(buf), "%.2f", r->alert_level);
            IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(3);
            snprintf(buf, sizeof(buf), "%d", r->target_edict);
            IG_TextUnformatted(buf);
        }
        IG_EndTable();
    }

    // Navmesh minimap
    int np, ne;
    ImguiSupport_Nav_Count(&np, &ne);
    snprintf(buf, sizeof(buf), "nav: %d pts %d edges", np, ne);
    IG_TextUnformatted(buf);

    if (np > 0) {
        const imgui_nav_point_t *pts = ImguiSupport_Nav_Points();
        float min_x = pts[0].x, max_x = pts[0].x;
        float min_y = pts[0].y, max_y = pts[0].y;
        for (int i = 1; i < np; i++) {
            if (pts[i].x < min_x) min_x = pts[i].x;
            if (pts[i].x > max_x) max_x = pts[i].x;
            if (pts[i].y < min_y) min_y = pts[i].y;
            if (pts[i].y > max_y) max_y = pts[i].y;
        }
        float W = max_x - min_x, H = max_y - min_y;
        if (W < 1) W = 1;
        if (H < 1) H = 1;

        IG_BeginCanvas("##nav", 256, 256);
        const imgui_nav_edge_t *eds = ImguiSupport_Nav_Edges();
        for (int i = 0; i < ne; i++) {
            float ax = (pts[eds[i].a].x - min_x) / W * 256.0f;
            float ay = (pts[eds[i].a].y - min_y) / H * 256.0f;
            float bx = (pts[eds[i].b].x - min_x) / W * 256.0f;
            float by = (pts[eds[i].b].y - min_y) / H * 256.0f;
            IG_CanvasLine(ax, ay, bx, by, 0xFF888888);
        }
        const imgui_nav_active_t *p = ImguiSupport_Nav_Path();
        if (p->has_path) {
            for (int i = 0; i + 1 < p->path_len; i++) {
                float ax = (p->path_xy[2*i+0]   - min_x) / W * 256.0f;
                float ay = (p->path_xy[2*i+1]   - min_y) / H * 256.0f;
                float bx = (p->path_xy[2*(i+1)+0] - min_x) / W * 256.0f;
                float by = (p->path_xy[2*(i+1)+1] - min_y) / H * 256.0f;
                IG_CanvasLine(ax, ay, bx, by, 0xFF00FF00);
            }
        }
        IG_EndCanvas();
    }
    IG_End();
}

static void draw_cvars(void)
{
    static char filter[64];
    char value_buf[128];
    char id_buf[144];

    if (!IG_Begin("Cvars", NULL, IG_WF_None)) { IG_End(); return; }

    IG_SetNextItemWidth(200);
    IG_InputText("filter", filter, (int)sizeof(filter), IG_WF_None);

    if (IG_BeginTable("##cvt", 3,
            IG_TF_Borders | IG_TF_ScrollY | IG_TF_RowBg | IG_TF_Resizable,
            0, -1))
    {
        IG_TableSetupScrollFreeze(0, 1);
        IG_TableSetupColumn("Name",        IG_TCF_WidthFixed,   140);
        IG_TableSetupColumn("Value",       IG_TCF_WidthFixed,   100);
        IG_TableSetupColumn("Description", IG_TCF_WidthStretch, 0);
        IG_TableHeadersRow();

        for (void *cv = ImguiSupport_GetCvarList(); cv; cv = ImguiSupport_CvarNext(cv))
        {
            const char *name = ImguiSupport_CvarName(cv);
            if (filter[0] && !strstr(name, filter)) continue;

            IG_TableNextRow();
            IG_TableSetColumnIndex(0);
            IG_TextUnformatted(name);
            IG_TableSetColumnIndex(1);

            snprintf(value_buf, sizeof(value_buf), "%s", ImguiSupport_CvarString(cv));
            snprintf(id_buf,    sizeof(id_buf),    "##v_%s", name);
            IG_SetNextItemWidth(-1);
            if (IG_InputText(id_buf, value_buf, (int)sizeof(value_buf),
                    IG_ITF_EnterReturnsTrue))
                ImguiSupport_CvarSet(name, value_buf);
            IG_TableSetColumnIndex(2);

            const char *desc = ImguiSupport_CvarDescription(name);
            if (desc) IG_TextUnformatted(desc);
        }
        IG_EndTable();
    }
    IG_End();
}

static void tab_complete_cb(IG_CompletionData *data)
{
    char completed[256];
    if (!ImguiSupport_TabComplete(data->buf, completed, (int)sizeof(completed)))
        return;
    int len, i;
    for (len = 0; completed[len]; len++) {}
    if (len >= data->buf_size) len = data->buf_size - 1;
    for (i = 0; i < len; i++) data->buf[i] = completed[i];
    data->buf[len]     = '\0';
    data->buf_text_len = len;
    data->cursor_pos   = len;
    data->buf_dirty    = 1;
}

static void draw_console(void)
{
    static int  auto_scroll = 1;
    static char input_buf[256];
    static int  reclaim_focus = 0;

    if (!IG_Begin("Console", NULL, IG_WF_None)) { IG_End(); return; }

    /* Checkbox pinned above the scroll region. */
    IG_Checkbox("Auto-scroll", &auto_scroll);

    /* Scrollable log: negative height reserves space for the input line. */
    float footer = IG_GetFrameHeightWithSpacing();
    IG_BeginChild("##log", 0, -footer, 0, 0);

    int total = ImguiSupport_GetNumConsoleLines();
    int show  = total < 200 ? total : 200;
    char buf[128];
    int i;
    for (i = show - 1; i >= 0; i--)
    {
        if (ImguiSupport_GetConsoleLine(i, buf, (int)sizeof(buf)) > 0)
            IG_TextUnformatted(buf);
    }
    if (auto_scroll)
        IG_SetScrollHereY(1.0f);

    IG_EndChild();

    /* Input line — always visible at the bottom. Tab completes. */
    IG_SetNextItemWidth(-1);
    if (IG_InputTextWithCompletion("##cmd", input_buf, (int)sizeof(input_buf),
                                   IG_ITF_EnterReturnsTrue, tab_complete_cb))
    {
        if (input_buf[0] != '\0')
        {
            ImguiSupport_ExecCommand(input_buf);
            input_buf[0] = '\0';
            auto_scroll  = 1;
        }
        reclaim_focus = 1;
    }
    if (reclaim_focus)
    {
        IG_SetKeyboardFocusHere(-1);
        reclaim_focus = 0;
    }

    IG_End();
}

static void draw_entities(void)
{
    char buf[64];
    int n = ImguiSupport_GetNumEdicts();

    if (!IG_Begin("Entities", NULL, IG_WF_None)) { IG_End(); return; }

    snprintf(buf, sizeof(buf), "%d edicts", n);
    IG_TextUnformatted(buf);

    if (IG_BeginTable("##ent", 5,
            IG_TF_Borders | IG_TF_ScrollY | IG_TF_RowBg | IG_TF_Resizable,
            0, -1))
    {
        IG_TableSetupScrollFreeze(0, 1);
        IG_TableSetupColumn("ID",    IG_TCF_WidthFixed, 36);
        IG_TableSetupColumn("Class", IG_TCF_WidthStretch, 0);
        IG_TableSetupColumn("X",     IG_TCF_WidthFixed, 56);
        IG_TableSetupColumn("Y",     IG_TCF_WidthFixed, 56);
        IG_TableSetupColumn("Z",     IG_TCF_WidthFixed, 56);
        IG_TableHeadersRow();

        for (int i = 1; i < n; i++)
        {
            const char *cls = NULL;
            float x = 0, y = 0, z = 0;
            ImguiSupport_GetEdict(i, &cls, &x, &y, &z);
            if (!cls) continue;

            IG_TableNextRow();
            IG_TableSetColumnIndex(0); snprintf(buf, sizeof(buf), "%d",   i);  IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(1); IG_TextUnformatted(cls);
            IG_TableSetColumnIndex(2); snprintf(buf, sizeof(buf), "%.0f", x);  IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(3); snprintf(buf, sizeof(buf), "%.0f", y);  IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(4); snprintf(buf, sizeof(buf), "%.0f", z);  IG_TextUnformatted(buf);
        }
        IG_EndTable();
    }
    IG_End();
}

// ---------------------------------------------------------------------------
// Public interface (declared in imgui_layer.h)
// ---------------------------------------------------------------------------

void ImguiLayer_Init(SDL_Window *w, SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchain_fmt)
{
    s_window = w;
    s_gpu    = gpu;

    IG_CreateContext();
    IG_EnableDocking();
    // Enable layout persistence (was disabled). ImGui writes "imgui.ini" to the
    // process CWD. If it's absent (first run / fresh build), bake the default
    // dock layout the first time the editor opens.
    {
        static const char *ini = "imgui.ini";
        FILE *f = fopen(ini, "rb");
        if (f) fclose(f);
        else   IG_RequestDefaultLayout();
        IG_SetIniFilename(ini);
    }
    IG_StyleColorsDark();

    IG_ImplSDL3_InitForOther(w);
    IG_ImplSDLGPU3_Init(gpu, swapchain_fmt);

    s_inited = 1;
}

void ImguiLayer_Shutdown(void)
{
    if (!s_inited) return;
    IG_ImplSDLGPU3_Shutdown();
    IG_ImplSDL3_Shutdown();
    IG_DestroyContext();
    s_inited = 0;
}

int ImguiLayer_ProcessEvent(SDL_Event *ev)
{
    if (!s_inited || !s_open) return 0;
    return IG_ImplSDL3_ProcessEvent(ev);
}

void ImguiLayer_Toggle(void)
{
    if (!s_inited) return;
    s_open = !s_open;
    SDL_SetWindowRelativeMouseMode(s_window, !s_open);

    // Freeze the perf overlay when the dev panel opens so the user can
    // hover the flame graph / sparklines without the data scrolling out
    // from under them.
    Perf_SetPaused(s_open);

    // Closing: drop any key/mouse state ImGui is tracking. We gate event
    // forwarding on s_open, so a KEY_DOWN that arrived while the layer was
    // open may have no corresponding KEY_UP forwarded if the layer closes
    // before the release event lands. Without this, the next time the
    // layer reopens, ImGui still thinks the key is held — and Shortcut()
    // checks with Repeat (e.g. is_cancel = Shortcut(Esc, Repeat) inside
    // InputText) auto-fire and immediately deactivate any freshly-clicked
    // text widget. Specifically hit when closing the editor with Esc.
    if (!s_open) IG_ClearInputs();
}

int ImguiLayer_IsOpen(void)    { return s_open; }
int ImguiLayer_WantsMouse(void){ return s_open; }

// PrepareGPU: build ImGui's draw lists and stage vertex/index buffers via the
// command buffer's transfer queue. Must run BEFORE the swapchain render pass.
void ImguiLayer_PrepareGPU(SDL_GPUCommandBuffer *cmd)
{
    s_should_render = 0;
    if (!s_inited) return;

    // Render modes:
    //   1. Dev overlay open (F3) — full panel set, game already paused upstream.
    //   2. Overlay closed, `showperf` cvar set — Perf panel only, as HUD.
    //   3. Neither — skip entirely (s_should_render stays 0).
    int perf_only = !s_open && Perf_ShowOverlay();
    if (!s_open && !perf_only) return;
    s_should_render = 1;

    Perf_PushScope("ImguiLayer_Render");

    IG_ImplSDLGPU3_NewFrame();
    IG_ImplSDL3_NewFrame();
    IG_NewFrame();

    {
        extern int  Editor_IsOpen(void);
        extern void Editor_DrawUI(void);
        extern void Editor_DrawViewport(void);
        if (perf_only)
        {
            compute_layout();
            draw_fps();
            draw_profile();
        }
        else if (Editor_IsOpen())
        {
            IG_HostDockSpace();   // passthrough host node; windows dock into it
            Editor_DrawUI();
        }
        else
        {
            IG_HostDockSpace();   // dev panels dock into the shared layout
            Editor_DrawViewport();   // the game as a Viewport panel (centre)
            compute_layout();
            draw_fps();
            draw_ai();
            draw_cvars();
            draw_entities();
            draw_console();
            DebugPanel_Draw(s_debug.x, s_debug.y, s_debug.w, s_debug.h);
            draw_profile();
        }
    }

    IG_Render();
    IG_ImplSDLGPU3_PrepareDrawData(cmd);

    Perf_PopScope();
}

// RenderGPU: emit ImGui's draw calls into an already-open render pass.
void ImguiLayer_RenderGPU(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass)
{
    if (!s_should_render) return;
    IG_ImplSDLGPU3_RenderDrawData(cmd, pass);
}
