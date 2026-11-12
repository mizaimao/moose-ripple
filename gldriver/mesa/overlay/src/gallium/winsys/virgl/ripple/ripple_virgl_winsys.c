/*
 * Mesa's virgl winsys over the moose-ripple GPU user client (docs/kext-uapi.md in the
 * moose-ripple repo, backend RIPPLE_GPU_BACKEND_VIRGL). Modelled on the DRM winsys
 * (../drm/virgl_drm_winsys.c):
 *   - a resource is a ripple buffer (virgl resource + guest backing), mapped with
 *     IOConnectMapMemory64;
 *   - the command stream goes to SUBMIT with the handles of the buffers it uses;
 *   - transfers go to TRANSFER, or into the stream as VIRGL_CCMD_TRANSFER3D;
 *   - fences are the kernel's fence numbers, waited for with WAIT.
 * The kernel side returns from SUBMIT before the GPU has run the commands (since 2026-11-11);
 * this winsys only relies on fences signalling in order and WAIT waiting for them.
 *
 * Copyright 2026 moose-ripple. MIT licence, as the rest of Mesa's virgl code.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "virgl/virgl_public.h"
#include "virgl/virgl_winsys.h"
#include "virtio-gpu/virgl_protocol.h"
#include "virgl_resource_cache.h"

#include "ripple_gpu_uapi.h"
#include "ripple_virgl_public.h"

/* Kernel limits (docs/kext-uapi.md, version 1). */
#define RIPPLE_MAX_HANDLES      4096
#define RIPPLE_MAX_CMD_BYTES    (320 * 1024)

/* Header and handle space kept in front of each command buffer, so a submit needs no copy. */
#define RIPPLE_PREFIX_BYTES     (sizeof(struct ripple_gpu_submit) + RIPPLE_MAX_HANDLES * 4)

#define CACHE_TIMEOUT_USEC      1000000

static unsigned work_count;       /* submits and uploads so far (ripple_virgl_work_count) */

/* GET_CAPS returns its blob inline (IOKit passes up to 4 KB that way). */
static_assert(sizeof(union virgl_caps) <= 4096, "virgl caps blob too big for GET_CAPS");

struct ripple_winsys {
   struct virgl_winsys base;
   io_connect_t conn;
   uint32_t ctx;
   struct ripple_gpu_info info;

   struct virgl_resource_cache cache;
   simple_mtx_t cache_mutex;

   simple_mtx_t fence_mutex;
   uint64_t signalled;                 /* every fence up to this one has signalled */
   uint64_t last_fence;                /* the newest fence number the kernel has given us */
};

struct virgl_hw_res {
   struct pipe_reference reference;
   enum pipe_texture_target target;
   uint32_t res_handle;                /* the ripple buffer handle = virgl resource id */
   uint32_t bind;
   uint32_t size;                      /* as requested */
   void *ptr;                          /* mapping, made on first use */
   uint64_t fence;                     /* last fence that used it */
   int num_cs_references;
   struct virgl_resource_cache_entry cache_entry;
};

struct ripple_fence {
   struct pipe_reference reference;
   uint64_t id;
};

struct ripple_cmd_buf {
   struct virgl_cmd_buf base;
   struct ripple_winsys *ws;
   uint8_t *mem;                       /* RIPPLE_PREFIX_BYTES, then the command dwords */

   unsigned nres, cres;
   struct virgl_hw_res **res;
   uint32_t *handles;
   char is_added[512];
   unsigned hash_index[512];
};

#define cache_entry_container_res(ptr) \
   (struct virgl_hw_res *)((char *)(ptr) - offsetof(struct virgl_hw_res, cache_entry))

static inline struct ripple_winsys *ripple_winsys(struct virgl_winsys *vws)
{
   return (struct ripple_winsys *)vws;
}

static inline struct ripple_cmd_buf *ripple_cmd_buf(struct virgl_cmd_buf *cbuf)
{
   return (struct ripple_cmd_buf *)cbuf;
}

