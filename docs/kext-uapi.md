# The GPU user-client API (kext uapi), version 2

The interface between user space (the GL bundle's Mesa winsys, test tools) and a moose-ripple
GPU kext. Header: [`include/ripple_gpu_uapi.h`](../include/ripple_gpu_uapi.h).

**GPU-neutral** (decision 0005). The kernel side offers generic objects: device info,
capability blobs, contexts, mappable buffers, command submission, transfers and fences. Three
things are **backend-defined**, named by `ripple_gpu_info.backend`: the command stream, the
capability blobs, and a buffer's creation parameters. A kext for another GPU family (say AMD
with radeonsi) implements the same methods with its own backend id and payloads, and only the
Mesa winsys for that backend has to know them. The first and so far only backend is virgl
(QEMU virtio-gpu with virglrenderer).

## Opening

Find the `RippleAccelerator` service (`IOServiceMatching("RippleAccelerator")`) and
`IOServiceOpen(service, mach_task_self(), RIPPLE_GPU_CLIENT_TYPE, &connection)`. The type,
`'RPG1'`, stays clear of the small numbers Apple's graphics stack uses for IOAccelerator
clients (0 = surface, and vendor conventions for GL and 2D contexts), which M5 needs. Contexts
and buffers belong to the connection and are destroyed when it closes or its task dies.

## Methods (`IOConnectCallMethod` selectors)

