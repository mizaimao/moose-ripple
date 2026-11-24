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
 * can wait (lock, unlock, render, present, escape) are timed too. The first line also says how
 * far Windows' tick clock moved against the performance counter. Under 100% the guest loses
 * timer ticks, and programs that time themselves by that clock run slow. */
enum D3D9TimedCallback {
   D3D9_TIMED_LOCK,
   D3D9_TIMED_UNLOCK,
   D3D9_TIMED_RENDER,
   D3D9_TIMED_PRESENT,
   D3D9_TIMED_ESCAPE,
   D3D9_TIMED_CALLBACKS
};

bool
D3D9TimesOn(void);

unsigned long long
D3D9TimesNow(void);

void
D3D9TimesCallback(enum D3D9TimedCallback which, unsigned long long start);
