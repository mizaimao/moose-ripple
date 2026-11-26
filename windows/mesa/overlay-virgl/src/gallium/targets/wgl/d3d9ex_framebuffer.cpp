/*
 * Copyright 2026 the moose-ripple authors
 * SPDX-License-Identifier: MIT
 *
 * A window's OpenGL framebuffer shown through Direct3D 9Ex, for a driver whose OpenGL and
 * Direct3D parts draw on the same GPU: the colour buffers are Direct3D render-target textures
 * made shareable, the OpenGL driver opens them by their shared handle and draws into them, and
 * a swap copies the back buffer's texture to the window's Direct3D back buffer and presents
 * it. The frame never leaves the GPU.
 *
 * Direct3D 9Ex is there on Windows Vista and Windows 7 alike and needs nothing of the display
 * driver beyond its Direct3D 9 part, so this also serves where the driver has no Direct3D 10
 * (Vista; a host without it), which the swap chain of dxgi_framebuffer.cpp needs.
 */

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/format/u_formats.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "stw_device.h"
#include "stw_pixelformat.h"
#include "stw_winsys.h"

#include "frontend/winsys_handle.h"

#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The front and the back colour buffer (ST_ATTACHMENT_FRONT_LEFT, ST_ATTACHMENT_BACK_LEFT) */
#define D3D9EX_NUM_BUFFERS 2

typedef HRESULT(WINAPI *direct3d_create9ex_t)(UINT, IDirect3D9Ex **);

/* RIPPLE_GL_LOG names a file: what this framebuffer made and what failed, a line each. */
static void
gl_log(const char *format, ...)
{
   const char *path = getenv("RIPPLE_GL_LOG");
   if (!path || !*path)
      return;
   FILE *f = fopen(path, "a");
   if (!f)
      return;
   va_list args;
   va_start(args, format);
   vfprintf(f, format, args);
   va_end(args);
   fputc('\n', f);
   fclose(f);
}

struct wgl_d3d9ex_framebuffer {
   struct stw_winsys_framebuffer base;

   struct pipe_screen *screen;
   HWND window;

   HMODULE d3d9;
   IDirect3D9Ex *d3d;
   IDirect3DDevice9Ex *device;

   D3DFORMAT format;
   unsigned width, height;
   unsigned presents, refused;
   /* Where the presents' time went, for RIPPLE_GL_LOG: waiting for the display, the copy, the
    * present itself (microseconds), and how many presents asked to wait. */
   uint64_t wait_us, copy_us, present_us;
   unsigned waited;
   IDirect3DTexture9 *d3d_textures[D3D9EX_NUM_BUFFERS];
   struct pipe_resource *textures[D3D9EX_NUM_BUFFERS];
};

static struct wgl_d3d9ex_framebuffer *
wgl_d3d9ex_framebuffer(struct stw_winsys_framebuffer *fb)
{
   return (struct wgl_d3d9ex_framebuffer *)fb;
}

static uint64_t
d3d9ex_now_us(void)
{
   LARGE_INTEGER now, frequency;
   QueryPerformanceCounter(&now);
   QueryPerformanceFrequency(&frequency);
   return (uint64_t)(now.QuadPart * 1000000.0 / frequency.QuadPart);
}

static D3DFORMAT
d3d9ex_format(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      return D3DFMT_A8R8G8B8;
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      return D3DFMT_X8R8G8B8;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
      return D3DFMT_A8B8G8R8;
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      return D3DFMT_X8B8G8R8;
   default:
      return D3DFMT_UNKNOWN;
   }
}

static void
d3d9ex_release_buffers(struct wgl_d3d9ex_framebuffer *fb)
{
   for (unsigned i = 0; i < D3D9EX_NUM_BUFFERS; ++i) {
      pipe_resource_reference(&fb->textures[i], NULL);
      if (fb->d3d_textures[i]) {
         fb->d3d_textures[i]->Release();
         fb->d3d_textures[i] = NULL;
      }
   }
}

static void
wgl_d3d9ex_framebuffer_destroy(struct stw_winsys_framebuffer *_fb, struct pipe_context *ctx)
{
   struct wgl_d3d9ex_framebuffer *fb = wgl_d3d9ex_framebuffer(_fb);
   struct pipe_fence_handle *fence = NULL;

   if (ctx) {
      /* What was drawn into the shared textures is with the host before they go. */
      ctx->flush(ctx, &fence, PIPE_FLUSH_HINT_FINISH);
      if (fence) {
         ctx->screen->fence_finish(ctx->screen, ctx, fence, OS_TIMEOUT_INFINITE);
         ctx->screen->fence_reference(ctx->screen, &fence, NULL);
      }
   }

   gl_log("%u presents, %u waited for the display: %llu ms waiting, %llu ms copying, %llu ms presenting",
          fb->presents, fb->waited, (unsigned long long)(fb->wait_us / 1000),
          (unsigned long long)(fb->copy_us / 1000), (unsigned long long)(fb->present_us / 1000));
   d3d9ex_release_buffers(fb);
   if (fb->device)
      fb->device->Release();
   if (fb->d3d)
      fb->d3d->Release();
   if (fb->d3d9)
      FreeLibrary(fb->d3d9);
   free(fb);
}

