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
| `RVD_PRESENT` | `struct rvd_present` | | show a scanout-capable GPU buffer owned by the caller's context and matching the current screen dimensions |
| `RVD_PRESENT_WINDOW` | `struct rvd_window`, then 1 to `RVD_WINDOW_RECTS` `struct rvd_rect` | | copy an OpenGL window's image on the host into the desktop image, inside the given screen rectangles (GDI's visible region); `ERROR_NOT_READY` when the display shows something else (a full-screen image, DOS, a game flipping VGA pages): the frame then goes through GDI |
| `RVD_DESKTOP` | 32-bit byte offset | | select and refresh the CPU-drawn page immediately. GDI uses offset zero |
| `RVD_DESKTOP` | `struct rvd_desktop` | | select the CPU page at `offset`. With `flags = RVD_DESKTOP_FLIP`, successive DirectDraw flips may share a host display update. Zero flags request an immediate refresh. With `RVD_DESKTOP_HEAP` the page lies in the `RVD_HEAP` block, `offset` bytes from its start (a flip chain's back buffer that the card's 8 MB had no room for): the card can't show it, so virtio takes the display for it if it had not yet |
| `RVD_DESKTOP` | empty | 32-bit flag, then (if there is room) the 32-bit set of `rvd_desktop` flags the VxD knows | query whether virtio owns the display. A newly loaded HAL uses this to resume CPU page flips, and the second word to learn whether pages in the heap can be shown |
| `RVD_HEAP` | bytes wanted | `struct rvd_heap` | one block of system memory for the display driver's DirectDraw surfaces that are never shown (made at the first call, returned again later; valid in every process). No GPU needed |
| `RVD_SET_TIMEOUT` | milliseconds (0: the default, 10 s; `RVD_TIMEOUT_AT_ONCE`: give up as soon as a request is sent, and keep the device stalled until set back) | contexts and buffers held for the host (optional) | how long a request may take before it fails with `ERROR_TIMEOUT`; for tests of the recovery, until the program that set it ends |
| `RVD_SET_TIMEOUT` | `struct rvd_upload_test` | 32-bit status | test an asynchronous CPU display upload. Set `ms` to zero and supply an `RVD_UPLOAD_*` action |
| `RVD_GET_STATS` | | `struct rvd_stats` | what the VxD holds now: contexts and buffers in use, the locked memory behind the buffers, contexts and buffers held for a host that stopped answering, the DirectDraw heap. No GPU needed; for tests that look for memory a long session loses (`tools/win9x/memlog.c`) |

The GPU contexts of a process end when it closes its last handle to the VxD (or ends): a
process can hold several, such as the DirectDraw DLL's for `RVD_HEAP` and Mesa's for OpenGL,
and closing one must not take the others' contexts away.

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
- **Command completion.** Ordinary requests use one pair of queue descriptors and a 512 KB
  request buffer. The VxD polls for that pair's reply under a mutex. A GPU flip also queues
  a fenced submit, which asks the host to finish the frame's rendering. Its reply has a
  separate descriptor pair and buffer, so the display command can follow immediately. The
  driver waits for a fence if all the available pairs are busy. Timeout recovery matches
  replies by descriptor ID.
- **Display updates.** The first GPU image is shown immediately. When successive flips
  update that same resource, the VxD tells the host display about the newest contents at
  most once per 17 ms. A timer delivers the last update even if drawing stops. This matters
  on a VNC host where each display notification can copy the whole frame to the console.
  After the first GPU presentation, QEMU also uses virtio for CPU-drawn pages. Before
  takeover, the HAL selects those pages through VGA's display-start register.
- **CPU page flips.** The HAL makes the target surface readable, passes its byte offset to
  the VxD with `RVD_DESKTOP_FLIP` and sets the VGA display start on success. The first flip
  from a new process or after a GPU image is shown refreshes the mirror immediately. Each
  subsequent flip selects its page immediately, including page zero. At the next 17 ms
  refresh interval, a flip or timer samples the latest selected page. The timer continues
  sampling the held page after flips stop, so direct writes and software cursor movement
  reach the screen. The four-byte offset request keeps its immediate behaviour. An older VxD
  that supports this request reads the offset at the start of the extended request too.
- **CPU mirror upload.** When a sample changes the CPU image already on screen, the VxD
  can queue one fenced `TRANSFER_TO_HOST_2D` on a spare descriptor pair. The guest can
  keep flipping while the host reads the mirror. Its backing stays immutable until the
  matching descriptor and fence reply arrive. A flip or timer then flushes the image if
  the same process, mode and CPU display still own it. The next sample reads the latest
  selected page. The first image, a new owner and a queue without a spare pair use the
  synchronous transfer path.
- **Display ownership.** Returning to GDI selects page zero immediately. Publishing a GPU
  image or changing modes ends the sequence of CPU flips. A timer already queued uses
  the current mode and display owner. When the tracked presenter closes its last device
  handle, the VxD restores page zero if that process still owns the display. For an
  80-column colour DOS text screen, the VxD reads the VGA
  character cells, loaded font and hardware cursor and renders them into a display image.
  A mode switch leaves the running GPU transport and application contexts intact.
