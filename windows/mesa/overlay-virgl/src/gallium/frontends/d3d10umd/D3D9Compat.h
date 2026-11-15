/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026 the moose-ripple authors
 *
 * The Direct3D 9 files were written against Mesa 26.1. These are the helpers of that version
 * they use which Mesa 24.3 does not have, written with what 24.3 has.
 */

#pragma once

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"

/* Give up one reference */
static inline void
pipe_resource_release(struct pipe_context *pipe, struct pipe_resource *resource)
{
   (void)pipe;
   pipe_resource_reference(&resource, NULL);
}

static inline void
pipe_sampler_view_release(struct pipe_sampler_view *view)
{
   pipe_sampler_view_reference(&view, NULL);
}

/* In 24.3 u_upload_data itself hands out a reference to the buffer */
#define u_upload_data_ref u_upload_data

#ifdef __cplusplus
/* The 26.1 form: the buffer in *outbuf without a reference, and the one reference the caller
 * has to give up later in *releasebuf. */
static inline void
u_upload_data(struct u_upload_mgr *upload, unsigned min_out_offset, unsigned size,
              unsigned alignment, const void *data, unsigned *out_offset,
              struct pipe_resource **outbuf, struct pipe_resource **releasebuf)
{
   *outbuf = NULL;
   u_upload_data(upload, min_out_offset, size, alignment, data, out_offset, outbuf);
   *releasebuf = *outbuf;
}
#endif
