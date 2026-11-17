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