static inline struct ripple_fence *ripple_fence(struct pipe_fence_handle *f)
{
   return (struct ripple_fence *)f;
}

/* ---- Fences -------------------------------------------------------------------------- */

static bool ripple_fence_done(struct ripple_winsys *ws, uint64_t id, uint32_t timeout_ms)
{
   bool done;

   simple_mtx_lock(&ws->fence_mutex);
   done = id <= ws->signalled;
   simple_mtx_unlock(&ws->fence_mutex);
   if (done)
      return true;

   uint64_t in[2] = { id, timeout_ms }, out = 0;
   uint32_t out_cnt = 1;
   if (IOConnectCallScalarMethod(ws->conn, RIPPLE_GPU_WAIT, in, 2, &out, &out_cnt) != KERN_SUCCESS)
      return true;                      /* the device is gone; nothing left to wait for */
   if (!out)
      return false;

   simple_mtx_lock(&ws->fence_mutex);
   if (id > ws->signalled)
      ws->signalled = id;
   simple_mtx_unlock(&ws->fence_mutex);
   return true;
}

/* Every submit and transfer gets a fence number from the kernel, in queue order. */
static void ripple_note_fence(struct ripple_winsys *ws, uint64_t id)
{
   simple_mtx_lock(&ws->fence_mutex);
   if (id > ws->last_fence)
      ws->last_fence = id;
   simple_mtx_unlock(&ws->fence_mutex);
}

static void ripple_fence_wait_forever(struct ripple_winsys *ws, uint64_t id)
{
   /* Not for ever after all: when the device has stopped answering, WAIT fails at once and
    * this was a tight loop of system calls with no way out (audit 2026-11-11). Give up after
    * a minute's worth of tries, as Mesa's own winsys does. */
   for (unsigned tries = 0; !ripple_fence_done(ws, id, 1000); tries++) {
      if (tries >= 60) {
         debug_printf("ripple: fence %llu never signalled; going on without it\n",
                      (unsigned long long)id);
         return;
      }
      usleep(10000);
   }
}

static struct pipe_fence_handle *ripple_fence_create(uint64_t id)
{
   struct ripple_fence *fence = CALLOC_STRUCT(ripple_fence);

   if (!fence)
      return NULL;
   pipe_reference_init(&fence->reference, 1);
   fence->id = id;
   return (struct pipe_fence_handle *)fence;
}

static struct pipe_fence_handle *ripple_cs_create_fence(struct virgl_winsys *vws, int fd)
{
   return NULL;                         /* no sync file descriptors here */
}

static bool ripple_fence_wait(struct virgl_winsys *vws, struct pipe_fence_handle *_fence,
                              uint64_t timeout)
{
   struct ripple_winsys *ws = ripple_winsys(vws);
   struct ripple_fence *fence = ripple_fence(_fence);

   if (timeout == 0)
      return ripple_fence_done(ws, fence->id, 0);
   if (timeout == OS_TIMEOUT_INFINITE) {
      ripple_fence_wait_forever(ws, fence->id);
      return true;
   }
   uint64_t ms = (timeout + 999999) / 1000000;
   return ripple_fence_done(ws, fence->id, ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms);
}

static void ripple_fence_reference(struct virgl_winsys *vws, struct pipe_fence_handle **dst,
                                   struct pipe_fence_handle *src)
{
   struct ripple_fence *d = ripple_fence(*dst), *s = ripple_fence(src);

   if (pipe_reference(d ? &d->reference : NULL, s ? &s->reference : NULL))
      FREE(d);
   *dst = src;
}

static void ripple_fence_server_sync(struct virgl_winsys *vws, struct virgl_cmd_buf *cbuf,
                                     struct pipe_fence_handle *fence)
{
   /* One connection, fences in order: nothing to do. */
}

static int ripple_fence_get_fd(struct virgl_winsys *vws, struct pipe_fence_handle *fence)
{
   return -1;
}

