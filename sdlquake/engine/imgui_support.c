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

    *classname = pr_strings + e->v.classname;
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
    { "sensitivity",        "Mouse look sensitivity" },
    { "m_pitch",            "Mouse pitch (up/down) scale. Negative to invert." },
    { "m_yaw",              "Mouse yaw (left/right) scale" },
    { "m_forward",          "Mouse forward/back scale" },
    { "m_side",             "Mouse strafe scale" },
    { "m_filter",           "Smooth mouse by averaging two frames (0/1)" },
    /* View */
    { "v_centerspeed",      "Speed at which view re-centres after keyboard look" },
    { "v_kicktime",         "Duration of weapon-kick view effect (seconds)" },
    { "v_kickroll",         "Roll intensity from weapon kick" },
    { "v_kickpitch",        "Pitch intensity from weapon kick" },
    { "v_gunkick",          "Enable weapon recoil view kick (0/1)" },
    { "cl_rollspeed",       "Speed at which roll effect decays" },
    { "cl_rollangle",       "Max roll angle while strafing" },
    { "cl_bob",             "Weapon bob magnitude while moving" },
    { "cl_bobcycle",        "Weapon bob cycle rate" },
    { "cl_bobup",           "Fraction of bob cycle spent moving up" },
    /* Client movement */
    { "cl_forwardspeed",    "Forward walk speed (units/s)" },
    { "cl_backspeed",       "Backward walk speed (units/s)" },
    { "cl_sidespeed",       "Strafe speed (units/s)" },
    { "cl_upspeed",         "Swim / fly up speed" },
    { "cl_movespeedkey",    "Run multiplier when holding the run key" },
    { "cl_yawspeed",        "Keyboard turn speed (yaw)" },
    { "cl_pitchspeed",      "Keyboard look speed (pitch)" },
    { "cl_anglespeedkey",   "Turn speed multiplier" },
    { "lookspring",         "Re-centre view when keyboard look is released (0/1)" },
    { "lookstrafe",         "Strafe instead of turning when mouselooking (0/1)" },
    { "freelook",           "Always-on free mouse look (0/1)" },
    /* HUD / screen */
    { "crosshair",          "Show crosshair (0=off, 1=on)" },
    { "cl_crossx",          "Horizontal crosshair offset in pixels" },
    { "cl_crossy",          "Vertical crosshair offset in pixels" },
    { "scr_conspeed",       "Console slide speed (pixels/second)" },
    { "scr_centertime",     "Seconds to display centre-print messages" },
    { "scr_showram",        "Show RAM usage icon on screen (0/1)" },
    { "scr_showturtle",     "Show turtle icon when frames take too long (0/1)" },
    { "scr_showpause",      "Show pause icon when paused (0/1)" },
    { "scr_printspeed",     "Characters per second for end-level text" },
    { "show_fps",           "Display frames-per-second counter (0/1)" },
    { "cl_sbar",            "Draw status bar (0=off, 1=on)" },
    { "cl_hudswap",         "Move health/ammo display to opposite side" },
    /* Renderer */
    { "r_speeds",           "Print BSP traversal stats to console each frame" },
    { "r_fullbright",       "Ignore lightmaps; draw everything at full brightness" },
    { "r_drawentities",     "Draw monsters, items, and other entities (0/1)" },
    { "r_drawviewmodel",    "Draw the player weapon model (0/1)" },
    { "r_shadows",          "Draw simple blob shadows under entities (0/1)" },
    { "r_wateralpha",       "Water surface opacity (0=invisible, 1=opaque)" },
    { "r_mirroralpha",      "Mirror surface opacity" },
    { "r_dynamic",          "Enable dynamic lights (muzzle flash, explosions) (0/1)" },
    { "r_novis",            "Disable PVS culling — draw all geometry (very slow)" },
    { "r_maxedges",         "Edge count limit before the renderer skips geometry" },
    { "r_maxsurfs",         "Surface count limit before the renderer skips geometry" },
    /* Server physics */
    { "sv_gravity",         "World gravity acceleration (default 800 units/s^2)" },
    { "sv_friction",        "Ground friction coefficient" },
    { "sv_stopspeed",       "Speed below which friction fully stops movement" },
    { "sv_maxspeed",        "Maximum ground movement speed (units/s)" },
    { "sv_accelerate",      "Ground acceleration rate" },
    { "sv_airaccelerate",   "Air-strafing acceleration rate" },
    { "sv_wateraccelerate", "Underwater acceleration rate" },
    { "sv_waterfriction",   "Underwater friction coefficient" },
    { "sv_aim",             "Auto-aim cone half-angle (1=off, lower=stronger)" },
    { "sv_nostep",          "Disable automatic step-up onto low ledges (0/1)" },
    { "sv_idealpitchscale", "Scale of auto-pitch correction on slopes" },
    /* Sound */
    { "volume",             "Master sound-effect volume (0–1)" },
    { "bgmvolume",          "CD music volume (0–1)" },
    { "snd_show",           "Print active sound channels to console each frame" },
    { "loadas8bit",         "Downsample all sounds to 8-bit on load (0/1)" },
    { "precache",           "Precache sounds and models at level load (0/1)" },
    /* Network / server */
    { "hostname",           "Server name shown in the multiplayer browser" },
    { "sys_ticrate",        "Dedicated-server tick rate (frames/second)" },
    { "net_messagetimeout", "Seconds of silence before dropping a connection" },
    { "deathmatch",         "Deathmatch mode (0=SP, 1=DM, 2=DM no powerups)" },
    { "coop",               "Co-operative multiplayer mode (0/1)" },
    { "teamplay",           "Team play mode (0=off, 1=no team damage, 2=team damage)" },
    { "samelevel",          "Restart the same level on death in SP (0/1)" },
    { "noexit",             "Prevent using level exits (0/1)" },
    { "pausable",           "Allow the game to be paused (0/1)" },
    { "skill",              "Difficulty: 0=easy  1=normal  2=hard  3=nightmare" },
    /* Misc */
    { "developer",          "Show developer / debug messages in the console (0/1)" },
    { "sys_nostdout",       "Suppress stdout output (0/1)" },
};

const char *ImguiSupport_CvarDescription(const char *name)
{
    int i, n = (int)(sizeof(s_cvar_descs) / sizeof(s_cvar_descs[0]));
    for (i = 0; i < n; i++)
        if (Q_strcmp(s_cvar_descs[i].name, name) == 0)
            return s_cvar_descs[i].desc;
    return NULL;
}
