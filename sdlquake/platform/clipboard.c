#include "clipboard.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void   *bytes;
    size_t  size;
} clip_payload_t;

/* SDL invokes this with mime_type=NULL when the clipboard is cleared
   or replaced; return NULL with *size=0 in that case. */
static const void *clip_provide(void *userdata, const char *mime_type, size_t *size)
{
    clip_payload_t *p = (clip_payload_t *)userdata;
    if (!p || !mime_type) { if (size) *size = 0; return NULL; }
    if (strcmp(mime_type, "image/png") != 0) { if (size) *size = 0; return NULL; }
    if (size) *size = p->size;
    return p->bytes;
}

static void clip_cleanup(void *userdata)
{
    clip_payload_t *p = (clip_payload_t *)userdata;
    if (!p) return;
    free(p->bytes);
    free(p);
}

int Clipboard_SetPNG(const void *png_bytes, size_t size)
{
    if (!png_bytes || size == 0) return 0;

    clip_payload_t *p = (clip_payload_t *)malloc(sizeof(*p));
    if (!p) return 0;
    p->bytes = malloc(size);
    if (!p->bytes) { free(p); return 0; }
    memcpy(p->bytes, png_bytes, size);
    p->size = size;

    const char *mimes[1] = { "image/png" };
    if (!SDL_SetClipboardData(clip_provide, clip_cleanup, p, mimes, 1)) {
        /* On failure SDL does not call the cleanup callback. */
        free(p->bytes);
        free(p);
        return 0;
    }
    return 1;
}
