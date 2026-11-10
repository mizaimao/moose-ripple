/*
 * The context config record GLEngine hands gldCreateContext (moose-ripple, M4). GLEngine reads
 * limits and capability bits from it for its own GL paths; after the dispatch takeover those
 * paths are bypassed, but GLEngine still sets up its context from it. We write what Apple's
 * software renderer (GLRendererFloat 1.6.13, `_gldSetConfigData`) writes for the same pixel
 * format, as read from 10.6.8's binaries (docs/re/gl-plugin-interface.md). GLEngine zeroes
 * the record first; offsets not written stay 0. Offsets are the same on i386 and x86_64 up to
 * 0x17f; the extension words that follow move with the pointer size.
 */
#include <stdint.h>
#include <string.h>

struct gld_pixel_format;

/* The fields of the pixel format node we use (layout: ripple_gl_target.c). */
struct gld_pixel_format_head {
   void *next;
   uint32_t renderer_id, flags, buffer_mode, color_mode, accum_mode, depth_mode, stencil_mode;
   uint16_t zero24, aux_buffers, sample_buffers, samples;
};

#if defined(__x86_64__)
#define CFG_EXTENSION_WORDS 0x198
#else
#define CFG_EXTENSION_WORDS 0x188
#endif

static void w8(uint8_t *c, unsigned off, uint8_t v) { c[off] = v; }
static void w16(uint8_t *c, unsigned off, uint16_t v) { memcpy(c + off, &v, 2); }
static void w32(uint8_t *c, unsigned off, uint32_t v) { memcpy(c + off, &v, 4); }
static void wf(uint8_t *c, unsigned off, float v) { memcpy(c + off, &v, 4); }
static void or32(uint8_t *c, unsigned off, uint32_t v)
{
   uint32_t old;
   memcpy(&old, c + off, 4);
   w32(c, off, old | v);
}

/* GLRendererFloat's color classes and the channel sizes it derives from them. */
static uint32_t
color_class(uint32_t mode)
{
   if (mode & 0x3c)       return 0x4;
   if (mode & 0x3fc0)     return 0x400;
   if (mode & 0xfc000)    return 0x8000;
   if (mode & 0x3f00000)  return 0x800000;
   if (mode & 0xc000000)  return 0x8000000;
   if (mode & 0x30000000) return 0x20000000;
   return 0;
}

static uint8_t
channel_bits(uint32_t cls, unsigned channel)
{
   switch (cls) {
   case 0x400:      return channel == 3 ? 1 : 5;
   case 0x8000:     return 8;
   case 0x8000000:  return 32;
   case 0x20000000: return 64;
   default:         return 0;
   }
}

static uint8_t
depth_bits(uint32_t mode)
{
   return mode & 0x1000 ? 32 : mode & 0x800 ? 24 : mode & 0x400 ? 16 : mode & 0x80 ? 8 : 0;
}

