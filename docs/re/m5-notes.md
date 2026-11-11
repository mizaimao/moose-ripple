# M5 working notes: Quartz Extreme on virgl

Date: 2026-11-10 (night). Status: QE on and the whole screen GL-composited on VM 210. Not
accepted yet (22:40: "not displaying normally"). After the 23:30 fixes the desktop,
menus, Exposé and Dashboard match Apple's software renderer on the same VM (paired captures
below) except the menu bar's lower edge. Open: that edge, the Dashboard ripple (not re-tested
since the fixes).
Plan: [m5-plan.md](m5-plan.md); research: [qe-decision.md](qe-decision.md) (R1),
[qe-windowserver-trace.md](qe-windowserver-trace.md) (R2), [qe-verification.md](qe-verification.md)
(R3, also how to check QE), [qe-display-surface.md](qe-display-surface.md) (R4); design notes
from VMsvga2: [vmsvga2-qe-design.md](vmsvga2-qe-design.md).

## How to run it (VM 210)

- Boot flags: `-v npci=0x2000 ripple_gl=1 ripple_qe=2 ripple_hwcursor=1 slto_us=4000000`
  (`ripple_qe=2` also logs every surface call to the kernel log; `ripple_qe=1` without).
- Installed: `RippleVirtioGPU.kext` (`scripts/install-kext.sh`), `RippleGLDriver.bundle` and
  `RippleGA.plugin` (`scripts/install-glbundle.sh`; checks: compare `md5` of the installed and
  built binary, a half-copied bundle crashed WindowServer once). WindowServer loads the bundle
  at start: `sudo killall -KILL WindowServer` (the session comes back by itself in about 25 s).