static void
wgl_d3d9ex_framebuffer_resize(struct stw_winsys_framebuffer *_fb, struct pipe_context *ctx,
                              struct pipe_resource *templ)
{
   struct wgl_d3d9ex_framebuffer *fb = wgl_d3d9ex_framebuffer(_fb);
   D3DPRESENT_PARAMETERS pp;

   (void)ctx;
   d3d9ex_release_buffers(fb);

   fb->format = d3d9ex_format(templ->format);
   fb->width = templ->width0;
   fb->height = templ->height0;

   memset(&pp, 0, sizeof(pp));
   pp.BackBufferWidth = fb->width;
   pp.BackBufferHeight = fb->height;
   pp.BackBufferFormat = D3DFMT_X8R8G8B8;
   pp.BackBufferCount = 1;
   pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
   pp.hDeviceWindow = fb->window;
   pp.Windowed = TRUE;
   pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

   if (!fb->device) {
      /* The program's floating-point state and threads are its own: OpenGL promises nothing
       * about either. */
      HRESULT hr = fb->d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, fb->window,
                                           D3DCREATE_HARDWARE_VERTEXPROCESSING |
                                           D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED |
                                           D3DCREATE_NOWINDOWCHANGES,
                                           &pp, NULL, &fb->device);
      gl_log("device for %ux%u, format %u: 0x%08lx", fb->width, fb->height, (unsigned)fb->format,
             (unsigned long)hr);
      if (FAILED(hr))
         fb->device = NULL;
   } else {
      HRESULT hr = fb->device->ResetEx(&pp, NULL);
      gl_log("device reset to %ux%u: 0x%08lx", fb->width, fb->height, (unsigned long)hr);
   }
}

static bool
wgl_d3d9ex_framebuffer_present(struct stw_winsys_framebuffer *_fb, int interval,
                               struct pipe_resource *res)
{
   struct wgl_d3d9ex_framebuffer *fb = wgl_d3d9ex_framebuffer(_fb);
   IDirect3DSurface9 *back = NULL, *source = NULL;
   bool shown = false;

   if (!fb->device)
      return false;

   /* The device presents at once (its interval cannot change without a reset, which would
    * cost the shared textures); a program that asked for a swap interval waits for the
    * display's next refresh here. */
   uint64_t t0 = d3d9ex_now_us();
   if (interval > 0) {
      fb->device->WaitForVBlank(0);
      fb->waited++;
   }
   uint64_t t1 = d3d9ex_now_us();
   fb->wait_us += t1 - t0;

   for (unsigned i = 0; i < D3D9EX_NUM_BUFFERS; ++i) {
      if (fb->textures[i] == res && fb->d3d_textures[i]) {
         fb->d3d_textures[i]->GetSurfaceLevel(0, &source);
         break;
      }
   }
   HRESULT hr_back = E_FAIL, hr_copy = E_FAIL, hr_present = E_FAIL;
   if (source && SUCCEEDED(hr_back = fb->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) &&
       SUCCEEDED(hr_copy = fb->device->StretchRect(source, NULL, back, NULL, D3DTEXF_NONE))) {
      uint64_t t2 = d3d9ex_now_us();
      fb->copy_us += t2 - t1;
      shown = SUCCEEDED(hr_present = fb->device->PresentEx(NULL, NULL, NULL, NULL, 0));
      fb->present_us += d3d9ex_now_us() - t2;
   }
   if (fb->presents++ < 3 || !shown)
      if (fb->presents < 20)
         gl_log("present %u (interval %d): source %p, back buffer 0x%08lx, copy 0x%08lx, present 0x%08lx", fb->presents,
                interval, (void *)source, (unsigned long)hr_back, (unsigned long)hr_copy, (unsigned long)hr_present);
   if (back)
      back->Release();
   if (source)
      source->Release();
   return shown;
}