/* ---- Resources ----------------------------------------------------------------------- */

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

static void ripple_hw_res_destroy(struct ripple_winsys *ws, struct virgl_hw_res *res)
{
   if (res->ptr)
      IOConnectUnmapMemory64(ws->conn, res->res_handle, mach_task_self(),
                             (mach_vm_address_t)(uintptr_t)res->ptr);
   uint64_t in = res->res_handle;
   IOConnectCallScalarMethod(ws->conn, RIPPLE_GPU_BUFFER_DESTROY, &in, 1, NULL, NULL);
   FREE(res);
}

static bool ripple_resource_is_busy(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   uint64_t fence = p_atomic_read(&res->fence);

   return fence && !ripple_fence_done(ripple_winsys(vws), fence, 0);
}

static void ripple_resource_wait(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   uint64_t fence = p_atomic_read(&res->fence);

   if (fence)
      ripple_fence_wait_forever(ripple_winsys(vws), fence);
}

static void ripple_resource_reference(struct virgl_winsys *vws, struct virgl_hw_res **dres,
                                      struct virgl_hw_res *sres)
{
   struct ripple_winsys *ws = ripple_winsys(vws);
   struct virgl_hw_res *old = *dres;

   if (pipe_reference(old ? &old->reference : NULL, sres ? &sres->reference : NULL)) {
      if (!can_cache_resource(old->bind)) {
         ripple_hw_res_destroy(ws, old);
      } else {
         simple_mtx_lock(&ws->cache_mutex);
         virgl_resource_cache_add(&ws->cache, &old->cache_entry);
         simple_mtx_unlock(&ws->cache_mutex);
      }
   }
   *dres = sres;
}

static struct virgl_hw_res *
ripple_resource_create_new(struct ripple_winsys *ws, struct virgl_resource_params params)
{
   struct virgl_hw_res *res = CALLOC_STRUCT(virgl_hw_res);
   struct ripple_gpu_buffer_create bc;
   struct ripple_virgl_buffer_params *p = (struct ripple_virgl_buffer_params *)bc.params;
   uint64_t out[2];
   uint32_t out_cnt = 2;

   if (!res)
      return NULL;
   memset(&bc, 0, sizeof(bc));
   bc.size = params.size ? params.size : 1;
   bc.params_size = sizeof(*p);
   p->target = params.target;
   p->format = pipe_to_virgl_format(params.format);
   p->bind = params.bind;
   p->width = params.width;
   p->height = params.height;
   p->depth = params.depth;
   p->array_size = params.array_size;
   p->last_level = params.last_level;
   p->nr_samples = params.nr_samples;
   p->flags = 0;                        /* the map flags are for blob resources only */

   if (IOConnectCallMethod(ws->conn, RIPPLE_GPU_BUFFER_CREATE, NULL, 0, &bc, sizeof(bc),
                           out, &out_cnt, NULL, NULL) != KERN_SUCCESS) {
      FREE(res);
      return NULL;
   }
   pipe_reference_init(&res->reference, 1);
   res->target = params.target;
   res->res_handle = (uint32_t)out[0];
   res->bind = params.bind;
   res->size = params.size;
   p_atomic_set(&res->num_cs_references, 0);
   virgl_resource_cache_entry_init(&res->cache_entry, params);
   return res;
}

static struct virgl_hw_res *
ripple_resource_create(struct virgl_winsys *vws, enum pipe_texture_target target,
                       const void *map_front_private, uint32_t format, uint32_t bind,
                       uint32_t width, uint32_t height, uint32_t depth, uint32_t array_size,
                       uint32_t last_level, uint32_t nr_samples, uint32_t flags, uint32_t size)
{
   struct ripple_winsys *ws = ripple_winsys(vws);
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
   return ripple_resource_create_new(ws, params);
}

