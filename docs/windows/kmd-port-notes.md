# The kernel driver: from the Windows 10 driver to Windows 7

2026-11-14, first reading. Working notes for milestone W1 (`feasibility.md`): what the
kernel part of virtio-win's unmerged 3D driver does, and what in it Windows 7 does not have.
Source read: pull request 943's branch at its last rebase (2025-09-05), folders
`viogpu/viogpu3d` and `viogpu/common`, about 10 800 lines, BSD-3-Clause. Nothing is copied
into this repository yet.

Points marked **check** are from memory of the driver model and have to be confirmed against
the kit's `d3dkmddi.h` once the toolchain is up.

## What the driver is

- One C++ kernel driver. `viogpu/common` holds the virtio transport (PCI capabilities,
  queues, the control and cursor commands); `viogpu3d` holds the WDDM side.
- **Display:** `viogpu_vidpn.cpp` (2100 lines): modes from the device's EDID or a built-in
  list, the present network callbacks, a software VSync timer that reports
  `DXGK_INTERRUPT_CRTC_VSYNC`, flips by pointing the scanout at another resource.
- **Memory:** one aperture segment, 4 GB, not visible to the CPU. Every allocation is a
  virtio-gpu resource; mapping it into the aperture attaches guest pages as its backing
  (`BuildPagingBuffer`, map and unmap only).
- **Drawing:** the user-mode part writes virgl commands into the command buffer Windows
  gives it; `Render` copies them into the DMA buffer with the list of allocations; `Patch`
  and `SubmitCommand` hand that to a worker thread, which sends it on the control queue and
  reports `DXGK_INTERRUPT_DMA_COMPLETED` when the host has answered.
- **Escapes:** a private channel from user mode for what does not fit a command buffer
  (capability sets, resource creation and info, waiting on a resource, context setup).
- **Presenting:** `Present` builds a small command for a copy between two resources, or
  takes the flip path.
- Its INF turns the scheduler's preemption off for the whole system
  (`Scheduler\EnablePreemption = 0`) and sets `TdrDebugMode`. Both are machine-wide settings.

## What Windows 7 does not have

The driver registers as WDDM 1.3 (Windows 8.1). Windows 7 is 1.1.

| In the Windows 10 driver | Model version | On Windows 7 |
|---|---|---|
| `DxgkCbAcquirePostDisplayOwnership`, `DxgkDdiStopDeviceAndReleasePostDisplayOwnership`: taking the firmware's or boot loader's picture over, and handing a picture back | 1.2 | Does not exist. The driver starts from VGA: it sets its own mode at the first commit, and Windows expects the device to be usable as VGA again after `DxgkDdiStopDevice` and for crash screens. `virtio-vga` has the VGA half; the driver has to return the device to it (a device reset does that in QEMU: **check**) |
| `DxgkDdiSystemDisplayEnable`, `DxgkDdiSystemDisplayWrite`: drawing the crash screen through the driver | 1.2 | Does not exist. Windows 7 draws crash screens through VGA after `DxgkDdiResetDevice`, so that entry point must really put the device back into VGA mode |
| `DxgkDdiQueryEngineStatus`, `DxgkDdiResetEngine` (stubs) | 1.2 | Drop. Timeouts go through `DxgkDdiResetFromTimeout` and `DxgkDdiRestartFromTimeout`, which the driver also has |
| `DxgkDdiCancelCommand`, `DxgkDdiGetNodeMetadata` | 1.3 | Drop |
| `DXGKQAITYPE_QUERYSEGMENT3` with `DXGK_SEGMENTDESCRIPTOR3` | 1.2 | Answer `DXGKQAITYPE_QUERYSEGMENT` with the first descriptor layout instead |
| `DXGK_DRIVERCAPS`: `WDDMVersion`, `SupportNonVGA`, `SupportSmoothRotation`, `SupportDirectFlip` | 1.2 and 1.3 | The structure is shorter; fill only the Windows 7 fields. `SectionBackedPrimary` should exist from 1.1: **check** |
| Registered interface version `DXGKDDI_INTERFACE_VERSION_WDDM1_3` | 1.3 | `DXGKDDI_INTERFACE_VERSION_WIN7`, and the driver must be compiled against that version so the structures have the Windows 7 sizes |
| INF for `NTamd64.10.0`, user-mode driver names listed four times | | Windows 7 sections; the first name is the Direct3D 9 part (the stub in W1), the second the Direct3D 10 part |