/* A program that draws to the front buffer of a single-buffered window: show it. */
static void
wgl_d3d9ex_framebuffer_flush_frontbuffer(struct stw_winsys_framebuffer *_fb, struct pipe_context *ctx)
{
   struct wgl_d3d9ex_framebuffer *fb = wgl_d3d9ex_framebuffer(_fb);

   (void)ctx;
   if (fb->textures[0])
      wgl_d3d9ex_framebuffer_present(_fb, 0, fb->textures[0]);
}

static struct pipe_resource *
wgl_d3d9ex_framebuffer_get_resource(struct stw_winsys_framebuffer *_fb,
                                    enum st_attachment_type statt)
{
   struct wgl_d3d9ex_framebuffer *fb = wgl_d3d9ex_framebuffer(_fb);
   const unsigned index = statt;
   HANDLE shared = NULL;

   if (!fb->device || index >= D3D9EX_NUM_BUFFERS || fb->format == D3DFMT_UNKNOWN) {
      if (fb->refused++ < 4)
         gl_log("buffer %u refused: device %p, format %u", index, (void *)fb->device, (unsigned)fb->format);
      return NULL;
   }

   if (fb->textures[index]) {
      pipe_reference(NULL, &fb->textures[index]->reference);
      return fb->textures[index];
   }

   HRESULT hr = fb->device->CreateTexture(fb->width, fb->height, 1, D3DUSAGE_RENDERTARGET,
                                          fb->format, D3DPOOL_DEFAULT,
                                          &fb->d3d_textures[index], &shared);
   gl_log("buffer %u: shared texture %ux%u format %u: 0x%08lx, handle %p", index, fb->width,
          fb->height, (unsigned)fb->format, (unsigned long)hr, (void *)shared);
   if (FAILED(hr) || !shared) {
      if (fb->d3d_textures[index]) {
         fb->d3d_textures[index]->Release();
         fb->d3d_textures[index] = NULL;
      }
      return NULL;
   }

   struct winsys_handle whandle;
   memset(&whandle, 0, sizeof(whandle));
   whandle.type = WINSYS_HANDLE_TYPE_WIN32_HANDLE;
   whandle.handle = shared;

   /* Said to be a framebuffer: on Vista the driver then maps it through a copy, because the
    * allocation behind it cannot be locked from here. */
   fb->textures[index] = fb->screen->resource_from_handle(fb->screen, NULL, &whandle,
                                                          PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   if (fb->textures[index])
      gl_log("buffer %u: opened as %ux%u, pipe format %u, bind 0x%x", index,
             fb->textures[index]->width0, fb->textures[index]->height0,
             (unsigned)fb->textures[index]->format, fb->textures[index]->bind);
   if (!fb->textures[index]) {
      gl_log("buffer %u: the OpenGL driver could not open the shared texture", index);
      fb->d3d_textures[index]->Release();
      fb->d3d_textures[index] = NULL;
      return NULL;
   }
   pipe_reference(NULL, &fb->textures[index]->reference);
   return fb->textures[index];
}

extern "C" {
struct stw_winsys_framebuffer *
wgl_create_d3d9ex_framebuffer(struct pipe_screen *screen, HWND hWnd, int iPixelFormat)
{
   const struct stw_pixelformat_info *pfi = stw_pixelformat_get_info(iPixelFormat);
   if (pfi->pfd.dwFlags & PFD_SUPPORT_GDI)
      return NULL;
   if (d3d9ex_format(pfi->stvis.color_format) == D3DFMT_UNKNOWN)
      return NULL;

   struct wgl_d3d9ex_framebuffer *fb = CALLOC_STRUCT(wgl_d3d9ex_framebuffer);
   if (!fb)
      return NULL;

   /* Looked up, not imported: the DLL this is in also loads where there is no Direct3D 9Ex. */
   fb->d3d9 = LoadLibraryA("d3d9.dll");
   direct3d_create9ex_t create =
      fb->d3d9 ? (direct3d_create9ex_t)(void *)GetProcAddress(fb->d3d9, "Direct3DCreate9Ex") : NULL;
   if (!create || FAILED(create(D3D_SDK_VERSION, &fb->d3d)) || !fb->d3d) {
      if (fb->d3d9)
         FreeLibrary(fb->d3d9);
      free(fb);
      return NULL;
   }

   fb->window = hWnd;
   fb->screen = screen;
   fb->base.destroy = wgl_d3d9ex_framebuffer_destroy;
   fb->base.resize = wgl_d3d9ex_framebuffer_resize;
   fb->base.present = wgl_d3d9ex_framebuffer_present;
   fb->base.get_resource = wgl_d3d9ex_framebuffer_get_resource;
   fb->base.flush_frontbuffer = wgl_d3d9ex_framebuffer_flush_frontbuffer;

   return &fb->base;
}
}