static void *ripple_resource_map(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   struct ripple_winsys *ws = ripple_winsys(vws);
   mach_vm_address_t addr = 0;
   mach_vm_size_t size = 0;

   if (res->ptr)
      return res->ptr;
   if (IOConnectMapMemory64(ws->conn, res->res_handle, mach_task_self(), &addr, &size,
                            kIOMapAnywhere) != KERN_SUCCESS)
      return NULL;
   res->ptr = (void *)(uintptr_t)addr;
   return res->ptr;
}

static int ripple_transfer(struct virgl_winsys *vws, struct virgl_hw_res *res, uint32_t dir,
                           const struct pipe_box *box, uint32_t stride, uint32_t layer_stride,
                           uint32_t buf_offset, uint32_t level)
{
   struct ripple_winsys *ws = ripple_winsys(vws);
   struct ripple_gpu_transfer t;
   uint64_t fence = 0;
   uint32_t out_cnt = 1;

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
    * backing itself (stride and layer_stride 0). */
   t.offset = buf_offset;
   if (IOConnectCallMethod(ws->conn, RIPPLE_GPU_TRANSFER, NULL, 0, &t, sizeof(t), &fence,
                           &out_cnt, NULL, NULL) != KERN_SUCCESS)
      return -1;
   /* Counted once the kernel has it, not before: another thread could show the screen in
    * between and remember a count this work had not reached the GPU for, and the display
    * that follows the work would then be skipped (audit 2026-11-11). */
   if (dir == RIPPLE_GPU_TO_DEVICE)
      p_atomic_inc(&work_count);
   p_atomic_set(&res->fence, fence);
   ripple_note_fence(ws, fence);
   return 0;
}

static int ripple_transfer_put(struct virgl_winsys *vws, struct virgl_hw_res *res,
                               const struct pipe_box *box, uint32_t stride,
                               uint32_t layer_stride, uint32_t buf_offset, uint32_t level)
{
   return ripple_transfer(vws, res, RIPPLE_GPU_TO_DEVICE, box, stride, layer_stride,
                          buf_offset, level);
}

static int ripple_transfer_get(struct virgl_winsys *vws, struct virgl_hw_res *res,
                               const struct pipe_box *box, uint32_t stride,
                               uint32_t layer_stride, uint32_t buf_offset, uint32_t level)
{
   return ripple_transfer(vws, res, RIPPLE_GPU_FROM_DEVICE, box, stride, layer_stride,
                          buf_offset, level);
}

static uint32_t ripple_resource_get_storage_size(struct virgl_winsys *vws,
                                                 struct virgl_hw_res *res)
{
   return res->size;
}

/* Sharing resources between processes isn't supported (yet). */
static struct virgl_hw_res *
ripple_resource_create_from_handle(struct virgl_winsys *vws, struct winsys_handle *whandle,
                                   struct pipe_resource *templ, uint32_t *plane,
                                   uint32_t *stride, uint32_t *plane_offset,
                                   uint64_t *modifier, uint32_t *blob_mem)
{
   return NULL;
}

static void ripple_resource_set_type(struct virgl_winsys *vws, struct virgl_hw_res *res,
                                     uint32_t format, uint32_t bind, uint32_t width,
                                     uint32_t height, uint32_t usage, uint64_t modifier,
                                     uint32_t plane_count, const uint32_t *plane_strides,
                                     const uint32_t *plane_offsets)
{
}

static bool ripple_resource_get_handle(struct virgl_winsys *vws, struct virgl_hw_res *res,
                                       uint32_t stride, struct winsys_handle *whandle)
{
   return false;
}

/* ---- Command buffers ----------------------------------------------------------------- */

static struct virgl_cmd_buf *ripple_cmd_buf_create(struct virgl_winsys *vws, uint32_t size)
{
   struct ripple_cmd_buf *cbuf;

