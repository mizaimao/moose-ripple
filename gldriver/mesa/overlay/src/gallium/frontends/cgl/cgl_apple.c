/*
 * Apple-only dispatch entries the CGL frontend implements itself (moose-ripple, M4), and the
 * report for entries nothing implements. GL_APPLE_fence is done with Flush/Finish: correct,
 * if slower than real fences.
 */
#include <stdio.h>
#include <string.h>

#include "util/simple_mtx.h"
#include "util/u_debug.h"

#include "cgl_context.h"
#include "cgl_dispatch.h"
#include "cgl_drawable.h"

void
ripple_cgl_unsupported(const char *name)
{
   /* Report each unsupported entry once per process. */
   static simple_mtx_t lock = SIMPLE_MTX_INITIALIZER;
   static const char *seen[32];
   static unsigned nseen;

   simple_mtx_lock(&lock);
   for (unsigned i = 0; i < nseen; i++) {
      if (seen[i] == name) {
         simple_mtx_unlock(&lock);
         return;
      }
   }
   if (nseen < 32)
      seen[nseen++] = name;
   simple_mtx_unlock(&lock);
   fprintf(stderr, "RippleGLDriver: unsupported GL call %s (ignored)\n", name);
}

void GLAPIENTRY
ripple_cgl_gen_fences(void *rend, GLsizei n, GLuint *fences)
{
   struct ripple_cgl_context *ctx = rend;

   for (GLsizei i = 0; i < n; i++)
      fences[i] = ++ctx->next_fence;
}

void GLAPIENTRY
ripple_cgl_delete_fences(void *rend, GLsizei n, const GLuint *fences)
{
}

void GLAPIENTRY
ripple_cgl_set_fence(void *rend, GLuint fence)
{
   RIPPLE_CGL_ENTER(rend);
   CALL_Flush(GET_DISPATCH(), ());
}

GLboolean GLAPIENTRY
ripple_cgl_is_fence(void *rend, GLuint fence)
{
   return fence != 0;
}

GLboolean GLAPIENTRY
ripple_cgl_test_fence(void *rend, GLuint fence)
{
   RIPPLE_CGL_ENTER(rend);
   CALL_Finish(GET_DISPATCH(), ());
   return GL_TRUE;
}

void GLAPIENTRY
ripple_cgl_finish_fence(void *rend, GLuint fence)
{
   RIPPLE_CGL_ENTER(rend);
   CALL_Finish(GET_DISPATCH(), ());
}

GLboolean GLAPIENTRY
ripple_cgl_test_object(void *rend, GLenum object, GLuint name)
{
   RIPPLE_CGL_ENTER(rend);
   CALL_Finish(GET_DISPATCH(), ());
   return GL_TRUE;
}

void GLAPIENTRY
ripple_cgl_finish_object(void *rend, GLenum object, GLuint name)
{
   RIPPLE_CGL_ENTER(rend);
   CALL_Finish(GET_DISPATCH(), ());
}

void GLAPIENTRY
ripple_cgl_swap(void *rend)
{
   /* CGLFlushDrawable on a double-buffered context: present the back buffer. */
   RIPPLE_CGL_ENTER(rend);
   ripple_cgl_context_swap(rend);
}

/* ---- Debug trace of GL calls (gen_dispatch.py emits the calls) ------------------------- *
 * On when /Library/Logs/RippleGL/trace exists when the bundle loads: the first calls of each
 * entry with their arguments, and every 10 s the call counts, into
 * /Library/Logs/RippleGL/trace-PROGRAM-PID.log. */
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

int ripple_cgl_tracing;
static FILE *trace_file;
static unsigned long trace_counts[1024];
static const char *trace_names[1024];
static time_t trace_last_dump;
static simple_mtx_t trace_lock = SIMPLE_MTX_INITIALIZER;

__attribute__((constructor)) static void
ripple_cgl_trace_init(void)
{
   if (access("/Library/Logs/RippleGL/trace", F_OK) != 0)
      return;
   char path[256];
   snprintf(path, sizeof(path), "/Library/Logs/RippleGL/trace-%s-%d.log", getprogname(),
            (int)getpid());
   trace_file = fopen(path, "a");
   if (trace_file) {
      setvbuf(trace_file, NULL, _IOLBF, 0);
      ripple_cgl_tracing = 1;
      trace_last_dump = time(NULL);
   }
}

