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

/* Debug trace of GL calls (cgl_apple.c): on when /Library/Logs/RippleGL/trace exists at load. */
extern int ripple_cgl_tracing;
void ripple_cgl_trace(unsigned index, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ripple_cgl_unsupported(const char *name);

#define RIPPLE_CGL_ENTER(rend)                                                        \
   do {                                                                               \
      if (unlikely(_glapi_get_context() != ((struct ripple_cgl_rend *)(rend))->glctx)) \
         ripple_cgl_make_current_slow(rend);                                          \
   } while (0)

/* Apple-only entries the frontend implements (cgl_apple.c); gen_dispatch.py puts them in
 * the table. */
void GLAPIENTRY ripple_cgl_gen_fences(void *rend, GLsizei n, GLuint *fences);
void GLAPIENTRY ripple_cgl_delete_fences(void *rend, GLsizei n, const GLuint *fences);
void GLAPIENTRY ripple_cgl_set_fence(void *rend, GLuint fence);
GLboolean GLAPIENTRY ripple_cgl_is_fence(void *rend, GLuint fence);
GLboolean GLAPIENTRY ripple_cgl_test_fence(void *rend, GLuint fence);
void GLAPIENTRY ripple_cgl_finish_fence(void *rend, GLuint fence);
GLboolean GLAPIENTRY ripple_cgl_test_object(void *rend, GLenum object, GLuint name);
void GLAPIENTRY ripple_cgl_finish_object(void *rend, GLenum object, GLuint name);
void GLAPIENTRY ripple_cgl_swap(void *rend);
void GLAPIENTRY ripple_cgl_pixel_storei(void *rend, GLenum pname, GLint param);
void GLAPIENTRY ripple_cgl_pixel_storef(void *rend, GLenum pname, GLfloat param);
void GLAPIENTRY ripple_cgl_tex_parameteri(void *rend, GLenum target, GLenum pname, GLint param);
void GLAPIENTRY ripple_cgl_tex_parameterf(void *rend, GLenum target, GLenum pname, GLfloat param);
void GLAPIENTRY ripple_cgl_tex_parameteriv(void *rend, GLenum target, GLenum pname, const GLint *params);
void GLAPIENTRY ripple_cgl_tex_parameterfv(void *rend, GLenum target, GLenum pname, const GLfloat *params);
void GLAPIENTRY ripple_cgl_hint(void *rend, GLenum target, GLenum mode);

/* Generated: the number of entries and the filler for a GLIFunctionDispatch table. */
extern const unsigned ripple_cgl_dispatch_count;
void ripple_cgl_fill_dispatch(void **table);

#endif
