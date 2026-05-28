#include <stdio.h>
#include <stdlib.h>
#include "libmodel.h"

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    b = (unsigned char *)malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int check_model(const char *path) {
    size_t len;
    unsigned char *buf = read_file(path, &len);
    lm_model_t *m = NULL;
    lm_result_t r;
    if (!buf) { printf("SKIP (missing): %s\n", path); return 0; }

    r = lm_load_mdl(buf, len, NULL, &m);
    if (r != LM_OK) {
        printf("FAIL parse %s: %s\n", path, lm_strerror(r));
        free(buf);
        return 1;
    }
    printf("OK %s: frames=%d skins=%d verts=%d tris=%d\n",
           path, m->numframes, m->numskins, m->numverts, m->numtris);
    if (m->numframes < 1 || m->numskins < 1 || m->numverts < 1 || m->numtris < 1) {
        printf("FAIL %s: nonsensical counts survived parse\n", path);
        lm_model_free(m); free(buf); return 1;
    }
    lm_model_free(m);

    /* truncation must be rejected, not crash */
    m = NULL;
    r = lm_load_mdl(buf, len / 2, NULL, &m);
    if (r == LM_OK) {
        printf("FAIL %s: truncated buffer parsed successfully\n", path);
        lm_model_free(m); free(buf); return 1;
    }
    printf("OK %s: truncation rejected (%s)\n", path, lm_strerror(r));

    /* bad magic must be rejected */
    if (len >= 4) {
        unsigned char save = buf[0];
        buf[0] ^= 0xFF;
        m = NULL;
        r = lm_load_mdl(buf, len, NULL, &m);
        buf[0] = save;
        if (r != LM_ERR_BAD_MAGIC) {
            printf("FAIL %s: bad magic not detected (%s)\n", path, lm_strerror(r));
            free(buf); return 1;
        }
        printf("OK %s: bad magic rejected\n", path);
    }

    free(buf);
    return 0;
}

int main(void) {
    int fails = 0;
    /* player.mdl = single frames + single skin; flame2.mdl exercises frame
       groups. Both exist in the committed shareware id1/progs set. */
    fails += check_model("id1/progs/player.mdl");
    fails += check_model("id1/progs/flame2.mdl");
    fails += check_model("id1/progs/armor.mdl");
    if (fails) { printf("\n%d FAILURE(S)\n", fails); return 1; }
    printf("\nALL OK\n");
    return 0;
}
