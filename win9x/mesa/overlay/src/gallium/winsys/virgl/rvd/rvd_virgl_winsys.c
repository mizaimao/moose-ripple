/*
 * Mesa's virgl winsys for Windows 9x: virgl through the moose-ripple mini-VDD (ripplevd.vxd,
 * interface in rvd.h; docs/win9x/vxd-interface.md in the moose-ripple repository). Modelled
 * on the DRM winsys (../drm/virgl_drm_winsys.c):
 *   - a resource is an RVD buffer: a virgl resource with guest memory behind it, which the
 *     VxD allocates and every process can address directly;
 *   - the command stream goes to RVD_SUBMIT, transfers to RVD_TRANSFER;
 *   - the VxD waits for the host's answer to every request, so there is nothing to wait for
 *     afterwards: fences are signalled when they are made;
 *   - a finished frame is read back into its buffer's memory and drawn on the window with
 *     GDI (flush_frontbuffer), which also clips it against other windows.
 *
 * Windows 2000 has no VxD: the same calls go to the display driver with ExtEscape
 * (win2k/rb2krvd.h and win2k/display_rvd.c), which keeps a buffer's memory in this process.
 *
 * The resource cache, reference counting and command buffer resource lists are adapted from
 * that file.
 *
 * Copyright 2014, 2015 Red Hat.
 * Copyright 2026 moose-ripple.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/format/u_format.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "virgl/virgl_public.h"
#include "virgl/virgl_resource.h"
#include "virgl/virgl_winsys.h"
#include "virtio-gpu/virgl_protocol.h"
#include "virgl_resource_cache.h"

#include "rvd.h"
#include "rvd_virgl_public.h"

/* The display driver's escape on Windows 2000 (win2k/rb2krvd.h). */
#define RB2K_RVD_ESCAPE 0x52420010
#define RB2K_GL_PRESENT_ESCAPE 0x52420012
#define RB2K_GL_RECT_PRESENT_ESCAPE 0x52420015
#define RB2K_GL_INLINE_PRESENT_ESCAPE 0x52420017

#define CACHE_TIMEOUT_USEC      1000000

struct rvd_winsys {
   struct virgl_winsys base;
   HANDLE dev;
   bool nt;                            /* Windows 2000: the display driver instead */
   int (WINAPI *draw_escape)(HDC, int, int, LPCSTR);
   int present_escape;
   simple_mtx_t call_mutex;            /* one call at a time */
   uint32_t ctx;
   struct rvd_info info;

   struct virgl_resource_cache cache;
   simple_mtx_t cache_mutex;
};

struct virgl_hw_res {
   struct pipe_reference reference;
   enum pipe_texture_target target;
   uint32_t res_handle;                /* the RVD buffer handle = virgl resource id */
   uint32_t bind;
   uint32_t size;                      /* as requested */
   uint32_t format;                    /* virgl_formats */
   uint32_t width, height;
   void *ptr;                          /* the buffer's memory */
   int num_cs_references;
   void *cs;                           /* the command buffer that listed it last, and where */
   unsigned cs_index;
   struct virgl_resource_cache_entry cache_entry;
};

struct rvd_fence {
   struct pipe_reference reference;
};

struct rvd_cmd_buf {
   struct virgl_cmd_buf base;
   struct rvd_winsys *ws;
   uint8_t *mem;                       /* struct rvd_submit, then the command dwords */

   unsigned nres, cres;
   struct virgl_hw_res **res;
   char is_added[512];
   unsigned hash_index[512];
};

#define cache_entry_container_res(ptr) \
   (struct virgl_hw_res *)((char *)(ptr) - offsetof(struct virgl_hw_res, cache_entry))

static inline struct rvd_winsys *rvd_winsys(struct virgl_winsys *vws)
{
   return (struct rvd_winsys *)vws;
}

static inline struct rvd_cmd_buf *rvd_cmd_buf(struct virgl_cmd_buf *cbuf)
{
   return (struct rvd_cmd_buf *)cbuf;
}

/* A log of what goes to the VxD (the first few thousand requests, and every failure), in the
 * file the environment variable RVD_LOG names. */
static FILE *rvd_log_file(int always)
{
   static FILE *f;
   static int tried, lines;

   if (!always && lines++ > 3000)
      return NULL;

   if (!tried) {
      const char *name = getenv("RVD_LOG");
      tried = 1;
      if (name && name[0])
         f = fopen(name, "w");
   }
   return f;
}

#define RVD_LOG(...) do { FILE *lf_ = rvd_log_file(0); if (lf_) { fprintf(lf_, __VA_ARGS__); fflush(lf_); } } while (0)
#define RVD_LOG_ALWAYS(...) do { FILE *lf_ = rvd_log_file(1); if (lf_) { fprintf(lf_, __VA_ARGS__); fflush(lf_); } } while (0)

