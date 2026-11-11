# M6: Core Image and real apps (notes)

Started 2026-11-11 about 02:45 on the owner's go. Handoff acceptance: Core Image filters run on the
GPU (Quartz Composer, Photo Booth effects); iMovie and Front Row launch and work; performance
numbers for 2-3 period OpenGL games against the software renderer. Standing rules: nothing
may look worse than the reference (paired captures), and the performance numbers in
`perf-notes.md` may not get slower.

## Status (04:30)

| App | Apple's software renderer (VM 210 without our driver) | Ours |
|---|---|---|
| Quartz Composer (Arabesque.qtz, Shell.qtz) | refuses to start ("Unsupported configuration") | runs, 60 frames/s (Arabesque) |
| Core Image Fun House (Aqua Blue.jpg) | white window, no image | black window, no image |
| Angry Birds Rio (32-bit) | refuses ("OpenGL 2.0 renderer not supported") | runs, title screen right, 59 frames/s |
| Real Racing 2 intro movie | plays (1 frame/s of GL flushes; Core Animation) | black |
| Real Racing 2 after the intro | black, 1 frame/s, 100% CPU | 3D menu renders, garbled (five side-by-side copies, interlaced lines), 23-30 frames/s |
| Command & Conquer 3 launcher | right | same as software |
| Command & Conquer 3 game (Cider, 32-bit) | "Cannot locate the DVD-ROM" (needs its disc) | the same dialog behind the launcher (needs its disc) |
| Flurry screen saver (OpenGL, 1280x800) | 29 frames/s (its cap), ScreenSaverEngine 66% CPU | 27 frames/s, ScreenSaverEngine 4% CPU, WindowServer 9% |
| Front Row | (M5) | works (M5) |
| iMovie | not installed (no iLife on VM 210) | - |
| Photo Booth effects | no camera in the VM | - |

Desktop, menus, Exposé, Dashboard re-checked after every bundle change below: unchanged.
Frame meter with all M6 changes (VM 213 running next to it): Dashboard 27-30 / 10-13
screens/s peak / mean, Exposé 23-29 / 11-17, the same as after the clock test.

Captures (git-ignored, `img/`): `m6-{qe,sw}-*.png` copied from the runs above.

## Fixes so far (commits a983859, 3001b44, f0a1cd9)

1. **Public CGL pbuffers** (CGLCreatePBuffer) keep their size in words 6-7 of the drawable
   description, repeated in 14-15; WindowServer's cgls ones in 9-10 (raw words logged for the
   first attaches; WindowServer's confirmed: 9-10). All of Quartz Composer's were refused.
2. **GL_APPLE_object_purgeable** answered in the frontend (Mesa has no implementation; Quartz
   Composer's texture cache raised GL_INVALID_OPERATION per call). Nothing is discarded.
3. **CoreGraphics' own contexts in apps** (Core Image's): they never send 0x2bd at the end of
   creation, so they were taken over at their first drawable attach, after GLEngine had
   already made textures and framebuffers. Now object calls try the takeover once creation
   has ended (`rend == gc`), and GLEngine's texture/framebuffer bindings made before are
   copied into Mesa at the next call.
4. **CGLTexImagePBuffer** sends gliSetInteger(997, {ID, target, format, width, height,
   0x8367, source, levels}) through the plugin record's own gli table (OpenGL 10K549:
   ctx+0x17e8, +0x78, record +0x10, gliSetInteger at +0x48); hooked like the cgls one. The
   texture shares the pbuffer's colour buffer (st_context_teximage), as on a Mac: what is
   drawn into the pbuffer later shows in it. Pbuffers made for rectangle textures get
   rectangle colour buffers.
