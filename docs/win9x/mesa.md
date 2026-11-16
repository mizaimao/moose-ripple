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

Not there yet: a faster way to the screen than the read-back (it is what limits the frame
rate above), a comparison against a real card of the time, and more programs.

## How a 2024 Mesa gets onto a 1998 Windows

Mesa needs a C11 and C++17 compiler; the compilers of the time (Visual C++ 6) can't build it.
The build uses llvm-mingw (clang, lld, compiler-rt, libc++, mingw-w64 with the old
`msvcrt.dll` as its C run-time), and four things make the result load and run on Windows 98:

- **Version 4.0 in the file headers**, and everything from the toolchain linked statically.
- **No native thread-local storage** (`-femulated-tls`). Windows before Vista gives a DLL
  loaded at run time no thread-local storage of its own. The emulation's run-time function
  (`__emutls_get_address`) is ours, in `win9x/compat/k32shim.c`: the compiler run-time's own
  aborts on Windows 98, where `TlsGetValue` leaves the last error untouched.
- **`win9x/compat/k32shim.c`**: the kernel32 functions newer than Windows 98 that the
  toolchain's libraries and Mesa call (one-time initialisation, slim locks, condition
  variables, fiber-local storage, `GlobalMemoryStatusEx` and a few more), written with what
  Windows 95 already has, plus `qsort_s`, which Windows 98's `msvcrt.dll` lacks. Windows 98
  refuses to load a file that imports a function it doesn't export, so the object defines each
  function and the import pointer for it, and is linked as an object file so that it comes
  before the import library.
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

The code is built for the Pentium II instruction set (no SSE2: Windows 98 predates it, and the
guest CPU model may be an old one).

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

## Things learnt the hard way

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
  an array of 256. Patch 0003 makes the list short (48 formats: 32-bit with and without
  alpha and 16-bit, three depth/stencil layouts, with and without back buffer and
  accumulation buffer), reports 32 colour bits for 32-bit formats as the cards of the time
  did, and adds 16-bit (5-6-5) formats for 16-bit screens that are drawn in 32 bits and
  converted by GDI on their way to the screen.
- **The host's log** is where rendering errors show (`journalctl` on Proxmox VE, lines from
  `QEMU[pid]`).

## Finding out why a program doesn't work

Environment variables, set before the program starts (in a batch file: `set NAME=value`):

| Variable | What |
|---|---|
| `RIPPLE_GL_TRACE=file` | one line for every call Windows' `opengl32.dll` makes into the driver (pixel formats asked for and set, contexts made and made current, buffer swaps, extension functions looked up): shows where a program gives up |
| `RVD_LOG=file` | the winsys' first few thousand requests to the VxD (buffers made, transfers, submissions, frames presented) |
| `LIBGL_ALWAYS_SOFTWARE=1` | Mesa's software renderer instead of the host GPU: is the fault in the GPU path? |
| `MESA_EXTENSION_MAX_YEAR=2000` | hide OpenGL extensions newer than that year, for programs that copy the extension list into a small buffer |