void
ripple_cgl_trace(unsigned index, const char *fmt, ...)
{
   if (index >= 1024)
      return;
   simple_mtx_lock(&trace_lock);
   unsigned long n = ++trace_counts[index];
   if (!trace_names[index])
      trace_names[index] = fmt;
   if (n <= 6) {
      va_list ap;
      va_start(ap, fmt);
      vfprintf(trace_file, fmt, ap);
      va_end(ap);
      fputc('\n', trace_file);
   }
   time_t now = time(NULL);
   if (now - trace_last_dump >= 10) {
      trace_last_dump = now;
      fprintf(trace_file, "---- counts ----\n");
      for (unsigned i = 0; i < 1024; i++) {
         if (trace_counts[i]) {
            const char *paren = strchr(trace_names[i], '(');
            fprintf(trace_file, "%8lu %.*s\n", trace_counts[i],
                    paren ? (int)(paren - trace_names[i]) : 40, trace_names[i]);
         }
      }
   }
   simple_mtx_unlock(&trace_lock);
}

/* ---- Apple-only enum values ------------------------------------------------------------- *
 * Hints Apple's GL accepts and Mesa refuses with GL_INVALID_ENUM: client storage, texture
 * storage hints, transform hint. WindowServer sets them on every texture and checks
 * glGetError, so they are taken here as no-ops (Mesa copies texture data anyway); everything
 * else goes on to Mesa. */
#define GL_UNPACK_CLIENT_STORAGE_APPLE   0x85B2
#define GL_TEXTURE_STORAGE_HINT_APPLE    0x85BC
#define GL_TRANSFORM_HINT_APPLE          0x85B1

void GLAPIENTRY
ripple_cgl_pixel_storei(void *rend, GLenum pname, GLint param)
{
   if (pname == GL_UNPACK_CLIENT_STORAGE_APPLE)
      return;
   RIPPLE_CGL_ENTER(rend);
   CALL_PixelStorei(GET_DISPATCH(), (pname, param));
}

void GLAPIENTRY
ripple_cgl_pixel_storef(void *rend, GLenum pname, GLfloat param)
{
   if (pname == GL_UNPACK_CLIENT_STORAGE_APPLE)
      return;
   RIPPLE_CGL_ENTER(rend);
   CALL_PixelStoref(GET_DISPATCH(), (pname, param));
}

void GLAPIENTRY
ripple_cgl_tex_parameteri(void *rend, GLenum target, GLenum pname, GLint param)
{
   if (pname == GL_TEXTURE_STORAGE_HINT_APPLE)
      return;
   RIPPLE_CGL_ENTER(rend);
   CALL_TexParameteri(GET_DISPATCH(), (target, pname, param));
}

void GLAPIENTRY
ripple_cgl_tex_parameterf(void *rend, GLenum target, GLenum pname, GLfloat param)
{
   if (pname == GL_TEXTURE_STORAGE_HINT_APPLE)
      return;
   RIPPLE_CGL_ENTER(rend);
   CALL_TexParameterf(GET_DISPATCH(), (target, pname, param));
}

void GLAPIENTRY
ripple_cgl_tex_parameteriv(void *rend, GLenum target, GLenum pname, const GLint *params)
{
   if (pname == GL_TEXTURE_STORAGE_HINT_APPLE)
      return;
   RIPPLE_CGL_ENTER(rend);
   CALL_TexParameteriv(GET_DISPATCH(), (target, pname, params));
}

void GLAPIENTRY
ripple_cgl_tex_parameterfv(void *rend, GLenum target, GLenum pname, const GLfloat *params)
{
   if (pname == GL_TEXTURE_STORAGE_HINT_APPLE)
      return;
   RIPPLE_CGL_ENTER(rend);
   CALL_TexParameterfv(GET_DISPATCH(), (target, pname, params));
}

void GLAPIENTRY
ripple_cgl_hint(void *rend, GLenum target, GLenum mode)
{
   if (target == GL_TRANSFORM_HINT_APPLE)
      return;
   RIPPLE_CGL_ENTER(rend);
   CALL_Hint(GET_DISPATCH(), (target, mode));
}

/* ---- Window offset: drawing into the screen through a rectangle of it -------------------- *
 * WindowServer composites each frame into its display surface shaped to the dirty rectangle,
 * in window coordinates relative to that rectangle; the drawable is the whole screen (it
 * reads it back for thumbnails). So window coordinates are shifted by the rectangle's GL
 * bottom-left on the screen, and the viewport and scissor queries shifted back. */
#include "main/mtypes.h"
#include "cgl_device.h"
#include "virgl/ripple/ripple_gpu_uapi.h"

static struct ripple_cgl_context *
offset_ctx(void *rend)
{
   struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
   return ctx && ctx->win_surface ? ctx : NULL;
}

void
ripple_cgl_refresh_window_offset(struct ripple_cgl_context *ctx)
{
   struct ripple_gpu_surface_info si;
   if (!ctx->win_surface || !ripple_cgl_device_surface_info(ctx->win_surface, &si))
      return;
   ctx->win_ox = si.x;
   ctx->win_oy = (int)si.screen_height - si.y - (int)si.height;
   ctx->win_w = si.width;
   ctx->win_h = si.height;
}

