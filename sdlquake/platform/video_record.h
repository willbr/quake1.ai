// video_record.h -- MPEG-1 capture of the software framebuffer (dev tool).
#ifndef VIDEO_RECORD_H
#define VIDEO_RECORD_H

void Video_Record_Init(void);          // register cvar + console commands
void Video_Record_CaptureFrame(void);  // call once per VID_Update; no-op unless recording
void Video_Record_Shutdown(void);      // flush/close on engine quit

#endif // VIDEO_RECORD_H
