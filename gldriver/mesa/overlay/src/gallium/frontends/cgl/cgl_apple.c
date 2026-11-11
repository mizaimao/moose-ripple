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

void
ripple_cgl_trace(unsigned index, const char *fmt, ...)
{
   if (index >= 1024)
      return;
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
   if (!ctx || !ctx->win_surface)
      return NULL;
   struct gl_framebuffer *fb = read ? ctx->glctx->ReadBuffer : ctx->glctx->DrawBuffer;
   return fb && _mesa_is_winsys_fbo(fb) ? ctx : NULL;
}

/* Put the program's viewport and scissor into Mesa, shifted if drawing into the window. */
static void
apply_viewport_scissor(struct ripple_cgl_context *ctx, bool shift)
{
   int ox = shift ? ctx->win_ox : 0, oy = shift ? ctx->win_oy : 0;
   if (ctx->lv_set)
      CALL_Viewport(GET_DISPATCH(), (ctx->lv[0] + ox, ctx->lv[1] + oy, ctx->lv[2], ctx->lv[3]));
   if (ctx->ls_set)
      CALL_Scissor(GET_DISPATCH(), (ctx->ls[0] + ox, ctx->ls[1] + oy, ctx->ls[2], ctx->ls[3]));
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
         ripple_cgl_refresh_window_offset(ctx);    /* a frame starts: the rectangle may be new */
         x += ctx->win_ox;
         y += ctx->win_oy;
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
         x += ctx->win_ox;
         y += ctx->win_oy;
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
   if (!ctx || !ctx->win_surface)
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
   struct ripple_cgl_context *ctx = ripple_cgl_context_for(rend);
   RIPPLE_CGL_ENTER(rend);
   /* GL_UNPACK_CLIENT_STORAGE_APPLE stays an error here too: answering it (with the tracked
    * value) made WindowServer drop the menu bar (VM 210, 2026-11-10); the error is part of
    * what keeps WindowServer off its client-storage paths. */
   CALL_GetIntegerv(GET_DISPATCH(), (pname, params));
   if (!ctx || !ctx->win_surface || !params)
      return;
   if (pname == GL_VIEWPORT && ctx->lv_set)
      memcpy(params, ctx->lv, sizeof(ctx->lv));
   else if (pname == GL_SCISSOR_BOX && ctx->ls_set)
      memcpy(params, ctx->ls, sizeof(ctx->ls));
}

/* Reads and copies from the window: shift the source rectangle. */
#define READ_OFFSET(rend, x, y)                                                          \
   do {                                                                                  \
      struct ripple_cgl_context *rctx_ = offset_ctx(rend, true);                         \
      if (rctx_) {                                                                       \
         (x) += rctx_->win_ox;                                                           \
         (y) += rctx_->win_oy;                                                           \
      }                                                                                  \
   } while (0)

void GLAPIENTRY
ripple_cgl_read_pixels(void *rend, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                       GLenum type, GLvoid *pixels)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y);
   CALL_ReadPixels(GET_DISPATCH(), (x, y, w, h, format, type, pixels));
}

void GLAPIENTRY
ripple_cgl_copy_pixels(void *rend, GLint x, GLint y, GLsizei w, GLsizei h, GLenum type)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y);   /* the destination (raster position) is already shifted */
   CALL_CopyPixels(GET_DISPATCH(), (x, y, w, h, type));
}

void GLAPIENTRY
ripple_cgl_copy_tex_image1D(void *rend, GLenum target, GLint level, GLenum ifmt, GLint x,
                            GLint y, GLsizei w, GLint border)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y);
   CALL_CopyTexImage1D(GET_DISPATCH(), (target, level, ifmt, x, y, w, border));
}

void GLAPIENTRY
ripple_cgl_copy_tex_image2D(void *rend, GLenum target, GLint level, GLenum ifmt, GLint x,
                            GLint y, GLsizei w, GLsizei h, GLint border)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y);
   CALL_CopyTexImage2D(GET_DISPATCH(), (target, level, ifmt, x, y, w, h, border));
}

void GLAPIENTRY
ripple_cgl_copy_tex_sub_image1D(void *rend, GLenum target, GLint level, GLint xoff, GLint x,
                                GLint y, GLsizei w)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y);
   CALL_CopyTexSubImage1D(GET_DISPATCH(), (target, level, xoff, x, y, w));
}

void GLAPIENTRY
ripple_cgl_copy_tex_sub_image2D(void *rend, GLenum target, GLint level, GLint xoff, GLint yoff,
                                GLint x, GLint y, GLsizei w, GLsizei h)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y);
   CALL_CopyTexSubImage2D(GET_DISPATCH(), (target, level, xoff, yoff, x, y, w, h));
}

void GLAPIENTRY
ripple_cgl_copy_tex_sub_image3D(void *rend, GLenum target, GLint level, GLint xoff, GLint yoff,
                                GLint zoff, GLint x, GLint y, GLsizei w, GLsizei h)
{
   RIPPLE_CGL_ENTER(rend);
   READ_OFFSET(rend, x, y);
   CALL_CopyTexSubImage3D(GET_DISPATCH(), (target, level, xoff, yoff, zoff, x, y, w, h));
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
};

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
   if (name != GL_EXTENSIONS || !s || !float_pixels_wanted())
      return s;
   GET_CURRENT_CONTEXT(glctx);
   if (!glctx->Extensions.ARB_texture_float)
      return s;
   const GLubyte *r = s;
   simple_mtx_lock(&lock);
   for (unsigned i = 0; i < 16; i++) {
      if (cache[i].mesa == s) {
         r = (const GLubyte *)cache[i].ext;
         break;
      }
      if (!cache[i].mesa) {
         static const char add[] = " GL_APPLE_float_pixels";
         size_t n = strlen((const char *)s);
         char *e = malloc(n + sizeof(add));
         if (e) {
            memcpy(e, s, n);
            memcpy(e + n, add, sizeof(add));
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
