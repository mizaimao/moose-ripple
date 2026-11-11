/*
 * Which of this process's window-server surfaces are not opaque (moose-ripple, M6).
 *
 * A GL surface is shown through a private window of ours (gld_window.c), which must be made
 * non-opaque when the surface is, or its transparent parts come out black. Nothing tells the
 * driver: Core Animation sets a layer-backed view's surface non-opaque with
 * CGSSetSurfaceOpacity directly, and CGLSetParameter(kCGLCPSurfaceOpacity) ends in the same
 * call inside OpenGL.framework. The getter, CGSGetSurfaceOpacity, can't be used: in 10.6.8 it
 * returns an error for every non-NULL result pointer (an inverted check). And making every
 * private window non-opaque costs a third of gl-gears' frame rate.
 *
 * So the setter is watched: the lazy symbol pointers to CGSSetSurfaceOpacity in every loaded
 * image are pointed at a function here, which notes the setting and calls the real one. A
 * surface set before this bundle was loaded is not known and counts as opaque, the default.
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "util/simple_mtx.h"

#include "gld_window.h"

#if defined(__LP64__)
typedef struct mach_header_64 mach_header_n;
typedef struct segment_command_64 segment_command_n;
typedef struct section_64 section_n;
typedef struct nlist_64 nlist_n;
#define LC_SEGMENT_N LC_SEGMENT_64
#else
typedef struct mach_header mach_header_n;
typedef struct segment_command segment_command_n;
typedef struct section section_n;
typedef struct nlist nlist_n;
#define LC_SEGMENT_N LC_SEGMENT
#endif

#define MAX_NOTED 128
static simple_mtx_t noted_lock = SIMPLE_MTX_INITIALIZER;
static struct { uint32_t cid, wid, sid; bool translucent; unsigned long stamp; } noted[MAX_NOTED];
static unsigned long noted_clock;

static int (*real_set_surface_opacity)(int cid, int wid, int sid, int opaque);

static int
hooked_set_surface_opacity(int cid, int wid, int sid, int opaque)
{
   unsigned slot = 0;

   simple_mtx_lock(&noted_lock);
   for (unsigned i = 0; i < MAX_NOTED; i++) {
      if (noted[i].stamp && noted[i].cid == (uint32_t)cid && noted[i].wid == (uint32_t)wid &&
          noted[i].sid == (uint32_t)sid) {
         slot = i;
         break;
      }
      if (noted[i].stamp < noted[slot].stamp)
         slot = i;                           /* a free one, or else the oldest */
   }
   noted[slot].cid = cid;
   noted[slot].wid = wid;
   noted[slot].sid = sid;
   noted[slot].translucent = (opaque & 0xff) == 0;      /* a C bool */
   noted[slot].stamp = ++noted_clock;
   simple_mtx_unlock(&noted_lock);
   return real_set_surface_opacity ? real_set_surface_opacity(cid, wid, sid, opaque) : 0;
}

bool
gld_surface_translucent(uint32_t cid, uint32_t wid, uint32_t sid)
{
   bool translucent = false;

   simple_mtx_lock(&noted_lock);
   for (unsigned i = 0; i < MAX_NOTED; i++) {
      if (noted[i].stamp && noted[i].cid == cid && noted[i].wid == wid && noted[i].sid == sid) {
         translucent = noted[i].translucent;
         break;
      }
   }
   simple_mtx_unlock(&noted_lock);
   return translucent;
}

/* dyld calls this for every image, loaded now or later: point its symbol pointers for
 * _CGSSetSurfaceOpacity at the hook. System frameworks on 10.6 import through
 * __DATA,__la_symbol_ptr on both architectures. */
static void
image_added(const struct mach_header *header, intptr_t slide)
{
   const mach_header_n *mh = (const mach_header_n *)header;
   const struct load_command *lc = (const struct load_command *)(mh + 1);
   const segment_command_n *linkedit = NULL;
   const struct symtab_command *symtab = NULL;
   const struct dysymtab_command *dysymtab = NULL;

   for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
      if (lc->cmd == LC_SEGMENT_N &&
          !strcmp(((const segment_command_n *)lc)->segname, SEG_LINKEDIT))
         linkedit = (const segment_command_n *)lc;
      else if (lc->cmd == LC_SYMTAB)
         symtab = (const struct symtab_command *)lc;
      else if (lc->cmd == LC_DYSYMTAB)
         dysymtab = (const struct dysymtab_command *)lc;
   }
   if (!linkedit || !symtab || !dysymtab || !dysymtab->nindirectsyms)
      return;
   uintptr_t base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
   const nlist_n *symbols = (const nlist_n *)(base + symtab->symoff);
   const char *strings = (const char *)(base + symtab->stroff);
   const uint32_t *indirect = (const uint32_t *)(base + dysymtab->indirectsymoff);

   lc = (const struct load_command *)(mh + 1);
   for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
      if (lc->cmd != LC_SEGMENT_N)
         continue;
      const segment_command_n *seg = (const segment_command_n *)lc;
      if (strcmp(seg->segname, SEG_DATA))
         continue;
      const section_n *sect = (const section_n *)(seg + 1);
      for (uint32_t s = 0; s < seg->nsects; s++, sect++) {
         uint32_t type = sect->flags & SECTION_TYPE;
         if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS)
            continue;
         void **slots = (void **)((uintptr_t)slide + sect->addr);
         uint32_t count = (uint32_t)(sect->size / sizeof(void *));
         for (uint32_t k = 0; k < count; k++) {
            if (sect->reserved1 + k >= dysymtab->nindirectsyms)
               break;
            uint32_t index = indirect[sect->reserved1 + k];
            if ((index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) || index >= symtab->nsyms)
               continue;
            uint32_t strx = symbols[index].n_un.n_strx;
            if (strx < symtab->strsize && !strcmp(strings + strx, "_CGSSetSurfaceOpacity") &&
                slots[k] != (void *)hooked_set_surface_opacity)
               slots[k] = (void *)hooked_set_surface_opacity;
         }
      }
   }
}

__attribute__((constructor)) static void
gld_opacity_init(void)
{
   *(void **)&real_set_surface_opacity = dlsym(RTLD_DEFAULT, "CGSSetSurfaceOpacity");
   if (real_set_surface_opacity)
      _dyld_register_func_for_add_image(image_added);
}
