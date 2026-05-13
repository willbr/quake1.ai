// debug_lines.h -- single-frame 3D debug line store for the software renderer.

#ifndef DEBUG_LINES_H
#define DEBUG_LINES_H

// Add a line to be drawn this frame. color is a Quake palette index (0-255).
// When ztest is non-zero the line is occluded by world geometry (sampled
// against d_pzbuffer at draw time). Lines are cleared after every
// DebugLines_Draw() call.
void DebugLines_Add(const float start[3], const float end[3],
                    int color, int ztest);

// Project and draw all queued lines into vid.buffer, then clear the queue.
// Called from VID_Update() before the SDL texture upload.
void DebugLines_Draw(void);

#endif // DEBUG_LINES_H
