# Mesa on Windows 98

Step 4 of [plan.md](plan.md): OpenGL for Windows 98 from a current Mesa (24.3.4, the release the
Mac OS X and Windows 7 sides use), built on the Linux build host with `win9x/mesa/build.sh`.

## State (2026-11-15)

Mesa runs on Windows 98 SE and draws with the host's GPU. `tools/win9x/gltest.c`, a small
OpenGL 1.1 program written the way a period game is (ChoosePixelFormat, wglCreateContext,
immediate mode), reports

    GL_VENDOR:   Mesa
    GL_RENDERER: virgl (Mesa Intel(R) UHD Graphics 630 (CFL GT2))
    GL_VERSION:  4.3 (Compatibility Profile) Mesa 24.3.4

in a Windows 98 SE guest on Proxmox VE (`vga: virtio-gl`), and passes its pixel checks (clear,
smooth shading, depth test, texture, blending) with Mesa's `opengl32.dll` and
`ripplegl.dll` next to it. The window shows the picture, clipped by
other windows as it should be. On a host without virgl the same DLLs fall back to Mesa's
software renderer (softpipe), and with Windows 98's own `opengl32.dll` (Microsoft's software
OpenGL 1.1) the program passes too, which is the comparison.

The pieces: Mesa's virgl driver, unchanged; a winsys for it
(`win9x/mesa/overlay/src/gallium/winsys/virgl/rvd/`) on our VxD's interface
([vxd-interface.md](vxd-interface.md)); and a small patch that adds both to Mesa's Windows
OpenGL target. A finished frame is read back from the host into the buffer's guest memory
and drawn on the window with GDI (`SetDIBitsToDevice`).

**Windows' own `opengl32.dll` loads it** as its installable client driver (ICD), so programs
need nothing in their own folder: the display driver answers the escape `OPENGL_GETINFO`
with the name `RIPPLEGL`, and the registry value of that name under
`HKLM\Software\Microsoft\Windows\CurrentVersion\OpenGLDrivers` is `ripplegl.dll` (the INF
writes it and copies the DLL to `WINDOWS\SYSTEM`). `gltest` alone in a folder then reports
the same renderer and passes.

**The Quake III Arena demo runs**: installed from id Software's installer, started from its
Start menu entry or `quake3.exe`, it finds "hardware acceleration", lists our renderer, uses
multitexture and compiled vertex arrays, and plays its demos in a window and full-screen (the
display driver changes the mode for it and back). `timedemo` with `demo001`, default settings,
32-bit colour, guest with one CPU on a Proxmox VE host (i9-9900K, Intel UHD 630):

| Mode | frames a second |
|---|---|
| 640x480 in a window | about 300 |
| 1024x768 full-screen | 155 |
| 640x480 in a window, 16-bit desktop | about 380 |

    quake3.exe +set timedemo 1 +set logfile 2 +demo demo001
    (the result is in demoq3\qconsole.log)

**The picture matches the reference.** The same five frames of `demo001`, taken with the
game's own `screenshot` command once on the host GPU and once with Mesa's software
renderer (`LIBGL_ALWAYS_SOFTWARE=1`), differ in about 1% of their pixels by more than 8 of
255, along polygon edges, with a mean difference of 1.0 (`tools/win9x/tgadiff.py`; in
timedemo mode every frame shows the next recorded game state, so frame N is the same
picture on a renderer of any speed). Side by side they look the same.

    echo timedemo 1 > demoq3\cmp.cfg          (then: demo demo001, wait 200, and five times
    ...                                        screenshot and wait, then quit)
    quake3.exe +set r_fullscreen 0 +set r_mode 3 +exec cmp.cfg

**Windows 98's own OpenGL screen savers** all run on the driver through `opengl32.dll`: 3D
Pipes (drawn straight to the screen, no back buffer), 3D Flower Box, 3D Flying Objects, 3D
Text (glyph outlines from `wglUseFontOutlines`) and 3D Maze (textures; it switches the screen
to 640x480 for itself). On the long-used test VM 3D Text and 3D Maze show a black screen with
every renderer, Microsoft's own included; in a fresh install of the same drivers they work,
on the host GPU's path and on Mesa's software renderer, so that is the old VM's doing.
`gltest text` checks the font helpers (`wglUseFontBitmaps` too, which many programs use for
text).

