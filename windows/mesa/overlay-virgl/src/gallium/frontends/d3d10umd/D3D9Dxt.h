/*
 * moose-ripple
 *
 * DXT1, DXT3 and DXT5 blocks to R8G8B8A8 texels, a block at a time, for a host renderer
 * without S3TC (UTM's, on Apple's GPUs through ANGLE), where the Direct3D 9 driver keeps DXT
 * textures as R8G8B8A8 and decompresses them when they are uploaded.
 *
 * Mesa's own unpack (util_format_dxt*_unpack_rgba_8unorm) fetches one texel at a time and works
 * out the block's colours again for each of the sixteen. That is the right code for sampling a
 * texel now and then, and slow for a whole texture: with the processor emulated, loading one
 * 3DMark06 test took three and a half minutes, most of it there. Here the four colours and
 * the sixteen alpha values of a block are worked out once. The arithmetic is the same as in
 * Mesa's src/util/format/texcompress_s3tc_tmp.h, so the texels are the same;
 * windows/mesa/tests/dxt-decode-check.c compares the two.
 *
 * MIT licence (LICENSE at the repository root).
 */
#ifndef D3D9_DXT_H
#define D3D9_DXT_H

#include <stdint.h>
#include <string.h>

enum d3d9_dxt_kind {
   D3D9_DXT_KIND_1,   /* 8 bytes a block, one bit of alpha */
   D3D9_DXT_KIND_3,   /* 16 bytes a block, four bits of alpha a texel */
   D3D9_DXT_KIND_5,   /* 16 bytes a block, alpha interpolated between two values */
};

/* One 16-bit colour (5:6:5) to three bytes, the low bits filled from the high ones. */
static inline void
d3d9_dxt_colour(unsigned packed, uint8_t rgb[3])
{
   rgb[0] = (uint8_t)(((packed >> 8) & 0xf8) | ((packed >> 13) & 0x7));
   rgb[1] = (uint8_t)(((packed >> 3) & 0xfc) | ((packed >> 9) & 0x3));
   rgb[2] = (uint8_t)(((packed << 3) & 0xf8) | ((packed >> 2) & 0x7));
}

/* width by height texels from rows of blocks (src_stride bytes from one row of blocks to the
 * next) to rows of R8G8B8A8 texels (dst_stride bytes a row). A width or height that is not a
 * multiple of four takes the part of the last blocks that it covers. */
static inline void
d3d9_dxt_decode(enum d3d9_dxt_kind kind, uint8_t *dst, size_t dst_stride, const uint8_t *src,
                size_t src_stride, unsigned width, unsigned height)
{
   const size_t block_size = kind == D3D9_DXT_KIND_1 ? 8 : 16;

   for (unsigned y = 0; y < height; y += 4) {
      const uint8_t *block = src + (size_t)(y / 4) * src_stride;
      const unsigned rows = height - y < 4 ? height - y : 4;

      for (unsigned x = 0; x < width; x += 4, block += block_size) {
         const unsigned columns = width - x < 4 ? width - x : 4;
         const uint8_t *colour = kind == D3D9_DXT_KIND_1 ? block : block + 8;
         const unsigned colour0 = colour[0] | (unsigned)colour[1] << 8;
         const unsigned colour1 = colour[2] | (unsigned)colour[3] << 8;
         const uint32_t codes = colour[4] | (uint32_t)colour[5] << 8 |
                                (uint32_t)colour[6] << 16 | (uint32_t)colour[7] << 24;
         uint8_t texel[4][4];

         d3d9_dxt_colour(colour0, texel[0]);
         d3d9_dxt_colour(colour1, texel[1]);
         texel[0][3] = texel[1][3] = texel[2][3] = texel[3][3] = 255;
         if (kind != D3D9_DXT_KIND_1 || colour0 > colour1) {
            for (unsigned c = 0; c < 3; ++c) {
               texel[2][c] = (uint8_t)((texel[0][c] * 2 + texel[1][c]) / 3);
               texel[3][c] = (uint8_t)((texel[0][c] + texel[1][c] * 2) / 3);
            }
         } else {
            /* DXT1 with the colours in this order: one colour between the two, and a
             * transparent black one */
            for (unsigned c = 0; c < 3; ++c) {
               texel[2][c] = (uint8_t)((texel[0][c] + texel[1][c]) / 2);
               texel[3][c] = 0;
            }
            texel[3][3] = 0;
         }

         uint8_t alpha[16];
         if (kind == D3D9_DXT_KIND_3) {
            for (unsigned n = 0; n < 16; ++n) {
               const unsigned nibble = (block[n / 2] >> (4 * (n & 1))) & 0xf;
               alpha[n] = (uint8_t)(nibble | nibble << 4);
            }
         } else if (kind == D3D9_DXT_KIND_5) {
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
               if (kind != D3D9_DXT_KIND_1)
                  out[3] = alpha[n];
            }
         }
      }
   }
}

#endif