Everything else the driver registers exists in WDDM 1.0 already (device and context
creation, allocations, paging buffer, render, patch, submit, preempt, present, the present
network callbacks, pointer, power, escape, interrupt and DPC).

## Open points for W1

1. **Who draws the desktop without 3D.** With the compositor off, Windows 7 draws GDI into
   a shadow and copies to the primary surface, either by locking it or through `Present`
   from a system-memory surface. The driver must make the primary reachable for that: a
   section-backed primary or a CPU-visible segment, then `TRANSFER_TO_HOST_2D` and
   `RESOURCE_FLUSH` for what changed. The Windows 10 driver leans on the compositor always
   being on. This is the part of W1 with no code to start from.
2. **The Direct3D 9 stub.** Windows 7 will not start a WDDM driver's device without a
   user-mode part. The stub opens the adapter, reports no 3D capabilities and implements
   only what the desktop needs (create, lock and unlock of the primary and of system-memory
   surfaces, present). Our own code; about the size of one source file.
3. **Preemption and timeouts.** Whether to keep the machine-wide switch or answer
   `DxgkDdiPreemptCommand` properly. For W1 (hardly any commands) either does.
4. **Pointer.** Disabled in the Windows 10 driver ("for now"). The device has a cursor
   queue; our Mac driver uses it. Software pointer first.
5. **32-bit.** The driver assumes a 4 GB aperture at `0xC0000000`; a 32-bit Windows 7 needs
   that looked at. Not in W1.
6. **Debugging.** A kernel debugger over a virtual serial port on the test VM, and
   snapshots before every install.

## How it will sit in this repository

Proposed, to be settled with the toolchain work before anything lands:

    windows/kmd/        the kernel driver (from virtio-win, BSD-3-Clause, notices kept)
    windows/umd9/       the Direct3D 9 part (the stub first)
    windows/mesa/       patches and build files for Mesa's Windows parts (as gldriver/mesa)
    windows/package/    INF, test-signing and install scripts
    third_party/        upstream notices, as on the Mac side

## The port so far (2026-11-14)

What was changed to get from the Windows 10 driver to one that builds for Windows 7 and can
be tried, in the order of the commits under `windows/kmd`:

- **Builds on Linux** against the Windows 7 kit (`windows/kmd/build.sh`): clang in cl mode,
  the newer annotations defined away (`compat/sal2.h`), two newer kernel names mapped
  (`compat/win7.h`), the port and control-register intrinsics as real functions
  (`compat/intrinsics.c`).
- **The PCI glue written anew** (`common/ripple_pci.cpp`): the one file of the import with a
  GPL header was left out.
- **The model 1.2 and 1.3 parts removed** as listed above; registers as
  `DXGKDDI_INTERFACE_VERSION_WIN7`.
- **Messages on the first serial port** in the default build (`DEBUG`, `LEVEL`), which the
  test VM writes to a file on its host. No kernel debugger needed so far.
- **Drawing without the compositor reaches the screen.** The driver showed a surface only at
  a flip. Windows 7 without its compositor never flips: it draws into a surface of its own
  and copies the changed pieces to the surface on screen (`DxgkDdiPresent` with `Blt`). Such
  a copy is now followed by a note of what changed (`VIOGPU_CMD_SCREEN_CHANGED`), and the
  flip thread has the host show that part (`RESOURCE_FLUSH`). This answers open point 1: the
  surface Windows draws into is an ordinary allocation whose pages the host reads
  (`TRANSFER_TO_HOST_3D`) before the copy; the Windows 10 driver had that already.
- **`DxgkDdiResetDevice` resets the virtio device**, which makes QEMU show the VGA side of
  the device again, for crash screens.
- **Both driver threads stop with the device**, also when Windows removes a device without
  stopping it (it does so when the start fails).
- **A completion event for user mode** (`VIOGPU_CMD_SIGNAL_EVENT`): the user-mode driver names
  an event in its command buffer, the kernel driver sets it when the buffer has run on the
  host. Windows 8 has a system way for that, Windows 7 has not.
- **Sizes checked in `Render`**: the command buffer and the patch list from user mode were
  copied without looking at the room; the DMA buffer is 320 KB now, enough for a full
  user-mode buffer.