   if (size * 4 > RIPPLE_MAX_CMD_BYTES)
      return NULL;
   cbuf = CALLOC_STRUCT(ripple_cmd_buf);
   if (!cbuf)
      return NULL;
   cbuf->ws = ripple_winsys(vws);
   cbuf->nres = 512;
   cbuf->res = CALLOC(cbuf->nres, sizeof(*cbuf->res));
   cbuf->handles = MALLOC(cbuf->nres * sizeof(*cbuf->handles));
   cbuf->mem = MALLOC(RIPPLE_PREFIX_BYTES + size * 4);
   if (!cbuf->res || !cbuf->handles || !cbuf->mem) {
      FREE(cbuf->res);
      FREE(cbuf->handles);
      FREE(cbuf->mem);
      FREE(cbuf);
      return NULL;
   }
   cbuf->base.buf = (uint32_t *)(cbuf->mem + RIPPLE_PREFIX_BYTES);
   return &cbuf->base;
}

static void ripple_clear_res_list(struct ripple_cmd_buf *cbuf, uint64_t fence)
{
   for (unsigned i = 0; i < cbuf->cres; i++) {
      if (fence)
         p_atomic_set(&cbuf->res[i]->fence, fence);
      p_atomic_dec(&cbuf->res[i]->num_cs_references);
      ripple_resource_reference(&cbuf->ws->base, &cbuf->res[i], NULL);
   }
   cbuf->cres = 0;
   memset(cbuf->is_added, 0, sizeof(cbuf->is_added));
}

static void ripple_cmd_buf_destroy(struct virgl_cmd_buf *_cbuf)
{
   struct ripple_cmd_buf *cbuf = ripple_cmd_buf(_cbuf);

   ripple_clear_res_list(cbuf, 0);
   FREE(cbuf->res);
   FREE(cbuf->handles);
   FREE(cbuf->mem);
   FREE(cbuf);
}

static bool ripple_lookup_res(struct ripple_cmd_buf *cbuf, struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle & (sizeof(cbuf->is_added) - 1);

   if (!cbuf->is_added[hash])
      return false;
   unsigned i = cbuf->hash_index[hash];
   if (i < cbuf->cres && cbuf->res[i] == res)
      return true;
   for (i = 0; i < cbuf->cres; i++) {
      if (cbuf->res[i] == res) {
         cbuf->hash_index[hash] = i;
         return true;
      }
   }
   return false;
}

static void ripple_add_res(struct ripple_cmd_buf *cbuf, struct virgl_hw_res *res)
{
   unsigned hash = res->res_handle & (sizeof(cbuf->is_added) - 1);

   if (cbuf->cres >= cbuf->nres) {
      unsigned n = cbuf->nres + 256;
      void *r = REALLOC(cbuf->res, cbuf->nres * sizeof(*cbuf->res), n * sizeof(*cbuf->res));
      if (!r)
         return;
      cbuf->res = r;
      void *h = REALLOC(cbuf->handles, cbuf->nres * sizeof(*cbuf->handles),
                        n * sizeof(*cbuf->handles));
      if (!h)
         return;
      cbuf->handles = h;
      cbuf->nres = n;
   }
   cbuf->res[cbuf->cres] = NULL;
   ripple_resource_reference(&cbuf->ws->base, &cbuf->res[cbuf->cres], res);
   cbuf->handles[cbuf->cres] = res->res_handle;
   cbuf->is_added[hash] = true;
   cbuf->hash_index[hash] = cbuf->cres;
   p_atomic_inc(&res->num_cs_references);
   cbuf->cres++;
}

static void ripple_emit_res(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf,
                            struct virgl_hw_res *res, bool write_buf)
{
   struct ripple_cmd_buf *cbuf = ripple_cmd_buf(_cbuf);
   bool listed = ripple_lookup_res(cbuf, res);

   if (write_buf)
      cbuf->base.buf[cbuf->base.cdw++] = res->res_handle;
   if (!listed)
      ripple_add_res(cbuf, res);
}

