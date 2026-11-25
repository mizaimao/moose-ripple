/*
 * moose-ripple
 *
 * DXT1, DXT3 and DXT5 blocks to R8G8B8A8 texels, a block at a time, for Mesa's compressed
 * format fallback (_mesa_unpack_s3tc): on a host renderer without S3TC (UTM's, ANGLE on
 * Apple's GPUs) Mesa keeps a DXT texture as R8G8B8A8 and decodes each upload on the CPU.
 *
 * Mesa's own unpack (util_format_dxt*_unpack_rgba_8unorm) fetches one texel at a time and works
 * out the block's colours again for each of the sixteen. Here the four colours and the sixteen
 * alpha values of a block are worked out once. The arithmetic is that of Mesa's
 * src/util/format/texcompress_s3tc_tmp.h, so the texels are the same;
 * gldriver/mesa/tests/s3tc-block-check.c compares the two. The same decoder serves the
 * Windows Vista/7 driver (windows/mesa/.../D3D9Dxt.h).
 *
 * Debug switches, read once per process:
 *   /Library/Logs/RippleGL/dxtmesa   use Mesa's texel-at-a-time unpack instead
 *   /Library/Logs/RippleGL/dxtstats  keep /Library/Logs/RippleGL/dxt-PROGRAM-PID.log up to date
 *                                    with the uploads, texels and time spent decoding
 *
 * MIT licence (LICENSE at the repository root).
 */
#ifndef RIPPLE_S3TC_H
#define RIPPLE_S3TC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

enum ripple_s3tc_kind {
   RIPPLE_S3TC_DXT1_RGB,    /* 8 bytes a block, alpha always one */
   RIPPLE_S3TC_DXT1_RGBA,   /* 8 bytes a block, one bit of alpha */
   RIPPLE_S3TC_DXT3,        /* 16 bytes a block, four bits of alpha a texel */
   RIPPLE_S3TC_DXT5,        /* 16 bytes a block, alpha interpolated between two values */
};

/* One 16-bit colour (5:6:5) to three bytes, the low bits filled from the high ones. */
static inline void
ripple_s3tc_colour(unsigned packed, uint8_t rgb[3])
{
   rgb[0] = (uint8_t)(((packed >> 8) & 0xf8) | ((packed >> 13) & 0x7));
   rgb[1] = (uint8_t)(((packed >> 3) & 0xfc) | ((packed >> 9) & 0x3));
   rgb[2] = (uint8_t)(((packed << 3) & 0xf8) | ((packed >> 2) & 0x7));
}

/* width by height texels from rows of blocks (src_stride bytes from one row of blocks to the
 * next) to rows of R8G8B8A8 texels (dst_stride bytes a row). A width or height that is not a
 * multiple of four takes the part of the last blocks that it covers. */
