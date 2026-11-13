# M6: Core Image and real apps (notes)

Started 2026-11-11 about 02:45. Handoff acceptance: Core Image filters run on the
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
  matrix work in `gl-feature-check`; not found yet. (Both Real Racing 2 entries: found and
  fixed 2026-11-12, see "a depth buffer the host could not make" at the end.)
- **Core Image Fun House:** Core Image issues no GL calls for drawImage into the view; the same
  on software (white). Not a regression; not understood. (2026-11-12: it shows the picture on
  our driver now, Aqua Blue.jpg, with either way of showing app windows.)
- **C&C 3 game:** needs its DVD on either renderer (a disc image would do).
- **App GL windows are read back** (the window path): every frame goes host -> guest -> window
  backing -> WindowServer upload. Angry Birds at 1280x800: about 20-30 ms per present. Sharing
  the app's GL surface with WindowServer on the GPU is the fix (a later performance item).
  (Done 2026-11-12: the kernel shows app windows itself, ioaccelerator-interface.md steps 1-9;
  Angry Birds Rio's title screen went from 32-39 to 58 frames/s.)

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

No iMovie (no iLife media on the NAS) and no camera, so (decided 05:10) Core Image is
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
  Mesa main (2026-11-11): candidates for an upstream report.
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

Seen live in testing: a widget being added flickers and can go missing for a few frames. A burst
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
while another job copied files on the rig at idle priority.

The first try crashed WindowServer at start: for WindowServer's own contexts the entry points
get GLEngine's context pointer, not ours (`ripple_cgl_context_for` maps it). `gen_fences` and
`swap_APPLE` had the same mistake and are fixed with it.

## Three points reported in testing, 2026-11-11 noon

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

## Concert effect, narrowed down (2026-11-11 afternoon)

Still washed out on ours, but now located: it is the composition's image accumulator
(`QCImageAccumulator`), the only one among the 61 effects.
- `CI_EFFECTS_FRAMES=n` renders n frames before the compared one: with 0 ours matches the
  software renderer (largest difference 5), with 1 it is 1.8% of pixels off, with 2 94%, with
  6 or 12 99.6%. Each frame adds brightness that the software renderer does not add.
- Per frame the trace shows a texture tied twice to the output pbuffer itself (the previous
  picture fed back), blur passes in framebuffer objects cleared to 0.25 or 0.258 on all four
  channels, the same value as the textures' border colour with `GL_CLAMP_TO_BORDER` (Core
  Image's fill outside an image), and source-over blending into the pbuffer.
- Not the cause: sharing the pbuffer's buffer instead of copying it (`pbuffer-tie`: the same
  result), `glPushClientAttrib` (never called here).
- Mesa reports one `GL_INVALID_FRAMEBUFFER_OPERATION in glBegin` in the run: a draw into an
  incomplete framebuffer, not yet tied to a pass.
- Next: find which pass differs by reading back each pass's target on ours for frames 0 and
  1 (the first frame where the feedback counts), starting with the two feedback textures'
  alpha: a fill of 0.25 alpha blended over itself each frame converges to white unless
  something limits it, which is what the picture looks like.

## Real Racing 2 intro: where it stands (paused 2026-11-11 for the release)

Core Animation's window context in the game draws into a window framebuffer that Mesa reports
incomplete and 0x0 (status 0x8CD7), although its drawable holds a 1280x800 back texture: its
clears and the video quad are dropped, so its frames are empty. The game's own context on the
same window has a complete 1280x800 framebuffer. `swapdebug` now logs the framebuffer status and
the drawable behind each swap. Next step when resumed: why that framebuffer is never validated
to the drawable's size.

## The same measurements on the GeForce 9600 GT (VM 211, 2026-11-11 evening)

Apple's NVIDIA driver at 1920x1080 (the monitor offers 1080p, 720p and TV modes, no 1280x800;
switching to 1280x720 with `tools/setmode` hung WindowServer until a reboot). One Finder
window, the same scripts:
- Frame meter, peak / mean screens per second, three runs: Dashboard 22.1-24.9 / 10.8-11.9,
  Exposé 15.9-18.0 / 7.4-10.5, menus 1.0 / 0.4.
- Flurry: 30 frames/s (glFlush), 2.3% of a CPU. Quartz Composer Arabesque: 58 frames/s,
  47% of a CPU. Angry Birds Rio title screen (full screen): 60 frames/s, 13% of a CPU.
