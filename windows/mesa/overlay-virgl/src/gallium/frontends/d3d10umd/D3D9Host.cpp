/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 the moose-ripple authors
 *
 * The answers to D3D9Host.h for virgl.
 */

#include "D3D9Host.h"
#include "RippleLog.h"

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/u_debug.h"

#include <stdarg.h>
#include <stdio.h>

#include <windows.h>

extern "C" {

void
yttrium_gdi_flush_labeled(struct pipe_context *ctx, struct pipe_fence_handle **fence,
                          unsigned flags, const char *label)
{
   (void)label;

   if (ctx && ctx->flush)
      ctx->flush(ctx, fence, flags);
}

bool
yttrium_gdi_flush_handoff(struct pipe_context *ctx, const char *label)
{
   yttrium_gdi_flush_labeled(ctx, NULL, 0, label);
   return true;
}

bool
yttrium_gdi_take_draw_failure(struct pipe_context *ctx)
{
   (void)ctx;
   return false;
}

void
yttrium_gdi_resource_set_allocation_ownership(struct pipe_resource *resource, bool owned)
{
   (void)resource;
   (void)owned;
}

/*
 * Asked when the runtime destroys a resource: may its allocation belong to the runtime's
 * resource? Then it has to be given back before the call returns. The ones tied to a runtime
 * resource are primaries and shared resources (D3D9CreateResource), and those carry the
 * shared binding; so do some others, for which the flush below is only early.
 */
bool
yttrium_gdi_resource_has_runtime_allocation(struct pipe_resource *resource)
{
   return resource && (resource->bind & PIPE_BIND_SHARED) != 0;
}

/*
 * Called for such a resource: let go of everything that still holds it. On virgl that is the
 * commands not yet sent to the host; once they are sent, dropping the last reference frees the
 * allocation at once (the winsys keeps no shared resource for reuse).
 */
unsigned
yttrium_gdi_pipeline_invalidate_resource(struct pipe_context *ctx,
                                         struct pipe_resource *resource)
{
   (void)resource;

   if (ctx && ctx->flush)
      ctx->flush(ctx, NULL, 0);
   return 0;
}

bool
yttrium_gdi_debug_get_bool_option(const char *name, bool dfault)
{
   return debug_get_bool_option(name, dfault);
}

void
yttrium_gdi_trace_warnf(const char *format, ...)
{
   va_list args;

   va_start(args, format);
   ripple_vlogf("", format, args);
   va_end(args);
}

void
yttrium_gdi_user_logf(const char *format, ...)
{
   /* The same text follows through yttrium_gdi_trace_warnf wherever this is called */
   (void)format;
}

}
