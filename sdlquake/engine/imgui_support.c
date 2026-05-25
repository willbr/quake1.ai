// imgui_support.c -- bridges Quake state to the Dear ImGui overlay
// Compiled with engine C flags (gnu89) so quakedef.h is safe to include.

#include "quakedef.h"
#include "imgui_support.h"

/* pr_strings is the progs string heap; string_t is just an offset into it. */
extern char *pr_strings;

/* Console circular buffer (defined in console.c). */
extern char *con_text;
extern int   con_current;
extern int   con_linewidth;
extern int   con_totallines;

void *ImguiSupport_GetCvarList(void)
{
    return cvar_vars;
}

const char *ImguiSupport_CvarName(void *cv)
{
    return ((cvar_t *)cv)->name;
}

const char *ImguiSupport_CvarString(void *cv)
{
    return ((cvar_t *)cv)->string;
}

void *ImguiSupport_CvarNext(void *cv)
{
    return ((cvar_t *)cv)->next;
}

void ImguiSupport_CvarSet(const char *name, const char *value)
{
    Cvar_Set((char *)name, (char *)value);
}

float ImguiSupport_CvarValue(const char *name)
{
    return Cvar_VariableValue((char *)name);
}

void ImguiSupport_CvarSetValue(const char *name, float v)
{
    char buf[32];
    /* %g gives "0" / "1" / "0.5" instead of "0.000000" — matches what users
     * type into the console and avoids the cvar string blowing up in width
     * on every UI tick. */
    snprintf(buf, sizeof(buf), "%g", v);
    Cvar_Set((char *)name, buf);
}

int ImguiSupport_CvarExists(const char *name)
{
    return Cvar_FindVar((char *)name) != NULL;
}

int ImguiSupport_GetNumEdicts(void)
{
    return sv.active ? sv.num_edicts : 0;
}

void ImguiSupport_GetEdict(int i, const char **classname,
                            float *x, float *y, float *z)
{
    *classname = NULL;
    if (!sv.active || i <= 0 || i >= sv.num_edicts)
        return;

    edict_t *e = EDICT_NUM(i);
    if (!e || e->free)
        return;

    *classname = e->v.classname ? e->v.classname : "";
    *x = e->v.origin[0];
    *y = e->v.origin[1];
    *z = e->v.origin[2];
}

void ImguiSupport_ExecCommand(const char *cmd)
{
    char buf[256];
    int len;
    for (len = 0; cmd[len] && len < (int)sizeof(buf) - 2; len++)
        buf[len] = cmd[len];
    buf[len++] = '\n';
    buf[len]   = '\0';
    Cbuf_AddText(buf);
}

int ImguiSupport_TabComplete(const char *partial, char *out, int out_size)
{
    char *match = Cmd_CompleteCommand((char *)partial);
    if (!match)
        match = Cvar_CompleteVariable((char *)partial);
    if (!match)
        return 0;

    int len;
    for (len = 0; match[len] && len < out_size - 2; len++)
        out[len] = match[len];
    out[len++] = ' ';  /* append space, matching original Quake behaviour */
    out[len]   = '\0';
    return 1;
}

int ImguiSupport_GetNumConsoleLines(void)
{
    if (!con_text || con_linewidth <= 0 || con_totallines <= 0) return 0;
    int n = con_current + 1;
    return n < con_totallines ? n : con_totallines;
}

/* Returns number of printable chars written (0 = line was blank). */
int ImguiSupport_GetConsoleLine(int from_bottom, char *buf, int buf_size)
{
    if (!con_text || con_linewidth <= 0 || buf_size <= 0) { buf[0] = '\0'; return 0; }

    int idx = ((con_current - from_bottom) % con_totallines + con_totallines) % con_totallines;
    const char *src = con_text + idx * con_linewidth;

    int max = buf_size - 1;
    if (max > con_linewidth) max = con_linewidth;

    int len = 0;
    int i;
    for (i = 0; i < max; i++) {
        char c = src[i] & 0x7F;   /* strip Quake color bit */
        if (c == '\0') break;
        if (c >= 32 && c < 127)
            buf[len++] = c;
    }
    while (len > 0 && buf[len - 1] == ' ') len--;  /* trim trailing spaces */
    buf[len] = '\0';
    return len;
}

/* ---------------------------------------------------------------------------
 * Cvar description table
 * --------------------------------------------------------------------------- */