5. **CGLTexImageIOSurface2D** sends gliSetInteger(910, {IOSurface ID, target, internal
   format, width, height, format, type, plane}); the IOSurface's pixels are copied into the
   bound texture (Core Video's video frames).
6. **ARB programs**: Apple's parser takes `ALIAS name = fragment.texcoord[5];` (and of
   constants); rewritten to ATTRIB / PARAM before Mesa parses (Core Animation's YUV programs).
7. **GL_UNPACK_CLIENT_STORAGE_APPLE** accepted without an error outside WindowServer (Core
   Animation counts any GL error as a failed frame, "rendering error 500").
8. **GL_DOUBLE vertex attributes** reported unsupported to Mesa so u_vbuf converts them (the
   virgl driver passed 64-bit formats through).
9. Diagnostics: shader compile/link failures with the info log in the debug log; GL trace
   marks context switches (the marker follows the first call of the new context), uniform
   names and matrix values; `tools/winlist` (windows and bounds, no GUI scripting needed);
   `tools/gl-feature-check` (Core Animation's video path step by step, ours vs software).

## Open

- **Real Racing 2 intro (black).** Per frame: Core Video's context uploads the IOSurface frame,
  converts it with an ARB program into a new 1280x720 RGB rectangle pbuffer, ties a texture
  to it (CGLTexImagePBuffer) and detaches; Core Animation's window context binds that texture
  and draws it with a GLSL program through a 3D colour table, then swaps. Its presents stay
  transparent where the program writes alpha 1, so the draw doesn't land. `gl-feature-check`
  repeats the pattern (rectangle pbuffer, tie, double positions, the exact vertex layout and
  program, the 3D table) and gets the right pixel on ours; the difference left is that the
  game's window context draws into a window drawable and swaps.
- **Real Racing 2 menu garbled.** The game's own context. GL_SHORT vertices with a texture
  matrix work in `gl-feature-check`; not found yet.
- **Core Image Fun House:** Core Image issues no GL calls for drawImage into the view; the same
  on software (white). Not a regression; not understood.
- **C&C 3 game:** needs its DVD on either renderer (a disc image from the owner would do).
- **App GL windows are read back** (the window path): every frame goes host -> guest -> window
  backing -> WindowServer upload. Angry Birds at 1280x800: about 20-30 ms per present. Sharing
  the app's GL surface with WindowServer on the GPU is the fix (a later performance item).

## Real Racing 2's intro, what was ruled out (04:30-05:20)

All in one thread. Per frame: Core Video's context A uploads the IOSurface frame (now works),
attaches a new rectangle pbuffer, converts with the ARB program (now parses), flushes, ties
texture N to the pbuffer; Core Animation's window context B binds N (the GL trace prints a
call before the context marker of its context), draws the quad with its GLSL program and the
3D colour table, and swaps. With `swapdebug`: B's swaps read back the centre of its back
buffer as 0,0,0,0 (after three opaque black ones at the start), viewport 1280x800, the
window framebuffer, no GL error. Ruled out by `gl-feature-check` (same contexts, pbuffer,
tie, exact program and vertex layout, into an FBO and into a context's own drawable: right
pixel), by the swap rectangle (always the full window; now honoured anyway), a copy instead
of the tie (also black), threads (one), viewport, culling, stencil, element buffers, blend.
Next idea: dump B's whole GL state at its draw (texture units, sampler state, program
uniforms as Mesa holds them) and compare with `gl-feature-check`'s.

## Game and app numbers against the software renderer (M6 acceptance)

Of the period OpenGL titles on VM 210, only Flurry runs on both renderers; Quartz Composer
and Angry Birds Rio refuse Apple's software renderer, Real Racing 2 shows only its intro
there (1 frame/s, 100% CPU afterwards), C&C 3 needs its disc.

| | software | ours |
|---|---|---|
| Flurry, frames/s (glFlush) | 29 | 27 |
| Flurry, ScreenSaverEngine CPU | 66% | 4% |
| Angry Birds Rio title screen, frames/s | refuses to start | 59 |
| Quartz Composer Arabesque, frames/s | refuses to start | 60 |
| Real Racing 2 menu, frames/s | 1 (black) | 23-30 (garbled) |

## Real Racing 2's intro, more (05:30-06:10)

- The pbuffers Core Video fills do hold the video (debug readback at the tie: brightest
  channel 190-235 a few seconds in), and Core Animation's quad is right: positions (0,40) to
  (1280,760), texture coordinates in pixels, texscale (1,1).
- In the game, with the tie, the window context's composite never lands (its surface stays
  alpha 0); with a copy instead of the tie (`pbuffer-copy`), it lands but samples black.
  Waiting for the GPU at the tie (`tie-finish`) changes nothing.
