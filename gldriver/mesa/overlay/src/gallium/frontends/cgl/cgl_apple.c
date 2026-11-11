/*
 * Apple-only dispatch entries the CGL frontend implements itself (moose-ripple, M4), and the
 * report for entries nothing implements. GL_APPLE_fence is done with Flush/Finish: correct,
 * if slower than real fences.
 */
#include <stdio.h>
#include <stdlib.h>
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
   ripple_cgl_client_storage_frame();
   ripple_cgl_context_swap(rend);
}

/* ---- Debug trace of GL calls (gen_dispatch.py emits the calls) ------------------------- *
 * On when /Library/Logs/RippleGL/trace exists when the bundle loads: the first calls of each
 * entry with their arguments, and every 10 s the call counts, into
 * /Library/Logs/RippleGL/trace-PROGRAM-PID.log. A file /Library/Logs/RippleGL/burst logs the
 * following calls in full (see ripple_cgl_trace). */
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int ripple_cgl_tracing;
static FILE *trace_file;
static unsigned long trace_counts[1024];
static const char *trace_names[1024];
static time_t trace_last_dump;
static time_t trace_last_burst_check;
static time_t trace_burst_mtime;
static unsigned long trace_burst_left;     /* calls still to log in full (burst mode) */
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

int
ripple_cgl_trace(unsigned index, const char *fmt, ...)
{
   int logged = 0;
   if (index >= 1024)
      return 0;
   simple_mtx_lock(&trace_lock);
   unsigned long n = ++trace_counts[index];
   if (!trace_names[index])
      trace_names[index] = fmt;
   time_t now = time(NULL);
   /* Burst: writing /Library/Logs/RippleGL/burst logs the next calls in full (the file may
    * hold their number; default 40000). Each new modification time starts one burst
    * (WindowServer runs as _windowserver and can't remove the file). */
   if (now != trace_last_burst_check) {
      trace_last_burst_check = now;
      struct stat st;
      if (stat("/Library/Logs/RippleGL/burst", &st) == 0 &&
          st.st_mtime != trace_burst_mtime) {
         trace_burst_mtime = st.st_mtime;
         unsigned long want = 0;
         FILE *b = fopen("/Library/Logs/RippleGL/burst", "r");
         if (b) {
            if (fscanf(b, "%lu", &want) != 1)
               want = 0;
            fclose(b);
         }
         trace_burst_left = want ? want : 40000;
         fprintf(trace_file, "---- burst %lu ----\n", trace_burst_left);
      }
   }
   if (n <= 6 || trace_burst_left) {
      va_list ap;
      va_start(ap, fmt);
      vfprintf(trace_file, fmt, ap);
      va_end(ap);
      fputc('\n', trace_file);
      if (trace_burst_left && --trace_burst_left == 0)
         fprintf(trace_file, "---- burst end ----\n");
      logged = 1;
   }
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
   return logged;
}

/* Text passed to the GL (ARB programs, GLSL), logged after its call line. */
void
ripple_cgl_trace_text(const char *what, const void *text, int len)
{
   if (!text)
      return;
   if (len < 0)
      len = (int)strlen(text);
   simple_mtx_lock(&trace_lock);
   fprintf(trace_file, "  ---- %s, %d bytes ----\n%.*s\n  ---- end ----\n", what, len,
           len > 16384 ? 16384 : len, (const char *)text);
   simple_mtx_unlock(&trace_lock);
}

static bool cs_readable(const void *p, size_t len);

/* Small one- and two-channel images (lookup tables): their first rows, logged after the call
 * line. Tightly packed rows are assumed (enough for a look). */
