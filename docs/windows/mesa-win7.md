# Mesa on Windows 7

Does Mesa 24.3.4, the release the Mac side uses, build for Windows 7 and run there, before
any driver of ours exists? Yes, with software rendering: Mesa's OpenGL draws in the Windows 7
test VM, and its Direct3D 10 driver (`libgallium_d3d10.dll`) gives a working Direct3D 10
device through Windows 7's own Direct3D runtime. 2026-11-14.

## Build

`windows/mesa/build-win7.sh`, on the Linux build host:

- Source: the release tarball (`mesa-24.3.4.tar.xz`, the same sha256 as the Mac side), unpacked
  into its own tree `.tools/mesa/src-24.3.4-win` and built in `.tools/mesa/build-win7-x64`, so
  nothing of the Mac side's tree or builds is touched. Patches in `windows/mesa/patches/`.
- Toolchain: llvm-mingw 20261104 (clang 23.1.5), the variant that links against `msvcrt.dll`,
  since Windows 7 has no Universal C runtime unless an update adds it. Downloaded into
  `.tools/llvm-mingw` from
  `https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/llvm-mingw-20261104-msvcrt-ubuntu-22.04-x86_64.tar.xz`
  (83 MB, sha256 `6e3342e2e0dfa56e9178f3c34b198f278bd5e603db4a759455d1439cc2230630`).
- Options: `-Dplatforms=windows -Dmin-windows-version=7 -Dgallium-drivers=softpipe
  -Dgallium-d3d10umd=true -Dopengl=true`, LLVM off, no Vulkan, no GLES or EGL.
  `min-windows-version=7` makes Mesa use its own futex replacement instead of
  `WaitOnAddress` (Windows 8). Mesa's `meson.build` still defines `_WIN32_WINNT=0x0A00`; that
  only unlocks declarations, and the DLLs import nothing newer than Windows 7 (below).
- Direct3D 10 headers: Mesa's `d3d10umd` wants the WDK's DDI headers in `include/winddk`
  (its `README.txt` lists them). The build copies them from WDK 7.1 (`d3d10umddi.h`,
  `d3dumddi.h`, `d3dkmthk.h`, `d3dkmddi.h`, `d3dkmdt.h`, `d3dukmdt.h`, `dxgiddi.h`,
  `dxgiformat.h`, `dxgitype.h`, `dxmini.h`, `wmidata.h` and the two tokenized program format
  headers) into the build's own tree; they are never committed.

What had to change:

| Where | Change | Why |
|---|---|---|
| Compiler flags | `-Wno-error=incompatible-pointer-types` | clang 23 makes it an error; Mesa's SPIR-V code passes a `jmp_buf` to MinGW's `__builtin_setjmp` |
| Local copy of `d3d10umddi.h` | `MAKE_##TYPE##(` becomes `MAKE_##TYPE(` | only MSVC accepts the second paste |
| Local copy of `d3d10umddi.h` | its global `const UINT`/`const INT` constants made `static` | in C files they are duplicate symbols; with MSVC Mesa hides them by including `d3d10_1.h` first |
| `patches/0011-x86-aligned-matrix-stack.patch` | the GL matrix stacks allocated 16-byte aligned | 32-bit only: `msvcrt.dll` aligns `calloc` to 8 bytes (below) |
| `patches/0010-win7-wdk71-d3dkmt-and-gdi.patch` | `D3DKMTPresent` takes a const pointer; `KMTQAITYPE_XBOX`, `KMTQAITYPE_PHYSICALADAPTER*` and `D3DKMTSetVidPnSourceOwner1` left out at the Windows 7 interface version; two helpers in `d3d10_gdi.c` made `static` | WDK 7.1's headers are older than the ones Mesa is written against |

Result, in `.tools/mesa/out-win7-x64`: `opengl32.dll` (199,680 bytes), `libgallium_wgl.dll`
(16 MB), `libglapi.dll`, `libgallium_d3d10.dll` (8.7 MB). Their imports are only
`KERNEL32`, `USER32`, `GDI32` and `msvcrt`, plus each other; C++ is linked in statically.

## In the Windows 7 test VM

VM 220 (Windows 7 SP1 x64, the Standard VGA adapter, [test-vms.md](test-vms.md)), with two
small test programs built by llvm-mingw (`tools/win7-hello/gltest.c` and `d3d10test.c`):