static bool rvd_call(struct rvd_winsys *ws, DWORD code, const void *in, DWORD in_len,
                     void *out, DWORD out_len)
{
   DWORD n = 0;

   if (ws->nt) {
      /* A screen DC for each call: a DC kept from the thread that made the screen is
       * refused once that thread has ended (threadtest), so every later call failed. */
      char stack[256];
      char *buf = in_len + 4 <= sizeof(stack) ? stack : MALLOC(in_len + 4);
      HDC dc;
      bool ok = false;

      if (!buf)
         return false;
      memcpy(buf, &code, 4);
      if (in_len)
         memcpy(buf + 4, in, in_len);
      simple_mtx_lock(&ws->call_mutex);
      dc = GetDC(NULL);
      if (dc) {
         ok = ExtEscape(dc, RB2K_RVD_ESCAPE, (int)(in_len + 4), buf, (int)out_len, out) > 0;
         ReleaseDC(NULL, dc);
      }
      simple_mtx_unlock(&ws->call_mutex);
      if (buf != stack)
         FREE(buf);
      return ok;
   }
   return DeviceIoControl(ws->dev, code, (void *)in, in_len, out, out_len, &n, NULL) != 0;
}

/* ---- Fences: every request is finished when the VxD returns ---------------------------------- */

static struct pipe_fence_handle *rvd_fence_create(void)
{
   struct rvd_fence *fence = CALLOC_STRUCT(rvd_fence);

   if (!fence)
      return NULL;
   pipe_reference_init(&fence->reference, 1);
   return (struct pipe_fence_handle *)fence;
}

static struct pipe_fence_handle *rvd_cs_create_fence(struct virgl_winsys *vws, int fd)
{
   return NULL;
}

static bool rvd_fence_wait(struct virgl_winsys *vws, struct pipe_fence_handle *fence,
                           uint64_t timeout)
{
   return true;
}

static void rvd_fence_reference(struct virgl_winsys *vws, struct pipe_fence_handle **dst,
                                struct pipe_fence_handle *src)
{
   struct rvd_fence *d = (struct rvd_fence *)*dst, *s = (struct rvd_fence *)src;

   if (pipe_reference(d ? &d->reference : NULL, s ? &s->reference : NULL))
      FREE(d);
   *dst = src;
}

static void rvd_fence_server_sync(struct virgl_winsys *vws, struct virgl_cmd_buf *cbuf,
                                  struct pipe_fence_handle *fence)
{
}

static int rvd_fence_get_fd(struct virgl_winsys *vws, struct pipe_fence_handle *fence)
{
   return -1;
}

/* ---- Resources ------------------------------------------------------------------------------- */

static inline bool can_cache_resource(uint32_t bind)
{
   return bind == VIRGL_BIND_CONSTANT_BUFFER ||
          bind == VIRGL_BIND_INDEX_BUFFER ||
          bind == VIRGL_BIND_VERTEX_BUFFER ||
          bind == VIRGL_BIND_CUSTOM ||
          bind == VIRGL_BIND_STAGING ||
          bind == VIRGL_BIND_DEPTH_STENCIL ||
          bind == VIRGL_BIND_RENDER_TARGET ||
          bind == 0;
}

static void rvd_hw_res_destroy(struct rvd_winsys *ws, struct virgl_hw_res *res)
{
   rvd_u32 handle = res->res_handle;

   RVD_LOG("destroy %u\n", res->res_handle);
   rvd_call(ws, RVD_BUFFER_DESTROY, &handle, sizeof(handle), NULL, 0);
   FREE(res);
}

static bool rvd_resource_is_busy(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   return false;
}

static void rvd_resource_wait(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
}

static void rvd_resource_reference(struct virgl_winsys *vws, struct virgl_hw_res **dres,
                                   struct virgl_hw_res *sres)
{
   struct rvd_winsys *ws = rvd_winsys(vws);
   struct virgl_hw_res *old = *dres;

   if (pipe_reference(old ? &old->reference : NULL, sres ? &sres->reference : NULL)) {
      if (!can_cache_resource(old->bind)) {
         rvd_hw_res_destroy(ws, old);
      } else {
         simple_mtx_lock(&ws->cache_mutex);
         virgl_resource_cache_add(&ws->cache, &old->cache_entry);
         simple_mtx_unlock(&ws->cache_mutex);
      }
   }
   *dres = sres;
}

