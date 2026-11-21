/* The RIPPLEVD interface on Windows 2000: the calls Mesa's virgl winsys makes
 * to Windows 98's ripplevd.vxd (win9x/minivdd/rvd.h), reached here through
 * the display DLL. Mesa sends ExtEscape(RB2K_RVD_ESCAPE) with the RVD code
 * in the first ULONG of the input and that call's input after it; the output
 * is the call's output. The display DLL keeps buffer memory in the calling
 * process and copies through the miniport's staging around each request. */
#ifndef RB2K_RVD_H
#define RB2K_RVD_H

#include "rvd.h"

#define RB2K_RVD_ESCAPE 0x52420010
#define IOCTL_RB2K_RVD CTL_CODE(FILE_DEVICE_VIDEO, 0x910, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Display DLL to miniport. */
enum {
    RB2K_RVD_OP_STAGING = 1,        /* out: RB2K_RVD_STAGING_INFO */
    RB2K_RVD_OP_INFO,               /* out: struct rvd_info (display size 0) */
    RB2K_RVD_OP_CAPS,               /* a: id, b: version, bytes: size; out: the caps */
    RB2K_RVD_OP_CONTEXT_CREATE,     /* u.name, bytes: its length; out: ULONG context */
    RB2K_RVD_OP_CONTEXT_DESTROY,    /* context */
    RB2K_RVD_OP_BUFFER_CREATE,      /* u.create; out: ULONG handle */
    RB2K_RVD_OP_BUFFER_DESTROY,     /* context, a: handle */
    RB2K_RVD_OP_SUBMIT,             /* context, bytes: commands at staging offset 0 */
    RB2K_RVD_OP_TRANSFER,           /* u.transfer */
    RB2K_RVD_OP_HELD                /* out: ULONG[3] contexts, buffers, pages held now */
};

#define RB2K_RVD_STAGING_SEGMENTS 48
#define RB2K_RVD_SEGMENT_BYTES 65536UL
#define RB2K_RVD_STAGING_BYTES (RB2K_RVD_STAGING_SEGMENTS * RB2K_RVD_SEGMENT_BYTES)
/* Mesa's command buffer holds 65536 + 1024 words. */
#define RB2K_RVD_MAX_SUBMIT (6UL * RB2K_RVD_SEGMENT_BYTES)
#define RB2K_RVD_MAX_BUFFER (32UL << 20)
#define RB2K_RVD_MAX_NAME 64

typedef struct {
    ULONG op, context, a, b, bytes;
    union {
        char name[RB2K_RVD_MAX_NAME];
        struct rvd_buffer_create create;
        struct rvd_transfer transfer;
    } u;
} RB2K_RVD_REQUEST;

/* Kernel addresses of the staging segments, valid while the driver runs. */
typedef struct {
    ULONG count, segmentBytes;
    PUCHAR data[RB2K_RVD_STAGING_SEGMENTS];
} RB2K_RVD_STAGING_INFO;

/* ExtEscape(RB2K_RVD_STATS_ESCAPE), no input: the time spent in each RVD call
 * code since the driver started, kernel and host included, for profiling. */
#define RB2K_RVD_STATS_ESCAPE 0x52420011
#define RB2K_RVD_STAT_CODES 16
typedef struct {
    LONGLONG frequency;                  /* ticks a second */
    ULONG calls[RB2K_RVD_STAT_CODES];
    LONGLONG ticks[RB2K_RVD_STAT_CODES];
    ULONG contexts, buffers, pages;     /* held for programs now, after a sweep */
    ULONG failures[RB2K_RVD_STAT_CODES];
} RB2K_RVD_STATS;

#endif
