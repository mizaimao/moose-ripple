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
    RB2K_GL_STATS gl;
    RB2K_GL_TIMING timing;
    static const char *const operations[RB2K_GL_TIME_COUNT] = {
        "partial_read", "frame_read", "upload", "copy", "flush"
    };
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    int i;
    if (!dc || ExtEscape(dc, RB2K_RVD_STATS_ESCAPE, 0, NULL, sizeof(s), (LPSTR)&s) <= 0 ||
        !s.frequency) {
        puts("RVDSTAT unavailable");
        return 1;
    }
    for (i = 1; i < RB2K_RVD_STAT_CODES; ++i) {
        if (s.calls[i])
            printf("RVDSTAT %s calls=%lu failed=%lu us=%I64d\n", names[i], s.calls[i], s.failures[i],
                   s.ticks[i] * 1000000 / s.frequency);
    }
    printf("RVDSTAT held contexts=%lu buffers=%lu pages=%lu\n", s.contexts, s.buffers, s.pages);
    if (ExtEscape(dc, RB2K_GL_STATS_ESCAPE, 0, NULL, sizeof(gl), (LPSTR)&gl) > 0)
        printf("RVDSTAT desktop frames=%lu readbacks=%lu pixels=%lu full=%lu uploads=%lu\n",
               gl.frames, gl.readbacks, gl.readbackPixels, gl.fullFrameReads, gl.uploads);
    if (ExtEscape(dc, RB2K_GL_TIMING_ESCAPE, 0, NULL, sizeof(timing), (LPSTR)&timing) > 0 &&
        timing.frequency > 0) {
        for (i = 0; i < RB2K_GL_TIME_COUNT; ++i) {
            const RB2K_GL_TIME *t = &timing.operations[i];
            printf("GLTIME %s calls=%lu failed=%lu ticks=%I64d frequency=%I64d\n",
                   operations[i], t->calls, t->failed, t->ticks, timing.frequency);
        }
    }
    DeleteDC(dc);
    return 0;
}
