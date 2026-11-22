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
enum { RB2K_D7_CLEAR, RB2K_D7_DRAW, RB2K_D7_READ, RB2K_D7_UPLOAD, RB2K_D7_FLUSH, RB2K_D7_OPS };
typedef struct {
    LONGLONG ticks, queueTicks, fenceTicks;
    ULONG calls, submits, fences, bytes, failed;
} RB2K_D7_TIME;
typedef struct {
    LONGLONG frequency;
    RB2K_D7_TIME op[RB2K_D7_OPS];
} RB2K_D7_PROFILE;
#define RB2K_D3D7_QUERY_LIMIT 24
typedef struct {
    RB2K_D3D7_STATS totals;
    ULONG surfaceCalls, contextCalls, clearCalls, commandCalls;
    ULONG lastCommand, lastError, errorOffset;
    ULONG halInfoCalls, enableCalls, nativeEnabled, callbackQueries, surfaceQueries;
    ULONG queryCount, queryIds[RB2K_D3D7_QUERY_LIMIT];
} RB2K_D3D7_DIAGNOSTICS;

#endif