static struct virgl_hw_res *
rvd_resource_create_new(struct rvd_winsys *ws, struct virgl_resource_params params)
{
   struct virgl_hw_res *res = CALLOC_STRUCT(virgl_hw_res);
   struct rvd_buffer_create bc;
   struct rvd_buffer buf;

   if (!res)
      return NULL;
   memset(&bc, 0, sizeof(bc));
   bc.context = ws->ctx;
   bc.size = params.size ? params.size : 1;
   /* A window's colour buffer is read back into its memory to be shown (flush_frontbuffer).
    * The driver asks for no memory behind textures when the host can copy between resources
    * (it then moves texture data through staging buffers), so ask for it here. */
   if ((params.bind & VIRGL_BIND_DISPLAY_TARGET) && params.nr_samples <= 1) {
      uint32_t frame = util_format_get_stride(params.format, params.width) * params.height;
      if (bc.size < frame)
         bc.size = frame;
   }
   bc.target = params.target;
   bc.format = pipe_to_virgl_format(params.format);
   bc.bind = params.bind;
   bc.width = params.width;
   bc.height = params.height;
   bc.depth = params.depth;
   bc.array_size = params.array_size;
   bc.last_level = params.last_level;
   /* Gallium says 0 or 1 for "not multisampled" (the WGL frontend says 1); to the host, 1 is
    * a multisampled texture with one sample, which no depth buffer goes with. */
   bc.nr_samples = params.nr_samples > 1 ? params.nr_samples : 0;
   bc.flags = 0;                        /* the map flags are for blob resources only */
   if (!rvd_call(ws, RVD_BUFFER_CREATE, &bc, sizeof(bc), &buf, sizeof(buf))) {
      debug_printf("rvd: buffer not made (error %lu): target %d format %d bind 0x%x %ux%ux%u "
                   "size %u\n", GetLastError(), params.target, params.format, params.bind,
                   params.width, params.height, params.depth, params.size);
      RVD_LOG_ALWAYS("create FAILED (error %lu): target %d format %d bind 0x%x %ux%ux%u size %u\n",
              GetLastError(), params.target, params.format, params.bind, params.width,
              params.height, params.depth, params.size);
      FREE(res);
      return NULL;
   }
   RVD_LOG("create %lu: target %d format %lu bind 0x%x %ux%ux%u levels %u samples %u size %u\n",
           buf.handle, params.target, bc.format, params.bind, params.width, params.height,
           params.depth, params.last_level + 1, params.nr_samples, params.size);
   pipe_reference_init(&res->reference, 1);
   res->target = params.target;
   res->res_handle = buf.handle;
   res->ptr = buf.address;
   res->bind = params.bind;
   res->size = params.size;
   res->format = bc.format;
   res->width = params.width;
   res->height = params.height;
   p_atomic_set(&res->num_cs_references, 0);
   virgl_resource_cache_entry_init(&res->cache_entry, params);
   return res;
}

static struct virgl_hw_res *
rvd_resource_create(struct virgl_winsys *vws, enum pipe_texture_target target,
                    const void *map_front_private, uint32_t format, uint32_t bind,
                    uint32_t width, uint32_t height, uint32_t depth, uint32_t array_size,
                    uint32_t last_level, uint32_t nr_samples, uint32_t flags, uint32_t size)
{
   struct rvd_winsys *ws = rvd_winsys(vws);
   struct virgl_resource_params params = { .size = size,
                                           .bind = bind,
                                           .format = format,
                                           .flags = flags,
                                           .nr_samples = nr_samples,
                                           .width = width,
                                           .height = height,
                                           .depth = depth,
                                           .array_size = array_size,
                                           .last_level = last_level,
                                           .target = target };

   if (can_cache_resource(bind)) {
      simple_mtx_lock(&ws->cache_mutex);
      struct virgl_resource_cache_entry *entry =
         virgl_resource_cache_remove_compatible(&ws->cache, params);
      simple_mtx_unlock(&ws->cache_mutex);
      if (entry) {
         struct virgl_hw_res *res = cache_entry_container_res(entry);
         pipe_reference_init(&res->reference, 1);
         return res;
      }
   }
   return rvd_resource_create_new(ws, params);
}

static void *rvd_resource_map(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   return res->ptr;
}

static int rvd_transfer(struct virgl_winsys *vws, struct virgl_hw_res *res, uint32_t dir,
                        const struct pipe_box *box, uint32_t stride, uint32_t layer_stride,
                        uint32_t buf_offset, uint32_t level)
{
   struct rvd_winsys *ws = rvd_winsys(vws);
   struct rvd_transfer t;

   memset(&t, 0, sizeof(t));
   t.context = ws->ctx;
   t.buffer = res->res_handle;
   t.direction = dir;
   t.level = level;
   t.x = box->x;
   t.y = box->y;
   t.z = box->z;
   t.width = box->width;
   t.height = box->height;
   t.depth = box->depth;
   /* Like the DRM winsys for non-blob resources: the host works out the layout of the
    * memory itself (stride and layer_stride 0). */
   t.offset = buf_offset;
   RVD_LOG("transfer %s %u (size %u): level %u box %d,%d,%d %dx%dx%d offset %u\n",
           dir == RVD_FROM_HOST ? "from" : "to", res->res_handle, res->size, level,
           box->x, box->y, box->z, box->width, box->height, box->depth, buf_offset);
   return rvd_call(ws, RVD_TRANSFER, &t, sizeof(t), NULL, 0) ? 0 : -1;
}

static int rvd_transfer_put(struct virgl_winsys *vws, struct virgl_hw_res *res,
                            const struct pipe_box *box, uint32_t stride,
                            uint32_t layer_stride, uint32_t buf_offset, uint32_t level)
{
   return rvd_transfer(vws, res, RVD_TO_HOST, box, stride, layer_stride, buf_offset, level);
}

static int rvd_transfer_get(struct virgl_winsys *vws, struct virgl_hw_res *res,
                            const struct pipe_box *box, uint32_t stride,
                            uint32_t layer_stride, uint32_t buf_offset, uint32_t level)
{
   return rvd_transfer(vws, res, RVD_FROM_HOST, box, stride, layer_stride, buf_offset, level);
}

