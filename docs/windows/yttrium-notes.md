# What Yttrium and vmlab-win3d give us

2026-11-14. Two 2026 projects did for Windows 10 and 11 what we want for Windows 7, on a
different host path (Vulkan through Venus, driver model 2.0). This is what was read, what
may be taken, and what their notes warn about. Their MIT and BSD files are cleared for use
here, with credit in the README; their MPL-2.0 files are read-only for good.

Read: the Yttrium kernel driver (`arehnman/yttrium-virtio-gpu`, branch `yttrium-wddm2`,
2026-11-01), its Mesa fork (`arehnman/virtio-win-mesa`, branch `yttrium-experimental`, Mesa
26.1.0, commit 76c095b), and the build recipe, patches and engineering notes of
`VMLabDev/vmlab-win3d`. Nothing is copied into this repository yet.

## What is in there, by licence

| Part | Size | Licence | For us |
|---|---|---|---|
| Kernel driver, WDDM side (`viogpu3d/*`) | 13 000 lines | MPL-2.0 | Read only. It is driver model 2.0 with blob memory anyway |
| Kernel driver, virtio transport (`viogpu/common`: queues, PCI, ids) | 5 000 lines | BSD-3-Clause (virtio-win's, with Yttrium's changes; no MPL mark) | **Take.** 1 260 changed lines in the queue code alone: control-queue back-pressure when descriptors run out, reference-counted waits, teardown that cannot lose a wake-up, notification suppression |
| Mesa: Direct3D 9 driver part (`frontends/d3d10umd/D3D9*`) | 14 700 lines | MIT | **Take as the base of our Direct3D 9 part** (below) |
| Mesa: Direct3D 10/11 part, changed (`frontends/d3d10umd`, the rest) | +16 000 lines over upstream | MIT (VMware's text) | Take the general fixes; see "Direct3D 10" |
| Mesa: a virgl winsys for Windows (`winsys/virgl/gdi`) | 1 300 lines | MIT (Red Hat's text) | **Take.** It is the piece that lets Mesa's virgl driver talk to the kernel driver; upstream Mesa never got it |
| Mesa: the bridge to the Direct3D runtime's kernel callbacks (`gdikmt_d3dddi.cpp`, `auxiliary/gdikmt`) | 1 200 lines and more | No licence header | Do not copy. Written for driver model 2.0 in any case; we write ours for 1.1 |
| Mesa: Yttrium's own Gallium driver on Vulkan (`winsys/yttrium`) | 60 000 lines | MPL-2.0 | Read only, and not needed: we draw through virgl |
| vmlab-win3d: build scripts, notes | | MIT | Recipe and notes used freely |
| vmlab-win3d: patches to Mesa's MIT files | 13 for Direct3D 9, about a dozen for Direct3D 10/11, a few for OpenGL | MIT (the licence of the file patched) | Take the ones for files we take |
| vmlab-win3d: patches to the kernel driver | 4 | MPL-2.0 files | Ideas only |

## The Direct3D 9 part

What it is: a thin layer. Entry point `OpenAdapter`; it reports the **Windows 7 level** of
the Direct3D 9 driver interface; 82 device functions are filled. State setters only store,
and each draw replays the state through plain Gallium calls. Shaders go through a copy of
Mesa nine's translator and come out as **TGSI**, which is what virgl takes. Vertex and pixel
shader 3.0, 4 render targets, 46 formats, DXT; multisampling 2, 4, 8.

What it leans on:

- **No fixed-function generator.** It expects the Direct3D 9 runtime to turn fixed-function
  state into generated shaders, which Windows does for a driver that offers no
  fixed-function hardware. Its comments name the Windows 10 runtime; whether Windows 7's
  runtime hands over the same shaders is the first thing to test. The driver still has to
  do fog, alpha test, point expansion (a geometry shader) and pre-transformed vertices.
- **Yttrium's driver in six places** with no stand-in (flush hand-off, late draw failures,
  the host's multisample counts and anisotropy), and two MPL headers are included. Those
  have to be replaced by our own code.
- **Vulkan habits that are wrong on virgl:** clears go to surfaces that are not bound (virgl
  clears only bound ones, and only on a host newer than the Proxmox one), and user clip
  planes are written by the shader but never switched on in the rasterizer.
- **No state cache:** every draw creates and destroys its blend, rasterizer, depth and
  sampler objects. On virgl each of those is a command to the host. A cache is needed
  before it can be fast.
- Not done: queries beyond a stub, patches, DXVA video, W-buffer, wireframe.

Moving it to our Mesa 24.3.4 is about eleven mechanical changes (capability lookups, shader
stage names, sampler-view and constant-buffer arguments, framebuffer surfaces held by
value, upload helper arguments). The other way round is to build the Windows side from the
fork's 26.1 tree without its MPL folders; see "Open choice".

## Direct3D 10

The fork's Direct3D 10/11 files carry general fixes worth having whatever tree we use: a
wrong null check after resource creation, query results that waited when told not to,
reference-counted stream-output targets, unbinding a destroyed resource, depth format
mappings, an empty input layout, shader token bounds checks, and the DXGI present, blit
and display-mode entry points that upstream left as stubs. It also adds Direct3D 10.1 and
11 device tables and shader model 5, which Windows 7's compositor does not need.

## Pitfalls their notes save us

Kernel driver and build:

- Linker: `/SECTION:INIT,d` alone leaves `DriverEntry` not executable and the guest crashes
  on load; they use `INIT,ERD`. clang's `intrin.h` must be included before `wdm.h`. Kit
  headers are included in any letter case: a case-insensitive overlay. Tracing macros (WPP)
  need a Windows-only tool: stub them, and keep `DbgPrint` alive in our builds, or there is
  nothing to debug with.
- A kernel GDI device presents without any user-mode driver having set a context up: the
  kernel driver must create a host context for itself at device creation.
- The desktop's shadow and staging surfaces must be created as cached memory, or copies to
  them crawl on AMD hosts.
- Timeouts: before answering a reset, stop interrupt and queue users, cancel old requests,
  wait for the device to report status zero, then free; recreate host contexts at restart.
- Online snapshots of a VM with virgl fail in QEMU ("virgl is not migratable"); offline
  ones work.

Direct3D 9 and DirectDraw:

- DirectDraw loads the Direct3D 9 driver part with an older interface number and builds its
  own capabilities from six queries; answering "not implemented" to one of them
  (`GETD3D7CAPS`) silently puts DirectDraw on software and makes DirectX's diagnostics show
  acceleration as unavailable.
- A write-only lock without "discard" must keep the surface's contents. A windowed
  DirectDraw blit to the screen arrives as a present from a plain off-screen surface, which
  must be made presentable. Colour-key and mirror blits must be drawn on the GPU.
- Pre-transformed vertices come with a pixel shader only; the driver generates the vertex
  shader for every declaration, and a declaration without diffuse gives opaque white.
  Fog is clamped per pixel, not per vertex. Points need colours, texture coordinates and fog
  carried through their expansion.
- A failing create call that reports "invalid argument" to the Direct3D 11 runtime removes
  the whole device.
- At presentation interval one the Direct3D 9 runtime waits for the vertical blank and then
  presents: a slow present halves the frame rate.

Install:

- A driver without a usable user-mode part starts and then shows problem code 43; a
  rejected signature shows code 52.
- Uninstalling leaves the adapter's registry key behind with our entries in it; they have
  to be removed by hand.
- Their INF set two machine-wide switches (preemption off, a debug mode for timeouts);
  vmlab-win3d removed them. We should not set them either.

Nothing in either project was run on Windows 7 or on driver model 1.x.

## Open choice: which Mesa for the Windows side

1. **Stay on 24.3.4**, the version the Mac side builds and the one shown today to build for
   Windows 7 and run its software OpenGL and Direct3D 10 there. Bring the Direct3D 9 part,
   the virgl winsys and the fixes back to it.
2. **Build from the fork's 26.1 tree** without its MPL folders. Less to carry back, and the
   Direct3D 10 fixes come along. Open: whether Mesa 26.1 still runs on Windows 7, and it
   parts ways with the Mac side's Mesa.

A software-only build of the fork's tree for Windows 7 answers the open point in a day; the
choice is made after that.