void
ripple_cgl_trace_pixels(GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels)
{
   if (!pixels || w <= 0 || h <= 0 || w > 4096)
      return;
   if (format == GL_BGRA && (type == GL_UNSIGNED_INT_8_8_8_8_REV || type == GL_UNSIGNED_BYTE)) {
      /* 32-bit images: eight samples of the first row (as 0xAARRGGBB). */
      if (!cs_readable(pixels, (size_t)w * 4))
         return;
      simple_mtx_lock(&trace_lock);
      fprintf(trace_file, "  row 0 samples:");
      for (int i = 0; i < 8; i++)
         fprintf(trace_file, " %08x", ((const uint32_t *)pixels)[(size_t)w * i / 8]);
      fputc('\n', trace_file);
      simple_mtx_unlock(&trace_lock);
      return;
   }
   if (format != GL_LUMINANCE && format != GL_ALPHA && format != GL_LUMINANCE_ALPHA &&
       format != GL_RED)
      return;
   unsigned comps = format == GL_LUMINANCE_ALPHA ? 2 : 1;
   unsigned size = type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_BYTE ? 1 :
                   type == GL_FLOAT ? 4 : 0;
   int rows = h < 4 ? h : 4;
   if (!size || !cs_readable(pixels, (size_t)w * comps * size * rows))
      return;
   simple_mtx_lock(&trace_lock);
   for (int y = 0; y < rows; y++) {
      fprintf(trace_file, "  row %d:", y);
      for (unsigned i = 0; i < w * comps; i++) {
         const char *p = (const char *)pixels + ((size_t)y * w * comps + i) * size;
         if (size == 2)
            fprintf(trace_file, " %u", *(const uint16_t *)p);
         else if (size == 1)
            fprintf(trace_file, " %u", *(const uint8_t *)p);
         else
            fprintf(trace_file, " %g", *(const float *)p);
      }
      fputc('\n', trace_file);
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
 * bottom-left on the screen, and the viewport and scissor queries give back what was set.
 * Only the window's own framebuffer is shifted: Core Image (the Dashboard ripple) renders
 * through framebuffer objects in their own coordinates, so binding one applies the viewport
 * and scissor unshifted, and binding the window again shifts them again. Reads and copies
 * are shifted only when they read the window. */
#include "main/mtypes.h"
#include "main/fbobject.h"
#include "cgl_device.h"
#include "virgl/ripple/ripple_gpu_uapi.h"

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

/* The context if the window offset applies to the framebuffer it draws into (read: reads
 * from), else NULL. */
static struct ripple_cgl_context *
offset_ctx(void *rend, bool read)
{
   struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
   if (!ctx || (!ctx->win_surface && !ctx->surf_h))
      return NULL;
   struct gl_framebuffer *fb = read ? ctx->glctx->ReadBuffer : ctx->glctx->DrawBuffer;
   return fb && _mesa_is_winsys_fbo(fb) ? ctx : NULL;
}

/* Where a rectangle in the program's window coordinates lands in the drawable. The screen
 * region: shifted to its place on the screen. Another accelerator surface: Apple's have the
 * GL origin at their top left, Mesa's drawables at the bottom left (WindowServer draws the
 * menu bar's 22 rows at y 0 of its 28-row background surface and means the top rows; at the
 * bottom they came out 6 rows low, VM 210 2026-11-10). For the screen region both agree as
 * long as the viewport is the region, which is how WindowServer draws. */
static inline int
win_x(const struct ripple_cgl_context *ctx, int x)
{
   return ctx->win_surface ? x + ctx->win_ox : x;
}

static inline int
win_y(const struct ripple_cgl_context *ctx, int y, int h)
{
   return ctx->win_surface ? y + ctx->win_oy : (int)ctx->surf_h - y - h;
}

/* Put the program's viewport and scissor into Mesa, mapped if drawing into the window. */
static void
apply_viewport_scissor(struct ripple_cgl_context *ctx, bool map)
{
   const int *v = ctx->lv, *s = ctx->ls;
   if (ctx->lv_set)
      CALL_Viewport(GET_DISPATCH(), (map ? win_x(ctx, v[0]) : v[0],
                                     map ? win_y(ctx, v[1], v[3]) : v[1], v[2], v[3]));
   if (ctx->ls_set)
      CALL_Scissor(GET_DISPATCH(), (map ? win_x(ctx, s[0]) : s[0],
                                    map ? win_y(ctx, s[1], s[3]) : s[1], s[2], s[3]));
}

void GLAPIENTRY
ripple_cgl_viewport(void *rend, GLint x, GLint y, GLsizei w, GLsizei h)
{
   struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx && w >= 0 && h >= 0) {
      ctx->lv[0] = x, ctx->lv[1] = y, ctx->lv[2] = w, ctx->lv[3] = h;
      ctx->lv_set = true;
      if (offset_ctx(rend, false)) {
         if (ctx->win_surface)
            ripple_cgl_refresh_window_offset(ctx);   /* a frame starts: the rectangle may be new */
         int my = win_y(ctx, y, h);
         x = win_x(ctx, x);
         y = my;
      }
   }
   CALL_Viewport(GET_DISPATCH(), (x, y, w, h));
}

void GLAPIENTRY
ripple_cgl_scissor(void *rend, GLint x, GLint y, GLsizei w, GLsizei h)
{
   struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
   RIPPLE_CGL_ENTER(rend);
   if (ctx && w >= 0 && h >= 0) {
      ctx->ls[0] = x, ctx->ls[1] = y, ctx->ls[2] = w, ctx->ls[3] = h;
      ctx->ls_set = true;
      if (offset_ctx(rend, false)) {
         int my = win_y(ctx, y, h);
         x = win_x(ctx, x);
         y = my;
      }
   }
   CALL_Scissor(GET_DISPATCH(), (x, y, w, h));
}

void GLAPIENTRY
ripple_cgl_bind_framebuffer(void *rend, GLenum target, GLuint framebuffer)
{
   struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
   RIPPLE_CGL_ENTER(rend);
   bool before = ctx && offset_ctx(rend, false);
   CALL_BindFramebufferEXT(GET_DISPATCH(), (target, framebuffer));
   if (!ctx || (!ctx->win_surface && !ctx->surf_h))
      return;
   bool after = offset_ctx(rend, false) != NULL;
   if (before != after)
      apply_viewport_scissor(ctx, after);
}

void GLAPIENTRY
ripple_cgl_clear(void *rend, GLbitfield mask)
{
   struct ripple_cgl_context *ctx = offset_ctx(rend, false);
   RIPPLE_CGL_ENTER(rend);
   struct gl_context *gl = ctx && ctx->win_surface ? ctx->glctx : NULL;
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
   struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
   RIPPLE_CGL_ENTER(rend);
   /* GL_UNPACK_CLIENT_STORAGE_APPLE stays an error here too: answering it (with the tracked
    * value) made WindowServer drop the menu bar (VM 210, 2026-11-10); the error is part of
    * what keeps WindowServer off its client-storage paths. */
   CALL_GetIntegerv(GET_DISPATCH(), (pname, params));
   if (!ctx || (!ctx->win_surface && !ctx->surf_h) || !params)
      return;
   if (pname == GL_VIEWPORT && ctx->lv_set)
      memcpy(params, ctx->lv, sizeof(ctx->lv));
   else if (pname == GL_SCISSOR_BOX && ctx->ls_set)
      memcpy(params, ctx->ls, sizeof(ctx->ls));
}

/* Reads and copies from the window: map the source rectangle (height h). */
#define READ_OFFSET(rend, x, y, h)                                                       \
   do {                                                                                  \
      struct ripple_cgl_context *rctx_ = offset_ctx(rend, true);                         \
      if (rctx_) {                                                                       \
         (y) = win_y(rctx_, (y), (h));                                                   \
         (x) = win_x(rctx_, (x));                                                        \
      }                                                                                  \
   } while (0)

void GLAPIENTRY
ripple_cgl_read_pixels(void *rend, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                       GLenum type, GLvoid *pixels)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y, h);
   CALL_ReadPixels(GET_DISPATCH(), (x, y, w, h, format, type, pixels));
}

