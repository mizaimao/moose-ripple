/*
 * Dispatch takeover (moose-ripple, M4; decision 0003, docs/re/gl-plugin-interface.md "Where
 * the takeover hooks in"). After Apple's CGLCreateContext has built a context's public
 * dispatch table, we install ours with OpenGL.framework's CGLSelectDispatch: it moves
 * GLEngine's table to the context's back table, where GLEngine's later rewrites land, and puts
 * ours in front, where libGL calls through it. Then the context's `rend` becomes our context,
 * which libGL passes as the first argument of every call.
 *
 * GLEngine never hands the driver the CGL object, so we find it through GLEngine's own
 * context. The offsets are those of 10.6.8 (10K549), x86_64 and i386; every step checks magic values
 * and refuses (leaving the context to GLEngine) if anything doesn't match.
 */
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util/simple_mtx.h"

#include "cgl_context.h"
#include "cgl_dispatch.h"
#include "cgl_takeover.h"

#define GLI_DISPATCH_ENTRIES   764

#if defined(__x86_64__)
#define CGL_MAGIC_OFFSET       0x18a0        /* in the CGLContextObject */
#define CGL_MAGIC              0x1b7f1843u
#define GC_MAGIC_OFFSET        0x6588        /* in GLEngine's context */
#define GC_MAGIC               0x3463a223u
#define GC_FRONT_DISPATCH      0x6550        /* holds &cgl->disp */
#define GC_BACK_DISPATCH       0x6558        /* holds the back table */
#define GC_FROM_CREATE_ARG5    0x8b0         /* gldCreateContext's 5th argument is gc + this */
#define HAVE_TAKEOVER_LAYOUT   1
#elif defined(__i386__)
#define CGL_MAGIC_OFFSET       0xc60
#define CGL_MAGIC              0x1b7f1843u
#define GC_MAGIC_OFFSET        0x5844
#define GC_MAGIC               0x3463a223u
#define GC_FRONT_DISPATCH      0x5820
#define GC_BACK_DISPATCH       0x5824
#define GC_FROM_CREATE_ARG5    0x890
#define HAVE_TAKEOVER_LAYOUT   1
#else
#define HAVE_TAKEOVER_LAYOUT   0
#endif

typedef int (*cgl_select_dispatch_fn)(void *cgl, void *table);
typedef void *(*cgl_back_dispatch_fn)(void *cgl);

struct cgl_object_head {        /* public part of struct _CGLContextObject (CGLContext.h) */
   void *rend;
   void *disp[GLI_DISPATCH_ENTRIES];
};

static void *dispatch_table[GLI_DISPATCH_ENTRIES];
static cgl_select_dispatch_fn select_dispatch;
static cgl_back_dispatch_fn back_dispatch;

/* Contexts taken over, by the GLEngine context pointer callers might pass as `rend` (CoreGraphics'
 * own contexts in WindowServer may): ripple_cgl_rend_for() maps it back. */
#define MAX_TAKEN 64
static struct {
   void *gc;
   struct ripple_cgl_context *ctx;
   void **shadow, **old_back;     /* CoreGraphics contexts: our back table, GLEngine's */
} taken[MAX_TAKEN];
static void *volatile last_gc;
static struct ripple_cgl_context *volatile last_ctx;
static simple_mtx_t taken_lock = SIMPLE_MTX_INITIALIZER;

static void
remember(void *gc, struct ripple_cgl_context *ctx, void **shadow, void **old_back)
{
   simple_mtx_lock(&taken_lock);
   for (unsigned i = 0; i < MAX_TAKEN; i++) {
      if (!taken[i].gc || taken[i].gc == gc) {
         taken[i].gc = gc;
         taken[i].ctx = ctx;
         taken[i].shadow = shadow;
         taken[i].old_back = old_back;
         break;
      }
   }
   simple_mtx_unlock(&taken_lock);
}

struct ripple_cgl_context *
ripple_cgl_rend_for(void *rend)
{
   /* Every GL call of a CoreGraphics context comes here: try the last hit first. */
   if (rend == last_gc)
      return last_ctx;
   struct ripple_cgl_context *ctx = NULL;
   simple_mtx_lock(&taken_lock);
   for (unsigned i = 0; i < MAX_TAKEN && !ctx; i++)
      if (taken[i].gc == rend)
         ctx = taken[i].ctx;
   if (ctx) {
      last_ctx = ctx;
      last_gc = rend;
   }
   simple_mtx_unlock(&taken_lock);
   return ctx;
}