- **Clean-up.** A context belongs to the process that made it, and a buffer to its context.
  When a program closes the device, exits or crashes, Windows sends the VxD a close message
  and the VxD removes that process's contexts and buffers on the host and frees their memory.
  (The device handle value Windows passes is the same in every process; the process tag is
  what tells clients apart.)
- **Errors.** A request the host refuses fails with `ERROR_GEN_FAILURE`, and the VxD writes a
  line on QEMU's debug port. A rendering error inside a command stream is not visible here:
  the host logs it (on Proxmox VE: `journalctl`, lines from `QEMU[pid]`) and from then on
  ignores that context's commands.
- **An ordinary request that takes too long.** If the host hasn't answered the main queue
  descriptor pair after 10 seconds, the request
  fails with `ERROR_TIMEOUT` and the device counts as stalled: its answer may still come, and
  until it does, the request buffer is the host's and nothing else is sent. Requests are
  refused at once with `ERROR_TIMEOUT` meanwhile; contexts and buffers can still be let go,
  and stay reserved (their IDs, and their memory, which the late request may still write)
  until the host answers. The next request after the answer finds the device back: the VxD
  has the host forget what was let go meanwhile, and work goes on. The stall covers the whole
  device: while it lasts, every program's requests fail, so a program may lose commands and
  draw wrongly until it makes new contexts; work after the answer is not affected. (A host
  that never answers can't be helped from inside the guest.) Before 2026-11-17 the first
  time-out ended 3D until Windows restarted. `RVD_SET_TIMEOUT` changes the limit for a test
  (`rvdtest stall`), until the program that set it ends.
- **A CPU mirror upload that takes too long.** Only that upload expires. Its descriptor
  and backing stay reserved until the reply arrives. Other GPU commands can continue on
  the main pair. When one desktop image is still being read and a second is on screen,
  a mode change can allocate a third. After the new image is visible, the VxD releases
  the previous one. It releases the pinned image when its reply arrives. If allocation
  fails, the current image stays on screen and the timer retries.
  A late reply releases the old upload without flushing it. If the reply fails fence
  validation, the VxD switches to synchronous uploads and keeps the suspect backing until
  the host confirms a fenced resource release.
- **Reaching the card.** Its virtio registers sit in a 64-bit memory range. Where the firmware
  puts that range below 4 GB (QEMU's usual layout, Proxmox), the VxD maps it. UTM gives the
  card an 8 GB host memory range, so the firmware puts all its 64-bit ranges above 4 GB, out
  of reach of 32-bit Windows; the VxD then reads and writes the registers through the card's
  PCI configuration window (virtio's configuration access capability), one register at a time.
  Before 2026-11-16 it mapped the range's lower half there, address 0, and wrote into the
  real-mode interrupt table: Windows hung at start-up on UTM.

Limits: 63 contexts, 4096 buffers, 32 MB behind one buffer, 511 KB of commands per submission.

## Test

`tools/win9x/rvdtest.c` (built by `tools/win9x/build-tools.sh`):

    rvdtest          what the VxD and the host report
    rvdtest clear    context, 64x64 render target, clear to orange on the host, read back, paint
    rvdtest leak     the same, leaving the context and buffer for the VxD's clean-up
    rvdtest stall    exercise the ordinary queue timeout and its recovery
    rvdtest async    exercise a held CPU upload during a mode change and an isolated timeout

Run `rvdtest async` after a full-screen program has presented a GPU frame in the same
boot. This sequence runs the check from a Windows command prompt.

    d3dtest 640 480 16 1
    rvdtest async

The test switches between 1024×768 and 640×480 at 16 bits with a real upload held.
It checks that an unrelated GPU context remains usable during the forced timeout, a fresh
upload gets its own reply, and a 3D clear reads back correctly. It checks resource counts
and restores the saved desktop mode. Exit code 0 means pass, 1 means failure, and 2
means the driver lacks the test interface or GPU presentation has not occurred.

The eight-byte test request returns the `RVD_UPLOAD_SUPPORTED` bit. Keep a device handle
open and use `RVD_UPLOAD_ARM` to hold the next upload from the calling process.
`RVD_UPLOAD_QUERY` reports `ARMED`, `HELD`, `TIMED_OUT` and `REPLIED` status bits
(each prefixed with `RVD_UPLOAD_`). The driver continues consuming real device replies
while it holds completion processing for the test, so `HELD` and `REPLIED` can both be
set. `RVD_UPLOAD_TIMEOUT` backdates the held upload to exercise the regular deadline check.
`RVD_UPLOAD_RELEASE`, closing the process's last device handle, or the 60-second limit
ends the hold.

`tools/win9x/memlog.c` prints, every few seconds, Windows' memory counters next to what the
VxD holds (`RVD_GET_STATS`); see [mesa.md](mesa.md#memory-over-a-long-session).

On a host without virgl (`-device virtio-vga` without `-gl`) the VxD still loads and
`RVD_GET_INFO` answers with `virgl` 0; buffers can't be made.

## Not there yet

- Completion interrupts. The VxD polls for ordinary requests.
- The cursor queue is set up and unused.
- OpenGL frames go through GDI while the pointer is over the window, and every 200 ms
  (windows use `RVD_PRESENT` or `RVD_PRESENT_WINDOW` otherwise since 2026-11-26).
