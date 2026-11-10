# M3 report: 3D transport

Date: 2026-11-10. Details: [notebook.md](notebook.md); API: [../kext-uapi.md](../kext-uapi.md).

## Accepted

The handoff's test: a guest command-line tool creates a context, submits a virgl clear,
reads the result back and verifies the pixels. `tools/virgl-clear` does exactly that through
our user client: 0 of 4096 pixels wrong, in both 64-bit and 32-bit processes, rendered by the
host's UHD 630 through virglrenderer.

## What exists now

- **Interrupt-driven virtio** (legacy INTx), up to 16 commands in flight, polling only as a fallback.
- **VIRGL negotiated**; the host's capability sets are readable (GLSL 4.60, 8 render targets,
  16x MSAA, 16384-px textures on this host).
- **3D commands**: contexts, 3D resources with guest backing, context attach, fenced
  `SUBMIT_3D` and 3D transfers.
- **GPU-neutral user-client API, version 1** (decision 0005): device info, capability blobs,
  contexts, mappable buffers, submit, transfer, fences; virgl is backend 1 and shows only in
  the command stream, the capability blobs and the buffer parameters.
- `RippleAccelerator`, an `IOAccelerator` under our framebuffer: the service M4's GL bundle and
  M5's WindowServer side will hang off.

## Limits and risks

- Version 1 is synchronous: submit and transfer return when the host has finished. Simple and
  correct; M4 may want asynchronous submission for throughput (the API already has fences).
- Commands are copied into kernel memory per submit (up to 256 KB). Fine for M4's start.
- No per-process limits beyond fixed tables (16 contexts, 256 buffers per connection).
- Still open from M2: the stability run (deferred), invisible panics, the one unexplained panic.

## Recommended next step

M4: the GL bundle. Build Mesa's virgl Gallium driver for 10.6 (i386 + x86_64) with a winsys on
this API, implement the `gld*` management calls, and hand each CGL context's dispatch table to
Mesa (decision 0003). First target: `cgl-info` reports our renderer and clears an FBO through it.
