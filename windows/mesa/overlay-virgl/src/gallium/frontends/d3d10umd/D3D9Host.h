/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 the moose-ripple authors
 *
 * What the Direct3D 9 files ask of the renderer underneath, under the names they use. The
 * files come from the Yttrium project's Mesa fork, where that project's own renderer answers
 * these. Here the renderer is virgl: a flush is a flush, and the questions about that
 * renderer's allocations and pipeline cache have nothing to answer (D3D9Host.cpp).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct pipe_context;
struct pipe_fence_handle;
struct pipe_resource;

/* Resource flags only that renderer knows: nothing to ask of virgl */
#define YTTRIUM_GDI_RESOURCE_FLAG_LOCAL_RENDER 0u
#define YTTRIUM_GDI_RESOURCE_FLAG_CPU_READBACK 0u

#ifdef __cplusplus
extern "C" {
#endif

void
yttrium_gdi_flush_labeled(struct pipe_context *ctx, struct pipe_fence_handle **fence,
                          unsigned flags, const char *label);

/* Flush and hand the frame over; false when the device is lost */
bool
yttrium_gdi_flush_handoff(struct pipe_context *ctx, const char *label);

/* Whether a draw since the last call failed in the renderer after it was accepted */
bool
yttrium_gdi_take_draw_failure(struct pipe_context *ctx);

void
yttrium_gdi_resource_set_allocation_ownership(struct pipe_resource *resource, bool owned);

bool
yttrium_gdi_resource_has_runtime_allocation(struct pipe_resource *resource);

unsigned
yttrium_gdi_pipeline_invalidate_resource(struct pipe_context *ctx,
                                         struct pipe_resource *resource);

bool
yttrium_gdi_debug_get_bool_option(const char *name, bool dfault);

void
yttrium_gdi_trace_warnf(const char *format, ...);

void
yttrium_gdi_user_logf(const char *format, ...);

#ifdef __cplusplus
}
#endif