Found while reading, not changed yet: the command object is created in `Present` and
`Render` and found again at `SubmitCommand` through a pointer in the DMA buffer's private
data. That only holds while every buffer is submitted exactly once, which is why the INF
turns preemption off. Open point 3 stands.

## Load tests

Test VM, snapshot before each run, driver test-signed, installed over the Standard VGA
adapter with `devcon update` (the INF needs `FeatureScore`, or Windows 7's display class
installer refuses it as not for this Windows).

1. **First build:** the driver loads, sets up the virtio queues, reads the host's capability
   sets (virgl and virgl2 are there), the EDID and the mode list; `DxgkDdiStartDevice`
   returns success; Windows removes the device at once. Bug check 0xCE a little later: the
   flip thread was still running in the unloaded driver (fixed, see above).
2. **Every call logged:** after `DxgkDdiStartDevice` Windows calls
   `DxgkDdiQueryChildRelations`, then `DxgkDdiStopDevice`, `DxgkDdiRemoveDevice` and unloads
   the driver. No `DxgkDdiQueryAdapterInfo`, no child status, no power call. So Windows 7
   gives up between reading the description of the monitor connector and its next question.
   First suspect: the Windows 10 driver describes the VGA device's output as a built-in
   panel that is always connected (`HpdAwarenessAlwaysConnected`, `D3DKMDT_VOT_INTERNAL`);
   the next build describes an ordinary connector that reports plugging
   (`HpdAwarenessInterruptible`, `D3DKMDT_VOT_HD15`).
3. **An ordinary connector, and the settings where Windows 7 keeps them** (a
   `.SoftwareSettings` section in the INF for the user-mode driver names): no change, the
   same three calls and then the stop. The connector description stays as changed (a
   built-in panel is wrong for a VM anyway).
4. **`LoadOrderGroup = Video`** for the service, on the idea that Windows 7 starts the device
   after its old-model fallback driver (VgaSave) otherwise and then refuses a WDDM driver: no
   change. VgaSave is running after each failed start, with or without the group, which
   says nothing about the order: it is the fallback.

What the four runs have in common: Device Manager shows Code 43, which is not a failed start.
Windows started the device and then switched it off on purpose, and it never asked the driver
for its capabilities. Nothing in the System event log.
5. **The PCI command register printed:** ports and memory decoding are both on (0x0107), so
   Windows has no reason to doubt that this is the VGA device. Under the Standard VGA driver
   the adapter owns the legacy VGA ranges; our driver's resource list has only the three
   memory ranges of the device, which is normal for a WDDM driver.

### The cause: two requirements of Windows 7 the Windows 10 driver never met

Found by disassembling Windows 7's `dxgkrnl.sys` from the test VM with Microsoft's public
symbols (kept outside the repository). The start goes `DpiFdoStartAdapter`:
`DpiDxgkDdiStartDevice`, `DpiFdoEnumChildDevices` (which calls `DxgkDdiQueryChildRelations`),
then `DxgkAddAdapter`, whose `DXGADAPTER::Initialize` does this before it asks the driver
anything:

- It reads `UserModeDriverName` from the adapter's registry key as a **required** value
  (and refuses an empty one). On Windows 7 that key is the one an INF's `.SoftwareSettings`
  section fills.
- It checks that 28 driver entry points are there and returns `STATUS_REVISION_MISMATCH`
  if one is missing. All of them were registered except **`DxgkDdiSetPalette`**. (For the
  optional ones, overlays, swizzling, `StopCapture`, `RecommendVidPnTopology` and others, it
  puts in defaults of its own.)
- It rejects interface version 0x2000 outright and takes 0x2001 and later as Windows 7
  drivers with the larger capability structure.
- After `DXGKQAITYPE_DRIVERCAPS` it requires **`WDDMVersion == DXGKDDI_WDDMv1`** (0x1000)
  from a driver of that version; zero is refused. With that value it also wants
  `CreateAllocation`, `Patch` and `Render`.
- More capability rules there: `SupportKernelModeCommandBuffer` needs `DxgkDdiRenderKm`;
  swizzling ranges need the swizzling entry points; with `MultiEngineAware` the number of
  processing nodes must not be zero.

A failure in there makes Windows stop the device again and show Code 43, with nothing in any
log. Both gaps are closed (`DxgkDdiSetPalette` does nothing: there is no 256-colour mode).