static uint32_t rvd_resource_get_storage_size(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   return res->size;
}

/* Sharing resources between processes isn't supported. */
static struct virgl_hw_res *
rvd_resource_create_from_handle(struct virgl_winsys *vws, struct winsys_handle *whandle,
                                struct pipe_resource *templ, uint32_t *plane,
                                uint32_t *stride, uint32_t *plane_offset,
                                uint64_t *modifier, uint32_t *blob_mem)
{
   return NULL;
}

static void rvd_resource_set_type(struct virgl_winsys *vws, struct virgl_hw_res *res,
                                  uint32_t format, uint32_t bind, uint32_t width,
                                  uint32_t height, uint32_t usage, uint64_t modifier,
                                  uint32_t plane_count, const uint32_t *plane_strides,
                                  const uint32_t *plane_offsets)
{
}

static bool rvd_resource_get_handle(struct virgl_winsys *vws, struct virgl_hw_res *res,
                                    uint32_t stride, struct winsys_handle *whandle)
{
   return false;
}

/* ---- Command buffers ------------------------------------------------------------------------- */

static struct virgl_cmd_buf *rvd_cmd_buf_create(struct virgl_winsys *vws, uint32_t size)
{
   struct rvd_winsys *ws = rvd_winsys(vws);
   struct rvd_cmd_buf *cbuf;

   if (size * 4 > ws->info.max_submit)
      return NULL;
   cbuf = CALLOC_STRUCT(rvd_cmd_buf);
   if (!cbuf)
      return NULL;
   cbuf->ws = ws;
   cbuf->nres = 512;
   cbuf->res = CALLOC(cbuf->nres, sizeof(*cbuf->res));
   cbuf->mem = MALLOC(sizeof(struct rvd_submit) + size * 4);
   if (!cbuf->res || !cbuf->mem) {
      FREE(cbuf->res);
      FREE(cbuf->mem);
      FREE(cbuf);
      return NULL;
   }
   cbuf->base.buf = (uint32_t *)(cbuf->mem + sizeof(struct rvd_submit));
   return &cbuf->base;
}

static void rvd_clear_res_list(struct rvd_cmd_buf *cbuf)
{
   for (unsigned i = 0; i < cbuf->cres; i++) {
      p_atomic_dec(&cbuf->res[i]->num_cs_references);
      rvd_resource_reference(&cbuf->ws->base, &cbuf->res[i], NULL);
   }
   cbuf->cres = 0;
   memset(cbuf->is_added, 0, sizeof(cbuf->is_added));
}

static void rvd_cmd_buf_destroy(struct virgl_cmd_buf *_cbuf)
{
   struct rvd_cmd_buf *cbuf = rvd_cmd_buf(_cbuf);

   rvd_clear_res_list(cbuf);
   FREE(cbuf->res);
   FREE(cbuf->mem);
   FREE(cbuf);
}

/* Is the resource in the command buffer's list (which keeps it alive until the commands that
 * name it have gone to the host)? The resource remembers where it was listed last; the list
 * is the judge, so a stale hint only costs the slow path. */
static bool rvd_lookup_res(struct rvd_cmd_buf *cbuf, struct virgl_hw_res *res)
{
   unsigned i = res->cs_index;

   if (res->cs == cbuf && i < cbuf->cres && cbuf->res[i] == res)
      return true;
   if (!p_atomic_read(&res->num_cs_references))
      return false;
   unsigned hash = res->res_handle & (sizeof(cbuf->is_added) - 1);
   if (!cbuf->is_added[hash])
      return false;
   i = cbuf->hash_index[hash];
   if (i < cbuf->cres && cbuf->res[i] == res) {
      res->cs = cbuf;
      res->cs_index = i;
      return true;
   }
   for (i = 0; i < cbuf->cres; i++) {
      if (cbuf->res[i] == res) {
         cbuf->hash_index[hash] = i;
         res->cs = cbuf;
         res->cs_index = i;
         return true;
      }
   }
   return false;
}

static void rvd_add_res(struct rvd_cmd_buf *cbuf, struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle & (sizeof(cbuf->is_added) - 1);

   if (cbuf->cres >= cbuf->nres) {
      unsigned n = cbuf->nres + 256;
      void *r = REALLOC(cbuf->res, cbuf->nres * sizeof(*cbuf->res), n * sizeof(*cbuf->res));
      if (!r)
         return;
      cbuf->res = r;
      cbuf->nres = n;
   }
   cbuf->res[cbuf->cres] = NULL;
   rvd_resource_reference(&cbuf->ws->base, &cbuf->res[cbuf->cres], res);
   cbuf->is_added[hash] = true;
   cbuf->hash_index[hash] = cbuf->cres;
   res->cs = cbuf;
   res->cs_index = cbuf->cres;
   p_atomic_inc(&res->num_cs_references);
   cbuf->cres++;
}

static void rvd_emit_res(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf,
                         struct virgl_hw_res *res, bool write_buf)
{
   struct rvd_cmd_buf *cbuf = rvd_cmd_buf(_cbuf);
   bool listed = rvd_lookup_res(cbuf, res);

