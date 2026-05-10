// debug_lines.c -- single-frame 3D debug line store for the software renderer.
//
// Game code submits lines via DebugLines_Add() each frame (e.g. navmesh edges).
// DebugLines_Draw() is called from VID_Update() before the framebuffer blit;
// it projects every queued line into vid.buffer using r_debugdraw primitives,
// then clears the queue for the next frame.

#include <string.h>
#include "quakedef.h"
#include "r_debugdraw.h"
#include "debug_lines.h"

#define MAX_DEBUG_LINES 4096

typedef struct {
    float start[3], end[3];
    int   color;
} debug_line_entry_t;

static debug_line_entry_t s_lines[MAX_DEBUG_LINES];
static int                s_count;

extern float scr_con_current;

void DebugLines_Add(const float start[3], const float end[3], int color)
{
    if (s_count >= MAX_DEBUG_LINES) return;
    debug_line_entry_t *l = &s_lines[s_count++];
    l->start[0] = start[0]; l->start[1] = start[1]; l->start[2] = start[2];
    l->end[0]   = end[0];   l->end[1]   = end[1];   l->end[2]   = end[2];
    l->color    = color;
}

void DebugLines_Draw(void)
{
    int i;
    if (!s_count) return;

    if (!vid.buffer || cls.state != ca_connected || !cl.worldmodel ||
        key_dest != key_game || scr_con_current > 0)
    {
        s_count = 0;
        return;
    }

    RDD_BeginFrame(1.0f);
    if (RDD_Visible())
    {
        for (i = 0; i < s_count; i++)
        {
            vec3_t va, vb;
            RDD_ToView(s_lines[i].start, va);
            RDD_ToView(s_lines[i].end,   vb);
            RDD_DrawLine3D_View(va, vb, s_lines[i].color);
        }
    }
    s_count = 0;
}
