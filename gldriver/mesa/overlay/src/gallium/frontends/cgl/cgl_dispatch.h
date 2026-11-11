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
/* Returns whether the call was logged; the data loggers below follow a logged call. */
int ripple_cgl_trace(unsigned index, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ripple_cgl_trace_text(const char *what, const void *text, int len);
void ripple_cgl_trace_pixels(GLsizei w, GLsizei h, GLenum format, GLenum type,
                             const void *pixels);
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
/* Client storage, emulated (cgl_apple.c). */
void GLAPIENTRY ripple_cgl_tex_image2D(void *rend, GLenum target, GLint level,
                                       GLenum internalformat, GLsizei width, GLsizei height,
                                       GLint border, GLenum format, GLenum type,
                                       const GLvoid *pixels);
void GLAPIENTRY ripple_cgl_tex_sub_image2D(void *rend, GLenum target, GLint level,
                                           GLint xoffset, GLint yoffset, GLsizei width,
                                           GLsizei height, GLenum format, GLenum type,
                                           const GLvoid *pixels);
void GLAPIENTRY ripple_cgl_bind_texture(void *rend, GLenum target, GLuint texture);
void GLAPIENTRY ripple_cgl_delete_textures(void *rend, GLsizei n, const GLuint *textures);
void GLAPIENTRY ripple_cgl_flush(void *rend);
void ripple_cgl_client_storage_frame(void);
/* GL_APPLE_float_pixels added to the extension string (cgl_apple.c). */
const GLubyte *GLAPIENTRY ripple_cgl_get_string(void *rend, GLenum name);
void GLAPIENTRY ripple_cgl_get_programiv(void *rend, GLenum target, GLenum pname, GLint *params);
/* GL_APPLE_object_purgeable, answered without Mesa (cgl_apple.c). */
GLenum GLAPIENTRY ripple_cgl_object_purgeable(void *rend, GLenum objectType, GLuint name, GLenum option);
GLenum GLAPIENTRY ripple_cgl_object_unpurgeable(void *rend, GLenum objectType, GLuint name, GLenum option);
void GLAPIENTRY ripple_cgl_get_object_parameteriv(void *rend, GLenum objectType, GLuint name,
                                                  GLenum pname, GLint *params);
void GLAPIENTRY ripple_cgl_tex_parameteri(void *rend, GLenum target, GLenum pname, GLint param);
void GLAPIENTRY ripple_cgl_tex_parameterf(void *rend, GLenum target, GLenum pname, GLfloat param);
void GLAPIENTRY ripple_cgl_tex_parameteriv(void *rend, GLenum target, GLenum pname, const GLint *params);
void GLAPIENTRY ripple_cgl_tex_parameterfv(void *rend, GLenum target, GLenum pname, const GLfloat *params);
void GLAPIENTRY ripple_cgl_hint(void *rend, GLenum target, GLenum mode);
void GLAPIENTRY ripple_cgl_viewport(void *rend, GLint x, GLint y, GLsizei w, GLsizei h);
void GLAPIENTRY ripple_cgl_bind_framebuffer(void *rend, GLenum target, GLuint framebuffer);
void GLAPIENTRY ripple_cgl_push_attrib(void *rend, GLbitfield mask);
void GLAPIENTRY ripple_cgl_pop_attrib(void *rend);
void GLAPIENTRY ripple_cgl_scissor(void *rend, GLint x, GLint y, GLsizei w, GLsizei h);
void GLAPIENTRY ripple_cgl_clear(void *rend, GLbitfield mask);
void GLAPIENTRY ripple_cgl_get_integerv(void *rend, GLenum pname, GLint *params);
void GLAPIENTRY ripple_cgl_read_pixels(void *rend, GLint x, GLint y, GLsizei w, GLsizei h,
                                       GLenum format, GLenum type, GLvoid *pixels);
void GLAPIENTRY ripple_cgl_copy_pixels(void *rend, GLint x, GLint y, GLsizei w, GLsizei h,
                                       GLenum type);
void GLAPIENTRY ripple_cgl_copy_tex_image1D(void *rend, GLenum target, GLint level, GLenum ifmt,
                                            GLint x, GLint y, GLsizei w, GLint border);
void GLAPIENTRY ripple_cgl_copy_tex_image2D(void *rend, GLenum target, GLint level, GLenum ifmt,
                                            GLint x, GLint y, GLsizei w, GLsizei h, GLint border);
void GLAPIENTRY ripple_cgl_copy_tex_sub_image1D(void *rend, GLenum target, GLint level,
                                                GLint xoff, GLint x, GLint y, GLsizei w);
void GLAPIENTRY ripple_cgl_copy_tex_sub_image2D(void *rend, GLenum target, GLint level,
                                                GLint xoff, GLint yoff, GLint x, GLint y,
                                                GLsizei w, GLsizei h);
void GLAPIENTRY ripple_cgl_copy_tex_sub_image3D(void *rend, GLenum target, GLint level,
                                                GLint xoff, GLint yoff, GLint zoff, GLint x,
                                                GLint y, GLsizei w, GLsizei h);
struct ripple_cgl_context;
void ripple_cgl_refresh_window_offset(struct ripple_cgl_context *ctx);
/* GLI parameters 997-999 (a surface as the bound texture's image), cgl_apple.c. */
int ripple_cgl_surface_texture(struct ripple_cgl_context *ctx, uint32_t sid, GLenum target,
                               GLenum ifmt, GLsizei w, GLsizei h);

/* Generated: the number of entries and the filler for a GLIFunctionDispatch table. */
extern const unsigned ripple_cgl_dispatch_count;
void ripple_cgl_fill_dispatch(void **table);

#endif
