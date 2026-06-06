/* jo_mpeg -- public-domain single-file MPEG-1 video writer.
 * http://jonolick.com  (C conversion by Wladislav Artsimovich).
 * Implementation lives in jo_mpeg.c; this is just the public prototype.
 *
 * `rgbx` is width*height pixels, 4 bytes each in R,G,B,X memory order (X
 * ignored). Each call appends one self-contained intra-coded frame, so
 * recording is: fopen -> jo_write_mpeg per frame -> fclose.
 * `fps` must be one of 24, 25, 30, 50, 60.
 */
#ifndef JO_INCLUDE_MPEG_H
#define JO_INCLUDE_MPEG_H

#include <stdio.h>

// Encode one frame straight to a FILE*.
extern void jo_write_mpeg(FILE *fp, const unsigned char *rgbx, int width, int height, int fps);

// Encode one frame into a malloc'd byte buffer (caller frees); *out_len gets
// the length. Lets frames be encoded off-thread and concatenated in order.
extern unsigned char *jo_encode_mpeg_frame(const unsigned char *rgbx, int width, int height, int fps, int *out_len);

#endif // JO_INCLUDE_MPEG_H
