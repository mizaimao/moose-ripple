# Mesa on Windows 98

Step 4 of [plan.md](plan.md): OpenGL for Windows 98 from a current Mesa (24.3.4, the release the
Mac OS X and Windows 7 sides use), built on the Linux build host with `win9x/mesa/build.sh`.

## State (2026-11-15)

Mesa runs on Windows 98 SE with its software renderer (softpipe): `tools/win9x/gltest.c`, a
small OpenGL 1.1 program written the way a period game is (ChoosePixelFormat,
wglCreateContext, immediate mode), reports `Mesa`, `softpipe`, `3.3 (Compatibility Profile)
Mesa 24.3.4` and passes its pixel checks (clear, smooth shading, depth test, texture,
blending) with Mesa's `opengl32.dll`, `libgallium_wgl.dll` and `libglapi.dll` next to it.
With Windows 98's own `opengl32.dll` (Microsoft's software OpenGL 1.1) the same program
passes too, which is the comparison.

Not there yet: the host GPU (the virgl driver with a winsys on our VxD,
[vxd-interface.md](vxd-interface.md)), and loading through Windows' own `opengl32.dll` as an
installable client driver.

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

| Renderer (Windows 98 SE, QEMU/KVM on the build host, 320x240 window) | frames a second |
|---|---|
| Microsoft's software OpenGL 1.1 (`GDI Generic`) | about 4900 |
| Mesa softpipe | 33 |

softpipe is Mesa's slow reference renderer; it is here to prove the port, not to be used.