void GLAPIENTRY
ripple_cgl_viewport(void *rend, GLint x, GLint y, GLsizei w, GLsizei h)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx) {
      ripple_cgl_refresh_window_offset(ctx);    /* a frame starts: the rectangle may be new */
      x += ctx->win_ox;
      y += ctx->win_oy;
   }
   CALL_Viewport(GET_DISPATCH(), (x, y, w, h));
}

void GLAPIENTRY
ripple_cgl_scissor(void *rend, GLint x, GLint y, GLsizei w, GLsizei h)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx) {
      x += ctx->win_ox;
      y += ctx->win_oy;
   }
   CALL_Scissor(GET_DISPATCH(), (x, y, w, h));
}

void GLAPIENTRY
ripple_cgl_clear(void *rend, GLbitfield mask)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   struct gl_context *gl = ctx ? ctx->glctx : NULL;
   if (gl && !(gl->Scissor.EnableFlags & 1)) {
      /* Clearing "the window" means the rectangle, not the screen. */
      struct gl_scissor_rect saved = gl->Scissor.ScissorArray[0];
      CALL_Scissor(GET_DISPATCH(), (ctx->win_ox, ctx->win_oy, ctx->win_w, ctx->win_h));
      CALL_Enable(GET_DISPATCH(), (GL_SCISSOR_TEST));
      CALL_Clear(GET_DISPATCH(), (mask));
      CALL_Disable(GET_DISPATCH(), (GL_SCISSOR_TEST));
      CALL_Scissor(GET_DISPATCH(), (saved.X, saved.Y, saved.Width, saved.Height));
      return;
   }
   CALL_Clear(GET_DISPATCH(), (mask));
}

void GLAPIENTRY
ripple_cgl_get_integerv(void *rend, GLenum pname, GLint *params)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   CALL_GetIntegerv(GET_DISPATCH(), (pname, params));
   if (ctx && params && (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX)) {
      params[0] -= ctx->win_ox;
      params[1] -= ctx->win_oy;
   }
}

void GLAPIENTRY
ripple_cgl_read_pixels(void *rend, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                       GLenum type, GLvoid *pixels)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx) {
      x += ctx->win_ox;
      y += ctx->win_oy;
   }
   CALL_ReadPixels(GET_DISPATCH(), (x, y, w, h, format, type, pixels));
}

void GLAPIENTRY
ripple_cgl_copy_pixels(void *rend, GLint x, GLint y, GLsizei w, GLsizei h, GLenum type)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx) {
      x += ctx->win_ox;
      y += ctx->win_oy;
   }
   CALL_CopyPixels(GET_DISPATCH(), (x, y, w, h, type));
}

void GLAPIENTRY
ripple_cgl_copy_tex_image1D(void *rend, GLenum target, GLint level, GLenum ifmt, GLint x,
                            GLint y, GLsizei w, GLint border)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx) {
      x += ctx->win_ox;
      y += ctx->win_oy;
   }
   CALL_CopyTexImage1D(GET_DISPATCH(), (target, level, ifmt, x, y, w, border));
}

void GLAPIENTRY
ripple_cgl_copy_tex_image2D(void *rend, GLenum target, GLint level, GLenum ifmt, GLint x,
                            GLint y, GLsizei w, GLsizei h, GLint border)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx) {
      x += ctx->win_ox;
      y += ctx->win_oy;
   }
   CALL_CopyTexImage2D(GET_DISPATCH(), (target, level, ifmt, x, y, w, h, border));
}

void GLAPIENTRY
ripple_cgl_copy_tex_sub_image1D(void *rend, GLenum target, GLint level, GLint xoff, GLint x,
                                GLint y, GLsizei w)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx) {
      x += ctx->win_ox;
      y += ctx->win_oy;
   }
   CALL_CopyTexSubImage1D(GET_DISPATCH(), (target, level, xoff, x, y, w));
}

void GLAPIENTRY
ripple_cgl_copy_tex_sub_image2D(void *rend, GLenum target, GLint level, GLint xoff, GLint yoff,
                                GLint x, GLint y, GLsizei w, GLsizei h)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx) {
      x += ctx->win_ox;
      y += ctx->win_oy;
   }
   CALL_CopyTexSubImage2D(GET_DISPATCH(), (target, level, xoff, yoff, x, y, w, h));
}

void GLAPIENTRY
ripple_cgl_copy_tex_sub_image3D(void *rend, GLenum target, GLint level, GLint xoff, GLint yoff,
                                GLint zoff, GLint x, GLint y, GLsizei w, GLsizei h)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx) {
      x += ctx->win_ox;
      y += ctx->win_oy;
   }
   CALL_CopyTexSubImage3D(GET_DISPATCH(), (target, level, xoff, yoff, zoff, x, y, w, h));
}
