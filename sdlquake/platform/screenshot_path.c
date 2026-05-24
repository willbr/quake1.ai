#include "screenshot_path.h"

#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  define ss_mkdir(p) _mkdir(p)
#else
#  include <sys/types.h>
#  define ss_mkdir(p) mkdir((p), 0755)
#endif

int Screenshot_NextPath(char *out, size_t outsz)
{
    if (!out || outsz < sizeof("screenshots/shot_9999.png"))
        return 0;

    /* mkdir best-effort; ignore EEXIST and any other error so a
       read-only cwd surfaces later as a stbi_write_png failure. */
    (void)ss_mkdir("screenshots");

    for (int i = 1; i <= 9999; i++) {
        snprintf(out, outsz, "screenshots/shot_%04d.png", i);
        struct stat st;
        if (stat(out, &st) != 0)
            return 1;
    }
    return 0;
}