void GLAPIENTRY
ripple_cgl_copy_pixels(void *rend, GLint x, GLint y, GLsizei w, GLsizei h, GLenum type)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y, h);   /* the destination (raster position) is already shifted */
   CALL_CopyPixels(GET_DISPATCH(), (x, y, w, h, type));
}

void GLAPIENTRY
ripple_cgl_copy_tex_image1D(void *rend, GLenum target, GLint level, GLenum ifmt, GLint x,
                            GLint y, GLsizei w, GLint border)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y, 1);
   CALL_CopyTexImage1D(GET_DISPATCH(), (target, level, ifmt, x, y, w, border));
}

void GLAPIENTRY
ripple_cgl_copy_tex_image2D(void *rend, GLenum target, GLint level, GLenum ifmt, GLint x,
                            GLint y, GLsizei w, GLsizei h, GLint border)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y, h);
   CALL_CopyTexImage2D(GET_DISPATCH(), (target, level, ifmt, x, y, w, h, border));
}

void GLAPIENTRY
ripple_cgl_copy_tex_sub_image1D(void *rend, GLenum target, GLint level, GLint xoff, GLint x,
                                GLint y, GLsizei w)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y, 1);
   CALL_CopyTexSubImage1D(GET_DISPATCH(), (target, level, xoff, x, y, w));
}

void GLAPIENTRY
ripple_cgl_copy_tex_sub_image2D(void *rend, GLenum target, GLint level, GLint xoff, GLint yoff,
                                GLint x, GLint y, GLsizei w, GLsizei h)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y, h);
   CALL_CopyTexSubImage2D(GET_DISPATCH(), (target, level, xoff, yoff, x, y, w, h));
}

void GLAPIENTRY
ripple_cgl_copy_tex_sub_image3D(void *rend, GLenum target, GLint level, GLint xoff, GLint yoff,
                                GLint zoff, GLint x, GLint y, GLsizei w, GLsizei h)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y, h);
   CALL_CopyTexSubImage3D(GET_DISPATCH(), (target, level, xoff, yoff, zoff, x, y, w, h));
}

/* ---- Surface textures (GLI parameters 997-999) ------------------------------------------- *
 * WindowServer's compositor gets what is behind a window for its background effects (the
 * translucent menu bar, menus, the Dashboard ripple) by binding a surface to the current
 * texture: cglsSetInteger(999, {surface, target, internal format, width, height}), straight to
 * GLEngine (cgl_takeover.c hooks it). GLEngine refuses it for us with kCGLBadState (its own
 * texture bindings never see ours), which left the texture without an image: drawn as plain
 * white (fixed function samples nothing and keeps the white vertex colour). Here the
 * surface's current pixels are copied into the bound texture: the screen region this context
 * composites from its window framebuffer, any other surface from its memory. */
static bool cs_readable(const void *p, size_t len);
static unsigned surface_texture_logs;

#include "main/texobj.h"

/* Copy the window framebuffer's rectangle (x, y, w, h) into the texture bound to `target`, top
 * row first: a surface texture has the surface's rows in memory order, like the memory path
 * below (the Dashboard ripple showed the desktop upside down with GL's order, VM 210
 * 2026-11-11). One blit through a scratch framebuffer object, rows reversed; the read
 * framebuffer is the window's. */
static void
copy_top_first(struct ripple_cgl_context *ctx, GLenum target, GLenum ifmt, int x, int y,
               int w, int h)
{
   struct gl_context *gl = ctx->glctx;
   struct gl_texture_object *tex = _mesa_get_current_tex_object(gl, target);
   if (!tex || !tex->Name)
      return;
   struct gl_pixelstore_attrib saved = gl->Unpack;
   memset(&gl->Unpack, 0, sizeof(gl->Unpack));
   gl->Unpack.Alignment = 4;
   CALL_TexImage2D(GET_DISPATCH(), (target, 0, ifmt, w, h, 0, GL_BGRA,
                                    GL_UNSIGNED_INT_8_8_8_8_REV, NULL));
   gl->Unpack = saved;
   if (!ctx->flip_fbo)
      CALL_GenFramebuffers(GET_DISPATCH(), (1, &ctx->flip_fbo));
   GLuint draw_fb = gl->DrawBuffer ? gl->DrawBuffer->Name : 0;
   bool scissor = gl->Scissor.EnableFlags & 1;
   CALL_BindFramebufferEXT(GET_DISPATCH(), (GL_DRAW_FRAMEBUFFER_EXT, ctx->flip_fbo));
   CALL_FramebufferTexture2D(GET_DISPATCH(), (GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                              target, tex->Name, 0));
   if (scissor)
      CALL_Disable(GET_DISPATCH(), (GL_SCISSOR_TEST));
   CALL_BlitFramebuffer(GET_DISPATCH(), (x, y, x + w, y + h, 0, h, w, 0, GL_COLOR_BUFFER_BIT,
                                         GL_NEAREST));
   if (scissor)
      CALL_Enable(GET_DISPATCH(), (GL_SCISSOR_TEST));
   CALL_FramebufferTexture2D(GET_DISPATCH(), (GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                              target, 0, 0));
   CALL_BindFramebufferEXT(GET_DISPATCH(), (GL_DRAW_FRAMEBUFFER_EXT, draw_fb));
}