| Test | Result |
|---|---|
| `gltest.exe` with Windows' own `opengl32.dll` | Microsoft, GDI Generic, OpenGL 1.1.0; the triangle is red |
| `gltest.exe` with Mesa's `opengl32.dll` next to it | Mesa, softpipe, OpenGL 3.3 (Compatibility Profile) Mesa 24.3.4; the triangle is red (255 0 0 read back) |
| `d3d10test.exe`: `D3D10CreateDevice(D3D10_DRIVER_TYPE_SOFTWARE)` on `libgallium_d3d10.dll` | device created; the render target cleared to green, a triangle drawn with a vertex and a pixel shader, copied to a staging texture and mapped: 255 0 0 255 inside the triangle, 0 255 0 255 outside |

The shaders are `tools/win7-hello/tri.hlsl` (vertices made from `SV_VertexID`, no vertex
buffer), compiled to DXBC for `vs_4_0` and `ps_4_0` by `hlsl2dxbc.c` with `D3DCompile` from
Wine 10's `d3dcompiler_47.dll` (which uses vkd3d-shader) on the build host, and embedded in the
test as `tri_vs.h` and `tri_ps.h`. Windows 7's runtime accepted the blobs.

So Mesa's Direct3D 10 user-mode driver loads under Windows 7's Direct3D 10 runtime, creates a
device, and its resource, clear, shader, draw, copy and map paths work. Not covered yet:
Direct3D 10.1 and its 9.x feature levels (what the desktop compositor uses), and the DXGI
present path.

## 32-bit

`ARCH=x86 windows/mesa/build-win7.sh` builds the same four DLLs for 32-bit Windows
(`.tools/mesa/build-win7-x86`, `out-win7-x86`); again only `KERNEL32`, `USER32`, `GDI32` and
`msvcrt` imports, and `OpenAdapter10` exported undecorated. Tested in VM 220 as 32-bit
programs (WOW64 on Windows 7 x64; there is no 32-bit Windows 7 VM):

- Direct3D 10: the same results as on x64, shader draw included.
- OpenGL first crashed in `libgallium_wgl.dll` (access violation in `_math_matrix_ctr`, an
  aligned SSE store): the GL matrix stacks come from `calloc`/`realloc`, which `msvcrt.dll`
  aligns to 8 bytes on 32-bit Windows, while `GLmatrix` is `alignas(16)`. Patch
  `0011-x86-aligned-matrix-stack.patch` allocates them with Mesa's `align_calloc`/
  `align_realloc`/`align_free`; with it the GL test draws (softpipe, OpenGL 3.3, the triangle
  red). Other `alignas(16)` structures on the plain heap could show the same on 32-bit.

## The Yttrium fork (Mesa 26.1) on Windows 7

Tried once (2026-11-14) to see whether a newer Mesa would do: `virtio-win-mesa` at 76c095b,
the same options, without Yttrium's own driver. It does not build against WDK 7.1's headers.
Core Mesa compiles; what breaks: a new shared helper (`src/gallium/auxiliary/gdikmt`, part of
`libgallium` on Windows, so OpenGL too) includes `d3dkmthk.h` without the DDK include path,
WDK 7.1's `d3dkmthk.h` needs SAL annotations the tree no longer defines, and the Direct3D
10/9 code uses Windows 8 and later DDI types (`D3D11_1_DDI_BLEND_DESC`,
`D3D11_1_DDI_LOGIC_OP_*`, `DXGI_DDI_ARG_PRESENT1`, `BLT1`, `OFFERRESOURCES`,
`RECLAIMRESOURCES`, the multiplane overlay calls). The Windows side therefore stays on Mesa
24.3.4; the fork's Direct3D 9 part (MIT) is to be carried back to it.

## The virgl variant: the Mesa side of virtio-win pull request 943