static const struct { const char *name; const char *desc; } s_cvar_descs[] = {
    /* Mouse */
    { "sensitivity",          "Mouse look sensitivity" },
    { "m_pitch",              "Mouse pitch (up/down) scale. Negative to invert." },
    { "m_yaw",                "Mouse yaw (left/right) scale" },
    { "m_forward",            "Mouse forward/back scale" },
    { "m_side",               "Mouse strafe scale" },
    { "m_filter",             "Smooth mouse by averaging two frames (0/1)" },
    { "in_mouse",             "Enable mouse input (0/1)" },
    { "in_dgamouse",          "Use XF86 DGA direct mouse on Linux (0/1)" },
    /* View */
    { "v_centerspeed",        "Speed at which view re-centres after keyboard look" },
    { "v_centermove",         "Distance player must walk before view auto-centres" },
    { "v_kicktime",           "Duration of weapon-kick view effect (seconds)" },
    { "v_kickroll",           "Roll intensity from weapon kick" },
    { "v_kickpitch",          "Pitch intensity from weapon kick" },
    { "v_gunkick",            "Enable weapon recoil view kick (0/1)" },
    { "v_idlescale",          "Scale of idle weapon sway when standing still (0=off)" },
    { "v_iroll_cycle",        "Idle roll oscillation speed" },
    { "v_iroll_level",        "Idle roll oscillation amplitude" },
    { "v_ipitch_cycle",       "Idle pitch oscillation speed" },
    { "v_ipitch_level",       "Idle pitch oscillation amplitude" },
    { "v_iyaw_cycle",         "Idle yaw oscillation speed" },
    { "v_iyaw_level",         "Idle yaw oscillation amplitude" },
    { "cl_rollspeed",         "Speed at which strafe roll effect decays" },
    { "cl_rollangle",         "Max roll angle while strafing" },
    { "cl_bob",               "Weapon bob magnitude while moving" },
    { "cl_bobcycle",          "Weapon bob cycle rate" },
    { "cl_bobup",             "Fraction of bob cycle spent moving up" },
    /* Client movement */
    { "cl_forwardspeed",      "Forward walk speed (units/s)" },
    { "cl_backspeed",         "Backward walk speed (units/s)" },
    { "cl_sidespeed",         "Strafe speed (units/s)" },
    { "cl_upspeed",           "Swim / fly up speed (units/s)" },
    { "cl_movespeedkey",      "Run multiplier when holding the run key" },
    { "cl_yawspeed",          "Keyboard turn speed (yaw)" },
    { "cl_pitchspeed",        "Keyboard look speed (pitch)" },
    { "cl_anglespeedkey",     "Turn speed multiplier" },
    { "lookspring",           "Re-centre view when keyboard look is released (0/1)" },
    { "lookstrafe",           "Strafe instead of turning when mouselooking (0/1)" },
    { "freelook",             "Always-on free mouse look (0/1)" },
    { "auxlook",              "Enable auxiliary look input device (0/1)" },
    /* Client misc */
    { "_cl_name",             "Player name shown in multiplayer" },
    { "_cl_color",            "Player colour (shirt/pants packed as single int)" },
    { "cl_nolerp",            "Disable entity movement interpolation between frames (0/1)" },
    { "cl_shownet",           "Network debug: 0=off  1=packet sizes  2=full breakdown" },
    /* HUD / screen */
    { "crosshair",            "Show crosshair (0=off, 1=on)" },
    { "cl_crossx",            "Horizontal crosshair offset in pixels" },
    { "cl_crossy",            "Vertical crosshair offset in pixels" },
    { "cl_sbar",              "Draw status bar (0=off, 1=on)" },
    { "cl_hudswap",           "Move health/ammo display to opposite side (0/1)" },
    { "scr_conspeed",         "Console slide speed (pixels/second)" },
    { "scr_centertime",       "Seconds to display centre-print messages" },
    { "scr_showram",          "Show RAM usage icon on screen (0/1)" },
    { "scr_showturtle",       "Show turtle icon when frames take too long (0/1)" },
    { "scr_showpause",        "Show pause icon when paused (0/1)" },
    { "scr_printspeed",       "Characters per second for end-level text" },
    { "scr_ofsx",             "View X offset (driven by weapon kick effects)" },
    { "scr_ofsy",             "View Y offset (driven by weapon kick effects)" },
    { "scr_ofsz",             "View Z offset (driven by weapon kick effects)" },
    { "show_fps",             "Display frames-per-second counter (0/1)" },
    /* Chase cam (third-person) */
    { "chase_active",         "Enable third-person chase camera (0/1)" },
    { "chase_back",           "Chase camera distance behind the player" },
    { "chase_up",             "Chase camera height above the player" },
    { "chase_right",          "Chase camera horizontal offset to the right" },
    /* Renderer */
    { "r_speeds",             "Print BSP traversal stats to console each frame" },
    { "r_fullbright",         "Ignore lightmaps; draw everything at full brightness (0/1)" },
    { "r_drawentities",       "Draw monsters, items, and other entities (0/1)" },
    { "r_drawbboxes",         "Debug: entity bounding boxes. 0=off, 0.25=light, 0.5=half, 1=opaque" },
    { "r_drawviewmodel",      "Draw the player weapon model (0/1)" },
    { "r_shadows",            "Draw blob shadows under entities (0/1)" },
    { "r_wateralpha",         "Water surface opacity (0=invisible, 1=opaque)" },
    { "r_mirroralpha",        "Mirror surface opacity" },
    { "r_dynamic",            "Enable dynamic lights (muzzle flash, explosions) (0/1)" },
    { "r_novis",              "Disable PVS culling — draw all geometry (very slow)" },
    { "r_maxedges",           "Edge count limit before the renderer skips geometry" },
    { "r_maxsurfs",           "Surface count limit before the renderer skips geometry" },
    { "r_waterwarp",          "Apply wave distortion to the screen underwater (0/1)" },
    { "r_drawflat",           "Draw all surfaces as flat-coloured (no textures) (0/1)" },
    { "r_draworder",          "Draw surfaces in BSP order without Z-sorting (debug)" },
    { "r_norefresh",          "Skip rendering entirely — show last frame (0/1)" },
    { "r_lightmap",           "Render raw lightmap only, no textures (0/1)" },
    { "r_ambient",            "Ambient light level added to every surface" },
    { "r_clearcolor",         "Software sky/background palette index (0–255)" },
    { "r_aliastransbase",     "Base translucency for alias (MD1) models" },
    { "r_aliastransadj",      "Translucency adjustment for alias models" },
    { "r_dspeeds",            "Print detailed renderer timing stats each frame" },
    { "r_timegraph",          "Show per-frame timing bar graph on screen (0/1)" },
    { "r_graphheight",        "Height in pixels of the r_timegraph bar" },
    { "r_numedges",           "Current frame edge count (read-only)" },
    { "r_numsurfs",           "Current frame surface count (read-only)" },
    { "r_polymodelstats",     "Print alias-model polygon stats each frame (0/1)" },
    { "r_reportedgeout",      "Warn when edge buffer limit is hit (0/1)" },
    { "r_reportsurfout",      "Warn when surface buffer limit is hit (0/1)" },
    /* Software renderer internals */
    { "d_mipcap",             "Maximum mip level cap for textures (0=full detail)" },
    { "d_mipscale",           "Mip-map scale bias (higher=blurrier but faster)" },
    { "d_subdiv16",           "Subdivide lit surfaces every 16 units (0/1)" },
    { "pixel_multiply",       "Pixel upscale factor for the software renderer" },
    /* Server physics */
    { "sv_gravity",           "World gravity acceleration (default 800 units/s²)" },
    { "sv_friction",          "Ground friction coefficient" },
    { "sv_stopspeed",         "Speed below which friction fully stops movement" },
    { "sv_maxspeed",          "Maximum ground movement speed (units/s)" },
    { "sv_maxvelocity",       "Maximum velocity any entity can reach (units/s)" },
    { "sv_accelerate",        "Ground acceleration rate" },
    { "sv_airaccelerate",     "Air-strafing acceleration rate" },
    { "sv_wateraccelerate",   "Underwater acceleration rate" },
    { "sv_waterfriction",     "Underwater friction coefficient" },
    { "sv_aim",               "Auto-aim cone half-angle (1=off, lower=stronger)" },
    { "sv_nostop",            "Disable automatic step-up onto low ledges (0/1)" },
    { "sv_nostep",            "Disable automatic step-up onto low ledges (0/1)" },
    { "sv_idealpitchscale",   "Scale of auto-pitch correction on slopes" },
    { "edgefriction",         "Extra friction at ledge edges to prevent sliding off" },
    /* Sound */
    { "volume",               "Master sound-effect volume (0–1)" },
    { "bgmvolume",            "CD music volume (0–1)" },
    { "bgmbuffer",            "CD music streaming buffer size (bytes)" },
    { "snd_show",             "Print active sound channels to console each frame (0/1)" },
    { "snd_noextraupdate",    "Skip extra sound mixing between frames (0/1)" },
    { "loadas8bit",           "Downsample all sounds to 8-bit on load (0/1)" },
    { "nosound",              "Disable all audio output (0/1)" },
    { "precache",             "Precache sounds and models at level load (0/1)" },
    { "ambient_level",        "Volume of ambient background sounds" },
    { "ambient_fade",         "Rate at which ambient sounds fade in/out" },
    /* Network / server */
    { "hostname",             "Server name shown in the multiplayer browser" },
    { "sys_ticrate",          "Dedicated-server tick rate (frames/second)" },
    { "net_messagetimeout",   "Seconds of silence before dropping a connection" },
    { "deathmatch",           "Deathmatch mode (0=SP, 1=DM, 2=DM no powerups)" },
    { "coop",                 "Co-operative multiplayer mode (0/1)" },
    { "teamplay",             "Team play mode (0=off, 1=no friendly fire, 2=friendly fire)" },
    { "fraglimit",            "Frag count to end a deathmatch (0=no limit)" },
    { "timelimit",            "Match time limit in minutes (0=no limit)" },
    { "samelevel",            "Restart the same level on death in SP (0/1)" },
    { "noexit",               "Prevent using level exits (0/1)" },
    { "pausable",             "Allow the game to be paused (0/1)" },
    { "nomonsters",           "Spawn no monsters (useful for demo/testing) (0/1)" },
    { "skill",                "Difficulty: 0=easy  1=normal  2=hard  3=nightmare" },
    /* Modem / serial networking */
    { "_config_com_modem",    "Enable modem mode on serial port (0/1)" },
    { "_config_com_port",     "Serial port number for modem networking" },
    { "_config_com_irq",      "Serial port IRQ for modem networking" },
    { "_config_com_baud",     "Serial port baud rate for modem networking" },
    { "_config_modem_init",   "AT init string sent to modem on connect" },
    { "_config_modem_hangup", "AT command to hang up the modem" },
    { "_config_modem_clear",  "AT command to reset the modem" },
    { "_config_modem_dialtype","Dial type: T=tone, P=pulse" },
    /* Joystick */
    { "joystick",             "Enable joystick / gamepad input (0/1)" },
    { "joyname",              "Joystick device name" },
    { "joybuttons",           "Number of joystick buttons to read" },
    { "joyadvanced",          "Use advanced per-axis joystick configuration (0/1)" },
    { "joyadvaxisx",          "Advanced: X-axis function mapping" },
    { "joyadvaxisy",          "Advanced: Y-axis function mapping" },
    { "joyadvaxisz",          "Advanced: Z-axis function mapping" },
    { "joyadvaxisr",          "Advanced: R-axis (roll) function mapping" },
    { "joyadvaxisu",          "Advanced: U-axis function mapping" },
    { "joyadvaxisv",          "Advanced: V-axis function mapping" },
    { "joyforwardthreshold",  "Forward/back axis dead zone" },
    { "joyforwardsensitivity","Forward/back axis sensitivity multiplier" },
    { "joysidethreshold",     "Strafe axis dead zone" },
    { "joysidesensitivity",   "Strafe axis sensitivity multiplier" },
    { "joypitchthreshold",    "Pitch axis dead zone" },
    { "joypitchsensitivity",  "Pitch axis sensitivity multiplier" },
    { "joyyawthreshold",      "Yaw axis dead zone" },
    { "joyyawsensitivity",    "Yaw axis sensitivity multiplier" },
    { "joywwhack1",           "Hardware workaround for joystick axis glitch #1" },
    { "joywwhack2",           "Hardware workaround for joystick axis glitch #2" },
    /* Host / timing */
    { "host_framerate",       "Lock to a fixed frame rate in seconds (0=unlimited)" },
    { "host_speeds",          "Print per-frame physics/render/sound timing (0/1)" },
    { "serverprofile",        "Show server frame timing profile each frame (0/1)" },
    /* Misc */
    { "developer",            "Show developer / debug messages in the console (0/1)" },
    { "sys_nostdout",         "Suppress stdout output (0/1)" },
    { "idgods",               "Enable id Software cheat/god commands (0/1)" },
    /* QuakeC saved-state slots (read/written by progs) */
    { "gamecfg",              "Current game configuration index (progs internal)" },
    { "savedgamecfg",         "Saved game configuration index (progs internal)" },
    { "saved1",               "Progs saved-state slot 1" },
    { "saved2",               "Progs saved-state slot 2" },
    { "saved3",               "Progs saved-state slot 3" },
    { "saved4",               "Progs saved-state slot 4" },
    { "scratch1",             "Progs scratch variable 1" },
    { "scratch2",             "Progs scratch variable 2" },
    { "scratch3",             "Progs scratch variable 3" },
    { "scratch4",             "Progs scratch variable 4" },
    { "temp1",                "Progs temporary variable" },
    /* Video */
    { "vid_mode",             "Video mode index (0=320×200, higher=larger)" },
    /* GL-only (inactive in software build) */
    { "gl_subdivide_size",    "GL: warp-surface subdivision size" },
    { "gl_cshiftpercent",     "GL: damage/powerup screen tint intensity (%)" },
    { "gl_cull",              "GL: enable back-face polygon culling (0/1)" },
    { "gl_clear",             "GL: clear the colour buffer each frame (0/1)" },
    { "gl_smoothmodels",      "GL: smooth-shade alias model polygons (0/1)" },
    { "gl_affinemodels",      "GL: use faster affine texture mapping on models (0/1)" },
    { "gl_polyblend",         "GL: apply screen blend effects (damage, liquids) (0/1)" },
    { "gl_flashblend",        "GL: render dynamic lights as simple coronas (0/1)" },
    { "gl_nocolors",          "GL: disable player colour skins (0/1)" },
    { "gl_playermip",         "GL: mipmap bias for player model textures" },
    { "gl_texsort",           "GL: sort surfaces by texture for draw-call batching (0/1)" },
    { "gl_ztrick",            "GL: alternate Z-buffer trick for faster clears (0/1)" },
    { "gl_keeptjunctions",    "GL: preserve T-junction fixes (0/1)" },
    { "gl_reporttjunctions",  "GL: report T-junction elimination stats (0/1)" },
    { "gl_finish",            "GL: call glFinish() each frame to sync GPU (0/1)" },
    { "gl_doubleeys",         "GL: stereoscopic rendering for 3D glasses (0/1)" },
};

