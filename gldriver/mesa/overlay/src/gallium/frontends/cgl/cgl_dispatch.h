/*
 * The CGL frontend's dispatch side (moose-ripple, M4). Each CGL context on our renderer has
 * its public dispatch table (`GLIFunctionDispatch`, 764 entries) filled with thunks generated
 * by gen_dispatch.py; libGL passes the context's `rend` pointer first, which is our
 * struct ripple_cgl_context. A thunk makes that context current for Mesa if it isn't (another
 * CGL context was current on this thread before) and calls Mesa's dispatch.
 */
#ifndef CGL_DISPATCH_H
#define CGL_DISPATCH_H

#include "util/glheader.h"
#include "util/macros.h"
#include "glapi/glapi.h"
#include "main/dispatch.h"

struct gl_context;

/* The start of struct ripple_cgl_context (cgl_context.h), all a thunk needs. */
struct ripple_cgl_rend {
   struct gl_context *glctx;
};

void ripple_cgl_make_current_slow(void *rend);
void ripple_cgl_unsupported(const char *name);

#define RIPPLE_CGL_ENTER(rend)                                                        \
   do {                                                                               \
      if (unlikely(_glapi_get_context() != ((struct ripple_cgl_rend *)(rend))->glctx)) \
         ripple_cgl_make_current_slow(rend);                                          \
   } while (0)

/* Generated: the number of entries and the filler for a GLIFunctionDispatch table. */
extern const unsigned ripple_cgl_dispatch_count;
void ripple_cgl_fill_dispatch(void **table);

#endif