   if (write_buf)
      cbuf->base.buf[cbuf->base.cdw++] = res->res_handle;
   if (!listed)
      rvd_add_res(cbuf, res);
}

static bool rvd_res_is_referenced(struct virgl_winsys *vws, struct virgl_cmd_buf *cbuf,
                                  struct virgl_hw_res *res)
{
   return p_atomic_read(&res->num_cs_references) != 0;
}

/* A buffer is empty when it holds only what Mesa puts in every one: the space kept for
 * transfers (ended by END_TRANSFERS, whose length covers the unused rest of it) and the choice
 * of sub-context (SET_SUB_CTX). Every buffer starts with its own SET_SUB_CTX, so leaving one
 * out changes nothing on the host. */
static bool rvd_cmd_buf_is_empty(const struct rvd_cmd_buf *cbuf)
{
   uint32_t i = 0, n = cbuf->base.cdw;

   while (i < n) {
      uint32_t head = cbuf->base.buf[i], cmd = head & 0xff;
      if (cmd != VIRGL_CCMD_NOP && cmd != VIRGL_CCMD_SET_SUB_CTX &&
          cmd != VIRGL_CCMD_END_TRANSFERS)
         return false;
      i += (head >> 16) + 1;
   }
   return true;
}

static int rvd_submit_cmd(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf,
                          struct pipe_fence_handle **fence)
{
   struct rvd_winsys *ws = rvd_winsys(vws);
   struct rvd_cmd_buf *cbuf = rvd_cmd_buf(_cbuf);
   struct rvd_submit *s = (struct rvd_submit *)cbuf->mem;
   int ret = 0;

   if (cbuf->base.cdw == 0)
      return 0;
   if (rvd_cmd_buf_is_empty(cbuf)) {
      /* Nothing for the host: the frontends flush with a fence after every frame, and Mesa then
       * submits even an empty buffer. Each submission is a round trip through the host, and the
       * fences here are complete when they are made, so leave it out. */
      RVD_LOG("empty submit left out\n");
      cbuf->base.cdw = 0;
      if (fence)
         *fence = rvd_fence_create();
      rvd_clear_res_list(cbuf);
      return 0;
   }
   s->context = ws->ctx;
   s->bytes = cbuf->base.cdw * 4;
   if (rvd_log_file(0)) {
      /* What the submission holds: commands by number (virgl_protocol.h), and what the transfers
       * in it move (TRANSFER3D, COPY_TRANSFER3D: box width, height and depth at dwords 9 to 11
       * of the payload; texels for a texture, bytes for a buffer). */
      unsigned count[64];
      unsigned long texels = 0;
      uint32_t i = 0, n = cbuf->base.cdw;
      char line[512];
      int len = 0;

      memset(count, 0, sizeof(count));
      while (i < n) {
         uint32_t head = cbuf->base.buf[i], cmd = head & 0xff, size = head >> 16;
         if (cmd < 64)
            count[cmd]++;
         if ((cmd == VIRGL_CCMD_TRANSFER3D || cmd == VIRGL_CCMD_COPY_TRANSFER3D) && size >= 11 &&
             i + 11 < n)
            texels += (unsigned long)cbuf->base.buf[i + 9] * cbuf->base.buf[i + 10] *
                      cbuf->base.buf[i + 11];
         i += size + 1;
      }
      for (i = 0; i < 64 && len < (int)sizeof(line) - 16; i++) {
         if (count[i])
            len += snprintf(line + len, sizeof(line) - len, " %u:%u", (unsigned)i, count[i]);
      }
      line[len] = 0;
      RVD_LOG("submit %lu bytes, transfers of %lu texels or bytes; commands%s\n", s->bytes,
              texels, line);
   }
   if (!rvd_call(ws, RVD_SUBMIT, s, sizeof(*s) + s->bytes, NULL, 0)) {
      debug_printf("rvd: SUBMIT failed (error %lu) - expect bad rendering\n", GetLastError());
      ret = -1;
   }
   cbuf->base.cdw = 0;
   if (fence && !ret)
      *fence = rvd_fence_create();
   rvd_clear_res_list(cbuf);
   return ret;
}

/* The colour buffer copied on the host into a reusable 32-bit image, made (again) at its size,
 * and the commands sent. Returns the image or NULL; a new one is in *replacement until the
 * caller keeps it. The copy keeps row order: Direct3D's HAL draws its top row into FBO row
 * zero, and Mesa keeps window frames top row first. */
static struct pipe_resource *
copy_to_image(struct pipe_context *pipe, struct pipe_resource *color, struct pipe_resource *dest,
              struct pipe_resource **replacement, bool flip)
{
   struct pipe_screen *screen = pipe->screen;
   struct pipe_fence_handle *submitted = NULL;
   struct pipe_blit_info blit = {0};