static inline void
ripple_s3tc_decode(enum ripple_s3tc_kind kind, uint8_t *dst, size_t dst_stride,
                   const uint8_t *src, size_t src_stride, unsigned width, unsigned height)
{
   const bool dxt1 = kind == RIPPLE_S3TC_DXT1_RGB || kind == RIPPLE_S3TC_DXT1_RGBA;
   const size_t block_size = dxt1 ? 8 : 16;

   for (unsigned y = 0; y < height; y += 4) {
      const uint8_t *block = src + (size_t)(y / 4) * src_stride;
      const unsigned rows = height - y < 4 ? height - y : 4;

      for (unsigned x = 0; x < width; x += 4, block += block_size) {
         const unsigned columns = width - x < 4 ? width - x : 4;
         const uint8_t *colour = dxt1 ? block : block + 8;
         const unsigned colour0 = colour[0] | (unsigned)colour[1] << 8;
         const unsigned colour1 = colour[2] | (unsigned)colour[3] << 8;
         const uint32_t codes = colour[4] | (uint32_t)colour[5] << 8 |
                                (uint32_t)colour[6] << 16 | (uint32_t)colour[7] << 24;
         uint8_t texel[4][4];

         ripple_s3tc_colour(colour0, texel[0]);
         ripple_s3tc_colour(colour1, texel[1]);
         texel[0][3] = texel[1][3] = texel[2][3] = texel[3][3] = 255;
         if (!dxt1 || colour0 > colour1) {
            for (unsigned c = 0; c < 3; ++c) {
               texel[2][c] = (uint8_t)((texel[0][c] * 2 + texel[1][c]) / 3);
               texel[3][c] = (uint8_t)((texel[0][c] + texel[1][c] * 2) / 3);
            }
         } else {
            /* DXT1 with the colours in this order: one colour between the two, and black,
             * transparent unless the format has no alpha */
            for (unsigned c = 0; c < 3; ++c) {
               texel[2][c] = (uint8_t)((texel[0][c] + texel[1][c]) / 2);
               texel[3][c] = 0;
            }
            if (kind == RIPPLE_S3TC_DXT1_RGBA)
               texel[3][3] = 0;
         }

         uint8_t alpha[16];
         if (kind == RIPPLE_S3TC_DXT3) {
            for (unsigned n = 0; n < 16; ++n) {
               const unsigned nibble = (block[n / 2] >> (4 * (n & 1))) & 0xf;
               alpha[n] = (uint8_t)(nibble | nibble << 4);
            }
         } else if (kind == RIPPLE_S3TC_DXT5) {
            const unsigned alpha0 = block[0], alpha1 = block[1];
            uint8_t ramp[8];
            ramp[0] = (uint8_t)alpha0;
            ramp[1] = (uint8_t)alpha1;
            if (alpha0 > alpha1) {
               for (unsigned code = 2; code < 8; ++code)
                  ramp[code] = (uint8_t)((alpha0 * (8 - code) + alpha1 * (code - 1)) / 7);
            } else {
               for (unsigned code = 2; code < 6; ++code)
                  ramp[code] = (uint8_t)((alpha0 * (6 - code) + alpha1 * (code - 1)) / 5);
               ramp[6] = 0;
               ramp[7] = 255;
            }
            uint64_t alpha_codes = 0;
            for (unsigned b = 0; b < 6; ++b)
               alpha_codes |= (uint64_t)block[2 + b] << (8 * b);
            for (unsigned n = 0; n < 16; ++n)
               alpha[n] = ramp[(alpha_codes >> (3 * n)) & 7];
         }

         for (unsigned j = 0; j < rows; ++j) {
            uint8_t *out = dst + (size_t)(y + j) * dst_stride + (size_t)x * 4;
            for (unsigned i = 0; i < columns; ++i, out += 4) {
               const unsigned n = j * 4 + i;
               memcpy(out, texel[(codes >> (2 * n)) & 3], 4);
               if (!dxt1)
                  out[3] = alpha[n];
            }
         }
      }
   }
}

/* The debug switches above, read once (on the Mac; elsewhere, as for this file's check on
 * the build host, both are off). */
struct ripple_s3tc_switches {
   int read, mesa, stats;
   unsigned long long uploads, texels, micros;
};

static inline struct ripple_s3tc_switches *
ripple_s3tc_switches(void)
{
   static struct ripple_s3tc_switches s;
   if (!s.read) {
#ifdef __APPLE__
      s.mesa = access("/Library/Logs/RippleGL/dxtmesa", F_OK) == 0;
      s.stats = access("/Library/Logs/RippleGL/dxtstats", F_OK) == 0;
#endif
      s.read = 1;
   }
   return &s;
}

static inline unsigned long long
ripple_s3tc_now(void)
{
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return (unsigned long long)tv.tv_sec * 1000000ull + (unsigned long long)tv.tv_usec;
}

/* After an upload with stats on: the totals so far, rewritten in place. */
static inline void
ripple_s3tc_count(struct ripple_s3tc_switches *s, unsigned width, unsigned height,
                  unsigned long long micros)
{
   char path[160];
   FILE *f;
   s->uploads += 1;
   s->texels += (unsigned long long)width * height;
   s->micros += micros;
#ifdef __APPLE__
   snprintf(path, sizeof(path), "/Library/Logs/RippleGL/dxt-%s-%d.log", getprogname(), (int)getpid());
#else
   snprintf(path, sizeof(path), "dxt-%d.log", (int)getpid());
#endif
   f = fopen(path, "w");
   if (!f)
      return;
   fprintf(f, "decoder=%s uploads=%llu texels=%llu decode_us=%llu at_us=%llu\n",
           s->mesa ? "mesa" : "blocks", s->uploads, s->texels, s->micros, ripple_s3tc_now());
   fclose(f);
}

#endif