const char *ImguiSupport_CvarDescription(const char *name)
{
    int i, n = (int)(sizeof(s_cvar_descs) / sizeof(s_cvar_descs[0]));
    for (i = 0; i < n; i++)
        if (Q_strcmp(s_cvar_descs[i].name, name) == 0)
            return s_cvar_descs[i].desc;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * AI dev panel shared state
 * --------------------------------------------------------------------------- */

static imgui_ai_row_t s_ai_rows[IMGUI_AI_MAX_ROWS];
static int            s_ai_count;

void ImguiSupport_AI_Clear(void) { s_ai_count = 0; }

void ImguiSupport_AI_Push(const imgui_ai_row_t *row) {
    if (s_ai_count >= IMGUI_AI_MAX_ROWS) return;
    s_ai_rows[s_ai_count++] = *row;
}

int ImguiSupport_AI_Count(void) { return s_ai_count; }

const imgui_ai_row_t *ImguiSupport_AI_Row(int i) {
    if (i < 0 || i >= s_ai_count) return 0;
    return &s_ai_rows[i];
}

/* ---------------------------------------------------------------------------
 * Nav minimap shared state
 * --------------------------------------------------------------------------- */
static imgui_nav_point_t  s_nav_pts[IMGUI_NAV_MAX_POINTS];
static imgui_nav_edge_t   s_nav_eds[IMGUI_NAV_MAX_EDGES];
static int                s_nav_np, s_nav_ne;
static imgui_nav_active_t s_nav_path;

void ImguiSupport_Nav_Set(const imgui_nav_point_t *pts, int np,
                          const imgui_nav_edge_t *eds, int ne) {
    if (np > IMGUI_NAV_MAX_POINTS) np = IMGUI_NAV_MAX_POINTS;
    if (ne > IMGUI_NAV_MAX_EDGES)  ne = IMGUI_NAV_MAX_EDGES;
    memcpy(s_nav_pts, pts, sizeof(imgui_nav_point_t) * np);
    memcpy(s_nav_eds, eds, sizeof(imgui_nav_edge_t)  * ne);
    s_nav_np = np;
    s_nav_ne = ne;
}

void ImguiSupport_Nav_SetPath(const imgui_nav_active_t *p) {
    s_nav_path = *p;
}

int ImguiSupport_Nav_Count(int *out_np, int *out_ne) {
    if (out_np) *out_np = s_nav_np;
    if (out_ne) *out_ne = s_nav_ne;
    return s_nav_np > 0;
}

const imgui_nav_point_t  *ImguiSupport_Nav_Points(void) { return s_nav_pts; }
const imgui_nav_edge_t   *ImguiSupport_Nav_Edges(void)  { return s_nav_eds; }
const imgui_nav_active_t *ImguiSupport_Nav_Path(void)   { return &s_nav_path; }