   *replacement = NULL;
   if (color->screen != screen || color->target != PIPE_TEXTURE_2D || color->nr_samples > 1)
      return NULL;
   if (!dest || dest->width0 != color->width0 || dest->height0 != color->height0) {
      struct pipe_resource templ = {
         .target = PIPE_TEXTURE_2D,
         .format = PIPE_FORMAT_B8G8R8X8_UNORM,
         .width0 = color->width0, .height0 = color->height0, .depth0 = 1, .array_size = 1,
         .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT,
      };
      if (!screen->is_format_supported(screen, templ.format, templ.target, 0, 0, templ.bind))
         templ.format = PIPE_FORMAT_B8G8R8A8_UNORM;
      if (!screen->is_format_supported(screen, templ.format, templ.target, 0, 0, templ.bind))
         return NULL;
      dest = *replacement = screen->resource_create(screen, &templ);
      if (!dest)
         return NULL;
   }
   blit.src.resource = color;
   blit.src.format = color->format;
   u_box_2d(0, 0, color->width0, color->height0, &blit.src.box);
   blit.dst.resource = dest;
   blit.dst.format = dest->format;
   blit.dst.box = blit.src.box;
   if (flip) {               /* rows turned over for the desktop image: see present_window */
      blit.src.box.y = color->height0;
      blit.src.box.height = -(int)color->height0;
   }
   blit.mask = PIPE_MASK_RGBA;
   blit.filter = PIPE_TEX_FILTER_NEAREST;
   pipe->blit(pipe, &blit);
   pipe->flush(pipe, &submitted, 0);
   if (!submitted) {        /* rvd_submit_cmd only returns a fence after the VxD accepts the commands */
      pipe_resource_reference(replacement, NULL);
      return NULL;
   }
   screen->fence_reference(screen, &submitted, NULL);
   return dest;
}

/* DirectDraw can show its full-screen render target without a guest-memory copy. The separate
 * scanout texture lets the HAL keep drawing or reading back its original 565 or RGBA surface.
 * Keep the old image alive across a resize until its replacement is on the display. */
bool rvd_virgl_present_color(struct pipe_context *pipe, struct pipe_resource *color,
                             struct pipe_resource **image)
{
   struct rvd_winsys *ws = rvd_winsys(virgl_screen(pipe->screen)->vws);
   struct pipe_resource *replacement, *dest = copy_to_image(pipe, color, *image, &replacement, false);
   bool shown = false;

   if (dest) {
      struct rvd_present present = {
         ws->ctx, virgl_resource(dest)->hw_res->res_handle, dest->width0, dest->height0
      };
      shown = rvd_call(ws, RVD_PRESENT, &present, sizeof(present), NULL, 0);
      if (shown && replacement)
         pipe_resource_reference(image, replacement);
   }
   pipe_resource_reference(&replacement, NULL);
   return shown;
}

/* Windows 9x: a window's frame copied on the host into the desktop image, inside the given
 * screen rectangles (four longs each, GDI's visible region), the client area's top-left at x, y.
 * false: the caller shows the frame with GDI. */
bool rvd_virgl_present_window(struct pipe_context *pipe, struct pipe_resource *color,
                              struct pipe_resource **image, long x, long y,
                              const long *rects, unsigned count)
{
   struct rvd_winsys *ws = rvd_winsys(virgl_screen(pipe->screen)->vws);
   struct {
      struct rvd_window w;
      struct rvd_rect r[RVD_WINDOW_RECTS];
   } call;
   struct pipe_resource *replacement, *dest;
   bool shown = false;
   unsigned i;

   if (ws->nt || !count || count > RVD_WINDOW_RECTS)
      return false;
   dest = copy_to_image(pipe, color, *image, &replacement, true);
   if (dest) {
      call.w.context = ws->ctx;
      call.w.buffer = virgl_resource(dest)->hw_res->res_handle;
      call.w.width = dest->width0;
      call.w.height = dest->height0;
      call.w.x = (rvd_u32)x;
      call.w.y = (rvd_u32)y;
      call.w.format = virgl_resource(dest)->hw_res->format;
      call.w.count = count;
      for (i = 0; i < count; i++) {
         call.r[i].left = (rvd_u32)rects[4 * i];
         call.r[i].top = (rvd_u32)rects[4 * i + 1];
         call.r[i].right = (rvd_u32)rects[4 * i + 2];
         call.r[i].bottom = (rvd_u32)rects[4 * i + 3];
      }
      shown = rvd_call(ws, RVD_PRESENT_WINDOW, &call,
                       sizeof(call.w) + count * sizeof(call.r[0]), NULL, 0);
      if (shown && replacement)
         pipe_resource_reference(image, replacement);
   }
   pipe_resource_reference(&replacement, NULL);
   return shown;
}

bool rvd_virgl_window_scanout(struct pipe_screen *screen)
{
   struct rvd_winsys *ws = rvd_winsys(virgl_screen(screen)->vws);
   rvd_u32 on = 0;
   /* Windows 2000 and XP: only with the display driver's 32-bit GPU desktop, which the display
    * returns to (RVD_DESKTOP without input answers whether it is on). */
   if (ws->nt)
      return rvd_call(ws, RVD_DESKTOP, NULL, 0, &on, sizeof(on)) && on;
   return true;
}

/* The display shows the GDI desktop again instead of the last GPU image: on Windows 9x the
 * mini-VDD's display start 0, on Windows 2000 and XP the display driver's GPU desktop. */