int
ripple_cgl_surface_texture(struct ripple_cgl_context *ctx, uint32_t sid, GLenum target,
                           GLenum ifmt, GLsizei w, GLsizei h)
{
   extern FILE *ripple_gld_dbg_file(void);
   FILE *log = surface_texture_logs < 300 ? ripple_gld_dbg_file() : NULL;
   struct ripple_gpu_surface_info si;
   const char *how = "unknown surface";
   int ret = 10008;                     /* kCGLBadValue */
   unsigned pw = 0, ph = 0;

   if (unlikely(ripple_cgl_tracing))      /* in the GL trace too (index past the table's) */
      ripple_cgl_trace(1000, "surface_texture(%u, %#x, %#x, %d, %d)", sid, target, ifmt, w, h);
   RIPPLE_CGL_ENTER(ctx);
   struct gl_context *gl = ctx->glctx;
   GLenum error = gl->ErrorValue;
   gl->ErrorValue = GL_NO_ERROR;
   memset(&si, 0, sizeof(si));
   struct ripple_cgl_drawable *pb = ripple_gld_pbuffer_lookup(sid, &pw, &ph);
   bool have_si = !pb && ripple_cgl_device_surface_info(sid, &si);
   if (w <= 0 || h <= 0 || (!pb && !have_si)) {
      /* nothing to read */
   } else if (pb && sid != ctx->attached_surface) {
      /* A pbuffer (cglsTexImagePBuffer, 998): rendered by another context, usually Core
       * Image's. Read from it for the copy, through the window-system read framebuffer. */
      GLuint read_fb = gl->ReadBuffer ? gl->ReadBuffer->Name : 0;
      ripple_cgl_context_set_read_drawable(ctx, pb);
      CALL_BindFramebufferEXT(GET_DISPATCH(), (GL_READ_FRAMEBUFFER_EXT, 0));
      CALL_CopyTexImage2D(GET_DISPATCH(), (target, 0, ifmt, 0, 0, MIN2((unsigned)w, pw),
                                           MIN2((unsigned)h, ph), 0));
      CALL_BindFramebufferEXT(GET_DISPATCH(), (GL_READ_FRAMEBUFFER_EXT, read_fb));
      ripple_cgl_context_set_read_drawable(ctx, NULL);
      si.width = pw;
      si.height = ph;
      how = "a pbuffer";
      ret = 0;
   } else if (sid == ctx->attached_surface) {
      /* The surface this context draws into: what has been drawn there so far, read from the
       * window framebuffer, from the buffer WindowServer draws into. For the screen region
       * being composited (the drawable is the whole screen) at the region's place. Its memory,
       * if it has any, is only written at the next present. */
      GLuint read_fb = gl->ReadBuffer ? gl->ReadBuffer->Name : 0;
      CALL_BindFramebufferEXT(GET_DISPATCH(), (GL_READ_FRAMEBUFFER_EXT, 0));
      GLenum saved_read = gl->ReadBuffer->ColorReadBuffer;
      GLenum draw_buf = gl->WinSysDrawBuffer ? gl->WinSysDrawBuffer->ColorDrawBuffer[0] : GL_NONE;
      if (draw_buf != GL_NONE)
         CALL_ReadBuffer(GET_DISPATCH(), (draw_buf));
      if (sid == ctx->win_surface)
         ripple_cgl_refresh_window_offset(ctx);
      int x = win_x(ctx, 0), y = win_y(ctx, 0, h);   /* the surface's top-left rows */
      copy_top_first(ctx, target, ifmt, x, y, w, h);
      if (draw_buf != GL_NONE)
         CALL_ReadBuffer(GET_DISPATCH(), (saved_read));
      CALL_BindFramebufferEXT(GET_DISPATCH(), (GL_READ_FRAMEBUFFER_EXT, read_fb));
      how = sid == ctx->win_surface ? "the window region" : "the drawable";
      ret = 0;
      if (have_si && sid != ctx->win_surface && si.backing_address) {
         /* A surface with memory: WindowServer works on that memory with the CPU next (the
          * translucent menu bar: Core Image over what was drawn here), so bring it up to date
          * now, not at the next present. */
         ripple_cgl_context_swap(ctx);
         how = "the drawable, memory updated";
      }
   } else if (have_si && si.backing_address && si.row_bytes >= (uint32_t)w * 4 &&
              (uint32_t)h <= si.height &&
              cs_readable((const void *)(uintptr_t)si.backing_address,
                          (size_t)si.row_bytes * (h - 1) + (size_t)w * 4)) {
      /* A surface with memory in this process: its rows as they are. Surface memory is in
       * GL's row order, bottom row first (presents write it that way, ripple_gl_target.c),
       * which is what a surface texture is: WindowServer flips its own transform before
       * drawing one. */
      struct gl_pixelstore_attrib saved = gl->Unpack;
      memset(&gl->Unpack, 0, sizeof(gl->Unpack));
      gl->Unpack.Alignment = 4;
      gl->Unpack.RowLength = si.row_bytes / 4;
      CALL_TexImage2D(GET_DISPATCH(), (target, 0, ifmt, w, h, 0, GL_BGRA,
                                       GL_UNSIGNED_INT_8_8_8_8_REV,
                                       (const void *)(uintptr_t)si.backing_address));
      gl->Unpack = saved;
      how = "its memory";
      ret = 0;
      if (log && w >= 42) {
         /* A picture of the left end (x 14-41), one character per pixel, memory row 0 first:
          * # dark, + mid, . light, space transparent. */
         const uint8_t *m = (const uint8_t *)(uintptr_t)si.backing_address;
         for (GLsizei r = 0; r < h; r++) {
            char line[29];
            for (int x = 14; x < 42; x++) {
               uint32_t p = *(const uint32_t *)(m + (size_t)si.row_bytes * r + 4 * x);
               unsigned v = (p >> 8) & 0xff, a = p >> 24;
               line[x - 14] = a < 0x20 ? ' ' : v < 100 ? '#' : v < 200 ? '+' : '.';
            }
            line[28] = 0;
            fprintf(log, "RippleGLDriver:   row %2d |%s|\n", r, line);
         }
      }
      if (log) {
         const uint8_t *m = (const uint8_t *)(uintptr_t)si.backing_address;
         fprintf(log, "RippleGLDriver: surface %u memory: row 1 %08x %08x, middle row %08x %08x, "
                 "last row %08x\n", sid, *(const uint32_t *)(m + si.row_bytes + 4 * (w / 3)),
                 *(const uint32_t *)(m + si.row_bytes + 4 * (w / 2)),
                 *(const uint32_t *)(m + (size_t)si.row_bytes * (h / 2) + 4 * (w / 3)),
                 *(const uint32_t *)(m + (size_t)si.row_bytes * (h / 2) + 4 * (w / 2)),
                 *(const uint32_t *)(m + (size_t)si.row_bytes * (h - 1) + 4 * (w / 3)));
      }
   } else {
      how = "no readable pixels";
   }
   GLenum ours = gl->ErrorValue;
   gl->ErrorValue = error;              /* the program never sees our calls */
   if (log) {
      surface_texture_logs++;
      fprintf(log, "RippleGLDriver: surface texture: surface %u (%ux%u at %d,%d, flags 0x%x, "
              "memory 0x%llx row bytes %u) -> target 0x%x format 0x%x %dx%d from %s, GL error "
              "0x%x\n", sid, si.width, si.height, si.x, si.y, si.flags,
              (unsigned long long)si.backing_address, si.row_bytes, target, ifmt, w, h, how, ours);
   }
   return ret;
}

