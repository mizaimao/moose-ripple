/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 the moose-ripple authors
 *
 * The user-mode driver's own log: what it refused and what it only approximates. A line goes
 * to the debugger's output, and to the file the environment variable RIPPLE_UMD_LOG names in
 * the process that loaded the driver, when it is set.
 */

#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void
ripple_logf(const char *format, ...);

void
ripple_vlogf(const char *prefix, const char *format, va_list args);

#ifdef __cplusplus
}
#endif