void
ripple_cgl_forget(struct ripple_cgl_context *ctx)
{
   simple_mtx_lock(&taken_lock);
   last_gc = NULL;
   last_ctx = NULL;
   for (unsigned i = 0; i < MAX_TAKEN; i++) {
      if (taken[i].ctx != ctx)
         continue;
      if (taken[i].shadow) {
         /* GLEngine destroys its context after ours: hand it its own back pointer again. */
         *(void ***)((uint8_t *)taken[i].gc + GC_BACK_DISPATCH) = taken[i].old_back;
         free(taken[i].shadow);
      }
      memset(&taken[i], 0, sizeof(taken[i]));
   }
   simple_mtx_unlock(&taken_lock);
}

static int
dispatch_noop(void)
{
   return 0;
}

/* ---- CoreGraphics' gliSetInteger calls ---------------------------------------------------- *
 * cglsSetInteger(cgls, pname, values) jumps to GLEngine's gliSetInteger through a function
 * table whose pointer sits right after the cgls block's dispatch table (CoreGraphics 10.6.8,
 * x86_64: table + 0x58). WindowServer binds surfaces to textures that way (997-999, see
 * ripple_cgl_surface_texture); GLEngine can't do that for our contexts. The table entry is
 * replaced once (it is shared by CoreGraphics' contexts); everything we don't handle goes on
 * to GLEngine. */
#include <mach/mach.h>
#include <mach/mach_vm.h>

#if defined(__x86_64__)
#define CGLS_GLI_TABLE_AT      (sizeof(void *) * (1 + GLI_DISPATCH_ENTRIES))
#define GLI_SET_INTEGER_SLOT   (0x58 / sizeof(void *))
#endif

typedef int (*gli_set_integer_fn)(void *gc, int pname, const int *values);
static gli_set_integer_fn gli_set_integer;

static int
hook_gli_set_integer(void *gc, int pname, const int *values)
{
   if (pname >= 997 && pname <= 999 && values) {
      struct ripple_cgl_context *ctx = ripple_cgl_rend_for(gc);
      if (ctx)
         return ripple_cgl_surface_texture(ctx, (uint32_t)values[0], (GLenum)values[1],
                                           (GLenum)values[2], values[3], values[4],
                                           pname == 997, (GLenum)values[6]);
   }
   if (pname == 910 && values) {               /* CGLTexImageIOSurface2D */
      struct ripple_cgl_context *ctx = ripple_cgl_rend_for(gc);
      if (ctx)
         return ripple_cgl_iosurface_texture(ctx, (const uint32_t *)values);
   }
   return gli_set_integer ? gli_set_integer(gc, pname, values) : 10004;   /* kCGLBadContext */
}

/* The page holding `p` is mapped readable and writable in this task. */
static bool
writable(const void *p)
{
   mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)p;
   mach_vm_size_t size = 0;
   vm_region_basic_info_data_64_t info;
   mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
   mach_port_t object = MACH_PORT_NULL;
   return mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64,
                         (vm_region_info_t)&info, &count, &object) == KERN_SUCCESS &&
          a <= (mach_vm_address_t)(uintptr_t)p &&
          (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE);
}

/* Put hook_gli_set_integer in `table` at `index` if GLEngine's gliSetInteger is there. */
static void
hook_set_integer_in(void **table, unsigned index, const char *what)
{
   extern FILE *ripple_gld_dbg_file(void);
   void *fn = table && writable(&table[index]) ? table[index] : NULL;
   Dl_info dli;
   if (fn == (void *)hook_gli_set_integer)
      return;
   if (!fn || !dladdr(fn, &dli) || !dli.dli_sname || strcmp(dli.dli_sname, "gliSetInteger") != 0 ||
       (gli_set_integer && fn != (void *)gli_set_integer)) {
      if (ripple_gld_dbg_file())
         fprintf(ripple_gld_dbg_file(), "RippleGLDriver: %s gliSetInteger not found (table %p, "
                 "entry %p): surface textures stay GLEngine's\n", what, (void *)table, fn);
      return;
   }
   gli_set_integer = (gli_set_integer_fn)fn;
   table[index] = (void *)hook_gli_set_integer;
   if (ripple_gld_dbg_file())
      fprintf(ripple_gld_dbg_file(), "RippleGLDriver: %s gliSetInteger hooked (table %p)\n", what,
              (void *)table);
}

