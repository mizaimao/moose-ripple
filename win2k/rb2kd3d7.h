#ifndef RB2K_D3D7_H
#define RB2K_D3D7_H

/* Read-only display escape for distinguishing native draws from the RGB
 * software device. Counts completed host operations and live D3D objects. */
#define RB2K_D3D7_STATS_ESCAPE 0x52420020
typedef struct {
    ULONG version, contexts, targets, clears, triangles;
} RB2K_D3D7_STATS;
#define RB2K_D3D7_STATS_VERSION 1
#define RB2K_D3D7_QUERY_LIMIT 24
typedef struct {
    RB2K_D3D7_STATS totals;
    ULONG surfaceCalls, contextCalls, clearCalls, commandCalls;
    ULONG lastCommand, lastError, errorOffset;
    ULONG halInfoCalls, enableCalls, nativeEnabled, callbackQueries, surfaceQueries;
    ULONG queryCount, queryIds[RB2K_D3D7_QUERY_LIMIT];
} RB2K_D3D7_DIAGNOSTICS;

#endif