| # | Method | Input | Output |
|---|---|---|---|
| 0 | `GET_INFO` | | struct `ripple_gpu_info`: API version, backend, capability blob ids and versions, name |
| 1 | `GET_CAPS` | scalars: blob id, version | struct: the blob (up to 4 KB) |
| 2 | `CONTEXT_CREATE` | scalar: flags (0) | scalar: context handle |
| 3 | `CONTEXT_DESTROY` | scalar: context handle | |
| 4 | `BUFFER_CREATE` | struct `ripple_gpu_buffer_create`: size, flags, backend params | scalars: buffer handle, backing size |
| 5 | `BUFFER_DESTROY` | scalar: buffer handle | |
| 6 | `SUBMIT` | struct `ripple_gpu_submit`, then `num_handles` uint32 buffer handles, then `cmd_size` bytes of commands | scalar: fence |
| 7 | `TRANSFER` | struct `ripple_gpu_transfer`: buffer, direction, level, box, layout in the backing | scalar: fence |
| 8 | `WAIT` | scalars: fence, timeout (ms) | scalar: 1 signalled, 0 timed out |
| 9 | `DISPLAY` | scalars: buffer handle, x, y, width, height | (none): shows that region of the buffer on the display instead of the console framebuffer, or presents it again; handle 0 gives the display back |
| 10 | `SURFACE_INFO` | scalar: accelerator surface id | struct `ripple_gpu_surface_info`: size, row bytes, screen position, screen size, flags (has CPU backing, is a region of the screen, a program presents on it), and the backing's address for the surface's own task |
| 11 | `SURFACE_PRESENT` | scalars: accelerator surface id, buffer handle, context handle, flags | scalar: 1 the buffer is on the screen now, 0 the surface is not a plain region of the screen at the moment |
| 12 | `SURFACE_TEXTURE` | scalars: accelerator surface id, buffer handle (one of the caller's; 0 = none), flags | scalar: 1 the buffer holds a frame of the surface now, 0 none has been presented yet |

**Display.** `DISPLAY` (added for M5) puts a GPU buffer on the screen instead of the console
framebuffer, and each further call with the same buffer presents its current contents. The
display goes back to the console framebuffer with handle 0, when the buffer is destroyed, when
the connection closes, or on a mode change.

`IOConnectMapMemory64(connection, buffer handle, ...)` maps a buffer's backing memory into
the caller; `TRANSFER` moves data between that backing and the GPU-side object.
Type `RIPPLE_GPU_MEMORY_FRAMEBUFFER` (0x80000000; buffer handles stay below it) maps the console
framebuffer's pixels instead, for 2D blitters that work on the frame directly (`RippleGA.plugin`).

**Version.** `GET_INFO` reports the API version; the GL bundle and the kext have to agree on
it, or the bundle does not offer its renderer. Version 2 (2026-11-12) added `SURFACE_PRESENT`
and `SURFACE_TEXTURE`, which the bundle now relies on for every window.

**Presenting on a surface.** `SURFACE_PRESENT` (added 2026-11-12) puts a finished frame on the
screen where a window's surface is, with no part for WindowServer: the kernel copies the
buffer, on the GPU, into the buffer on the display, inside each visible rectangle of the
surface as WindowServer last shaped it, and flushes the display. The surface is the one
WindowServer creates when a GL driver binds a window's surface as accelerated
(docs/re/ioaccelerator-interface.md, traces 3 and 4), named by the same ID. The copy runs in
the given context, after what that context has submitted. Flag `RIPPLE_GPU_PRESENT_TOP_FIRST`
(1): the buffer's first row is the picture's top row; without it, its bottom row, as OpenGL
draws. A result of 0 is not an error: the surface is hidden, or WindowServer is drawing the
window itself just now (while it moves, for example), and the caller shows the frame another
way. `TRANSFER` now attaches a buffer to the context itself when no `SUBMIT` has named it
yet.

**A surface as a compositor's texture.** `SURFACE_TEXTURE` (added 2026-11-12) is for the
process that draws windows itself when the kernel does not show them (WindowServer: a moving
window, Exposé). It names one of its own buffers as the texture of a surface that a program
presents on, and from then on the kernel copies every presented frame into that buffer, on
the GPU, top row first, before `SURFACE_PRESENT` returns; the latest frame is copied in at
once. The compositor just draws with the buffer whenever the program tells it there is a new
frame. One buffer per surface: naming another replaces it, 0 or destroying the buffer ends it.
Flag `RIPPLE_GPU_TEXTURE_ONCE` (1): only copy the latest frame in now. `SURFACE_INFO` reports
such a surface with `RIPPLE_GPU_SURFACE_FRAMES`. The standard surface client's Flush, which
WindowServer sends when it wants a surface shown where it has just shaped it, is answered the
same way as a present: the latest frame is copied to the screen if the surface is a plain
region of it.

**Surfaces.** `SURFACE_INFO` (added for M5) describes one of the standard IOAccelerator surfaces
(the type 0 client WindowServer and GL apps open): what a GL driver needs to render for it.
A surface shaped without backing is a region of the screen (`RIPPLE_GPU_SURFACE_SCREEN`); one
with CPU backing (`RIPPLE_GPU_SURFACE_BACKING`) gives its address only to the task that owns it.

**Fences.** `SUBMIT` and `TRANSFER` return a fence; `WAIT` blocks until it has signalled.
Since 2026-11-11 (performance stage) `SUBMIT` returns once the device has taken the commands,
not when the GPU has run them; `TRANSFER` returns when the data has been copied. Fences are
numbered in submission order. `WAIT` with a timeout of 0 only reports; with a timeout it makes
the device finish everything submitted so far, then reports. `DISPLAY` shows a finished frame
(it waits the same way first). Before, every `SUBMIT` waited for the GPU, which cost about
half of the frame rate.

**Limits** (version 1, raised 2026-11-10 for Mesa): commands up to 320 KB per `SUBMIT` (Mesa's
virgl driver flushes at 260 KB), up to 4096 buffer handles per `SUBMIT`, up to 65536 buffers per
connection, buffers up to 256 MB each, capability blobs up to 4 KB. Each connection keeps one
320 KB physically contiguous command buffer in the kernel from its first `SUBMIT` on.
`tools/gpu-stress` checks these limits.

## Backend: virgl (`RIPPLE_GPU_BACKEND_VIRGL` = 1)

These are the only places virgl shows through:
- **Commands:** the virgl protocol ([`third_party/virgl/virgl_protocol.h`](../third_party/virgl/virgl_protocol.h)).
  Buffer handles in `SUBMIT` are attached to the context before the commands run; inside the
  commands, a buffer is named by its handle (the virgl resource handle).
- **Capability blobs:** virgl capsets, id 1 (`virgl_caps_v1`) and id 2 (`virgl_caps_v2`)
  ([`third_party/virgl/virgl_hw.h`](../third_party/virgl/virgl_hw.h)).
- **Buffer params:** `struct ripple_virgl_buffer_params` (target, format, bind, size, levels,
  samples, flags), i.e. a virgl resource.

The kernel side maps the methods to virtio-gpu commands (`CTX_CREATE`, `RESOURCE_CREATE_3D`,
`RESOURCE_ATTACH_BACKING`, `CTX_ATTACH_RESOURCE`, `SUBMIT_3D`, `TRANSFER_TO/FROM_HOST_3D`;
an empty fenced `SUBMIT_3D` waits for the GPU). None of that is visible in the API.
