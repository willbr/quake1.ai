// r_paths.h -- debug patrol-path overlay (cvars r_drawpaths, r_drawpaths_what).

#ifndef SDLQUAKE_R_PATHS_H
#define SDLQUAKE_R_PATHS_H

void RPaths_Init(void);   // register cvars; call after Cvar/Cmd are up
void RPaths_Draw(void);   // rasterise paths into vid.buffer; call before VID_Update blit

#endif