/* ---- Client storage (GL_APPLE_client_storage), emulated ---------------------------------- *
 * Apple's GL reads a client-storage texture's memory when it draws. WindowServer relies on that
 * for app GL surfaces under Quartz Extreme: it makes tiles of the surface's shared memory once
 * and never uploads them again, so with Mesa's copy at glTexImage2D the window kept whatever
 * the memory held then (Front Row all white, gl-gears frozen). The enum itself keeps failing
 * (gen_dispatch.py says why), but 2D and rectangle textures specified while it is set are
 * remembered with their memory and unpack state. Until the program updates one itself
 * (glTexSubImage2D, as WindowServer does for ordinary windows), it is uploaded again from that
 * memory the first time it is bound in each frame (frames end at glFlush and swaps), if the
 * memory is still mapped and readable; otherwise it is forgotten. */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "util/u_atomic.h"
#include "main/context.h"
#include "main/glformats.h"
#include "main/image.h"
#include "main/teximage.h"
#include "main/texobj.h"

#define CS_MAX 2048

struct cs_tex {
   struct gl_texture_object *obj;   /* NULL: free slot */
   GLenum target, format, type;
   GLsizei width, height;
   const GLvoid *pixels;
   struct gl_pixelstore_attrib unpack;   /* BufferObj always NULL */
   unsigned gen;                         /* frame of the last upload */
   bool explicit_update;                 /* the program updates it: leave it alone */
   uint8_t *shadow;                      /* the memory as last uploaded (rows packed), or NULL */
   size_t shadow_size;
};
/* Shadow copies in all: unchanged memory isn't uploaded again (Dashboard re-uploaded about 40
 * unchanged textures a frame, each a round trip through QEMU; performance stage 2026-11-11). */
#define CS_SHADOW_MAX (128u << 20)
static size_t cs_shadow_bytes;

static struct cs_tex cs_texs[CS_MAX];
static unsigned cs_used;                 /* slots in use are below this */
static unsigned cs_gen = 1;
static simple_mtx_t cs_lock = SIMPLE_MTX_INITIALIZER;

static struct cs_tex *
cs_find(const struct gl_texture_object *obj)
{
   for (unsigned i = 0; i < cs_used; i++)
      if (cs_texs[i].obj == obj)
         return &cs_texs[i];
   return NULL;
}

static void
cs_forget(const struct gl_texture_object *obj)
{
   struct cs_tex *t = obj ? cs_find(obj) : NULL;
   if (t) {
      t->obj = NULL;
      free(t->shadow);
      cs_shadow_bytes -= t->shadow_size;
      t->shadow = NULL;
      t->shadow_size = 0;
      while (cs_used && !cs_texs[cs_used - 1].obj)
         cs_used--;
   }
}

/* The whole range mapped and readable in this task. */
static bool
cs_readable(const void *p, size_t len)
{
   mach_vm_address_t addr = (mach_vm_address_t)(uintptr_t)p, end = addr + len;
   while (addr < end) {
      mach_vm_address_t a = addr;
      mach_vm_size_t size = 0;
      vm_region_basic_info_data_64_t info;
      mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t object = MACH_PORT_NULL;
      if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64,
                         (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS ||
          a > addr || !(info.protection & VM_PROT_READ))
         return false;
      addr = a + size;
   }
   return true;
}

/* Upload `t` again from its memory; its texture is bound to its target. */
static bool
cs_refresh(struct gl_context *glctx, struct cs_tex *t)
{
   const GLubyte *first = _mesa_image_address2d(&t->unpack, t->pixels, t->width, t->height,
                                                t->format, t->type, 0, 0);
   GLint stride = _mesa_image_row_stride(&t->unpack, t->width, t->format, t->type);
   GLint bpp = _mesa_bytes_per_pixel(t->format, t->type);
   if (!first || stride <= 0 || bpp <= 0 ||
       !cs_readable(first, (size_t)stride * (t->height - 1) + (size_t)bpp * t->width))
      return false;
   /* Unchanged since the last upload: nothing to do. */
   size_t row = (size_t)bpp * t->width, size = row * t->height;
   if (t->shadow && t->shadow_size == size) {
      GLsizei y = 0;
      while (y < t->height && !memcmp(first + (size_t)stride * y, t->shadow + row * y, row))
         y++;
      if (y == t->height)
         return true;
   } else if (!t->shadow && cs_shadow_bytes + size <= CS_SHADOW_MAX) {
      t->shadow = malloc(size);
      if (t->shadow) {
         t->shadow_size = size;
         cs_shadow_bytes += size;
      }
   }
   if (t->shadow && t->shadow_size == size)
      for (GLsizei y = 0; y < t->height; y++)
         memcpy(t->shadow + row * y, first + (size_t)stride * y, row);
   struct gl_pixelstore_attrib saved = glctx->Unpack;
   GLenum error = glctx->ErrorValue;
   glctx->Unpack = t->unpack;
   CALL_TexSubImage2D(GET_DISPATCH(), (t->target, 0, 0, 0, t->width, t->height, t->format,
                                       t->type, t->pixels));
   glctx->Unpack = saved;
   glctx->ErrorValue = error;          /* the program never sees this upload */
   return true;
}