void rvd_virgl_show_desktop(struct pipe_screen *screen)
{
   struct rvd_winsys *ws = rvd_winsys(virgl_screen(screen)->vws);
   rvd_u32 offset = 0;
   rvd_call(ws, RVD_DESKTOP, &offset, sizeof(offset), NULL, 0);
}

/* ---- A finished frame goes on the window ----------------------------------------------------- */
/* DrawEscape can present an NT5 window from its GPU resource. The GDI path
 * reads top-down rows into guest memory and paints the window's visible region. */
static void rvd_flush_frontbuffer(struct virgl_winsys *vws, struct virgl_cmd_buf *cbuf,
                                  struct virgl_hw_res *res, unsigned level, unsigned layer,
                                  void *winsys_drawable_handle, struct pipe_box *sub_box)
{
   HDC dc = (HDC)winsys_drawable_handle;
   struct rvd_winsys *ws = rvd_winsys(vws);
   struct {
      BITMAPINFOHEADER h;
      DWORD masks[3];
   } bmi;
   struct pipe_box box;
   unsigned bpp;

   RVD_LOG("present %u: format %u %ux%u\n", res->res_handle, res->format, res->width, res->height);
   if (!dc || !res->ptr)
      return;
   memset(&bmi, 0, sizeof(bmi));
   switch (res->format) {
   case VIRGL_FORMAT_B8G8R8A8_UNORM:
   case VIRGL_FORMAT_B8G8R8X8_UNORM:
      bpp = 32;
      bmi.h.biCompression = BI_RGB;
      break;
   case VIRGL_FORMAT_B5G6R5_UNORM:
      bpp = 16;
      bmi.h.biCompression = BI_BITFIELDS;
      bmi.masks[0] = 0xF800;
      bmi.masks[1] = 0x07E0;
      bmi.masks[2] = 0x001F;
      break;
   default:
      return;
   }
   if (ws->draw_escape && level == 0 && layer == 0 && bpp == 32) {
      struct rvd_present frame = {ws->ctx, res->res_handle, res->width, res->height};
      /* GDI supplies the destination window and its visible region to the display driver. */
      if (ws->draw_escape(dc, ws->present_escape, sizeof(frame), (LPCSTR)&frame) == 1)
         return;
   }
   u_box_2d(0, 0, res->width, res->height, &box);
   /* GDI wants every row to start on a 4-byte boundary. */
   if (rvd_transfer(vws, res, RVD_FROM_HOST, &box, 0, 0, 0, level))
      return;
   if (bpp == 16 && (res->width & 1))
      return;                           /* TODO: a stride for odd widths at 16 bits */

   bmi.h.biSize = sizeof(bmi.h);
   bmi.h.biWidth = res->width;
   bmi.h.biHeight = -(LONG)res->height;
   bmi.h.biPlanes = 1;
   bmi.h.biBitCount = bpp;
   SetDIBitsToDevice(dc, 0, 0, res->width, res->height, 0, 0, 0, res->height, res->ptr,
                     (BITMAPINFO *)&bmi, DIB_RGB_COLORS);
}

/* ---- Device ---------------------------------------------------------------------------------- */

static int rvd_get_caps(struct virgl_winsys *vws, struct virgl_drm_caps *caps)
{
   struct rvd_winsys *ws = rvd_winsys(vws);
   rvd_u32 in[2] = { 1, 0 };

   virgl_ws_fill_new_caps_defaults(caps);
   for (unsigned i = 0; i < ws->info.num_caps && i < RVD_MAX_CAPS; i++) {
      if (ws->info.caps_ids[i] == 2 || (ws->info.caps_ids[i] == 1 && in[0] != 2)) {
         in[0] = ws->info.caps_ids[i];
         in[1] = ws->info.caps_versions[i];
      }
   }
   return rvd_call(ws, RVD_GET_CAPS, in, sizeof(in), &caps->caps,
                   in[0] == 2 ? sizeof(union virgl_caps) : sizeof(struct virgl_caps_v1)) ? 0 : -1;
}

static int rvd_get_fd(struct virgl_winsys *vws)
{
   return -1;
}

static bool rvd_cache_entry_is_busy(struct virgl_resource_cache_entry *entry, void *user)
{
   return false;
}

static void rvd_cache_entry_release(struct virgl_resource_cache_entry *entry, void *user)
{
   rvd_hw_res_destroy(user, cache_entry_container_res(entry));
}

static void rvd_close(HANDLE dev)
{
   if (dev != INVALID_HANDLE_VALUE)
      CloseHandle(dev);
}

static void rvd_winsys_destroy(struct virgl_winsys *vws)
{
   struct rvd_winsys *ws = rvd_winsys(vws);
   rvd_u32 ctx = ws->ctx;

   virgl_resource_cache_flush(&ws->cache);
   rvd_call(ws, RVD_CONTEXT_DESTROY, &ctx, sizeof(ctx), NULL, 0);
   rvd_close(ws->dev);
   simple_mtx_destroy(&ws->call_mutex);
   simple_mtx_destroy(&ws->cache_mutex);
   FREE(ws);
}