- `ci-effects`: filters 23 of 24 match software rendering (CIEdges 2.6% of pixels), 1.0-3.2 ms
  a frame; Photo Booth effects 54 of 61 match, 0.2-0.3 ms for most. The 7 that differ: Blue
  Print 4.3%, City Lights 4.1%, Comic Book 3.4%, Concert 93.6%, Neon 17.0%, Pinch 78.3%,
  Pointillize 2.4%. So Concert, Neon, Pinch and the others differ on real hardware too: most of
  our differences are a GPU's, not our driver's.
- Calculator's window is silver-grey with light keys here; on our driver its body is black
  (issue #12).

## Real Racing 2 and Chess: a depth buffer the host could not make (2026-11-12)

Both faults of Real Racing 2 (black intro, garbled menu) and Chess's black board had one cause.
The frontend gave every pixel format with a stencil buffer a `Z24_UNORM_S8_UINT` depth buffer
without asking the screen. virglrenderer has `S8_UINT_Z24_UNORM` but not that one: it logged
"unknown format is 19" and made nothing, QEMU still answered the creation with success, and
the first command that used the buffer (CREATE_SURFACE for the depth attachment) failed as
"Illegal resource". virglrenderer then drops the rest of that command buffer and leaves the
process's context in error, so later commands, of every GL context in the process, are lost
more or less at random: the game's own picture came out in pieces, and Core Video's and Core
Animation's draws for the intro never happened.

Fix: the depth, stencil and accumulation formats of a window or pbuffer are chosen from what
the screen supports (`cgl_context.c`, `depth_stencil_format`). Real Racing 2 plays its intro
and draws its 3D menu right, at 56 frames/s; Chess shows its board.
`gl-feature-check stencil` draws with the stencil and depth buffers of such a drawable and
reads the result back; `tex3d` and `yuv` (Core Video's real conversion program) came out of
the search and pass on both renderers.

How it was found, and two traps on the way:
- **The host says what it refuses.** On a Proxmox host, QEMU's output is in the journal:
  `journalctl | grep -E 'vrend|unknown format'` showed one "Illegal resource" per start of
  the game. Look there first when something draws black or in pieces. The new debug switch
  `/Library/Logs/RippleGL/reslog` logs every buffer a process makes and destroys, to look the
  number up.
- **The swap debug log damaged what it inspected.** It called Mesa's completeness test, which
  is for framebuffer objects, on a window framebuffer; for one without a depth buffer (Core
  Animation's) that leaves it 0x0 and incomplete. The "incomplete framebuffer" recorded above
  as where the intro stood was this log's own doing. Fixed.
- **Read-backs lie once the host context is in error.** A read-back is a command too; when
  the host drops it, glReadPixels returns whatever the staging memory held, here the video
  frame uploaded just before. A scratch texture cleared to a known colour read back as video
  data. So pixel values logged from inside a broken process prove nothing.

## Real Racing 2 in UTM: a host without S3TC (2026-11-12)

On the Proxmox rig the fix above was enough. In UTM on an Apple-silicon Mac the game's menu
still came out wrong: the 2D menu right, the car and scenery covered in coloured block noise,
and nothing in the host's output.

Cause: the host there (ANGLE, OpenGL ES on Metal) offers no S3TC texture compression, so our
renderer did not list `GL_EXT_texture_compression_s3tc` and refused DXT uploads with
GL_INVALID_ENUM. Every Mac GPU of the time has S3TC, and the game uploads DXT textures without
asking. `gl-feature-check dxt` showed it in one run: "upload error 0x500, s3tc NOT listed".

Fix: the CGL frontend sets Mesa's `allow_compressed_fallback` option (`cgl_context.c`), as
Mesa's own frontends do by default. With it Mesa lists the extension and decodes such textures
on the CPU when the host cannot take them. A host with S3TC is not affected (the rig still
gets DXT natively; `dxt` and `generic` give the same results as before). In UTM the menu is
drawn right at 57 to 59 frames/s, and Chess shows its board.

In a race the game makes about 12 frames/s in UTM. Played in testing the same day: playable
at that rate. Open, for the performance pass: its render thread uses a whole
emulated CPU: about a third in the game's own lens flare test
(`M3GMesh::IntersectsWithLineFast`), about a third in our draw path (sampler state encoding and
a new transfer for each vertex upload), and four more threads poll with `usleep`.

Looking at a guest's screen without a screenshot from the host: two debug switches write what
the driver has, as PPM files in `/Library/Logs/RippleGL/` (`scripts/sheet.sh` converts them).
`touch displaydump` gives the whole screen at WindowServer's next redraw (`display.ppm`);
while an app's window is shown by the kernel WindowServer does not redraw, and `touch swapdump`
gives each app's frame at its next swap instead (`swap-PID-CONTEXT.ppm`). The log lines alone
were not enough here: they showed presents at a good rate while the picture was wrong.
