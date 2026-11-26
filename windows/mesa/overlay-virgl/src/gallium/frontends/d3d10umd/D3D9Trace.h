/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 the moose-ripple authors
 *
 * See D3D9Trace.cpp.
 */

#pragma once

#include "D3D9Private.h"

/* RIPPLE_UMD_TRACE: 0 none, 1 calls that fail, 2 every call, 3 with what each was about */
int
D3D9TraceLevel(void);

void
D3D9TraceAdapterFuncs(D3DDDI_ADAPTERFUNCS *funcs);

void
D3D9TraceDeviceFuncs(D3DDDI_DEVICEFUNCS *funcs);

/* RIPPLE_UMD_TIMES (any value, with RIPPLE_UMD_LOG): where a frame's time goes, written to the
 * driver log every 256 presents. For each entry point of the drawing thread: the time inside
 * it, and the time that passes outside the driver after it returns. The kernel callbacks that
 * can wait (lock, unlock, render, present, escape) and the two that make and free an allocation
 * are timed too, and so are each way a copy between two surfaces is done, the three steps of an
 * upload and the steps by which Unlock sends a buffer to the host. The first line also says how
 * far Windows' tick clock moved against the performance counter. Under 100% the guest loses
 * timer ticks, and programs that time themselves by that clock run slow. */
enum D3D9TimedCallback {
   D3D9_TIMED_LOCK,
   D3D9_TIMED_UNLOCK,
   D3D9_TIMED_RENDER,
   D3D9_TIMED_PRESENT,
   D3D9_TIMED_ESCAPE,
   D3D9_TIMED_ALLOCATE,
   D3D9_TIMED_DEALLOCATE,
   /* the ways one surface is copied to another (Blt and its relatives) */
   D3D9_TIMED_COPY_READBACK,
   D3D9_TIMED_COPY_UPLOAD,
   D3D9_TIMED_COPY_CPU_BLIT,
   D3D9_TIMED_COPY_MEMORY,
   D3D9_TIMED_COPY_SKIPPED,
   D3D9_TIMED_COPY_CONVERT,
   D3D9_TIMED_COPY_SCALED,
   D3D9_TIMED_COPY_REGION,
   /* the steps of writing memory to a texture */
   D3D9_TIMED_UPLOAD_MAP,
   D3D9_TIMED_UPLOAD_COPY,
   D3D9_TIMED_UPLOAD_UNMAP,
   /* the steps of Unlock for a buffer: one kept in a shadow (the pages written since the last
    * upload are found, mapped, copied and unmapped, and the watch on its pages is set again),
    * and one that was mapped directly */
   D3D9_TIMED_BUFFER_PAGES,
   D3D9_TIMED_BUFFER_MAP,
   D3D9_TIMED_BUFFER_COPY,
   D3D9_TIMED_BUFFER_UNMAP,
   D3D9_TIMED_BUFFER_REWATCH,
   D3D9_TIMED_BUFFER_DIRECT_UNMAP,
   D3D9_TIMED_TEXTURE_DIRECT_UNMAP,
   D3D9_TIMED_CALLBACKS
};

bool
D3D9TimesOn(void);

unsigned long long
D3D9TimesNow(void);

void
D3D9TimesCallback(enum D3D9TimedCallback which, unsigned long long start);