static struct virgl_winsys *rvd_winsys_create(void)
{
   struct rvd_winsys *ws;
   char name[64];
   rvd_u32 ctx = 0;
   HANDLE dev;
   bool nt = false;

   dev = CreateFileA(RVD_DEVICE_NAME, 0, 0, NULL, 0, 0, NULL);
   if (dev == INVALID_HANDLE_VALUE) {
      /* Windows 2000: the display driver answers through GDI when it has a GPU. */
      DWORD escape = RB2K_RVD_ESCAPE;
      HDC dc;

      if (GetVersion() & 0x80000000UL)
         return NULL;
      dc = GetDC(NULL);
      if (!dc)
         return NULL;
      nt = ExtEscape(dc, QUERYESCSUPPORT, sizeof(escape), (LPCSTR)&escape, 0, NULL) > 0;
      ReleaseDC(NULL, dc);
      if (!nt)
         return NULL;
   }
   ws = CALLOC_STRUCT(rvd_winsys);
   if (!ws) {
      rvd_close(dev);
      return NULL;
   }
   ws->dev = dev;
   ws->nt = nt;
   if (nt) {
      const char *present = getenv("RIPPLE_GL_PRESENT");
      if (present) {
         if (!strcmp(present, "1"))
            ws->present_escape = RB2K_GL_PRESENT_ESCAPE;
         else if (!strcmp(present, "rect"))
            ws->present_escape = RB2K_GL_RECT_PRESENT_ESCAPE;
         else if (!strcmp(present, "inline"))
            ws->present_escape = RB2K_GL_INLINE_PRESENT_ESCAPE;
      }
      if (ws->present_escape)
         ws->draw_escape = (void *)GetProcAddress(GetModuleHandleA("gdi32.dll"), "DrawEscape");
   }
   simple_mtx_init(&ws->call_mutex, mtx_plain);
   if (!rvd_call(ws, RVD_GET_INFO, NULL, 0, &ws->info, sizeof(ws->info)) ||
       ws->info.version != RVD_INTERFACE_VERSION || !ws->info.virgl) {
      rvd_close(dev);
      simple_mtx_destroy(&ws->call_mutex);
      FREE(ws);
      return NULL;
   }
   /* The host's logs name the context after the program. */
   name[0] = 0;
   GetModuleFileNameA(NULL, name, sizeof(name));
   {
      const char *base = strrchr(name, '\\');
      base = base ? base + 1 : name;
      if (!rvd_call(ws, RVD_CONTEXT_CREATE, base, (DWORD)strlen(base), &ctx, sizeof(ctx))) {
         rvd_close(dev);
         simple_mtx_destroy(&ws->call_mutex);
         FREE(ws);
         return NULL;
      }
   }
   ws->ctx = ctx;
   virgl_resource_cache_init(&ws->cache, CACHE_TIMEOUT_USEC, rvd_cache_entry_is_busy,
                             rvd_cache_entry_release, ws);
   simple_mtx_init(&ws->cache_mutex, mtx_plain);

   ws->base.destroy = rvd_winsys_destroy;
   ws->base.get_fd = rvd_get_fd;
   ws->base.transfer_put = rvd_transfer_put;
   ws->base.transfer_get = rvd_transfer_get;
   ws->base.resource_create = rvd_resource_create;
   ws->base.resource_reference = rvd_resource_reference;
   ws->base.resource_map = rvd_resource_map;
   ws->base.resource_wait = rvd_resource_wait;
   ws->base.resource_is_busy = rvd_resource_is_busy;
   ws->base.resource_create_from_handle = rvd_resource_create_from_handle;
   ws->base.resource_set_type = rvd_resource_set_type;
   ws->base.resource_get_handle = rvd_resource_get_handle;
   ws->base.resource_get_storage_size = rvd_resource_get_storage_size;
   ws->base.cmd_buf_create = rvd_cmd_buf_create;
   ws->base.cmd_buf_destroy = rvd_cmd_buf_destroy;
   ws->base.emit_res = rvd_emit_res;
   ws->base.submit_cmd = rvd_submit_cmd;
   ws->base.res_is_referenced = rvd_res_is_referenced;
   ws->base.get_caps = rvd_get_caps;
   ws->base.cs_create_fence = rvd_cs_create_fence;
   ws->base.fence_wait = rvd_fence_wait;
   ws->base.fence_reference = rvd_fence_reference;
   ws->base.fence_server_sync = rvd_fence_server_sync;
   ws->base.fence_get_fd = rvd_fence_get_fd;
   ws->base.flush_frontbuffer = rvd_flush_frontbuffer;
   ws->base.supports_fences = 0;
   ws->base.supports_encoded_transfers = 1;   /* VIRGL_CCMD_TRANSFER3D reads the memory */
   ws->base.supports_coherent = 0;            /* no blob resources */
   return &ws->base;
}

struct pipe_screen *rvd_virgl_screen_create(void)
{
   struct virgl_winsys *vws = rvd_winsys_create();
   struct pipe_screen *screen;

   if (!vws)
      return NULL;
   screen = virgl_create_screen(vws, NULL);
   if (!screen)
      vws->destroy(vws);
   return screen;
}
