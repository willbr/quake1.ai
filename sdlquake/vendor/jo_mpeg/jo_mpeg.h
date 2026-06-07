/* jo_mpeg -- public-domain single-file MPEG-1 video writer.
 * http://jonolick.com  (C conversion by Wladislav Artsimovich).
 * Implementation lives in jo_mpeg.c; this is just the public prototype.
 *
 * `rgbx` is width*height pixels, 4 bytes each in R,G,B,X memory order (X
 * ignored). Each call appends one self-contained intra-coded frame (its own
 * closed GOP). `frame_index` is the frame's 0-based stream position — it sets
 * the GOP timecode, so a progressing clock and seeking work. Recording is:
 * fopen -> jo_write_mpeg(.., i) for i=0,1,2,... -> jo_mpeg_end_sequence -> fclose.
 * `fps` must be one of 24, 25, 30, 50, 60.
 */
#ifndef JO_INCLUDE_MPEG_H
#define JO_INCLUDE_MPEG_H

#include <stdio.h>

// Encode one frame straight to a FILE*.
extern void jo_write_mpeg(FILE *fp, const unsigned char *rgbx, int width, int height, int fps, int frame_index);

// Encode one frame into a malloc'd byte buffer (caller frees); *out_len gets
// the length. Lets frames be encoded off-thread and concatenated in order.
extern unsigned char *jo_encode_mpeg_frame(const unsigned char *rgbx, int width, int height, int fps, int frame_index, int *out_len);

// Write the single trailing sequence-end code. Call once after the last frame.
extern void jo_mpeg_end_sequence(FILE *fp);

#endif // JO_INCLUDE_MPEG_H