`VARIANT=virgl windows/mesa/build-win7.sh` builds Mesa's virgl driver for Windows 7 x64 as the
user-mode half of a virtio-gpu display driver: `viogpu_wgl.dll` (OpenGL, through Mesa's own
`opengl32.dll` or as the display driver's OpenGL client driver) and `viogpu_d3d10.dll`
(Direct3D 10 and 10.1). It cannot draw until the kernel driver exists; what is checked so far is
that it builds and loads.

Source (in `windows/mesa/patches-virgl/`, applied only for this variant, in its own source tree
`.tools/mesa/src-24.3.4-win-virgl`): the five commits by Max R on `viogpu_win` of
`gitlab.freedesktop.org/max8rr8/mesa`
(Mesa merge request 24223), on top of upstream 366e7e2ddc7. They apply to 24.3.4 unchanged:

| Patch | What it does | Licence of what it adds |
|---|---|---|
| `0001-viogpu-wgl-WGL-GDI-Framebuffer-improvements` | WGL: framebuffer handling for a GDI-presenting driver | changes to MIT files |
| `0002-viogpu-gdikmt-Introduce-gdikmt` | the build hooks and a new handle type for the kernel bridge, **without the bridge itself** (below) | changes to MIT files |
| `0003-viogpu-d3d10umd-Viogpu-for-windows-improvements` | the Direct3D 10 driver: talks to a kernel driver through the bridge, formats, DXGI present | changes to MIT files |
| `0004-viogpu-virgl-gdi-Introduce-virgl-winsys-for-windows` | virgl's winsys for Windows (`winsys/virgl/gdi`), and the private kernel interface header `src/virtio/wddm/viogpu_wddm_driver.h` | winsys: MIT (Red Hat header); interface header: BSD-3-Clause (Red Hat) |
| `0005-viogpu-targets-Add-virgl-to-wgl-and-d3d10-targets` | virgl in the WGL and Direct3D 10 targets; `dxgi_framebuffer.cpp` | MIT (Microsoft header, from Mesa's d3d12 WGL code) |

Scanned for GPL, LGPL and MPL text: none in any patch. Four files of commit 77e160421bd, the
kernel bridge `gdikmt` (`auxiliary/gdikmt/gdikmt.h`, `gdikmt_d3dkmt.c`,
`frontends/d3d10umd/gdikmt_d3dddi.cpp`, `gdikmt_d3dddi.h`), carry no licence header at all.
Decision (2026-11-14): we write our own bridge; those four files are never committed. Ours is
in `windows/mesa/overlay-virgl/` (MIT, same four file names, written from the description in
"What the rest expects from the kernel bridge" below): the build copies it over the source
tree after the patches, and the source tree's stamp covers it, so a change to it unpacks the
tree again.

Our patches on top: `0010` and `0011` as above, `0012` (the DDK headers on the include path of
the bridge, the virgl winsys and the WGL target: with MinGW, `d3dkmthk.h` comes from
`include/winddk`), `0013` (a pointer-to-handle cast clang rejects on 64-bit), `0014` (the
Windows 7 completion event and which allocations belong to the runtime, below), `0015` (`bool`
for the `boolean` type Mesa has dropped, which the winsys still used: the original bridge header
defined it, ours does not). The build also leaves WDK 7.1's `dxgiformat.h` and `dxgitype.h` out
of `include/winddk`, so the DXGI framebuffer code sees MinGW's newer ones, and has the local
`d3dkmthk.h` include Mesa's `winddk_compat.h` (empty SAL annotations) itself.

Result: `viogpu_wgl.dll` imports only `GDI32`, `KERNEL32`, `USER32`, `msvcrt` and
`libglapi.dll`; `viogpu_d3d10.dll` only `KERNEL32` and `msvcrt`. The bridge loads the D3DKMT
functions from `gdi32.dll` at run time, so nothing Windows 7 lacks is imported. In VM 220 both
load with all imports resolved (`tools/win7-hello/dllload.c`); `OpenAdapter10`,
`OpenAdapter10_2`, `DrvCreateContext`, `DrvSetPixelFormat` and `DrvValidateVersion` are there.

Windows 8 and later assumptions found:

- Command completion: after each submission the bridge asks the kernel to signal a CPU event
  when the GPU is done, through `SignalSynchronizationObject2` with a CPU event (the D3DKMT
  function from `gdi32` on the OpenGL path, the runtime's callback of the same name on the
  Direct3D path). Both are WDDM 1.2 (Windows 8). On Windows 7 (`0014`) the winsys ends the
  command buffer with a command of the kernel driver's private protocol,
  `VIOGPU_CMD_SIGNAL_EVENT`, and the kernel driver sets the event when the buffer has run on the
  host; with no room left for that command, the event is set at submission.
- Nothing else: the Direct3D driver reports the Direct3D 10.0, 10.0.x, 10.0.7 and 10.1 DDI
  versions only, which Windows 7's runtime has.

The private protocol between these DLLs and the kernel driver (escape packets for resource
info and busy state, and the per-allocation data exchanged at allocation creation and open) is
in `viogpu_wddm_driver.h` and must match the kernel driver (`windows/kmd`).

### What the rest expects from the kernel bridge

Described from how the other patches use it, for writing our own (names as the callers use
them; nothing taken over from the four files):

- A device object standing for one adapter and device in the kernel driver, with a table of
  operations, and two ways to make one: from a window's device context (`gdikmt_create_from_hdc`,
  used by the WGL target: open the adapter behind the HDC and create a device on it with the
  D3DKMT functions), and from the Direct3D runtime (`gdikmt_d3dddi_fill_basefuncs`, used when
  the Direct3D driver creates its device: the same operations, carried out through the runtime's
  device callbacks and handle; the Direct3D device keeps this bridge device in its own state as
  `gdikmt_device_d3dddi`).
- Device operations the virgl winsys calls: query adapter information (driver-private data),
  escape (driver-private packets), create allocations for a resource (in: count, per-allocation
  info, resource-level private data; out: resource handle), destroy them, open shared
  allocations from a global handle (in: handle, count, per-allocation open info, private data;
  plus a buffer for all private data), query a shared allocation, lock an allocation for CPU
  access, create a context, present, destroy; the Direct3D side also uses flush, and set display
  mode for DXGI's present.
- A context object holding the current command buffer, allocation list and patch-location list
  with their sizes, with a submit operation (`gdikmt_render`): command offset and length, the
  number of allocations and patch locations used, the sizes wanted for the next buffers with
  flags to grow each, and an optional event to signal on completion. After a submit the context
  points at the buffers the kernel hands back.
- Helper entry points with the same shape as the operations: `gdikmt_createallocation`,
  `gdikmt_openallocation`, `gdikmt_render`.

## The Direct3D 9 part (2026-11-15)

Windows 7 programs that use Direct3D 9, and Windows' own assessment tool, need a Direct3D 9
user-mode driver. Ours is the one of the Yttrium project's Mesa fork (its `virtio-win-mesa`
at commit 76c095b, Mesa 26.1.0; `docs/windows/yttrium-notes.md`): the ten files
`src/gallium/frontends/d3d10umd/D3D9*`, all MIT, by Ake Rehnman, with the shader translator
taken from Mesa's "nine" (MIT, Joakim Sindholt and Christoph Bumiller). They are in
`windows/mesa/overlay-virgl/` with their notices, and are built into the same DLL as the
Direct3D 10 driver, which then also exports `OpenAdapter` (patch 0019).

