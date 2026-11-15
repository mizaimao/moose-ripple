# The mini-VDD's interface for user-mode drivers

`ripplevd.vxd` (the mini-VDD, `win9x/minivdd/`) owns the virtio-gpu device. Win32 code reaches it
with `CreateFile("\\\\.\\RIPPLEVD", ...)` and `DeviceIoControl`. This is the Windows 9x side's
counterpart of `docs/kext-uapi.md`; it is still small and will change while step 3 of
[plan.md](plan.md) goes on.

The VxD does the transport (virtio 1.0 over PCI, the queues, waiting for answers). What is sent
is decided in user mode: `RVD_COMMAND` carries a complete virtio-gpu control request, header
first, and returns the device's answer.

| Code | In | Out |
|---|---|---|
| `RVD_INFO` (1) | nothing | `struct rvd_info`: interface version, whether the host renders 3D (virgl), number of capability sets, the host display's size, the transfer area's physical address and size, the largest request |
| `RVD_COMMAND` (2) | a virtio-gpu request, up to 64 KB (e.g. `SUBMIT_3D` with its commands) | the answer, up to 4 KB |
| `RVD_AREA_READ` (3) | a 32-bit offset | bytes from the transfer area |
| `RVD_AREA_WRITE` (4) | a 32-bit offset, then the bytes | nothing |

The **transfer area** is 2 MB of physically contiguous memory the VxD allocates when Windows
starts. A user-mode driver names it in `RESOURCE_ATTACH_BACKING` as a resource's guest-side
storage, so that `TRANSFER_TO_HOST` and `TRANSFER_FROM_HOST` have somewhere to go; the two
area calls copy between it and the program.

Limits of this first version, to be lifted as the OpenGL driver needs: one transfer area for
everybody, no fences (every request is waited for), no interrupts (answers are polled), no
per-process clean-up of contexts and resources, and the screen stays the VGA frame buffer
(nothing is shown from a host resource directly).

`tools/win9x/rvdtest.c` exercises it: `rvdtest` prints what the VxD and the host report, and
`rvdtest clear` has the host GPU clear a texture and reads it back.