Not there yet: a faster way to the screen than the read-back for windows that don't cover
the whole screen (full-screen windows have one since 2026-11-26, below), a comparison against
a real card of the time, and more programs.

### Full-screen OpenGL from the GPU (2026-11-26)

A window whose client area is the whole screen, with nothing over it and the pointer hidden
(a full-screen game, a screen saver), is no longer read back for GDI. Its finished frame is
copied on the host GPU into a display image and handed to the mini-VDD, the way Direct3D's
flip chain is shown ([directdraw.md](directdraw.md#full-screen-gpu-presentation)); patch 0014
does it in Mesa's WGL target. Any frame that doesn't qualify (a menu or the taskbar over the
window, the pointer shown, Windows 95, which lacks `GetCursorInfo`) takes the GDI path, and
the first such frame after GPU ones gives the display back to the desktop. `RIPPLE_GL_SCANOUT=0`
keeps every frame on the GDI path.

Intel host (VM 230 alone, one boot, the same driver files; the first two rows with
`RIPPLE_GL_SCANOUT=0`):

| Test | GDI path | From the GPU |
|---|---:|---:|
| `gltest 10 full`, 1280×720×32, frames a second | 520.4 | 3,409.1 |
| Quake III demo, `timedemo demo001`, 1024×768 full screen, fps | 293.7 | 617.1 |

Both `gltest` runs passed their pixel checks. Captures from QEMU's VNC server showed the
`gltest` scene and Quake III upright and in their colours; with the Start menu opened over the
running `gltest`, the menu showed over the live scene, and the desktop came back when each
program ended. [The three captures](img/gl-fullscreen-rig.webp)

    gltest 10 full                   (the window covers the screen at its current mode)
    quake3.exe +set timedemo 1 +set r_fullscreen 1 +set r_mode 6 +set nextdemo quit +demo demo001

## How a 2024 Mesa gets onto a 1998 Windows

Mesa needs a C11 and C++17 compiler; the compilers of the time (Visual C++ 6) can't build it.
The build uses llvm-mingw (clang, lld, compiler-rt, libc++, mingw-w64 with the old
`msvcrt.dll` as its C run-time). The build supplies the following compatibility code.

- **Version 4.0 in the file headers**, and everything from the toolchain linked statically.
- **No native thread-local storage** (`-femulated-tls`). Windows before Vista gives a DLL
  loaded at run time no thread-local storage of its own. The emulation's run-time function
  (`__emutls_get_address`) is ours, in `win9x/compat/k32shim.c`: the compiler run-time's own
  aborts on Windows 98, where `TlsGetValue` leaves the last error untouched. The shim also
  supplies libc++abi's two accessors for per-thread C++ exception state, compiled with
  the same emulation. This leaves the driver's native PE thread-local storage directory empty.
- **`win9x/compat/k32shim.c`**: the kernel32 functions newer than Windows 98 that the
  toolchain's libraries and Mesa call (one-time initialisation, slim locks, condition
  variables, fiber-local storage, `GlobalMemoryStatusEx` and a few more), written with what
  Windows 95 already has, plus `qsort_s`, which Windows 98's `msvcrt.dll` lacks. Windows 98
  refuses to load a file that imports a function it doesn't export, so the object defines each
  function and the import pointer for it, and is linked as an object file so that it comes
  before the import library. A thread's run-time data is freed when the thread ends: its
  emulated thread-local variables, the destructors of Mesa's C11 thread-specific storage, and
  (none are linked in today) C++ `thread_local` destructors and fiber-local storage callbacks.
  Windows 98 runs no thread-exit callbacks for a DLL loaded at run time, so the driver's entry
  point calls these itself (patch 0009).
- **Windows 95 kernel imports.** The shim resolves `IsDebuggerPresent`, `SwitchToThread`
  and `TryEnterCriticalSection` at run time. When the OS omits these exports, the
  debugger and try-lock checks return false and the thread-yield call sleeps briefly.
- **C11 threads for Windows 98** (`win9x/mesa/overlay/src/c11/impl/threads_win32.c` replaces
  Mesa's file): Mesa's version needs Vista's condition variables, and
  `TryEnterCriticalSection`, which Windows 98 exports but which always fails there. The
  thread and thread-storage parts are adapted from Mesa's file, so this one keeps its licence
  (Boost Software License 1.0, the notice in the file); the winsys keeps Red Hat's MIT notice
  from the DRM winsys it is adapted from.

`tools/win9x/check-imports.py` compares what a built file imports with what Windows 98 SE's own
system DLLs export (unpacked from the setup cabinets into `.tools/win9x/w98dlls/sys`); the
Mesa build runs it on every DLL and fails when something is missing. It can't see functions
that exist on Windows 98 only as stubs: those show up in testing.

Mesa's own sources target the Pentium II instruction set. Several routines in the
packaged llvm-mingw runtime use SSE instructions that Windows 95 does not enable.
The build compiles `mingw_pformat` and four decimal-conversion routines for the
Pentium II so OpenGL can start. It also compiles `roundf`, which Mesa calls when
a program changes the mipmap level bias, and `lroundf`, which converts pixel
coordinates in `glDrawPixels`. Direct3D uses that path to restore a back buffer
after a game writes into it.
It downloads llvm-mingw 20261104's matching mingw-w64 source revision on first use,
checks the archive's SHA-256 and caches it under `.tools/src/mesa-dl`. The compiler
flags and source dependencies are in `win9x/mesa/runtime.mk`.

Mesa also generates vertex-copy routines at run time. On Windows 95 OSR2.5,
a virtual Pentium III reported SSE support even though the OS disabled XMM
instructions. Quake III hit an illegal instruction in Mesa's generated 12-byte
vertex copy. The Win9x build now selects scalar vertex copies on Windows 95.
Rendering still uses virgl. Windows 98 and Me use the usual CPU feature checks.

## Build and test

    win9x/mesa/build.sh             # .tools/mesa/out-win9x/ripplegl.dll and opengl32.dll
    tools/win9x/build-tools.sh      # build/win9x/tools/gltest.exe (Visual C++ 6)

`ripplegl.dll` is the driver: Mesa's OpenGL with everything inside. Installed (in
`WINDOWS\SYSTEM`, with the registry value above) it serves every program through Windows'
`opengl32.dll`. For a test without installing, copy `ripplegl.dll` and Mesa's `opengl32.dll`
(a thin front for it) next to `gltest.exe` and run `gltest > out.txt`.

`gltest` spinning its small scene, vertical sync off (frames a second; the scene is trivial, so
this measures the cost of a frame's way to the screen, not the GPU):

| Renderer (Windows 98 SE guest) | 320x240 | 640x480 |
|---|---|---|
| Microsoft's software OpenGL 1.1 (`GDI Generic`), build host | about 4900 | |
| Mesa softpipe, build host | 33 to 500 | |
| Mesa virgl, Proxmox VE host with Intel UHD 630 | 1500 to 1800 | about 680 |
| the same through Windows' `opengl32.dll` (ICD) | about 1100 | |

`gltest` also prints the free "system resources" (Windows 9x's small fixed heaps for windows
and drawing objects) and free memory before and after: both must come back to where they
were.

`threadtest` (built with the others) checks for memory a long-running program loses: 300
threads, one after the other, each making a context, drawing a frame, checking a pixel and
cleaning up. It fails when the program's memory grows by 1 KB or more a thread after the
first 50. `threadtest 100 same` does the same in one thread (a leak per context rather than
per thread), and a third argument stops each round early (1: create and delete the context
only, 2: also make it current), to find the step that leaks; 4 lets each thread end with its
context still current, as a careless program does. Before the fixes below it lost about
375 KB a context and 30 KB more a thread; now its memory stays flat (UTM and Proxmox VE,
2026-11-17). With step 4 about 950 bytes a thread remain, under the 1 KB limit: likely
Windows' `opengl32.dll` releasing the context after the driver has cleaned up the thread,
which allocates some thread-local variables again (not chased further).

## Memory over a long session

`tools/win9x/memlog.c` writes one line every few seconds: free memory, Windows' memory
manager's own counters (free, locked, disk cache, committed; the ones System Monitor shows),
the free system resources, and what our VxD holds (`RVD_GET_STATS`: contexts, buffers, the
locked memory behind them, buffers kept for a host that stopped answering). Started next to a
game with `start /m memlog 20 0 e9`, it writes to QEMU's debug port, which the host can read
while the game runs. Give it a count or end it before restarting Windows: Windows 98 holds up
a restart for a console program that is still running.

Unreal Tournament on Direct3D, DM-Turbine and the maps after it, 26 minutes (2026-11-17,
Proxmox VE, 256 MB guest):

| minutes | free MB | disk cache MB | VxD buffers | VxD locked MB |
|---|---|---|---|---|
| 0 (before) | 211 | 13 | 0 | 0 |
| 1 | 125 | 36 | 150 | 6.1 |
| 8 | 105 | 36 | 453 | 8.1 |
| 14 (map change) | 105 | 36 | 479 | 6.2 |
| 22 | 99 | 36 | 525 | 7.5 |
| 24 (map change) | 78 | 58 | 524 | 6.4 |
| 26 | 77 | 58 | 525 | 7.5 |

The driver's share levels off after the first maps' textures are made and does not grow with
playing time. Free memory falls in steps when a map is loaded, and the step at 24 minutes is
Windows' disk cache taking the free memory for the map's files (cache and locked memory grow by
the same 22 MB), which it gives back when a program asks. An earlier 30-minute run that saw
16 MB less free memory measured free memory alone, which this cache is part of. (The Direct3D
driver draws through this OpenGL driver, so the run covers both.)

After the game ends the VxD holds nothing (0 contexts, 0 buffers). Free memory came back to
151 MB: 46 MB more disk cache than before the game, and 14 MB more in use otherwise (not
traced; the DirectDraw heap, made by the first DirectDraw program and kept until Windows ends,
is one candidate). A second start of the game, 3 minutes of play and quitting again left free
memory 29 MB lower, with the disk cache 29 MB larger: the 14 MB did not grow, and nothing is
lost from one run to the next.

## Things learnt the hard way

- **The virgl driver kept a copy of every shader.** It cloned the shader Mesa hands it, and
  the converter freed only the clone: about 375 KB lost for every context that drew, so a
  program that makes contexts now and then (a game between levels) grew without limit.
  Upstream Mesa fixed it in June 2026 (commit 0b1479eb, "virgl: fix memory leak on shader
  translation"); patch 0008 is that fix for 24.3.4.
- **Sample counts.** Mesa's Windows frontend asks for window buffers with a sample count of
  1; the host takes 1 for "multisampled, one sample" and such a colour buffer doesn't go with
  a plain depth buffer (`GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE`, nothing drawn). The winsys
  sends 0 for anything up to 1.
- **No memory behind textures.** When the host can copy between resources, the virgl driver
  asks for textures without guest memory and moves texture data through staging buffers. The
  read-back of a finished frame needs memory behind the window's colour buffer, so the winsys
  asks for it for display targets. Without it the first read-back failed on the host, the
  host put the context in its error state and ignored everything after: the program ran at
  17000 "frames" a second, showed nothing, and after a few such runs Windows 98 itself stopped
  drawing its windows.
- **Pixel formats the way 1999 expects them.** Mesa offers over a thousand pixel formats on
  this host (every colour layout, depth layout, sample count, with and without accumulation
  buffer), and says "24 colour bits" for a 32-bit format without alpha. Windows' own OpenGL
  screen savers look for a format whose colour bits equal the screen's depth with no alpha,
  found none, and gave up after five tries with a black screen; Quake III keeps the list in
  an array of 256. Patch 0003 makes the list short (72 formats: 32-bit with and without
  alpha and 16-bit; depth 24, depth 16, or depth 24 with 8 stencil bits; with and without
  back buffer and accumulation buffer), reports 32 colour bits for 32-bit formats as the cards of the time
  did, and adds 16-bit (5-6-5) formats for 16-bit screens that are drawn in 32 bits and
  converted by GDI on their way to the screen.
- **A program that asks for a stencil buffer got Microsoft's software OpenGL.** Until
  2026-11-17 the list had no format with a stencil buffer: patch 0003 kept Mesa's
  `Z24_UNORM_S8_UINT` and left out `S8_UINT_Z24_UNORM` as a duplicate, but virgl hosts have
  only the second (GL's `DEPTH24_STENCIL8`). Windows' `ChoosePixelFormat` then picked one of
  its own software formats, which have 8 stencil bits: Unreal Tournament's OpenGL renderer
  ran on "GDI Generic" without any warning. `tools/win9x/pftest.c` asks `ChoosePixelFormat`
  for what programs of the time request and fails when a request in the screen's colour depth
  gets a software format, or when the driver offers no formats at all.
- **A sleep before every buffer swap cost Unreal Tournament most of its frame rate.** Mesa's
  Windows frontend emulates a swap interval (waiting for the screen's refresh) by sleeping
  before each swap, and its default interval is 1. On Windows 98, `Sleep` wakes only at the
  next timer tick, 55 ms apart, and the refresh rate Windows reports there is 0 ("the
  adapter's default"), which Mesa clamped to 30 Hz: a program that never sets the interval
  waited about 55 ms per frame. Unreal Tournament's OpenGL renderer ran at 57 to 62 ms a
  frame, Quake III and `gltest` were fast because they set the interval to 0 themselves.
  Patch 0010 makes 0 the default (as drivers of the time had it), reads 0 and 1 from Windows
  as 60 Hz, and waits by yielding (`Sleep(0)`) until the period is up instead of sleeping,
  counting the next period from the end of the wait. The yielding keeps the CPU busy, so it
  happens only for programs that ask for an interval. `WGL_SWAP_INTERVAL` sets an interval
  for every program (0 included) and then overrides what a program asks for itself. Unreal
  Tournament's OpenGL renderer on DM-Turbine, after 20 and 30 seconds of play: 3.0 to 3.3 ms
  a frame; with `WGL_SWAP_INTERVAL=1`, 16.5 to 16.6 ms, one 60 Hz period (Proxmox VE, Intel
  UHD 630, no other VM on the host). UT's `stat fps` read much lower in the first seconds of
  a map than later (2.2 ms against 57 ms in the same view before the fix), so take readings
  after 20 seconds or more.
- **The Direct3D driver can't use `wglMakeCurrent`.** On Windows 98 it checks the window's
  pixel format through GDI and KERNEL32, which take locks of the whole system, and the
  Direct3D HAL is called with DirectDraw holding Windows' 16-bit lock: a game's drawing thread
  hung there (Tomb Raider: The Last Revelation, [directdraw.md](directdraw.md#games-2026-11-17)).
  Patch 0011 exports `RippleMakeCurrent(DHGLRC)`, which makes a context current with no
  drawable (or none, with 0) and looks at no window or device context; the HAL takes its
  context from `DrvCreateContext` and its functions from `DrvGetProcAddress`, all in
  `ripplegl.dll`. It lets go of a context without a flush (release behaviour NONE): the HAL
  lets go at the end of every Direct3D call, and a flush each time sent the host a command
  stream per call and cost 3DMark 2000 a quarter of its score.
- Patch 0012 exports `RipplePresentColor()` for the Direct3D HAL. With the HAL's framebuffer
  bound, it copies the colour attachment into a reusable 32-bit display image on the GPU,
  submits the drawing, and asks the mini-VDD to display that image. The GPU converts 565
  colour during the copy. CPU access to a DirectDraw surface can still require a read-back.
- **The host's log** is where rendering errors show (`journalctl` on Proxmox VE, lines from
  `QEMU[pid]`).

## Finding out why a program doesn't work

Environment variables, set before the program starts (in a batch file: `set NAME=value`):

| Variable | What |
|---|---|
| `RIPPLE_GL_TRACE=file` | one line for every call Windows' `opengl32.dll` makes into the driver (pixel formats asked for and set, contexts made and made current, buffer swaps, extension functions looked up): shows where a program gives up |
| `RVD_LOG=file` | the winsys' first few thousand requests to the VxD (buffers made, transfers, frames presented, and for each submission the virgl commands in it by number and how much its transfers move) |
| `LIBGL_ALWAYS_SOFTWARE=1` | Mesa's software renderer instead of the host GPU: is the fault in the GPU path? |
| `RIPPLE_GL_SCANOUT=0` | a full-screen window read back for GDI like any other: is the fault in the GPU display path? |
| `MESA_EXTENSION_MAX_YEAR=2000` | hide OpenGL extensions newer than that year, for programs that copy the extension list into a small buffer |