static bool
cs_target(GLenum target)
{
   return target == GL_TEXTURE_2D || target == GL_TEXTURE_RECTANGLE_ARB;
}

void GLAPIENTRY
ripple_cgl_pixel_storei(void *rend, GLenum pname, GLint param)
{
   RIPPLE_CGL_ENTER(rend);
   if (pname == GL_UNPACK_CLIENT_STORAGE_APPLE) {
      struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
      if (ctx)
         ctx->client_storage = param != 0;
   }
   CALL_PixelStorei(GET_DISPATCH(), (pname, param));   /* client storage: still refused */
}

void GLAPIENTRY
ripple_cgl_pixel_storef(void *rend, GLenum pname, GLfloat param)
{
   RIPPLE_CGL_ENTER(rend);
   if (pname == GL_UNPACK_CLIENT_STORAGE_APPLE) {
      struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
      if (ctx)
         ctx->client_storage = param != 0.0f;
   }
   CALL_PixelStoref(GET_DISPATCH(), (pname, param));
}

void GLAPIENTRY
ripple_cgl_tex_image2D(void *rend, GLenum target, GLint level, GLenum internalformat,
                       GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
                       const GLvoid *pixels)
{
   struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
   RIPPLE_CGL_ENTER(rend);
   CALL_TexImage2D(GET_DISPATCH(), (target, level, internalformat, width, height, border,
                                    format, type, pixels));
   if (level != 0 || !cs_target(target))
      return;
   GET_CURRENT_CONTEXT(glctx);
   struct gl_texture_object *obj = _mesa_get_current_tex_object(glctx, target);
   if (!obj)
      return;
   simple_mtx_lock(&cs_lock);
   cs_forget(obj);
   /* Made as asked (the refused enum leaves an error pending, so ErrorValue can't tell). */
   const struct gl_texture_image *img = obj->Image[0][0];
   if (ctx && ctx->client_storage && pixels && width > 0 && height > 0 && obj->Name &&
       !glctx->Unpack.BufferObj && img && img->Width == (GLuint)width &&
       img->Height == (GLuint)height) {
      struct cs_tex *t = cs_find(NULL);
      if (!t && cs_used < CS_MAX)
         t = &cs_texs[cs_used++];
      if (t) {
         t->obj = obj;
         t->target = target;
         t->format = format;
         t->type = type;
         t->width = width;
         t->height = height;
         t->pixels = pixels;
         t->unpack = glctx->Unpack;
         t->unpack.BufferObj = NULL;
         t->gen = cs_gen;
         t->explicit_update = false;
      }
   }
   simple_mtx_unlock(&cs_lock);
}

void GLAPIENTRY
ripple_cgl_tex_sub_image2D(void *rend, GLenum target, GLint level, GLint xoffset,
                           GLint yoffset, GLsizei width, GLsizei height, GLenum format,
                           GLenum type, const GLvoid *pixels)
{
   RIPPLE_CGL_ENTER(rend);
   if (cs_used && cs_target(target)) {
      GET_CURRENT_CONTEXT(glctx);
      struct gl_texture_object *obj = _mesa_get_current_tex_object(glctx, target);
      simple_mtx_lock(&cs_lock);
      struct cs_tex *t = obj ? cs_find(obj) : NULL;
      if (t)
         t->explicit_update = true;
      simple_mtx_unlock(&cs_lock);
   }
   CALL_TexSubImage2D(GET_DISPATCH(), (target, level, xoffset, yoffset, width, height, format,
                                       type, pixels));
}

void GLAPIENTRY
ripple_cgl_bind_texture(void *rend, GLenum target, GLuint texture)
{
   RIPPLE_CGL_ENTER(rend);
   CALL_BindTexture(GET_DISPATCH(), (target, texture));
   if (!cs_used || !texture || !cs_target(target))
      return;
   GET_CURRENT_CONTEXT(glctx);
   struct gl_texture_object *obj = _mesa_get_current_tex_object(glctx, target);
   simple_mtx_lock(&cs_lock);
   struct cs_tex *t = obj ? cs_find(obj) : NULL;
   if (t && !t->explicit_update && t->gen != cs_gen && !glctx->Unpack.BufferObj) {
      t->gen = cs_gen;
      if (!cs_refresh(glctx, t))
         cs_forget(obj);
   }
   simple_mtx_unlock(&cs_lock);
}

void GLAPIENTRY
ripple_cgl_delete_textures(void *rend, GLsizei n, const GLuint *textures)
{
   RIPPLE_CGL_ENTER(rend);
   if (cs_used && n > 0 && textures) {
      GET_CURRENT_CONTEXT(glctx);
      simple_mtx_lock(&cs_lock);
      for (GLsizei i = 0; i < n; i++)
         if (textures[i])
            cs_forget(_mesa_lookup_texture(glctx, textures[i]));
      simple_mtx_unlock(&cs_lock);
   }
   CALL_DeleteTextures(GET_DISPATCH(), (n, textures));
}

void
ripple_cgl_client_storage_frame(void)
{
   p_atomic_inc(&cs_gen);              /* a frame ends: client memory is read again */
}

void GLAPIENTRY
ripple_cgl_flush(void *rend)
{
   RIPPLE_CGL_ENTER(rend);
   ripple_cgl_client_storage_frame();
   CALL_Flush(GET_DISPATCH(), ());
}

/* ---- GL_APPLE_float_pixels -------------------------------------------------------------- *
 * WindowServer marks its compositor floating-point capable only if the extension string has
 * GL_APPLE_float_pixels (docs/re/qe-display-surface.md); its enums are those of
 * ARB_texture_float and ARB_half_float_pixel, which Mesa implements. So the name can be added
 * whenever Mesa has ARB_texture_float. Off by default (M6 work): with it WindowServer moves
 * drawing to Core Image on the GPU and the menu bar came out black (VM 210, 2026-11-10).
 * Opt in with RIPPLE_FLOAT_PIXELS=1, or for WindowServer the file
 * /Library/Logs/RippleGL/float_pixels when it starts. */
