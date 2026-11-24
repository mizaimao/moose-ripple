/* Print the native Direct3D 7 draw counters of the display driver. Run it
 * before and after an application; the difference is what that run drew,
 * set aside and uploaded. */
#include <windows.h>
#include <stdio.h>
#include "rb2kd3d7.h"

int main(void)
{
    static const char *const skips[RB2K_D7_SKIPS] = {
        "fill", "stencil", "stage1", "vertex", "texture", "combiner", "primitive", "clear"
    };
    static const char *const ignored[RB2K_D7_IGNORED] = { "fog", "colorkey", "clipplane", "mipmap", "lighting" };
    RB2K_D7_DRAWSTATS s;
    HDC dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    ULONG i;
    if (!dc || ExtEscape(dc, RB2K_D3D7_DRAW_ESCAPE, 0, NULL, sizeof(s), (LPSTR)&s) <= 0 ||
        s.version != RB2K_D7_DRAWSTATS_VERSION) {
        puts("D7DRAW unavailable");
        return 1;
    }
    printf("D7DRAW drawn=%lu calls=%lu skipped_calls=%lu textures=%lu uploads=%lu upload_bytes=%lu "
           "checks=%lu upload_failures=%lu check_failures=%lu\n", s.drawn, s.drawCalls, s.skippedCalls,
           s.textures, s.uploads, s.uploadBytes, s.checks, s.uploadFailures, s.checkFailures);
    printf("D7DRAW skipped");
    for (i = 0; i < RB2K_D7_SKIPS; ++i) printf(" %s=%lu", skips[i], s.skipped[i]);
    printf(" ignored");
    for (i = 0; i < RB2K_D7_IGNORED; ++i) printf(" %s=%lu", ignored[i], s.ignored[i]);
    printf("\nD7DRAW textures_by_format none=%lu argb8888=%lu xrgb8888=%lu rgb565=%lu argb1555=%lu xrgb1555=%lu argb4444=%lu video=%lu texblts=%lu texblt_failures=%lu\n",
           s.registered[0], s.registered[1], s.registered[2], s.registered[3], s.registered[4],
           s.registered[5], s.registered[6], s.videoTextures, s.texBlts, s.texBltFailures);
    printf("D7DRAW dp2_calls=%lu dp2_failures=%lu last_failure=%08lx last_failure_op=%lu draw_failures=%lu",
           s.dp2Calls, s.dp2Failures, s.lastFailure, s.lastFailureOp, s.drawFailures);
    printf("\nD7DRAW opcodes");
    for (i = 0; i < RB2K_D7_OPCODES; ++i) if (s.opcodes[i]) printf(" %lu=%lu", i, s.opcodes[i]);
    printf("\nD7DRAW refused range=%lu vbcaps=%lu linear=%lu address=%lu probe=%lu immediate=%lu length=%lu "
           "other=%lu bad_positions=%lu\n", s.failSites[0], s.failSites[1], s.failSites[2], s.failSites[3],
           s.failSites[4], s.failSites[5], s.failSites[6], s.failSites[7], s.badPositions);
    printf("D7DRAW last_refused op=%lu first=%lu low=%lu high=%lu available=%lu offset=%lu linear=%lu "
           "flags=%lx stride=%lu count=%lu\n", s.lastRange[0], s.lastRange[1], s.lastRange[2], s.lastRange[3],
           s.lastRange[4], s.lastRange[5], s.lastRange[6], s.lastRange[7], s.lastRange[8], s.lastRange[9]);
    printf("D7DRAW last_draw_error=%lu color=%lx alpha=%lx blend=%lx alphatest=%lx depth=%lx texture=%lu "
           "sampler=%lx zbuffer=%lu\n", s.lastDrawError, s.lastDrawState[0], s.lastDrawState[1],
           s.lastDrawState[2], s.lastDrawState[3], s.lastDrawState[4], s.lastDrawState[5],
           s.lastDrawState[6], s.lastDrawState[7]);
    for (i = 0; i < sizeof(s.states) / sizeof(s.states[0]) && s.states[i].calls; ++i)
        printf("D7STATE %lu calls=%lu primitives=%lu color=%lx alpha=%lx blend=%lx alphatest=%lx depth=%lx "
               "cull=%lu shade=%lu fvf=%lx texformat=%lu transform=%lu\n", i, s.states[i].calls, s.states[i].primitives,
               s.states[i].colorStage, s.states[i].alphaStage, s.states[i].blend, s.states[i].alphaTest,
               s.states[i].depthState, s.states[i].cull, s.states[i].shade, s.states[i].fvf, s.states[i].texFormat,
               s.states[i].transform);
    printf("D7DRAW state_overflow=%lu past_window_calls=%lu skipped_triangles=%lu\n", s.stateOverflow, s.beyondCalls,
           s.beyondVertices);
    printf("D7XFORM types");
    for (i = 0; i < 32; ++i) if (s.transformTypes[i]) printf(" %lu=%lu", i, s.transformTypes[i]);
    putchar('\n');
    printf("D7TEXW writes=%lu checks_skipped=%lu\n", s.textureWrites, s.checksSkipped);
    {
        static const int depth[3] = { 8, 16, 32 };
        int m;
        for (m = 0; m < 3; ++m)
            printf("D7DDMODE bpp=%d info=%lu info_failed=%lu reason=%lu enable_failed=%lu\n", depth[m],
                   s.ddModes[m][0], s.ddModes[m][1], s.ddModes[m][2], s.ddModes[m][3]);
    }
    if (s.frequency > 0)
        printf("D7TIME requests=%lu dp2_us=%I64d check_us=%I64d upload_us=%I64d copy_us=%I64d draw_us=%I64d\n", s.drawRequests,
               s.dp2Ticks * 1000000 / s.frequency, s.checkTicks * 1000000 / s.frequency,
               s.uploadTicks * 1000000 / s.frequency, s.copyTicks * 1000000 / s.frequency,
               s.drawTicks * 1000000 / s.frequency);
    printf("D7REG mipmaps=%lu managed=%lu chains=%lu stops=%lu,%lu,%lu,%lu,%lu\n", s.regMipmaps, s.regManaged,
           s.mipChains, s.mipStops[0], s.mipStops[1], s.mipStops[2], s.mipStops[3], s.mipStops[4]);
    printf("D7SURF refused=%lu caps=%08lx flags=%08lx size=%lux%lu format=%08lx count=%lu\n", s.surfaceRefused, s.refusedCaps,
           s.refusedFlags, s.refusedWidth, s.refusedHeight, s.refusedFormat, s.refusedCount);
    printf("D8INFO version=%lx buffers=%lu stream_draws=%lu requests", s.d8Version, s.d8Buffers, s.d8StreamDraws);
    for (i = 0; i < 32; ++i) if (s.d8Requests[i]) printf(" %lu=%lu", i, s.d8Requests[i]);
    printf(" contexts");
    for (i = 0; i < 8; ++i) if (s.d8ContextKinds[i]) printf(" %lu=%lu", i, s.d8ContextKinds[i]);
    printf("\n");
    if (s.d8LogCount) {
        printf("D8GDI2");
        for (i = 0; i < s.d8LogCount && i < 24; ++i) printf(" %lx/%lu", s.d8Log[i][0], s.d8Log[i][1]);
        printf("\n");
    }
    for (i = 0; i < s.d8TraceCount && i < 64; ++i)
        printf("D8TRACE %lu op=%lu n=%lu flags=%lx w=%lx,%lx,%lx,%lx\n", i, s.d8Trace[i][0], s.d8Trace[i][1],
               s.d8Trace[i][2], s.d8Trace[i][3], s.d8Trace[i][4], s.d8Trace[i][5], s.d8Trace[i][6]);
    if (s.historyStop) {
        ULONG n = s.historyNext < 32 ? s.historyNext : 32, k;
        for (k = 0; k < n; ++k) {
            const ULONG *h = s.history[(s.historyNext - n + k) % 32];
            printf("D7HIST %lu flags=%lx voffset=%lu vlength=%lu surface=%08lx vbuffer=%08lx linear=%lu "
                   "req=%lu min_start=%ld max_end=%lu hr=%08lx\n", s.historyNext - n + k, h[0], h[1], h[2],
                   h[3], h[4], h[5], h[6], (LONG)h[7], h[8], h[9]);
        }
    }
    {
        /* Miniport time per operation and the scene flight, totals since boot. */
        static const char *const names[RB2K_D7_OPS] = { "clear", "draw", "read", "upload", "flush", "present" };
        RB2K_D7_PROFILE p;
        RB2K_D7_FLIGHT f;
        if (ExtEscape(dc, RB2K_D3D7_PROFILE_ESCAPE, 0, NULL, sizeof(p), (LPSTR)&p) > 0 && p.frequency > 0)
            for (i = 0; i < RB2K_D7_OPS; ++i)
                printf("D7PROFILE op=%s calls=%lu submits=%lu bytes=%lu failed=%lu op_us=%I64d queue_us=%I64d "
                       "fence_us=%I64d\n", names[i], p.op[i].calls, p.op[i].submits, p.op[i].bytes, p.op[i].failed,
                       p.op[i].ticks * 1000000 / p.frequency, p.op[i].queueTicks * 1000000 / p.frequency,
                       p.op[i].fenceTicks * 1000000 / p.frequency);
        if (ExtEscape(dc, RB2K_D3D7_FLIGHT_ESCAPE, 0, NULL, sizeof(f), (LPSTR)&f) > 0 && f.frequency > 0)
            printf("D7FLIGHT queued=%lu completed=%lu peak=%lu throttle_waits=%lu throttle_us=%I64d "
                   "access_waits=%lu access_us=%I64d\n", f.queued, f.completed, f.peak, f.throttleWaits,
                   f.throttleTicks * 1000000 / f.frequency, f.accessWaits, f.accessTicks * 1000000 / f.frequency);
    }
    DeleteDC(dc);
    return 0;
}