static bool ripple_res_is_referenced(struct virgl_winsys *vws, struct virgl_cmd_buf *cbuf,
                                     struct virgl_hw_res *res)
{
   return p_atomic_read(&res->num_cs_references) != 0;
}

unsigned
ripple_virgl_work_count(void)
{
   return p_atomic_read(&work_count);
}

/* One SUBMIT. `cmds` must have RIPPLE_PREFIX_BYTES of room in front of it. */
static int ripple_submit(struct ripple_winsys *ws, const uint32_t *handles, unsigned nh,
                         uint32_t *cmds, unsigned ndw, uint64_t *fence)
{
   uint8_t *start = (uint8_t *)cmds - nh * 4 - sizeof(struct ripple_gpu_submit);
   struct ripple_gpu_submit *s = (struct ripple_gpu_submit *)start;
   uint32_t out_cnt = 1;

   memcpy(start + sizeof(*s), handles, nh * 4);
   s->context = ws->ctx;
   s->num_handles = nh;
   s->cmd_size = ndw * 4;
   s->flags = 0;
   kern_return_t kr = IOConnectCallMethod(ws->conn, RIPPLE_GPU_SUBMIT, NULL, 0, start,
                                          sizeof(*s) + nh * 4 + ndw * 4, fence, &out_cnt, NULL,
                                          NULL);
   p_atomic_inc(&work_count);          /* after the call: see ripple_transfer */
   if (kr == KERN_SUCCESS && fence)
      ripple_note_fence(ws, *fence);
   return kr == KERN_SUCCESS ? 0 : -1;
}

static int ripple_submit_cmd(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf,
                             struct pipe_fence_handle **fence)
{
   struct ripple_winsys *ws = ripple_winsys(vws);
   struct ripple_cmd_buf *cbuf = ripple_cmd_buf(_cbuf);
   uint64_t id = 0;
   unsigned first = 0;
   int ret = 0;

   if (cbuf->base.cdw == 0)
      return 0;

   /* More buffers than one SUBMIT can name: attach the first ones with NOP submits. */
   if (cbuf->cres > RIPPLE_MAX_HANDLES) {
      uint8_t *nop = MALLOC(RIPPLE_PREFIX_BYTES + 4);
      if (!nop) {
         ret = -1;
         goto out;
      }
      uint32_t *cmd = (uint32_t *)(nop + RIPPLE_PREFIX_BYTES);
      while (!ret && cbuf->cres - first > RIPPLE_MAX_HANDLES) {
         cmd[0] = VIRGL_CMD0(VIRGL_CCMD_NOP, 0, 0);
         ret = ripple_submit(ws, cbuf->handles + first, RIPPLE_MAX_HANDLES, cmd, 1, &id);
         first += RIPPLE_MAX_HANDLES;
      }
      FREE(nop);
   }
   if (!ret)
      ret = ripple_submit(ws, cbuf->handles + first, cbuf->cres - first, cbuf->base.buf,
                          cbuf->base.cdw, &id);
out:
   if (ret)
      debug_printf("ripple: SUBMIT failed - expect bad rendering\n");
   cbuf->base.cdw = 0;
   if (fence && !ret)
      *fence = ripple_fence_create(id);
   ripple_clear_res_list(cbuf, ret ? 0 : id);
   return ret;
}

static void ripple_flush_frontbuffer(struct virgl_winsys *vws, struct virgl_cmd_buf *cbuf,
                                     struct virgl_hw_res *res, unsigned level, unsigned layer,
                                     void *winsys_drawable_handle, struct pipe_box *sub_box)
{
   /* Presentation is the CGL frontend's job (it copies finished frames itself). */
}

/* ---- Device -------------------------------------------------------------------------- */