static void
hook_cgls_set_integer(void *cgls)
{
#if defined(__x86_64__)
   void **slot = (void **)((uint8_t *)cgls + CGLS_GLI_TABLE_AT);
   hook_set_integer_in(writable(slot) ? *slot : NULL, GLI_SET_INTEGER_SLOT, "cgls");
#endif
}

/* Public CGL contexts reach GLEngine's gliSetInteger through the plugin record's own function
 * table: CGLTexImagePBuffer sends gliSetInteger(997, {ID, target, format, width, height,
 * 0x8367, source, levels}) there (OpenGL 10K549, x86_64: ctx+0x17e8 private block, +0x78 the
 * records, record +0x10 the gli table, gliSetInteger at +0x48). Real Racing 2's Core Animation
 * layers, M6. */
static void
hook_cgl_set_integer(void *cgl)
{
#if defined(__x86_64__)
   void **priv = (void **)((uint8_t *)cgl + 0x17e8);
   uint8_t *p = writable(priv) ? *priv : NULL;
   void **recs = p && writable(p + 0x78) ? *(void **)(p + 0x78) : NULL;
   void **table = recs && writable((uint8_t *)recs + 0x10) ? *(void ***)((uint8_t *)recs + 0x10)
                                                          : NULL;
   hook_set_integer_in(table, 0x48 / sizeof(void *), "CGL");
#endif
}

__attribute__((constructor)) static void
ripple_cgl_takeover_init(void)
{
   if (ripple_cgl_dispatch_count != GLI_DISPATCH_ENTRIES) {
      fprintf(stderr, "RippleGLDriver: %u dispatch entries, expected %u\n",
              ripple_cgl_dispatch_count, GLI_DISPATCH_ENTRIES);
      return;
   }
   ripple_cgl_fill_dispatch(dispatch_table);
   select_dispatch = (cgl_select_dispatch_fn)dlsym(RTLD_DEFAULT, "CGLSelectDispatch");
   back_dispatch = (cgl_back_dispatch_fn)dlsym(RTLD_DEFAULT, "CGLBackDispatch");
}

void *
ripple_cgl_gc_from_create_arg(void *arg5)
{
#if HAVE_TAKEOVER_LAYOUT
   /* GLEngine writes its magic only after the renderers' contexts exist, so it is checked
    * at takeover time, not here. */
   if (arg5)
      return (uint8_t *)arg5 - GC_FROM_CREATE_ARG5;
#endif
   return NULL;
}

int
ripple_cgl_take_over(void *gc, struct ripple_cgl_context *ctx, bool cgls_only)
{
#if HAVE_TAKEOVER_LAYOUT
   if (!gc || !select_dispatch || !back_dispatch || !dispatch_table[0])
      return -1;
   if (*(uint32_t *)((uint8_t *)gc + GC_MAGIC_OFFSET) != GC_MAGIC)
      return -2;
   void **front = *(void ***)((uint8_t *)gc + GC_FRONT_DISPATCH);
   if (!front)
      return -3;
   struct cgl_object_head *cgl = (struct cgl_object_head *)((uint8_t *)front - sizeof(void *));
   if (cgls_only && *(uint32_t *)((uint8_t *)cgl + CGL_MAGIC_OFFSET) == CGL_MAGIC)
      return -14;
   if (*(uint32_t *)((uint8_t *)cgl + CGL_MAGIC_OFFSET) != CGL_MAGIC) {
      /* Not a CGL context: CoreGraphics' private "cgls" copy of CGL makes WindowServer's
       * contexts (docs/re/qe-decision.md, gl-plugin-interface.md). Its block is {gc, front
       * table, private data}; the back table it hands GLEngine is a shared all-zero constant,
       * so GLEngine writes every update into the front table, and CoreGraphics calls
       * front[n](gc, ...). Give GLEngine a writable back table of its own (its updates land
       * there), put ours in front, and leave the first word gc: cgls passes it to GLEngine. */
      void **back = *(void ***)((uint8_t *)gc + GC_BACK_DISPATCH);
      Dl_info dli;
      if (cgl->rend != gc)
         return -4;
      if (!back)
         return -10;
      for (unsigned i = 0; i < GLI_DISPATCH_ENTRIES; i++)
         if (back[i])
            return -11;
      if (!dladdr(back, &dli) || !dli.dli_fname || !strstr(dli.dli_fname, "CoreGraphics"))
         return -12;
      void **shadow = malloc(GLI_DISPATCH_ENTRIES * sizeof(void *));
      if (!shadow)
         return -13;
      for (unsigned i = 0; i < GLI_DISPATCH_ENTRIES; i++)
         shadow[i] = front[i] ? front[i] : (void *)dispatch_noop;
      *(void ***)((uint8_t *)gc + GC_BACK_DISPATCH) = shadow;
      for (unsigned i = 0; i < GLI_DISPATCH_ENTRIES; i++)
         front[i] = dispatch_table[i];
      remember(gc, ctx, shadow, back);
      if (cgls_only) {                    /* from an object call: GLEngine had the first calls */
         ctx->sync_table = shadow;
         ctx->sync_gc = gc;
      }
      hook_cgls_set_integer(cgl);
      return 1;
   }
   if (cgl->rend != gc)
      return -5;

   /* CGLCreateContext must have finished moving GLEngine's table to the front: the back
    * table is then all zero. */
   void **back = back_dispatch(cgl);
   if (!back)
      return -6;
   for (unsigned i = 0; i < GLI_DISPATCH_ENTRIES; i++)
      if (back[i])
         return -7;

   select_dispatch(cgl, dispatch_table);
   if (cgl->disp[0] != dispatch_table[0])
      return -8;
   cgl->rend = ctx;
   remember(gc, ctx, NULL, NULL);
   hook_cgl_set_integer(cgl);
   return 0;
#else
   return -9;
#endif
}