#include <unistd.h>
static bool
float_pixels_wanted(void)
{
   static int wanted = -1;
   if (wanted < 0) {
      const char *e = getenv("RIPPLE_FLOAT_PIXELS");
      wanted = (e && *e == '1') || access("/Library/Logs/RippleGL/float_pixels", F_OK) == 0;
   }
   return wanted;
}
const GLubyte *GLAPIENTRY
ripple_cgl_get_string(void *rend, GLenum name)
{
   /* Mesa keeps a string per context: one copy per string, kept for the process's life (a
    * caller may hold on to it). */
   static simple_mtx_t lock = SIMPLE_MTX_INITIALIZER;
   static struct { const GLubyte *mesa; char *ext; } cache[16];

   RIPPLE_CGL_ENTER(rend);
   const GLubyte *s = CALL_GetString(GET_DISPATCH(), (name));
   if (name == GL_RENDERER && s) {
      /* Apple style, "<GPU> OpenGL Engine", from the host GPU's name in Mesa's string ("virgl
       * (Mesa Intel(R) UHD Graphics 630 (CFL GT2))" gives "Intel UHD Graphics 630 OpenGL
       * Engine"). Core Image picks its GPU tier by such names ("Intel ", "ATI Radeon HD",
       * "NVIDIA GeForce GT ", ...) and renders on the CPU for names it doesn't know, where it
       * can't read WindowServer's textures (the menu bar's tint and the Dashboard ripple).
       * Override: RIPPLE_GL_RENDERER or the file /Library/Logs/RippleGL/renderer. */
      static char renderer[128];
      static int renderer_read;
      simple_mtx_lock(&lock);
      if (!renderer_read) {
         renderer_read = 1;
         const char *env = getenv("RIPPLE_GL_RENDERER");
         FILE *f = env ? NULL : fopen("/Library/Logs/RippleGL/renderer", "r");
         if (env)
            snprintf(renderer, sizeof(renderer), "%s", env);
         else if (f && fgets(renderer, sizeof(renderer), f))
            renderer[strcspn(renderer, "\n")] = 0;
         if (f)
            fclose(f);
         if (!renderer[0]) {
            const char *p = (const char *)s;
            char gpu[96];
            size_t n = 0;
            if (!strncmp(p, "virgl (", 7))
               p += 7;
            if (!strncmp(p, "Mesa ", 5))
               p += 5;
            for (; *p && *p != ')' && n < sizeof(gpu) - 1; p++) {
               if (!strncmp(p, "(R)", 3) || !strncmp(p, "(TM)", 4)) {
                  p = strchr(p, ')');
                  continue;
               }
               if (*p == '(')
                  break;                    /* " (CFL GT2)": the code name */
               gpu[n++] = *p;
            }
            while (n && gpu[n - 1] == ' ')
               n--;
            gpu[n] = 0;
            if (n)
               snprintf(renderer, sizeof(renderer), "%s OpenGL Engine", gpu);
         }
      }
      simple_mtx_unlock(&lock);
      if (renderer[0])
         return (const GLubyte *)renderer;
      return s;
   }
   if (name != GL_EXTENSIONS || !s)
      return s;
   /* What is added: GL_APPLE_client_storage (emulated, see "Client storage" above) and
    * GL_APPLE_texture_range (accepted, no effect), which Core Image requires before it uses the
    * GPU; GL_APPLE_float_pixels (opt-in, above); and for experiments the names in
    * RIPPLE_GL_EXTENSIONS or the file /Library/Logs/RippleGL/extensions (read once). */
   static char extra[512];
   static int extra_read;
   simple_mtx_lock(&lock);
   if (!extra_read) {
      extra_read = 1;
      GET_CURRENT_CONTEXT(glctx);
      if (!strstr((const char *)s, "GL_APPLE_client_storage"))
         strcat(extra, " GL_APPLE_client_storage");
      if (!strstr((const char *)s, "GL_APPLE_texture_range"))
         strcat(extra, " GL_APPLE_texture_range");
      if (float_pixels_wanted() && glctx->Extensions.ARB_texture_float)
         strcat(extra, " GL_APPLE_float_pixels");
      const char *env = getenv("RIPPLE_GL_EXTENSIONS");
      char file[256] = "";
      FILE *f = env ? NULL : fopen("/Library/Logs/RippleGL/extensions", "r");
      if (f) {
         size_t n = fread(file, 1, sizeof(file) - 1, f);
         file[n] = 0;
         fclose(f);
      }
      const char *more = env ? env : file;
      for (const char *p = more; *p;) {
         while (*p == ' ' || *p == '\n' || *p == '\t')
            p++;
         size_t n = strcspn(p, " \n\t");
         if (n && strlen(extra) + n + 2 < sizeof(extra)) {
            strcat(extra, " ");
            strncat(extra, p, n);
         }
         p += n;
      }
   }
   const GLubyte *r = s;
   for (unsigned i = 0; extra[0] && i < 16; i++) {
      if (cache[i].mesa == s) {
         r = (const GLubyte *)cache[i].ext;
         break;
      }
      if (!cache[i].mesa) {
         size_t n = strlen((const char *)s), m = strlen(extra);
         char *e = malloc(n + m + 1);
         if (e) {
            memcpy(e, s, n);
            memcpy(e + n, extra, m + 1);
            cache[i].mesa = s;
            cache[i].ext = e;
            r = (const GLubyte *)e;
         }
         break;
      }
   }
   simple_mtx_unlock(&lock);
   return r;
}

/* ---- Program limits ---------------------------------------------------------------------- *
 * Core Image reads the same limits for vertex and fragment programs before it uses the GPU,
 * including the ALU/TEX/indirection ones ARB_fragment_program defines only for fragment
 * programs. Apple's GL answers them for vertex programs; Mesa raises GL_INVALID_ENUM, and Core
 * Image then renders on the CPU (where it can't read WindowServer's textures: the menu bar's
 * tint came out plain white, VM 210 2026-11-11). Vertex programs get the instruction limit
 * as their ALU limit and no texture instructions. */
