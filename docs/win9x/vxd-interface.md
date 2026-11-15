# The mini-VDD's interface for user-mode drivers

`ripplevd.vxd` (`win9x/minivdd/`) owns QEMU's virtio-gpu device inside Windows 9x. User-mode
drivers (Mesa's virgl driver through its winsys, [mesa.md](mesa.md)) reach the host's GPU
through it with `DeviceIoControl`. The definitions are in `win9x/minivdd/rvd.h`, shared by the
VxD, the test program and the winsys; this is version 2 of the interface.

    HANDLE dev = CreateFile("\\\\.\\RIPPLEVD", 0, 0, NULL, 0, 0, NULL);
    DeviceIoControl(dev, RVD_GET_INFO, NULL, 0, &info, sizeof info, &n, NULL);

| Code | In | Out | What |
|---|---|---|---|
| `RVD_GET_INFO` | | `struct rvd_info` | interface version, whether the host renders 3D (virgl), its capability sets, display size, limits |
| `RVD_GET_CAPS` | set ID, version | the blob | one capability set (what the host's renderer can do) |
| `RVD_CONTEXT_CREATE` | a name | context ID | a rendering context on the host; the name shows in the host's logs |
| `RVD_CONTEXT_DESTROY` | context ID | | the context and every buffer made for it |
| `RVD_BUFFER_CREATE` | `struct rvd_buffer_create` | `struct rvd_buffer` | a host GPU object (a virgl resource: texture, vertex buffer, ...) with guest memory behind it |
| `RVD_BUFFER_DESTROY` | handle | | |
| `RVD_SUBMIT` | `struct rvd_submit`, commands | | a virgl command stream for a context |
| `RVD_TRANSFER` | `struct rvd_transfer` | | copy between a buffer's memory and its GPU object, either way |

The interface has the shape of the Mac OS X kernel driver's (`docs/kext-uapi.md`): contexts,
buffers, submit, transfer. What goes through it (resource parameters, the command stream) is
virgl's and is decided in user mode.

## How it works

- **Buffers and their memory.** For a buffer the VxD allocates fixed system pages, tells the
  host their physical addresses (`RESOURCE_ATTACH_BACKING`, one entry per run of adjacent
  pages) and returns the linear address. Windows 9x maps system pages into every process, so
  the driver reads and writes the memory in place: a texture upload is "fill the memory, then
  `RVD_TRANSFER` to the host", a read-back the other way round. A buffer's handle is its virgl
  resource ID, which is what the command stream names.
- **Requests are synchronous.** The VxD copies a request into its own physically contiguous
  buffer (512 KB, so a command submission can be that large), puts it on the control queue and
  polls until the device has answered. Nothing is outstanding when `DeviceIoControl` returns,
  so there are no fences yet: a later read-back sees everything submitted before it. Ring 0
  code in Windows 9x is not pre-empted by other threads, so requests never interleave.
- **Clean-up.** A context belongs to the process that made it, and a buffer to its context.
  When a program closes the device, exits or crashes, Windows sends the VxD a close message
  and the VxD removes that process's contexts and buffers on the host and frees their memory.
  (The device handle value Windows passes is the same in every process; the process tag is
  what tells clients apart.)
- **Errors.** A request the host refuses fails with `ERROR_GEN_FAILURE`, and the VxD writes a
  line on QEMU's debug port. A rendering error inside a command stream is not visible here:
  the host logs it (on Proxmox VE: `journalctl`, lines from `QEMU[pid]`) and from then on
  ignores that context's commands. If the device doesn't answer for 10 seconds the VxD stops
  using it.

Limits: 63 contexts, 4096 buffers, 32 MB behind one buffer, 511 KB of commands per submission.

## Test

`tools/win9x/rvdtest.c` (built by `tools/win9x/build-tools.sh`):

    rvdtest          what the VxD and the host report
    rvdtest clear    context, 64x64 render target, clear to orange on the host, read back, paint
    rvdtest leak     the same, leaving the context and buffer for the VxD's clean-up

On a host without virgl (`-device virtio-vga` without `-gl`) the VxD still loads and
`RVD_GET_INFO` answers with `virgl` 0; buffers can't be made.

## Not there yet

- Interrupts and fences: the VxD polls, which stalls the guest for as long as the host takes
  to answer (microseconds for most requests).
- The cursor queue is set up and unused.
- A scanout of its own: the screen is still the VGA frame buffer, and finished frames are read
  back and drawn there by GDI.
