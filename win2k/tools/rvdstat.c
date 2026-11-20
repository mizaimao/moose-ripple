/* Print the display driver's OpenGL request counters (rb2krvd.h): calls and
 * total time per RVD call since Windows started, kernel and host included.
 * Run it before and after a program and subtract to profile that program.
 * It also prints the contexts, buffers and pages held for programs now. */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include "rb2krvd.h"

static const char *const names[RB2K_RVD_STAT_CODES] = {
    "", "get_info", "get_caps", "context_create", "context_destroy", "buffer_create",
    "buffer_destroy", "submit", "upload", "heap", "set_timeout", "get_stats", "present",
    "desktop", "readback", ""
};

int main(void)
{
    RB2K_RVD_STATS s;
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    int i;
    if (!dc || ExtEscape(dc, RB2K_RVD_STATS_ESCAPE, 0, NULL, sizeof(s), (LPSTR)&s) <= 0 ||
        !s.frequency) {
        puts("RVDSTAT unavailable");
        return 1;
    }
    for (i = 1; i < RB2K_RVD_STAT_CODES; ++i) {
        if (s.calls[i])
            printf("RVDSTAT %s calls=%lu us=%I64d\n", names[i], s.calls[i],
                   s.ticks[i] * 1000000 / s.frequency);
    }
    printf("RVDSTAT held contexts=%lu buffers=%lu pages=%lu\n", s.contexts, s.buffers, s.pages);
    DeleteDC(dc);
    return 0;
}