static int ripple_get_caps(struct virgl_winsys *vws, struct virgl_drm_caps *caps)
{
   struct ripple_winsys *ws = ripple_winsys(vws);
   uint32_t id = 1, version = 0;

   virgl_ws_fill_new_caps_defaults(caps);
   for (unsigned i = 0; i < ws->info.num_caps && i < 8; i++) {
      if (ws->info.caps_ids[i] == 2 || (ws->info.caps_ids[i] == 1 && id != 2)) {
         id = ws->info.caps_ids[i];
         version = ws->info.caps_versions[i];
      }
   }
   uint64_t in[2] = { id, version };
   size_t size = id == 2 ? sizeof(union virgl_caps) : sizeof(struct virgl_caps_v1);
   return IOConnectCallMethod(ws->conn, RIPPLE_GPU_GET_CAPS, in, 2, NULL, 0, NULL, NULL,
                              &caps->caps, &size) == KERN_SUCCESS ? 0 : -1;
}

static int ripple_get_fd(struct virgl_winsys *vws)
{
   return -1;
}

static bool ripple_cache_entry_is_busy(struct virgl_resource_cache_entry *entry, void *user)
{
   struct ripple_winsys *ws = user;

   return ripple_resource_is_busy(&ws->base, cache_entry_container_res(entry));
}

static void ripple_cache_entry_release(struct virgl_resource_cache_entry *entry, void *user)
{
   ripple_hw_res_destroy(user, cache_entry_container_res(entry));
}

static void ripple_winsys_destroy(struct virgl_winsys *vws)
{
   struct ripple_winsys *ws = ripple_winsys(vws);

   virgl_resource_cache_flush(&ws->cache);
   uint64_t in = ws->ctx;
   IOConnectCallScalarMethod(ws->conn, RIPPLE_GPU_CONTEXT_DESTROY, &in, 1, NULL, NULL);
   IOServiceClose(ws->conn);
   simple_mtx_destroy(&ws->cache_mutex);
   simple_mtx_destroy(&ws->fence_mutex);
   FREE(ws);
}

static struct virgl_winsys *ripple_winsys_create(io_connect_t conn,
                                                 const struct ripple_gpu_info *info)
{
   struct ripple_winsys *ws;
   uint64_t in = 0, ctx = 0;
   uint32_t out_cnt = 1;

   if (info->backend != RIPPLE_GPU_BACKEND_VIRGL)
      return NULL;
   if (IOConnectCallScalarMethod(conn, RIPPLE_GPU_CONTEXT_CREATE, &in, 1, &ctx, &out_cnt)
       != KERN_SUCCESS)
      return NULL;
   ws = CALLOC_STRUCT(ripple_winsys);
   if (!ws) {
      in = ctx;
      IOConnectCallScalarMethod(conn, RIPPLE_GPU_CONTEXT_DESTROY, &in, 1, NULL, NULL);
      return NULL;
   }
   ws->conn = conn;
   ws->ctx = (uint32_t)ctx;
   ws->info = *info;
   virgl_resource_cache_init(&ws->cache, CACHE_TIMEOUT_USEC, ripple_cache_entry_is_busy,
                             ripple_cache_entry_release, ws);
   simple_mtx_init(&ws->cache_mutex, mtx_plain);
   simple_mtx_init(&ws->fence_mutex, mtx_plain);

   ws->base.destroy = ripple_winsys_destroy;
   ws->base.get_fd = ripple_get_fd;
   ws->base.transfer_put = ripple_transfer_put;
   ws->base.transfer_get = ripple_transfer_get;
   ws->base.resource_create = ripple_resource_create;
   ws->base.resource_reference = ripple_resource_reference;
   ws->base.resource_map = ripple_resource_map;
   ws->base.resource_wait = ripple_resource_wait;
   ws->base.resource_is_busy = ripple_resource_is_busy;
   ws->base.resource_create_from_handle = ripple_resource_create_from_handle;
   ws->base.resource_set_type = ripple_resource_set_type;
   ws->base.resource_get_handle = ripple_resource_get_handle;
   ws->base.resource_get_storage_size = ripple_resource_get_storage_size;
   ws->base.cmd_buf_create = ripple_cmd_buf_create;
   ws->base.cmd_buf_destroy = ripple_cmd_buf_destroy;
   ws->base.emit_res = ripple_emit_res;
   ws->base.submit_cmd = ripple_submit_cmd;
   ws->base.res_is_referenced = ripple_res_is_referenced;
   ws->base.get_caps = ripple_get_caps;
   ws->base.cs_create_fence = ripple_cs_create_fence;
   ws->base.fence_wait = ripple_fence_wait;
   ws->base.fence_reference = ripple_fence_reference;
   ws->base.fence_server_sync = ripple_fence_server_sync;
   ws->base.fence_get_fd = ripple_fence_get_fd;
   ws->base.flush_frontbuffer = ripple_flush_frontbuffer;
   ws->base.supports_fences = 0;
   ws->base.supports_encoded_transfers = 1;   /* VIRGL_CCMD_TRANSFER3D reads the backing */
   ws->base.supports_coherent = 0;            /* no blob resources */
   return &ws->base;
}

