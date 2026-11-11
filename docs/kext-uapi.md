# The GPU user-client API (kext uapi), version 1

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

**Display.** `DISPLAY` (added for M5) puts a GPU buffer on the screen instead of the console
framebuffer, and each further call with the same buffer presents its current contents. The
display goes back to the console framebuffer with handle 0, when the buffer is destroyed, when
the connection closes, or on a mode change.

`IOConnectMapMemory64(connection, buffer handle, ...)` maps a buffer's backing memory into
the caller; `TRANSFER` moves data between that backing and the GPU-side object.

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