- `gl-feature-check sequence` repeats the game's order frame by frame (window context with a
  3D table made with client storage; per frame a new rectangle RGB pbuffer drawn by context
  A, tied or copied while attached, detached; drawn through the table by the window context):
  right on ours for both, every frame (Apple's software renderer lags one frame tied).
  `window-check` does the same into a real window (NSOpenGLContext, swap rectangle on): right
  too, and the window shows it.
So the remaining difference is in Core Video's or Core Animation's own objects (the CVOpenGL
buffer pool's pbuffers, the CARenderer context), not in the GL path as such. Parked.

## Front Row (05:40)

Works on the current build (kext and bundle of 2085cab plus the cs-error switch): menu,
reflections, navigation ([capture](img/m6-qe-frontrow.png)). It has to be started with
Cmd+Esc (`qm sendkey 210 meta_l-esc` after a click on the desktop); `open -a "Front Row"`
starts the process but it never takes the screen, on any build (bisected through the M5-era
kext and bundle before that was clear). Desktop, menu, Dashboard re-captured: unchanged.
Frame meter (rig alone): Exposé 26-28 / 9-15 screens/s peak / mean, Dashboard 23-26 / 13.

Build note: after building an older commit from a git worktree, `scripts/build.sh` in the main
tree didn't rebuild (rsync -a gave dev.lan the older source mtimes, older than the objects).
Touch the sources, or build old commits elsewhere.

## Real Racing 2's menu, ruled out (05:45-06:00)

The garbling is in the GPU framebuffer itself: a dump of the game's back buffer at its swap
(`swapdump` switch: `/Library/Logs/RippleGL/swap-PID.ppm`) shows the same five side-by-side
copies with interlaced rows, so it isn't the window present path. Hiding S3TC
(`MESA_EXTENSION_OVERRIDE=-GL_EXT_texture_compression_s3tc`, game started from ssh) changes
the screen shown (the UI panels, top half only) but not the pattern. Each feature the menu
uses works on its own in `gl-feature-check` (ours and software agree): DXT1 uploads (`dxt`),
RGBA uploaded as GL_COMPRESSED_RGBA 2048x2048 with mipmaps (`generic`; we pick DXT5, Apple
DXT3), GL_SHORT attributes from a buffer object at stride 24 (`vbo`), GL_SHORT client arrays
with a texture matrix, a strip drawn from vertex 32. Colour buffers are always 32-bit. Open.

## Core Image filters and Photo Booth effects (06:00-06:20)

No iMovie (no iLife media on the NAS) and no camera, so (coordinator, 05:10) Core Image is
covered with `tools/ci-effects` instead: a photo (Clown Fish.jpg, 480x300) through
1. 24 Core Image filters on our GPU context against Core Image's own software renderer, and
2. all 61 image-filter compositions in /System/Library/Compositions (Photo Booth's effects,
   the photo standing in for the camera) through QCRenderer, our renderer against Apple's
   software renderer.

It found three bugs, fixed:
- **Crash in Core Image** (`update_single_program_texture`, CurrentTex[-1]): Mesa's ARB parser
  kept the previous text's samplers in SamplersUsed when a program object got new text. Mesa
  patch 0006.
- **Abort in Core Image** (double free in `st_program_string_notify`): a vertex program
  re-specified twice freed base_serialized_nir again. Mesa patch 0007. Both still in upstream
  Mesa main (2026-11-11): candidates for an upstream report, the owner's call.
- **GL_APPLE_row_bytes** (pack and unpack row stride in bytes) was refused: now kept per
  context and applied as row length around uploads and readbacks (not in WindowServer).
  ASCII Art came out empty before.
- **CGLTexImagePBuffer** copies again by default: sharing the pbuffer's buffer showed Black &
  White, X-Ray and Line Screen upside down (a window-system buffer is stored flipped against a
  texture). Sharing is behind a debug switch (`pbuffer-tie`).

Results now:
- Filters: 23 of 24 within 1-2 levels of the software renderer or under 1% of pixels;
  CIEdges differs on 3.9% (edge thresholds, the same picture). (Before the copy change,
  CIComicEffect differed on 17.6% and CIZoomBlur on 1.1%.)
  Our GPU takes 2-90 ms per 480x300 frame here against software's 1-5 ms: each render is a
  round trip through QEMU (later performance pass).
- Photo Booth effects: 55 of 61 match (within 1-2 levels or under 1% of pixels). Differ:
  Concert (ours washed out: a blend or accumulator), Neon (17%, glow detail), Blue Print (4%),
  Comic Book (3%), Pointillize (2%, random dots), Pinch (5%, sub-pixel sampling; looks the
  same). Ours 0.4-0.8 ms per frame for most, software 2-3 ms.
- Contact sheets: [Photo Booth page 1](img/m6-booth-1.png), [filters page 3](img/m6-ci-filters-3.png).
- Re-checked after the change: desktop, menus, Dashboard, Quartz Composer (Arabesque 60 fps),
  Front Row. Frame meter: Dashboard 28 / 16.5 screens/s peak / mean, Exposé 21 / 7.4.

## Real Racing 2 intro, more ruled out (06:20-06:40)

- CGLTexImagePBuffer's copy is now sent at once (a flush after the copy): unsent, it reached the
  GPU after the next context's draw. Correct on its own; the intro stays black.
- At the window context's swaps (`swapdebug` now prints the raster state): scissor test,
  stencil, depth, alpha test, blend, logic op all off, colour mask full, draw buffer GL_BACK,
  the window framebuffer. Its frame (one full frame, attributed per context) is: clear 1280x720
  at y 40, glScissor(0,0,0,0) with the test off, bind the video texture (rectangle, unit 0) and
  the 3D table (unit 1), program 5, five interleaved attributes (double position), one
  GL_QUADS from client indices, unbind, swap. Nothing it draws reaches the back buffer.
- `window-check` with glScissor(0,0,0,0) too (WC_ZERO_SCISSOR=1): right.
- The game's element-buffer binds are its own context's, not Core Animation's.
Next: replay that frame's calls in-process from the window context (a debug switch that redoes
the draw with a known texture), to tell the program, the vertex data and the target apart.

## Dashboard: a widget being added flickered (2026-11-11 morning; fixed, 4292cff)

The owner saw it live: a widget being added flickers and can go missing for a few frames. A burst
of display frames during the ripple (`scripts/screen-vnc.sh -n 30`, tiled with
`tools/vncgrab/sheet.py`) showed it: frames with the ripple but no widget, and frames with
no ripple at all, between right ones (3 of 16 ripple frames without the widget).

Cause: WindowServer's display surface is single-buffered, and our driver put the screen on
the display at every flush of its context. dtrace on `ripple_cgl_device_display` in
WindowServer during one ripple (94 frames):

| Caller | Display updates |
|---|---|
| Core Image, `fe_gl_flush` / `fe_gl_unbind_texture_object` (glFlushRenderAPPLE, mid-frame) | 138 |
| `_CGXGLCompositeLayers` (glFlushRenderAPPLE, after the layers) | 94 |
| `CGXGLAccelFinish` (glFlush, the end of the frame) | 94 |

- `glFlushRenderAPPLE` was mapped to Mesa's `glFlush`. Apple defines it (GL_APPLE_flush_render)
  as a flush that does not show a single-buffered drawable.
- With that fixed, 56 updates still came from Mesa's own flush when another context becomes
  current (`_mesa_make_current`): WindowServer changes to Core Image's context mid-frame.
- The GL_APPLE_fence calls were done with `glFlush`/`glFinish` and showed the drawable too.

Now a single-buffered drawable is shown only by the program's `glFlush`, `glFinish` or a swap
(`present_ok` in the context, checked in `drawable_flush_front`); `glFlushRenderAPPLE`,
`glFinishRenderAPPLE` and the fence calls flush without showing. One display update per frame.

Checked on VM 210: the widget is in all 48 ripple frames of three runs (a pixel inside it,
`pngpix.py`). Desktop, File menu, Exposé, Dashboard, Front Row (menu, navigation, back),
gl-gears and `window-check` look as before. Not slower: Dashboard 71-74 / 37-44 frames/s and
29-31 / 13.4-17.1 screens/s (64-67 / 31-35 and 24.7-30.5 / 11-15.3 before), Exposé screens/s
21-28 / 8.6-13.4 over five runs (18-27 / 9.3-15.2), gl-gears 45-52 frames/s (40-45). Measured
while files were being copied on the rig at idle priority.

The first try crashed WindowServer at start: for WindowServer's own contexts the entry points
get GLEngine's context pointer, not ours (`ripple_cgl_context_for` maps it). `gen_fences` and
`swap_APPLE` had the same mistake and are fixed with it.

## the owner's three points of 2026-11-11 noon

Audit of the same day: [audit-2026-11-11.md](audit-2026-11-11.md).

**1. Black glitches at the button at the top right of a Finder window (fixed, 3f15659).**
Clicking it shows or hides the toolbar with an animation. For that AppKit puts an overlay
window in front, drawn by Core Animation with OpenGL: a snapshot of the window and its shadow
with transparent margins, on a surface Core Animation marks non-opaque
(`CGSSetSurfaceOpacity`, straight to the window server, not through CGL). Our driver shows a
GL surface through a private window bound to it, and that window was opaque: WindowServer
drew the surface without blending (RGB tiles, blend off), the margins black, for about ten
frames. Now the private window follows the surface.
- `CGSGetSurfaceOpacity` can't be used: in 10.6.8 it returns error 1001 for every non-NULL
  pointer (an inverted check). `gld_opacity.c` watches the setter instead, by pointing every
  loaded image's symbol pointer for it at a function that notes the setting.
- A non-opaque private window for every surface also worked, but gl-gears fell from about 48
  to about 30 frames/s (each present waited about 9 ms longer), so it is only done for
  surfaces that are non-opaque.
- A non-opaque window's pixels are colour-matched from the window's own colour space (pure
  green came out as 46 255 4), so it also gets the surface's colour space.
- Checked: 56 frames of the animation, both directions, none with black.

**2. Photo Booth (fixed, 3f15659).** Without a camera it should show a black view with "There
is no connected camera", a round shutter button and the photo strip. All three were white.
Core Animation draws all of an app's layer-backed views with one GL context and attaches it
to each view's surface in turn. The driver kept the private window (which holds a surface's
picture) with the context, so each attach to the next surface destroyed the previous one's.
The private window and the render targets now belong to the surface.
Also found there: the swap rectangle (Core Animation's partial updates) was applied upside
down; `swaprect-check` shows ours now matches Apple's software renderer pixel for pixel.
Live effects still need a camera to test.

**3. Real Racing 2, probe only (out of this milestone's scope).** Two separate problems, both
in the game's own process:
- *Intro video: nothing is drawn.* One cause found and fixed (33a9097): Core Animation's
  colour table for video is a 32x32x32 texture made with client storage, which the emulation
  did not cover for 3D textures. The frames Core Animation's context hands over are still
  empty (all zero, `presentdump`). Ruled out today with the trace: the vertices are right
  ((0,40) to (1280,760), `vertex_matrix` 2/1280, 2/800), the shaders compile and link (they
  use `highp`/`lowp`, which Mesa takes), the video texture is the one tied from the pbuffer.
  Not yet looked at: the uniforms `texscale0..2` and whether the tied texture is complete in
  the window context (a copy made in another context of the share group).
- *The game's own picture is sheared:* a 1280-wide image shown as five side-by-side copies,
  each a row lower, squeezed into the top fifth, which is a 1280-pixel row read 256 pixels at
  a time. This morning it was already like that in the game's draw buffer, before our present
  path (`swapdump`); not re-checked after today's changes. Ruled out: `GL_UNPACK_ROW_LENGTH`
  (the game sets 0, 32, 640, 1280 only), row bytes and `glPushClientAttrib` (never called),
  the winsys transfer stride.
- With point 1 fixed the intro no longer shows black but the sheared picture behind it (the
  video layer is transparent and empty).
- Estimate: one to two days. The intro needs the remaining state of one draw compared with a
  working copy (`window-check` already draws the same quad right, so the difference is
  small). The shear needs the game's frame path traced once from its render target to the
  screen; the tools for both exist now (`trace` with vertex data, `swapdump`, `presentdump`).