- Debug switches, all under `/Library/Logs/RippleGL` (world-writable; its existence turns on
  the bundle's per-process log):
  - `trace` present when a process starts: GL call trace, first 6 calls per entry plus call
    counts every 10 s, in `trace-PROGRAM-PID.log`. Logged calls also show ARB program and
    GLSL text, samples of 32-bit and lookup-table images, and `surface_texture(...)`.
  - `burst` (write a number into it; each new modification time counts): the next N calls in
    full. Written before a WindowServer restart it traces the start.
  - Mesa's own errors ("GL_INVALID_VALUE in glOrtho") go to `mesa-PROGRAM-PID.log`, or to
    `/var/log/system.log` for WindowServer (set-user-id: Mesa ignores `MESA_LOG_FILE` there).
  - `stats:` lines every 5 s in the bundle log: presents, frames/s while something moves, CPU
    cost per present.
- Checks: `scripts/qe-check.sh`, `build/grab` (QE on, display captured, and
  WindowServer's own copy of the screen), `build/gl-check` (offscreen GL checks; `-s` for
  Apple's software renderer as the reference), `build/fb-dump` (the framebuffer memory),
  `VMID=210 scripts/screen-vnc.sh [-n N -i S]` (the real display, one or N frames),
  `tools/vncgrab/pngpix.py` (pixel values across captures, run on dev.lan's `.venv`).

## What it took (in order)

1. Accelerator properties `AccelCaps` = 3 and `IOAccelRevision` = 2: WindowServer then loads
   our GL bundle and turns QE on (`CGDisplayUsesOpenGLAcceleration` = 1).
2. The surface user client (type 0) for WindowServer's surfaces.
3. Taking over CoreGraphics' own GL contexts ("cgls", not CGL): GLEngine gets a writable
   shadow back table at gc+0x6558, our table goes in front, the first word stays GLEngine's
   context (cgls passes it to GLEngine) and the thunks map it to ours.
4. Render-op slots 27-29 filled (GLEngine calls flush/swap there).
5. `RippleGA.plugin` (copy and fill blitters): without a GA plugin on the framebuffer,
   WindowServer never composites the display itself with GL (R4).
6. The display surface (id 1) is shaped to each frame's dirty rectangle and WindowServer draws
   that rectangle at the window origin (`glViewport(0, 0, w, h)`, `glOrtho(x, x+w, y+h, y)`).
   The display drawable is the whole screen, persistent, shown with `DISPLAY`; each frame's
   rectangle is a window into it: viewport, scissor, clears, reads and copies are shifted by
   the rectangle's position, only while the window's own framebuffer is bound (Core Animation
   and Core Image use framebuffer objects in their own coordinates). Exposé reads the screen
   back for its thumbnails, which needs the whole screen in the drawable.
7. Apple hint enums (`GL_TEXTURE_STORAGE_HINT_APPLE`, `GL_TRANSFORM_HINT_APPLE`) accepted as
   no-ops. `GL_UNPACK_CLIENT_STORAGE_APPLE` keeps failing, set or queried (accepting it made
   WindowServer reuse texture memory, answering the query made it drop the menu bar).
   `GL_APPLE_float_pixels` can be advertised (opt-in, M6: `RIPPLE_FLOAT_PIXELS=1`, or the
   file `float_pixels` in the debug folder for WindowServer).
8. Client storage emulated: WindowServer makes tiles of an app GL surface's shared memory once
   and never uploads them again (Apple's GL reads that memory when it draws). Textures made
   with client storage are uploaded again from their memory at the first bind of each frame,
   until the program updates them itself. Before this, Front Row stayed white and GL windows
   showed their first frame.
9. Mesa patch 0005: no alpha-only formats on rectangle textures. `GL_ALPHA` rectangles sampled
   wrong through virglrenderer 0.10.4 on the UHD 630, so window shadows were missing on the
   desktop and opaque black in Exposé (`gl-check` shows it; 2D alpha textures were fine).
10. GL windows are written bottom row first (GL's order) into their window memory: every GL
   window was upside down since M4 (gl-gears' blue gear at the bottom).
11. Surface textures. WindowServer gets what is behind a window (menu bar background, menu
   blur, the Dock's shelf) by binding a surface to the current texture:
   `cglsSetInteger(999, {surface, target, format, width, height})` (also 997, 998), straight
   to GLEngine's `gliSetInteger`. GLEngine refused it with kCGLBadState (its texture
   bindings never see ours), so those textures had no image and fixed function drew them in
   the plain white vertex colour. `cgl_takeover.c` now replaces the `gliSetInteger` entry of
   CoreGraphics' GLI table (after the cgls block's dispatch table, +0x58) and
   `ripple_cgl_surface_texture` fills the bound texture: from the window framebuffer for the
   surface the context draws into, from memory for other surfaces. Fixed: the missing Apple
   menu logo, black bars under menu titles after a menu closed, the dark block top left
   (the owner's 22:52 screenshot).
12. Accelerator surfaces other than the screen region (menu bar background 1280x28, Dock
   1248x44) have the GL origin at the top left, like the screen region: WindowServer draws
   the menu bar's 22 rows with `glViewport(0, 0, 1280, 22)` and means the top rows. Viewport,
   scissor and reads map `y` to `height - y - h` there (`win_y`), and their memory is written
   in Mesa's resource order. Before, the menu bar strip with the Apple logo came out upside
   down, then 6 rows low (23:2x).

## Results (VM 210, 2026-11-10)

- QE on, renderer ours (`qe-check`, `grab`). Desktop, menus, shadows right
  ([desktop](img/m5-desktop-gl-composited-3.png)).
- Exposé right: windows move, the rest dims ([Exposé](img/m5-expose.png)).
- Dashboard right: uniform dimming (pixel values at 55% everywhere, Dock and icons included),
  widgets, widget bar sliding the screen up ([widget bar](img/m5-dashboard-widget-bar.png)).
  Adding a widget leaves the screen right afterwards.
- Front Row (needs QE) runs: menu, reflections and navigation right
  ([Front Row](img/m5-front-row.png)). It takes the display (captured) and draws through a
  full-screen window.
- gl-gears animates upright at 36 frames/s in a window ([gl-gears](img/m5-gl-gears-upright.png)).
- Cursor: the virtio hardware cursor (`RippleCursor` = hardware), a plane QEMU draws over the
  composited screen, so it can't tear with it. VNC captures don't show that plane.
- Compositor: about 28 presents/s even idle (the dialog's pulsing default button), 31-37
  frames/s during Exposé and Dashboard, 5-9 ms of CPU per present (flush and `DISPLAY`);
  WindowServer at 5-20% CPU.
- Display lag (`scripts/vnc-lag.sh`, method in
  [qe-verification.md](qe-verification.md)): key press to the first changed pixel, medians of
  5, QE on 210 against software on 212 (2560x1600, not like for like): Exposé open 234 ms
  against 585, close 180 against 601; Dashboard open 246 against 113, close 258 against 339.
  Steps are QEMU's 30 ms VNC tick. Exposé starts about 2.5 times sooner under QE; Dashboard's
  first change comes about 130 ms later.

## Paired captures, QE against software (VM 210, 2026-11-10 23:17-23:30)

Same VM, same session state; software = booted without `ripple_gl`/`ripple_qe` (Apple's
software compositor), QE = default config (no `float_pixels`).

| Scene | QE | Software | 9600 GT (VM 211, 1920x1080, `grab` in the guest) |
|---|---|---|---|
| Desktop with a Finder window | [qe](img/m5-qe-desktop.png) | [sw](img/m5-sw-desktop.png) | [9600 GT](img/m5-9600gt-desktop.png) |
| Menu open | [qe](img/m5-qe-menu-open.png) | [sw](img/m5-sw-menu-open.png) | [9600 GT](img/m5-9600gt-menu.png) (Finder menu) |
| Exposé | [qe](img/m5-qe-expose.png) | [sw](img/m5-sw-expose.png) | [9600 GT](img/m5-9600gt-expose.png) |
| Dashboard | [qe](img/m5-qe-dashboard.png) | [sw](img/m5-sw-dashboard.png) | [9600 GT](img/m5-9600gt-dashboard.png) |

The 9600 GT's menu bar is translucent: at x 1300 over the pink wallpaper it runs 255,224,255
(row 1), 240,195,229 (row 11), 208,167,198 (row 20), and row 21 is the wallpaper darkened
(80,30,68). That is `CIMenuGenerator` on the GPU; neither our QE nor software has it.

By numbers (`tools/vncgrab/pngpix.py`): desktop identical; Dock within 2-7 levels; Apple
logo rows identical. Differences: the menu bar's lower edge (software fades to 234 and 214 in
rows 16-20 with a dark line of 76 in row 21; QE stays 255 with a line of 150), and menus blur
what is behind them under QE (WindowServer's own GL background blur; software shows it sharp
through the translucent menu). The software menu bar is opaque on this VM too, so
"translucent" isn't the software look.

## Open problems

- **Menu bar lower edge:** the fade and dark line come from WindowServer's `CIMenuGenerator`
  (`_CGXUpdateMagicMenuBar`), which runs on the CPU only for unaccelerated backing stores;
  for an accelerated one WindowServer expects to apply it on the GPU while compositing
  (layer filter, `sCGXEnableServerCI`), and that path doesn't run yet. Core Image on the GPU
  in WindowServer (M6 territory).
- **Dashboard ripple backdrop white:** while a new widget ripples (about 1 s), the 522x598
  rectangle around it is white apart from the rippling widget
  ([frame](img/m5-dashboard-ripple-white.png)); the screen is right again afterwards. Core
  Image renders the ripple on the CPU inside WindowServer and uploads the result each frame
  (texture 522x598, drawn opaque at 379,101), so its backdrop input is white. Ruled out: GL
  errors, the framebuffer memory (filled red with `fb-dump`: still white), the window offset
  (framebuffer objects unshifted: still white), WindowServer's software copy of the screen
  (`grab` with Dashboard open is right), and `GL_APPLE_float_pixels` (extensions 0xf like the
  9600 GT, desktop unchanged, ripple still white). Next: find where WindowServer's CPU path
  gets "what is behind" under QE, e.g. with lldb on WindowServer on 210 around the ripple's
  `tex_sub_image2D` source buffer.
- Don't answer `glGetIntegerv(GL_UNPACK_CLIENT_STORAGE_APPLE)`: with an answer (even the
  tracked value) WindowServer dropped the menu bar. Mesa's `GL_INVALID_ENUM` stays.
- The framebuffer memory stays black under QE (the display shows a GPU texture). Anything
  reading the screen through it (old screen-capture tools) gets black.
- `texture_range_APPLE` is unsupported (ignored); harmless so far.
- WindowServer's Mesa messages for the refused client-storage enum fill `system.log` while the
  debug folder exists (`GL_INVALID_ENUM in glPixelStore`).
