/*
 * The GPU device behind the CGL frontend (moose-ripple, M4): one connection to the kext's
 * user client and one gallium screen per process, made on first use. GPU-neutral apart from
 * choosing the winsys by the backend the kernel reports (docs/kext-uapi.md, decision 0005).
 */
#include <stdbool.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/api.h"
#include "state_tracker/st_context.h"
#include "pipe/p_screen.h"
#include "util/simple_mtx.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "virgl/ripple/ripple_virgl_public.h"
#include "virgl/ripple/ripple_gpu_uapi.h"

#include "cgl_device.h"

static simple_mtx_t device_lock = SIMPLE_MTX_INITIALIZER;
static struct pipe_frontend_screen *device_fscreen;
static bool device_failed;
static uint32_t device_backend;   /* RIPPLE_GPU_BACKEND_* of the screen */

static int
ripple_cgl_get_param(struct pipe_frontend_screen *fscreen, enum st_manager_param param)
{
   return 0;
}

bool
ripple_cgl_device_info(struct ripple_gpu_info *info)
{
   io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                  IOServiceMatching(RIPPLE_GPU_SERVICE_CLASS));
   io_connect_t conn;
   size_t size = sizeof(*info);
   bool ok;

   if (!svc)
      return false;
   ok = IOServiceOpen(svc, mach_task_self(), RIPPLE_GPU_CLIENT_TYPE, &conn) == KERN_SUCCESS;
   IOObjectRelease(svc);
   if (!ok)
      return false;
   ok = IOConnectCallStructMethod(conn, RIPPLE_GPU_GET_INFO, NULL, 0, info, &size)
        == KERN_SUCCESS && info->api_version == RIPPLE_GPU_API_VERSION;
   IOServiceClose(conn);
   return ok;
}

static struct pipe_screen *
ripple_cgl_create_screen(void)
{
   io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                  IOServiceMatching(RIPPLE_GPU_SERVICE_CLASS));
   struct ripple_gpu_info info;
   io_connect_t conn;
   size_t size = sizeof(info);

   if (!svc)
      return NULL;
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), RIPPLE_GPU_CLIENT_TYPE, &conn);
   IOObjectRelease(svc);
   if (kr != KERN_SUCCESS)
      return NULL;
   if (IOConnectCallStructMethod(conn, RIPPLE_GPU_GET_INFO, NULL, 0, &info, &size)
       != KERN_SUCCESS || info.api_version != RIPPLE_GPU_API_VERSION) {
      IOServiceClose(conn);
      return NULL;
   }
   switch (info.backend) {
   case RIPPLE_GPU_BACKEND_VIRGL:
      device_backend = info.backend;
      return ripple_virgl_screen_create(conn, &info, NULL);   /* owns conn from here */
   default:
      debug_printf("ripple: unknown GPU backend %u\n", info.backend);
      IOServiceClose(conn);
      return NULL;
   }
}

struct pipe_frontend_screen *
ripple_cgl_device_fscreen(void)
{
   struct pipe_frontend_screen *fscreen;

   simple_mtx_lock(&device_lock);
   if (!device_fscreen && !device_failed) {
      /* Report OpenGL 2.1 and GLSL 1.20, as Apple's 10.6 drivers do (HANDOFF M4); Mesa's
       * extensions stay available. Read once by Mesa, before the first context; a value the
       * user set wins. */
      setenv("MESA_GL_VERSION_OVERRIDE", "2.1", 0);
      setenv("MESA_GLSL_VERSION_OVERRIDE", "120", 0);
      struct pipe_screen *screen = ripple_cgl_create_screen();
      if (screen) {
         device_fscreen = CALLOC_STRUCT(pipe_frontend_screen);
         if (device_fscreen) {
            device_fscreen->screen = screen;
            device_fscreen->get_param = ripple_cgl_get_param;
         } else {
            screen->destroy(screen);
         }
      }
      device_failed = !device_fscreen;
   }
   fscreen = device_fscreen;
   simple_mtx_unlock(&device_lock);
   return fscreen;
}

void
ripple_cgl_device_release(void)
{
   simple_mtx_lock(&device_lock);
   if (device_fscreen) {
      st_screen_destroy(device_fscreen);
      device_fscreen->screen->destroy(device_fscreen->screen);
      FREE(device_fscreen);
      device_fscreen = NULL;
   }
   device_failed = false;
   simple_mtx_unlock(&device_lock);
}