What was changed to bring them from that tree to Mesa 24.3.4 on virgl:

- **The renderer underneath.** The files call that project's own renderer in a dozen places
  and include two of its headers (MPL-2.0, not used here). `D3D9Host.h` and `D3D9Host.cpp`
  (ours) answer the same calls for virgl: a flush is a flush, the rest has nothing to answer.
  The multisample counts and the anisotropy limit, which came from that renderer, are fixed
  for now: no multisampling, 16 samples of anisotropic filtering (Windows 7's Direct3D 9
  wants that many for a hardware device, `docs/windows/kmd-port-notes.md`).
- **Mesa 26.1 to 24.3.** Capability lookups by function instead of the screen's tables;
  one more argument to `set_sampler_views`, `set_constant_buffer` and `ureg_tex_insn`;
  framebuffer and clear surfaces created as objects instead of held by value; three small
  helpers that 24.3 lacks (`D3D9Compat.h`, ours).
- **The kernel bridge** (`gdikmt_d3dddi`, ours) learned what a Direct3D 9 device needs:
  presents go through the device's own present callback with the destination the runtime
  names, and an opened resource is described in Direct3D 9's structure.
- **The kernel driver's allocation data** has no blob memory flag and no stride on Windows 7;
  the two places that read them use what the format says.

Two faults showed in the host's log at the first test, both for resources Gallium allows and
virglrenderer 0.10 refuses, and both are put right in the virgl driver (patch 0020): a texture
without any binding (the Direct3D 9 code reads pictures back through such textures), and a
sample count of one for "not multisampled" (the same thing patch 0016 met in the Direct3D 10
driver).

**First test** (`tools/win7-hello/d3d9test.c`, test VM, Aero on): a hardware device with
vertex processing in the driver is created in a window, the back buffer cleared to green, a
red triangle drawn from screen-space vertices, the picture read back (right in both colours)
and presented: the window shows it. Nothing in the host's log.

Not looked at yet: everything beyond that test. The fork's notes say what to expect
(`docs/windows/yttrium-notes.md`): fixed-function drawing leans on the Direct3D 9 runtime
turning it into shaders, user clip planes are not switched on, and every draw creates and
destroys its state objects, which on virgl is a command to the host each.
