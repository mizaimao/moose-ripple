/* What the Direct3D driver left out or could not do, as counts that a test or benchmark run
 * fails on.
 *
 *   ExtEscape(dc, RB2K_D3D7_SKIPS_ESCAPE, 0, NULL, sizeof(RB2K_SKIPS), (LPSTR)&record)
 *
 * fills the record and returns a positive number. The counts run from the loading of the
 * display driver. Plain C89: the pixel tests shared with the Windows 9x driver include this
 * file too (tools/drvcheck.h). */
#ifndef RBSKIPS_H
#define RBSKIPS_H

#define RB2K_D3D7_SKIPS_ESCAPE 0x52420024
#define RB2K_SKIPS_VERSION 1

enum {
    RB2K_SKIP_KIND_DRAWS,       /* primitives of draws left out: fill mode, stencil, texture stages,
                                   vertex format, texture kind or format, primitive kind */
    RB2K_SKIP_KIND_CLEARS,      /* clears left out */
    RB2K_SKIP_KIND_STATES,      /* states that had no effect (fog, colour key, clip plane, mip map and
                                   lighting cases) and Direct3D 9 commands passed over */
    RB2K_SKIP_KIND_SHADERS,     /* vertex shaders, pixel shaders and stage programs not translated */
    RB2K_SKIP_KIND_FORMATS,     /* vertex declarations that could not be resolved */
    RB2K_SKIP_KIND_SURFACES,    /* render target and depth buffer pairs refused; texture copies,
                                   uploads and upload checks that failed */
    RB2K_SKIP_KIND_CALLS,       /* driver calls that failed */
    RB2K_SKIP_KINDS
};
#define RB2K_SKIP_KIND_NAMES { "draws", "clears", "states", "shaders", "formats", "surfaces", "calls" }

typedef struct { unsigned long version, count[RB2K_SKIP_KINDS]; } RB2K_SKIPS;

#endif
