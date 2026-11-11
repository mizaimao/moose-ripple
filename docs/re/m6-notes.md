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
| Command & Conquer 3 game (Cider, 32-bit) | not tried yet | context made, no drawable attached, process idles; "invalid context" from CoreGraphics |
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
- **C&C 3 game:** check on software first.
- **App GL windows are read back** (the window path): every frame goes host -> guest -> window
  backing -> WindowServer upload. Angry Birds at 1280x800: about 20-30 ms per present. Sharing
  the app's GL surface with WindowServer on the GPU is the fix (a later performance item).