void
ripple_gld_fill_config(void *cfg, const struct gld_pixel_format *pf_opaque)
{
   const struct gld_pixel_format_head *pf = (const void *)pf_opaque;
   uint8_t *c = cfg;
   uint32_t color = color_class(pf->color_mode), accum = color_class(pf->accum_mode);
   unsigned k = pf->samples == 0 ? 0 : pf->samples <= 4 ? 2 : pf->samples <= 9 ? 3 : 4;

   w32(c, 0x00, 0xc);
   wf(c, 0x04, 1.0f);
   w32(c, 0x08, 0x4000);
   w32(c, 0x0c, 0x4000);
   w32(c, 0x10, 1);
   w32(c, 0x14, 1);
   w8(c, 0x18, 0xa);
   w8(c, 0x19, 8);
   w8(c, 0x1a, 8);
   w8(c, 0x1b, 0);
   w8(c, 0x1c, 0xc);
   w16(c, 0x1e, 0);
   w8(c, 0x2c, (pf->color_mode & 0x3ffffffc) != 0);
   w8(c, 0x2d, (pf->buffer_mode & 0x8) != 0);
   w8(c, 0x2e, 0);
   w8(c, 0x2f, (color & 0x3f000000) != 0);
   w8(c, 0x30, 0);
   w8(c, 0x31, channel_bits(color, 0) == 8);
   w8(c, 0x32, 0);
   w8(c, 0x33, (uint8_t)pf->aux_buffers);
   for (unsigned i = 0; i < 4; i++) {
      w8(c, 0x34 + i, channel_bits(color, i));
      w8(c, 0x38 + i, channel_bits(accum, i));
   }
   w8(c, 0x3c, depth_bits(pf->depth_mode));
   w8(c, 0x3d, depth_bits(pf->stencil_mode));
   w8(c, 0x3e, pf->sample_buffers != 0);
   w8(c, 0x3f, (uint8_t)(k * k));
   wf(c, 0x40, 0.1f);
   wf(c, 0x44, 64.0f);
   wf(c, 0x48, 0.0001f);
   wf(c, 0x4c, 0.1f);
   wf(c, 0x50, 64.0f);
   wf(c, 0x54, 0.0001f);
   w8(c, 0x58, 0);
   w8(c, 0x59, 0);
   w8(c, 0x5b, 1);
   w8(c, 0x5c, 0);
   w8(c, 0x5d, 1);
   w8(c, 0x5e, 1);
   w32(c, 0x68, 0x1000);
   w32(c, 0x6c, 0);
   w32(c, 0x70, 4);
   w32(c, 0x74, 0x13);
   w32(c, 0x78, 0);
   w32(c, 0x7c, 0x80);
   w32(c, 0x80, 0x80);
   w32(c, 0x84, 0x20);
   wf(c, 0x88, 16.0f);
   wf(c, 0x8c, 16.0f);
   w16(c, 0x90, 8);
   w16(c, 0x92, 0x10);
   w16(c, 0x94, 0x10);
   w16(c, 0x96, 8);
   w16(c, 0x98, 0x4000);
   w16(c, 0x9a, 0x4000);
   w16(c, 0x9c, 0x4000);
   w16(c, 0x9e, 0x4000);
   w16(c, 0xa0, 0x2000);
   w8(c, 0xa2, 5);
   w16(c, 0xa4, 0x83f0);
   w16(c, 0xa6, 0x83f1);
   w16(c, 0xa8, 0x83f2);
   w16(c, 0xaa, 0x83f3);
   w16(c, 0xac, 0x8837);
   w32(c, 0xc4, 0x4000);
   for (unsigned b = 0xc8; b <= 0xf8; b += 0x18) {
      w32(c, b + 0x00, 0x40000);
      w16(c, b + 0x04, 0x20);
      w16(c, b + 0x06, 0x400);
      w32(c, b + 0x08, 0xffff);
      w16(c, b + 0x0c, 0);
      w32(c, b + 0x10, 0);
      w32(c, b + 0x14, 0x100);
   }
   w32(c, 0x110, 0x40000);
   w32(c, 0x114, 0x40000);
   w32(c, 0x118, 0x40000);
   w16(c, 0x11c, 0);
   w16(c, 0x11e, 0);
   w32(c, 0x120, 0);
   w16(c, 0x124, 0x1000);
   w16(c, 0x12c, 1);
   w32(c, 0x130, 0x1000);
   w32(c, 0x134, 0x40);
   w32(c, 0x13c, 0x10);
   w32(c, 0x140, 0x40);
   w32(c, 0x144, 0x1000);
   w32(c, 0x148, 0x400);
   w32(c, 0x14c, 0x1000);
   w32(c, 0x154, 0x100000);
   w32(c, 0x158, 0xfffffff8);
   w32(c, 0x15c, 7);
   w32(c, 0x160, 0x40);
   w32(c, 0x168, 0x10);
   w32(c, 0x16c, 0x20);
   w32(c, 0x170, 0x40);
   w32(c, 0x174, 0x20);
   w32(c, 0x178, 0x20);
   w8(c, 0x17c, 1);
   w16(c, 0x17e, 1);
   /* The two pointer slots after 0x17f stay NULL, as GLRendererFloat leaves them. */
   or32(c, CFG_EXTENSION_WORDS + 0x0, 0xbfffffff);
   or32(c, CFG_EXTENSION_WORDS + 0x4, 0xff7bfffb);
   or32(c, CFG_EXTENSION_WORDS + 0x8, 0x3b5940cc);
   or32(c, CFG_EXTENSION_WORDS + 0xc, 0x3);
}
