// r_bbox.h -- debug entity bounding-box overlay (cvar r_drawbboxes).

#ifndef SDLQUAKE_R_BBOX_H
#define SDLQUAKE_R_BBOX_H

void RBBox_Init(void);   // register cvar; call after Cvar/Cmd are up
void RBBox_Draw(void);   // rasterise bboxes into vid.buffer; call before VID_Update blit

#endif