/* 64-bit float vertex formats (GL_DOUBLE through glVertexAttribPointer: Core Animation sends
 * its positions that way) are taken as-is by the virgl driver for any plain format, and the
 * host drew nothing with them (Real Racing 2's intro video, M6). Reported unsupported, so
 * u_vbuf converts them to 32-bit floats before they go out. */
static bool (*virgl_is_format_supported)(struct pipe_screen *, enum pipe_format,
                                         enum pipe_texture_target, unsigned, unsigned, unsigned);

static bool
ripple_is_format_supported(struct pipe_screen *screen, enum pipe_format format,
                           enum pipe_texture_target target, unsigned samples,
                           unsigned storage_samples, unsigned bind)
{
   if ((bind & PIPE_BIND_VERTEX_BUFFER) &&
       (format == PIPE_FORMAT_R64_FLOAT || format == PIPE_FORMAT_R64G64_FLOAT ||
        format == PIPE_FORMAT_R64G64B64_FLOAT || format == PIPE_FORMAT_R64G64B64A64_FLOAT))
      return false;
   return virgl_is_format_supported(screen, format, target, samples, storage_samples, bind);
}

struct pipe_screen *ripple_virgl_screen_create(io_connect_t conn,
                                               const struct ripple_gpu_info *info,
                                               const struct pipe_screen_config *config)
{
   struct virgl_winsys *vws = ripple_winsys_create(conn, info);
   struct pipe_screen *screen;

   if (!vws) {
      IOServiceClose(conn);
      return NULL;
   }
   screen = virgl_create_screen(vws, config);
   if (!screen) {
      vws->destroy(vws);
      return NULL;
   }
   virgl_is_format_supported = screen->is_format_supported;
   screen->is_format_supported = ripple_is_format_supported;
   return screen;
}

/* ---- For the CGL frontend: the screen's connection and a resource's buffer handle ------- */

#include "virgl/virgl_resource.h"
#include "virgl/virgl_screen.h"

io_connect_t ripple_virgl_screen_connection(struct pipe_screen *screen)
{
   return ripple_winsys(virgl_screen(screen)->vws)->conn;
}

void ripple_virgl_wait_idle(struct pipe_screen *screen)
{
   struct ripple_winsys *ws = ripple_winsys(virgl_screen(screen)->vws);
   uint64_t id;

   simple_mtx_lock(&ws->fence_mutex);
   id = ws->last_fence;
   simple_mtx_unlock(&ws->fence_mutex);
   if (id)
      ripple_fence_wait_forever(ws, id);      /* no kernel call if it has signalled already */
}

uint32_t ripple_virgl_screen_context(struct pipe_screen *screen)
{
   return ripple_winsys(virgl_screen(screen)->vws)->ctx;
}

uint32_t ripple_virgl_resource_handle(struct pipe_resource *res)
{
   struct virgl_resource *vres = virgl_resource(res);
   return vres && vres->hw_res ? vres->hw_res->res_handle : 0;
}
