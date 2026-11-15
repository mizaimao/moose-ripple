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
smooth shading, depth test, texture, blending) with Mesa's `opengl32.dll`,
`libgallium_wgl.dll` and `libglapi.dll` next to it. The window shows the picture, clipped by
other windows as it should be. On a host without virgl the same DLLs fall back to Mesa's
software renderer (softpipe), and with Windows 98's own `opengl32.dll` (Microsoft's software
OpenGL 1.1) the program passes too, which is the comparison.

The pieces: Mesa's virgl driver, unchanged; a winsys for it
(`win9x/mesa/overlay/src/gallium/winsys/virgl/rvd/`) on our VxD's interface
([vxd-interface.md](vxd-interface.md)); and a small patch that adds both to Mesa's Windows
OpenGL target. A finished frame is read back from the host into the buffer's guest memory
and drawn on the window with GDI (`SetDIBitsToDevice`).

Not there yet: loading through Windows' own `opengl32.dll` as an installable client driver
(so that games find it without DLLs in their folder), a faster way to the screen than the
read-back, and real programs (first: the Quake III Arena demo).

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
- **C11 threads of our own** (`win9x/mesa/overlay/src/c11/impl/threads_win32.c` replaces
  Mesa's file): Mesa's version needs Vista's condition variables, and
  `TryEnterCriticalSection`, which Windows 98 exports but which always fails there.

`tools/win9x/check-imports.py` compares what a built file imports with what Windows 98 SE's own
system DLLs export (unpacked from the setup cabinets into `.tools/win9x/w98dlls/sys`); the
Mesa build runs it on every DLL and fails when something is missing. It can't see functions
that exist on Windows 98 only as stubs: those show up in testing.

The code is built for the Pentium II instruction set (no SSE2: Windows 98 predates it, and the
guest CPU model may be an old one).

## Build and test

    win9x/mesa/build.sh             # .tools/mesa/out-win9x/{opengl32,libgallium_wgl,libglapi}.dll
    tools/win9x/build-tools.sh      # build/win9x/tools/gltest.exe (Visual C++ 6)

Copy the three DLLs and `gltest.exe` into one folder in the guest and run
`gltest > out.txt`. `gltest` alone in a folder uses Windows' own OpenGL.

`gltest` spinning its small scene, vertical sync off (frames a second; the scene is trivial, so
this measures the cost of a frame's way to the screen, not the GPU):

| Renderer (Windows 98 SE guest) | 320x240 | 640x480 |
|---|---|---|
| Microsoft's software OpenGL 1.1 (`GDI Generic`), build host | about 4900 | |
| Mesa softpipe, build host (waits for the display rate) | 33 | |
| Mesa virgl, Proxmox VE host with Intel UHD 630 | about 1500 | about 680 |

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
- **The host's log** is where rendering errors show (`journalctl` on Proxmox VE, lines from
  `QEMU[pid]`). `RVD_LOG=file` in the environment makes the winsys write its first few
  thousand requests to a file.
