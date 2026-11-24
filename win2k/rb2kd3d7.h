#ifndef RB2K_D3D7_H
#define RB2K_D3D7_H

/* Read-only display escape for distinguishing native draws from the RGB
 * software device. Counts completed host operations and live D3D objects. */
#define RB2K_D3D7_STATS_ESCAPE 0x52420020
typedef struct {
    ULONG version, contexts, targets, clears, triangles;
} RB2K_D3D7_STATS;
#define RB2K_D3D7_STATS_VERSION 1
#define RB2K_D3D7_PROFILE_ESCAPE 0x52420021
/* A present's fenceTicks is its readback from the host; the rest is the copy
 * to the screen. */
enum { RB2K_D7_CLEAR, RB2K_D7_DRAW, RB2K_D7_READ, RB2K_D7_UPLOAD, RB2K_D7_FLUSH, RB2K_D7_PRESENT, RB2K_D7_OPS };
typedef struct {
    LONGLONG ticks, queueTicks, fenceTicks;
    ULONG calls, submits, fences, bytes, failed;
} RB2K_D7_TIME;
typedef struct {
    LONGLONG frequency;
    RB2K_D7_TIME op[RB2K_D7_OPS];
} RB2K_D7_PROFILE;
#define RB2K_D3D7_FLIGHT_ESCAPE 0x52420022
typedef struct {
    LONGLONG frequency, throttleTicks, accessTicks;
    ULONG limit, peak, pending, queued, completed, throttleWaits, accessWaits, failed;
} RB2K_D7_FLIGHT;
/* Read-only counts of what the native path drew or set aside, in triangles
 * unless named otherwise. A skipped draw is consumed and leaves the target
 * unchanged; the reason is the first unsupported feature found. */
#define RB2K_D3D7_DRAW_ESCAPE 0x52420023
enum { RB2K_D7_SKIP_FILL, RB2K_D7_SKIP_STENCIL, RB2K_D7_SKIP_STAGE1, RB2K_D7_SKIP_VERTEX,
       RB2K_D7_SKIP_TEXTURE, RB2K_D7_SKIP_COMBINER, RB2K_D7_SKIP_PRIMITIVE, RB2K_D7_SKIP_CLEAR,
       RB2K_D7_SKIPS };
enum { RB2K_D7_IGNORED_FOG, RB2K_D7_IGNORED_COLORKEY, RB2K_D7_IGNORED_CLIPPLANE,
       RB2K_D7_IGNORED_MIPMAP, RB2K_D7_IGNORED_LIGHTING, RB2K_D7_IGNORED };
#define RB2K_D7_OPCODES 48
typedef struct {
    ULONG version, drawn, drawCalls, skippedCalls;
    ULONG skipped[RB2K_D7_SKIPS], ignored[RB2K_D7_IGNORED];
    ULONG textures, uploads, uploadBytes, checks, uploadFailures, checkFailures;
    ULONG opcodes[RB2K_D7_OPCODES];
    ULONG registered[8];        /* texture registrations by decoded format, zero unsupported */
    ULONG videoTextures, texBlts, texBltFailures, drawFailures;
    ULONG dp2Calls, dp2Failures, lastFailure, lastFailureOp;
    /* Diagnostics: why vertex data was refused, the last refused range,
     * the last host draw error and the distinct drawn state combinations. */
    ULONG badPositions, failSites[8];
    ULONG lastRange[10];        /* op, first, low, high, available, offset, linear, flags, stride, count */
    ULONG lastDrawError, lastDrawState[8];
    ULONG stateOverflow, beyondCalls, beyondVertices;   /* draws past the vertex window, triangles skipped */
    /* The last 32 command buffers: flags, vertex offset, length,
     * kernel surface, its address and size, requested size, lowest start,
     * highest start plus index, result. Recording stops 16 buffers after the
     * first draw that indexes past the stated window. */
    ULONG transformTypes[32];   /* SETTRANSFORM types received; 31 counts any other */
    /* Texture surfaces refused at CanCreateSurface or CreateSurface, with
     * the last one's caps, description flags, size, pixel format (flags << 8
     * | bits) and surface count; mipmapped and managed textures registered. */
    ULONG surfaceRefused, refusedCaps, refusedFlags, refusedWidth, refusedHeight, refusedFormat, refusedCount;
    ULONG regMipmaps, regManaged;
    ULONG lightsBeyond;   /* SETLIGHT records for an index past the 32 kept */
    /* Mipmapped textures registered with a full chain, and why a chain stopped
     * early: no attach list, no attached surface, not a mipmap, size, memory. */
    ULONG mipChains, mipStops[5];
    /* Display-driver time: whole DrawPrimitives2 calls, and within them the
     * once-a-scene texture reads and checksums and the texture uploads. */
    LONGLONG frequency, dp2Ticks, checkTicks, uploadTicks;
    LONGLONG copyTicks, drawTicks;   /* the command-buffer copy; draw requests to the miniport */
    ULONG drawRequests;              /* IOCTL_RB2K_D3D7_DRAWS requests carrying the draws */
    ULONG textureWrites, checksSkipped;   /* texture locks and blits seen; checks they saved */
    /* DirectDraw setup per desktop depth (8, 16, 32 bits): DrvGetDirectDrawInfo
     * calls, its failures, the last failure's reason (1 no lock, 2 no mapping,
     * 3 capability query failed), and DrvEnableDirectDraw failures. */
    ULONG ddModes[3][4];
    ULONG historyStop, historyNext;
    ULONG history[32][10];
    struct {
        ULONG colorStage, alphaStage, blend, alphaTest, depthState, cull, shade, fvf, texFormat, transform;
        ULONG calls, primitives;
    } states[64];
} RB2K_D7_DRAWSTATS;
enum { RB2K_D7_FAIL_RANGE, RB2K_D7_FAIL_VBCAPS, RB2K_D7_FAIL_LINEAR, RB2K_D7_FAIL_ADDRESS,
       RB2K_D7_FAIL_PROBE, RB2K_D7_FAIL_IMMEDIATE, RB2K_D7_FAIL_LENGTH, RB2K_D7_FAIL_OTHER };
#define RB2K_D7_DRAWSTATS_VERSION 1
#define RB2K_D3D7_QUERY_LIMIT 24
typedef struct {
    RB2K_D3D7_STATS totals;
    ULONG surfaceCalls, contextCalls, clearCalls, commandCalls;
    ULONG lastCommand, lastError, errorOffset;
    ULONG halInfoCalls, enableCalls, nativeEnabled, callbackQueries, surfaceQueries;
    ULONG queryCount, queryIds[RB2K_D3D7_QUERY_LIMIT];
} RB2K_D3D7_DIAGNOSTICS;

#endif
