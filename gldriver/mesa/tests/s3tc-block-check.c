/* Compares the GL driver's block-at-a-time DXT decoder (ripple_s3tc.h) with Mesa's
 * texel-at-a-time one on random blocks, for DXT1 (with and without alpha), DXT3 and DXT5 and
 * for sizes that are not multiples of four. Runs on the build host; MESA is the Mesa source
 * tree the driver is built from:
 *   cc -O2 -I"$MESA/include" -I"$MESA/src" -Igldriver/mesa/overlay/src \
 *      gldriver/mesa/tests/s3tc-block-check.c -o s3tc-block-check && ./s3tc-block-check
 * Prints the number of texels compared; exit code 1 and the first difference if any.
 * MIT licence (LICENSE at the repository root).
 */
#include <stdio.h>
#include <stdlib.h>
#include "util/macros.h"
#include "util/format/texcompress_s3tc_tmp.h"
#include "util/format/ripple_s3tc.h"

typedef void (*fetch_fn)(GLint, const GLubyte *, GLint, GLint, GLvoid *);

static unsigned long long state = 0x9e3779b97f4a7c15ull;

static unsigned
next(void)
{
   state ^= state << 13;
   state ^= state >> 7;
   state ^= state << 17;
   return (unsigned)(state >> 24);
}

int
main(void)
{
   static const struct { enum ripple_s3tc_kind kind; fetch_fn fetch; unsigned block; const char *name; } kinds[] = {
      {RIPPLE_S3TC_DXT1_RGB, fetch_2d_texel_rgb_dxt1, 8, "DXT1 RGB"},
      {RIPPLE_S3TC_DXT1_RGBA, fetch_2d_texel_rgba_dxt1, 8, "DXT1 RGBA"},
      {RIPPLE_S3TC_DXT3, fetch_2d_texel_rgba_dxt3, 16, "DXT3"},
      {RIPPLE_S3TC_DXT5, fetch_2d_texel_rgba_dxt5, 16, "DXT5"},
   };
   static const unsigned sizes[][2] = {{64, 64}, {1, 1}, {2, 3}, {5, 7}, {13, 4}, {4, 9}, {130, 66}};
   unsigned long long compared = 0;

   for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
      for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
         const unsigned width = sizes[s][0], height = sizes[s][1];
         const unsigned across = (width + 3) / 4, down = (height + 3) / 4;
         const size_t src_stride = (size_t)across * kinds[k].block + 24;   /* a pitch wider than the row */
         const size_t dst_stride = (size_t)width * 4 + 12;
         uint8_t *src = malloc(src_stride * down);
         uint8_t *ours = malloc(dst_stride * height);
         if (!src || !ours)
            return 2;
         for (unsigned round = 0; round < 400; ++round) {
            for (size_t i = 0; i < src_stride * down; ++i)
               src[i] = (uint8_t)next();
            /* colours in both orders and equal, which DXT1 treats differently */
            if (round % 3 == 0)
               memcpy(src + (kinds[k].block == 8 ? 2 : 10), src + (kinds[k].block == 8 ? 0 : 8), 2);
            memset(ours, 0x5a, dst_stride * height);
            ripple_s3tc_decode(kinds[k].kind, ours, dst_stride, src, src_stride, width, height);
            for (unsigned y = 0; y < height; ++y) {
               for (unsigned x = 0; x < width; ++x) {
                  uint8_t mesa[4];
                  const uint8_t *block = src + (size_t)(y / 4) * src_stride + (size_t)(x / 4) * kinds[k].block;
                  kinds[k].fetch(0, block, (GLint)(x & 3), (GLint)(y & 3), mesa);
                  const uint8_t *mine = ours + (size_t)y * dst_stride + (size_t)x * 4;
                  if (memcmp(mesa, mine, 4)) {
                     printf("%s %ux%u at %u,%u: Mesa %02x%02x%02x%02x, block decoder %02x%02x%02x%02x\n",
                            kinds[k].name, width, height, x, y, mesa[0], mesa[1], mesa[2], mesa[3],
                            mine[0], mine[1], mine[2], mine[3]);
                     return 1;
                  }
                  ++compared;
               }
               /* nothing written past the row */
               if (ours[(size_t)y * dst_stride + (size_t)width * 4] != 0x5a) {
                  printf("%s %ux%u: wrote past the end of row %u\n", kinds[k].name, width, height, y);
                  return 1;
               }
            }
         }
         free(src);
         free(ours);
      }
   }
   printf("%llu texels equal\n", compared);
   return 0;
}