/* GLEngine's dispatch slots used to read its state (gliDispatch.h order). */
#define GLI_SLOT_GET_INTEGERV  104

void
ripple_cgl_sync_from_glengine(struct ripple_cgl_context *ctx)
{
   typedef void (*get_integerv_fn)(void *gc, GLenum pname, GLint *params);
   void *gc = ctx->sync_gc;
   void **table = ctx->sync_table;
   ctx->sync_gc = NULL;
   ctx->sync_table = NULL;
   if (!gc || !table || table[GLI_SLOT_GET_INTEGERV] == (void *)dispatch_noop)
      return;
   get_integerv_fn get = (get_integerv_fn)table[GLI_SLOT_GET_INTEGERV];
   static const GLenum bindings[][2] = {
      { GL_TEXTURE_1D, GL_TEXTURE_BINDING_1D },
      { GL_TEXTURE_2D, GL_TEXTURE_BINDING_2D },
      { GL_TEXTURE_3D, GL_TEXTURE_BINDING_3D },
      { GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BINDING_CUBE_MAP },
      { GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_BINDING_RECTANGLE_ARB },
   };
   GLint active = GL_TEXTURE0, fb = 0, rb = 0, names[5] = { 0 };
   get(gc, GL_ACTIVE_TEXTURE, &active);
   for (unsigned i = 0; i < 5; i++)
      get(gc, bindings[i][1], &names[i]);
   get(gc, GL_FRAMEBUFFER_BINDING_EXT, &fb);
   get(gc, GL_RENDERBUFFER_BINDING_EXT, &rb);

   /* Names GLEngine generated are unknown to Mesa; binding one creates it (compatibility). */
   if (active >= GL_TEXTURE0 && active < GL_TEXTURE0 + 32)
      CALL_ActiveTexture(GET_DISPATCH(), ((GLenum)active));
   for (unsigned i = 0; i < 5; i++)
      if (names[i] > 0)
         CALL_BindTexture(GET_DISPATCH(), (bindings[i][0], (GLuint)names[i]));
   if (fb > 0)
      CALL_BindFramebufferEXT(GET_DISPATCH(), (GL_FRAMEBUFFER_EXT, (GLuint)fb));
   if (rb > 0)
      CALL_BindRenderbufferEXT(GET_DISPATCH(), (GL_RENDERBUFFER_EXT, (GLuint)rb));

   extern FILE *ripple_gld_dbg_file(void);
   if (ripple_gld_dbg_file())
      fprintf(ripple_gld_dbg_file(), "RippleGLDriver: took GLEngine's bindings over: unit 0x%x, "
              "textures 1D %d 2D %d 3D %d cube %d rect %d, framebuffer %d, renderbuffer %d\n",
              active, names[0], names[1], names[2], names[3], names[4], fb, rb);
}