void GLAPIENTRY
ripple_cgl_get_programiv(void *rend, GLenum target, GLenum pname, GLint *params)
{
   RIPPLE_CGL_ENTER(rend);
   if (target == GL_VERTEX_PROGRAM_ARB && params &&
       pname >= GL_MAX_PROGRAM_ALU_INSTRUCTIONS_ARB &&
       pname <= GL_MAX_PROGRAM_NATIVE_TEX_INDIRECTIONS_ARB) {
      if (pname == GL_MAX_PROGRAM_ALU_INSTRUCTIONS_ARB)
         CALL_GetProgramivARB(GET_DISPATCH(), (target, GL_MAX_PROGRAM_INSTRUCTIONS_ARB, params));
      else if (pname == GL_MAX_PROGRAM_NATIVE_ALU_INSTRUCTIONS_ARB)
         CALL_GetProgramivARB(GET_DISPATCH(),
                              (target, GL_MAX_PROGRAM_NATIVE_INSTRUCTIONS_ARB, params));
      else
         *params = 0;
   } else {
      CALL_GetProgramivARB(GET_DISPATCH(), (target, pname, params));
   }
   /* Mesa says "unlimited" (INT_MAX) for the native instruction limits; Apple's drivers give
    * real numbers, and Core Image's arithmetic on INT_MAX goes wrong. */
   bool limit = (pname >= GL_MAX_PROGRAM_ALU_INSTRUCTIONS_ARB &&
                 pname <= GL_MAX_PROGRAM_NATIVE_TEX_INDIRECTIONS_ARB) ||
                (pname >= GL_MAX_PROGRAM_INSTRUCTIONS_ARB && pname <= GL_MAX_PROGRAM_ENV_PARAMETERS_ARB);
   if (params && limit && *params > 65535)
      *params = 16384;
}

/* ---- glPushAttrib / glPopAttrib and the window offset ------------------------------------ *
 * Mesa restores the viewport and scissor on glPopAttrib, but the program's own values (lv,
 * ls) that the window offset is applied to are kept here: WindowServer blurs a menu's
 * background through a framebuffer object between glPushAttrib(GL_VIEWPORT_BIT) and
 * glPopAttrib, and with the stale viewport the menu was drawn squeezed and misplaced when it
 * went back to the screen (VM 210, 2026-11-11). So the values are saved and restored here too,
 * and re-applied for the framebuffer bound after the pop. */

void GLAPIENTRY
ripple_cgl_push_attrib(void *rend, GLbitfield mask)
{
   struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
   RIPPLE_CGL_ENTER(rend);
   CALL_PushAttrib(GET_DISPATCH(), (mask));
   if (!ctx)
      return;
   if (ctx->attrib_depth < RIPPLE_CGL_ATTRIB_DEPTH) {
      ctx->attrib[ctx->attrib_depth].mask = mask;
      memcpy(ctx->attrib[ctx->attrib_depth].lv, ctx->lv, sizeof(ctx->lv));
      memcpy(ctx->attrib[ctx->attrib_depth].ls, ctx->ls, sizeof(ctx->ls));
      ctx->attrib[ctx->attrib_depth].lv_set = ctx->lv_set;
      ctx->attrib[ctx->attrib_depth].ls_set = ctx->ls_set;
   }
   ctx->attrib_depth++;
}

void GLAPIENTRY
ripple_cgl_pop_attrib(void *rend)
{
   struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
   RIPPLE_CGL_ENTER(rend);
   CALL_PopAttrib(GET_DISPATCH(), ());
   if (!ctx || !ctx->attrib_depth)
      return;
   ctx->attrib_depth--;
   if (ctx->attrib_depth >= RIPPLE_CGL_ATTRIB_DEPTH)
      return;
   GLbitfield mask = ctx->attrib[ctx->attrib_depth].mask;
   bool restored = false;
   if (mask & GL_VIEWPORT_BIT) {
      memcpy(ctx->lv, ctx->attrib[ctx->attrib_depth].lv, sizeof(ctx->lv));
      ctx->lv_set = ctx->attrib[ctx->attrib_depth].lv_set;
      restored = true;
   }
   if (mask & GL_SCISSOR_BIT) {
      memcpy(ctx->ls, ctx->attrib[ctx->attrib_depth].ls, sizeof(ctx->ls));
      ctx->ls_set = ctx->attrib[ctx->attrib_depth].ls_set;
      restored = true;
   }
   /* Mesa put back whatever it had then (shifted or not, for the framebuffer bound at the
    * push); put the program's values in again for the framebuffer bound now. */
   if (restored && (ctx->win_surface || ctx->surf_h))
      apply_viewport_scissor(ctx, offset_ctx(rend, false) != NULL);
}

/* GL_APPLE_object_purgeable. Quartz Composer marks its cached image textures purgeable and
 * asks for them back (M6, 2026-11-11); Mesa has no implementation, so each call raised
 * GL_INVALID_OPERATION. The data is never discarded here: an object made purgeable reports the
 * state asked for, and comes back retained. */
GLenum GLAPIENTRY
ripple_cgl_object_purgeable(void *rend, GLenum objectType, GLuint name, GLenum option)
{
   return option == GL_RELEASED_APPLE ? GL_RELEASED_APPLE : GL_VOLATILE_APPLE;
}

GLenum GLAPIENTRY
ripple_cgl_object_unpurgeable(void *rend, GLenum objectType, GLuint name, GLenum option)
{
   return GL_RETAINED_APPLE;
}

void GLAPIENTRY
ripple_cgl_get_object_parameteriv(void *rend, GLenum objectType, GLuint name, GLenum pname,
                                  GLint *params)
{
   if (params && pname == GL_PURGEABLE_APPLE)
      *params = GL_FALSE;
}