/* The screen's own kernel connection (the frontend's calls share it with the winsys). */
static io_connect_t
device_connection(void)
{
   struct pipe_frontend_screen *fscreen = ripple_cgl_device_fscreen();

   if (!fscreen)
      return 0;
   switch (device_backend) {
   case RIPPLE_GPU_BACKEND_VIRGL:
      return ripple_virgl_screen_connection(fscreen->screen);
   default:
      return 0;
   }
}

bool
ripple_cgl_device_surface_info(uint32_t id, struct ripple_gpu_surface_info *out)
{
   io_connect_t conn = device_connection();
   uint64_t in = id;
   size_t size = sizeof(*out);

   return conn && IOConnectCallMethod(conn, RIPPLE_GPU_SURFACE_INFO, &in, 1, NULL, 0, NULL, NULL,
                                      out, &size) == KERN_SUCCESS;
}

bool
ripple_cgl_device_display(struct pipe_resource *res, unsigned x, unsigned y, unsigned w,
                          unsigned h)
{
   io_connect_t conn = device_connection();
   uint32_t handle = 0;

   switch (device_backend) {
   case RIPPLE_GPU_BACKEND_VIRGL:
      handle = res ? ripple_virgl_resource_handle(res) : 0;
      break;
   }
   if (!conn || (res && !handle))
      return false;
   uint64_t in[5] = { handle, x, y, w, h };
   return IOConnectCallScalarMethod(conn, RIPPLE_GPU_DISPLAY, in, 5, NULL, NULL) == KERN_SUCCESS;
}

/* ---- The screen texture (Quartz Extreme, M5) -------------------------------------------- *
 * WindowServer renders each frame's dirty rectangle into its display surface; the frame on
 * screen is this persistent screen-sized texture, each rectangle copied into its place on the
 * GPU, then shown with DISPLAY. Rows run bottom-up in both, as GL draws them. */
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/box.h"
#include "util/u_inlines.h"

static struct pipe_resource *screen_tex;
static simple_mtx_t screen_tex_lock = SIMPLE_MTX_INITIALIZER;

bool
ripple_cgl_device_present_region(struct st_context *st, struct pipe_resource *src,
                                 int x, int y, unsigned w, unsigned h,
                                 unsigned screen_w, unsigned screen_h)
{
   struct pipe_frontend_screen *fscreen = ripple_cgl_device_fscreen();
   struct pipe_context *pipe = st->pipe;
   bool ok;

   if (!fscreen || !src || !screen_w || !screen_h)
      return false;
   simple_mtx_lock(&screen_tex_lock);
   if (screen_tex && (screen_tex->width0 != screen_w || screen_tex->height0 != screen_h))
      pipe_resource_reference(&screen_tex, NULL);
   if (!screen_tex) {
      struct pipe_resource t;
      memset(&t, 0, sizeof(t));
      t.target = PIPE_TEXTURE_2D;
      t.format = PIPE_FORMAT_B8G8R8A8_UNORM;
      t.width0 = screen_w;
      t.height0 = screen_h;
      t.depth0 = 1;
      t.array_size = 1;
      t.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SCANOUT;
      screen_tex = fscreen->screen->resource_create(fscreen->screen, &t);
   }
   /* Clip the rectangle (screen coordinates, top-left origin) to the screen and to what the
    * source holds, then place it: GL rows are bottom-up, so its bottom edge is at
    * screen_h - (y + h). */
   int x0 = x, y0 = y, x1 = x + (int)w, y1 = y + (int)h;
   if (x0 < 0) x0 = 0;
   if (y0 < 0) y0 = 0;
   if (x1 > (int)screen_w) x1 = screen_w;
   if (y1 > (int)screen_h) y1 = screen_h;
   ok = screen_tex != NULL;
   if (ok && x1 > x0 && y1 > y0) {
      struct pipe_box box;
      int sx = x0 - x, sh_ = y1 - y0;
      int sy = (int)h - (y1 - y);              /* source rows from the bottom */
      if (sx + (x1 - x0) <= (int)src->width0 && sy >= 0 && sy + sh_ <= (int)src->height0) {
         u_box_2d(sx, sy, x1 - x0, sh_, &box);
         pipe->resource_copy_region(pipe, screen_tex, 0, x0, screen_h - y1, 0, src, 0, &box);
      }
   }
   simple_mtx_unlock(&screen_tex_lock);
   if (!ok)
      return false;
   st_context_flush(st, 0, NULL, NULL, NULL);
   return ripple_cgl_device_display(screen_tex, 0, 0, screen_w, screen_h);
}
