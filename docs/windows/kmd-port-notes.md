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
   queue; our Mac driver uses it. Software pointer first. (Done 2026-11-15: "The hardware
   pointer" below.)
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

6. **Both fixes, values still only in `.SoftwareSettings`:** no change, and the registry of
   the test VM showed why. On Windows 7 the `.SoftwareSettings` section of a display INF lands
   in a `Settings` subkey of the adapter's driver key
   (`Control\Class\{4d36e968-...}\000N\Settings`). The required `UserModeDriverName` is read
   from the driver key itself, where the other vendors' drivers on that machine have it and
   where a plain `AddReg` of the install section writes. Run 2 had it there (and failed on the
   missing entry point); runs 3 to 6 failed one step earlier, on the missing value, after it
   had been moved. The INF has the three display values in both places now.
7. **The adapter is accepted.** Windows 7 asks for capabilities and segments, maps two
   ranges of the aperture (without an allocation: its own buffers), reads the EDID and the
   seven modes, and goes through its mode proposals (`IsSupportedVidPn`,
   `EnumVidPnCofuncModality`). It creates a device, a context, the surface it draws the
   desktop into (standard allocation type 2) and the shared primary (type 1), each at
   1024x768 and again at 640x480, and each time tears everything down right after the
   primary's `OpenAllocation`, without a failing call on the driver's side and without ever
   committing a mode. The screen stays off ("Display output is not active"), no crash.

### Tools for looking into Windows

- Disassembly of the Windows 7 display kernel (`dxgkrnl.sys`, `dxgmms1.sys`, `watchdog.sys`
  copied out of the test VM) with names from Microsoft's public symbol files, which the
  symbol server hands out for the exact build. Never committed.
- QEMU's gdb stub on the test VM (`gdbserver` through the VM's QMP socket, bound to the
  host's loopback), driven by gdb on the build machine. The loaded modules are found from
  the interrupt table (the kernel's base) and the kernel's debugger data block (the module
  list).
- Windows' display kernel writes every internal error to a log in `watchdog.sys`, kept in
  memory only if `HKLM\SYSTEM\CurrentControlSet\Control\Watchdog\Logging` has values
  `N-LevelSizeLg2` (N = 1..7, the size of each level's ring as a power of two). Each entry
  holds the caller's address and up to five values, usually a status code. Read out through
  the gdb stub, this says which check fired.

8. **Windows' own error log switched on** (the `Watchdog\Logging` values), same driver as run
   7. Read out through the gdb stub, the log named the check: `VIDMM_GLOBAL::CreateOneAllocation`
   in `dxgmms1.sys` refuses the shared primary with `STATUS_INVALID_PARAMETER`. A primary may
   only use segments the CPU can reach, unless the driver's memory management capabilities
   have the bit for a section-backed primary (bit 3), in which case its segments must be
   apertures. Windows 7 reads that bit although the Windows 7 kit's header does not name it.
   The Windows 10 driver sets it; it had gone out with the things the kit lacks.
9. **With the section-backed primary:** Windows commits a mode, sets the source address and
   presents. **The first desktop on our driver**, 1024x768, with one fault: icons repeated and
   the bottom strip showed the top of the picture. Every partial update took its pixels from
   the surface's top left corner, because the transfer to the host named no offset into the
   surface's memory.
10. **With the offset:** the Windows 7 desktop is drawn correctly on the virtio-gpu device by
    our driver, without the compositor: wallpaper, icons, taskbar, Start menu, a command
    prompt. Windows reports "Driver is running", 1024x768 in 32-bit colour. `dwm.exe` runs but
    does not compose (window frames are the basic ones), as expected with no Direct3D yet.
    Screens were taken through the host's VNC server; QEMU's `screendump` writes nothing while
    the picture is a host GPU surface.

What the Windows 10 driver needed to show a Windows 7 desktop, in the end: `DxgkDdiSetPalette`,
`WDDMVersion`, the section-backed primary bit, `UserModeDriverName` in the driver key, the
offset in partial transfers, and telling the host to show what was copied to the screen
without a flip. Not yet looked at in this state: the Windows log's complaints about the EDID
(`STATUS_MONITOR_UNKNOWN_DESCRIPTOR_FORMAT`), mode changes, the pointer, stopping and
restarting the device, the VGA hand-back at a crash screen, and the two machine-wide
settings in the INF.

What Windows' log still complains about with the desktop up (run 10), as the next things to
look at: `DxgkDdiEscape` answers `STATUS_INVALID_BUFFER_SIZE` to escapes without data (39
times; who sends them is not known yet); `DxgkDdiSetPointerPosition` answers
`STATUS_NOT_IMPLEMENTED` although Windows calls it without pointer capabilities;
`DXGADAPTER::DisableAllPrimaries` reports an assertion four times during the mode set; the
EDID is judged invalid (`DpiIsValidEdid`, `STATUS_MONITOR_UNKNOWN_DESCRIPTOR_FORMAT`), so the
monitor is a generic one; `IsValidContentScaling` objects to the scaling our paths name.

### The W1 checks (run 10 and 11, 2026-11-14)

| Check | Result |
|---|---|
| Desktop, Start menu, a window | drawn correctly; at 800x600 the same picture as the Standard VGA driver gives |
| Modes | all seven of the device's list offered (640x480 to 1888x1062); a change to 1280x800 and back to 800x600 through `ChangeDisplaySettings` works |
| Driver restart (`devcon restart`) | stops, unloads, loads and starts again with the desktop back; no thread left behind |
| Crash screen | after an NMI Windows resets the device through `DxgkDdiResetDevice` and its "Hardware Malfunction" screen is shown by the VGA side of the device |
| Pointer | drawn by Windows itself (no hardware pointer yet); `DxgkDdiSetPointerPosition` now answers success |
| Sleep | **not survived.** Windows offers S3 and goes to sleep; on waking the screen stays off. Two causes: the driver's threads went on using the closed virtio queues (fixed), and QEMU resets every device when it wakes a guest, so all surfaces and contexts on the host are gone and would have to be made anew (not written). Until then the INF switches sleep and hibernation off for the machine |

The Windows log's steady `DxgkDdiEscape` errors come from escapes of 20 bytes or more that
are not ours: something on the test machine (it has two other vendors' display software
installed) asks every adapter. They are refused as they should be.

## Direct3D on the host GPU: the first test (2026-11-14)

`tools/win7-hello/d3d10test.c` in its `hw` mode asks Windows for a Direct3D 10 device on the
display adapter's own driver, clears a render target, draws a red triangle from a vertex buffer
with a vertex and a pixel shader, and reads the picture back. That is the whole chain: the
Direct3D runtime, Mesa's Direct3D 10 driver with the virgl driver under it, our bridge, our
kernel driver, QEMU, virglrenderer and the host's GPU.

Result on the test rig (QEMU 9.2, virglrenderer 0.10.4): **the device is created, the shaders
are accepted, and the pixel inside the triangle reads back red (255 0 0 255).** The pixel
outside reads 0 0 0 0 instead of the green it was cleared to: clearing a render target that is
not the bound one is not carried out (see below).

What had to be put right on the way:

- The user-mode driver names its capability set at context creation; the host refused that
  (no context types there), so every device failed. The kernel driver now asks for a plain
  context on such a host.
- Direct3D's "one sample" went to the host as a sample count of one; virglrenderer 0.10 then
  refuses buffers and makes multisample textures (Mesa patch 0016).
- The first version of the test made its triangle from `SV_VertexID` without a vertex buffer.
  The rig's virglrenderer has no base-vertex input for shaders and does not draw without
  vertex elements. Programs do not draw that way; the test was changed, the limits stand.

Open after this test, in the order they will matter for the compositor:

1. **Clearing a render target that is not bound.** The virgl driver has a host command for it
   in newer versions; on this host nothing happened, and the depth-stencil clear was a null
   pointer. Done in the guest now (Mesa patch 0017: bind, clear, bind back), and the test
   reads back both pixels as expected: 255 0 0 255 inside, 0 255 0 255 outside.
2. Which further things Mesa's Direct3D 10 driver sends that virglrenderer 0.10 lacks. The
   feasibility study expected the host's version to matter here; so far everything could be
   met in the guest, and the rig was not changed.
3. A driver update in place: `devcon update` with an unchanged INF version keeps the old
   file. Replacing the file (rename the loaded one, copy the new one) and `devcon restart`
   works, also for the user-mode DLL.

## The compositor's first answer (2026-11-14)

With Direct3D 10 working, the Desktop Window Manager was restarted (`net stop uxsms`,
`net start uxsms`; composition is switched on in the test user's registry). It makes a device
on our driver, destroys it again and logs event 9007: "The Desktop Window Manager was unable
to start because WDDM is not in use" (once before that it exited with 0xC00002FE).

The adapter is a WDDM adapter and Direct3D 10 runs on it, so what it misses is most likely the
usual test for the driver model, a Direct3D 9Ex object, which needs a Direct3D 9 user-mode
driver. The INF names the Direct3D 10 DLL in that place, which has no Direct3D 9 entry point.
So the Direct3D 9 part planned as a stub for W1 is needed before the compositor will even
try: next step, and the first thing to find out is how much of Direct3D 9 it wants (the
object and its capabilities only, or drawing too).

A probe (`tools/win7-hello/d3d9probe.c`) narrows it: `Direct3DCreate9Ex` succeeds on our
adapter, so Direct3D 9 does take it for a WDDM adapter; `GetDeviceCaps` for the hardware
device fails with `D3DERR_NOTAVAILABLE`, because there is no Direct3D 9 driver behind it.
Which of the two the compositor's test looks at is to be read out of `dwm.exe` and
`dwmcore.dll` next.

**Where the refusal comes from** (read out of `dwm.exe` and `dwmcore.dll` with their public
symbols, 2026-11-14 and 15): the event is raised in `CDwmAppHost::VerifyDisplayModesViaMIL`,
which looks at what its composition engine reports about the display: "WDDM is not in use"
when the engine's WDDM flag is zero. The engine sets that flag once it has a Direct3D device
for the adapter (`CDisplay::ReadGraphicsAccelerationCaps`, `CD3DDeviceTable::CreateD3DDevice`),
and this is how it gets one:

1. It asks the adapter for the Direct3D 10.1 interface, then for the Direct3D 10 one
   (`IDXGIAdapter::CheckInterfaceSupport`). That gives feature level 10.1 or 10.0, or 9.3 when
   the driver has neither.
2. It asks the display kernel which WDDM version the driver is (`D3DKMTQueryAdapterInfo`,
   `KMTQAITYPE_DRIVERVERSION`). Below WDDM 1.1 (the number 1105) the level drops to 9.3,
   whatever step 1 said.
3. It calls `D3D10CreateDevice1` on the adapter at that level and then at each lower one, and
   takes the first that works. The levels 9.x run on the driver's Direct3D 9 part, through
   Windows' own layer.

So on a WDDM 1.0 driver the compositor needs a complete Direct3D 9 driver, and on a WDDM 1.1
driver with a Direct3D 10 part it needs no Direct3D 9 at all. An earlier version of this
section said the engine takes its capabilities from Direct3D 9; that was a wrong reading.
`tools/win7-hello/dwmprobe.c` asks the same three things with the same arguments and prints
the answers.

Tool note: `win32k.sys`, `dwm.exe`, `dwmcore.dll` and `uDWM.dll` were rearranged after
linking, so their symbols need the address table in the symbol file (`resym.py` applies it
now); `dxgkrnl.sys` and `dxgmms1.sys` were not.

## Aero (2026-11-15)

**What makes a driver WDDM 1.1 for Windows 7.** Not the interface version given in
`DriverEntry`, and not `DXGK_DRIVERCAPS.WDDMVersion`. The display kernel (`dxgkrnl.sys`,
`DxgkQueryAdapterInfo` and `DXGADAPTER::Initialize`) reports 1105 only when the driver has all
three of `DxgkDdiRenderKm`, `DxgkDdiQueryVidPnHWCapability` and
`PresentationCaps.SupportKernelModeCommandBuffer`, and 1000 (WDDM 1.0) otherwise. The three
are GDI hardware acceleration: with them GDI asks the driver for the surfaces of what it draws
in windows (`D3DKMDT_STANDARDALLOCATION_GDISURFACE`) and sends its drawing as commands through
`DxgkDdiRenderKm`.

Our driver registers the entry point and sets the capability, and refuses the GDI surface
type. GDI then keeps its surfaces in system memory, as it does on a WDDM 1.0 driver, and no
command arrives (none in the tests). Drawing GDI's six operations on the host is left for the
performance work.

**Result (load test 13).** A fresh test VM, the package installed as the repository has it
(kernel driver and the Direct3D 10 driver, nothing for Direct3D 9), one reboot: the compositor
starts by itself. Glass window frames with the blurred desktop behind them, shadows, the
translucent taskbar and Flip 3D are drawn on the host GPU, still on the stock virglrenderer
0.10.4. `dwmprobe` there: Direct3D 10 interface present, WDDM version 1105, a device at
feature level 10.0 made.

**Black rectangles on the display, and their cause.** At first parts of the taskbar (half of
the Start button, an icon, the clock) and patches of window frames stayed black or old on the
host's picture after they had changed. Three checks found it:

1. `tools/win7-hello/d3d10tex.c` updates a texture the way the compositor uploads what changed
   in a window (`UpdateSubresource`, rectangle by rectangle) and reads it back: all correct.
   Not the upload.
2. A screenshot taken inside the guest was right at the moment the host's picture was wrong.
   So the compositor's picture was whole and the wrong one was only what the display showed.
3. The kernel driver built with `TRACE_PRESENT=1` (a line for every present): the compositor
   flips between two buffers and copies nothing.

After a flip the runtime has the driver rotate the swap chain's buffers, and the Direct3D 10
driver moved a view to its new buffer only when the view was bound again. The compositor
leaves its render target bound over the flip, so it went on drawing into the buffer that was
on screen, and the next flip showed the other one without that drawing. Patch 0018
(`windows/mesa/patches-virgl`) moves the views that are bound when the buffers rotate. Since
then the taskbar, window frames, dialogs and Flip 3D are whole on the display.

**The compositor's own test switch.** `Force10OnWDDM1_0` (a DWORD of 1 under
`HKCU\Software\Microsoft\Avalon.Graphics`, read by `dwmcore.dll`) makes the compositor use
Direct3D 10 on a WDDM 1.0 driver. It showed that the Direct3D 10 path works on our driver
before the kernel driver was changed. It is per user and nothing for a package.

**Direct3D 9's list of checks.** On the way here the Direct3D 9 capability answers were made
acceptable to Windows 7 (`windows/umd9`, not in the package). Direct3D 9 offers a hardware
device only if the driver's capabilities pass `IsD3DHALSupported` in `d3d9.dll`; when they do
not, it silently keeps one display format and reports no hardware device
(`D3DERR_NOTAVAILABLE` from `GetDeviceCaps`). At shader model 3 the list includes: dynamic
textures, the scissor test, 1 to 4 render targets (6 user clip planes with more than one),
all comparison functions, the usual blend factors, textures of 4096 or more with a repeat of
8192, a guard band of 8192, 256 vertex shader constants, point size in the vertex format,
stream offsets shared between elements, and anisotropic filtering of 16 samples or more with
its raster capability and minification filter. The last one was what ours failed.
`d3d9probe HEX` prints what the Direct3D 9 object kept from the driver.

**The W3 checks (load test 14, 2026-11-15).** A fresh test VM, the package with patch 0018,
one reboot. On the display, with no setting changed by hand:

| Check | Result |
|---|---|
| The compositor starts by itself and stays up | yes; it also came back by itself after Windows' assessment tool had switched it off for its run (the only compositor events in the log) |
| Glass window frames with the blurred desktop behind, shadows | right |
| Translucent taskbar, Start button, notification area, clock | whole |
| Live thumbnail over a taskbar button | right |
| Flip 3D | right |
| Desktop peek (windows turn to glass outlines) | right |
| Host's renderer | no error in its log |

Not measured: Windows' own compositor assessment (`winsat dwm`) refuses to run ("no LDDM
Support"), because it tests through Direct3D 9, which the driver does not have yet.

**Next to the GeForce 9600 GT.** The same five screens (desktop, a window, a live thumbnail,
Flip 3D, desktop peek) were taken on the reference: the test VM's base snapshot started with
the card passed through instead of the virtio display, NVIDIA's driver, 1024x768, pictures
taken inside Windows. Laid next to ours they show no difference in the glass, the shadows,
the thumbnail, the stacked windows of Flip 3D or the glass outlines of peek; the only ones
are the mouse pointer (our pictures come from the host's display, which has it) and a
notification icon that had not appeared yet. The captures are kept out of the repository
(they show the test account's desktop).

## Preemption, and no machine-wide settings (2026-11-15)

The Windows 10 driver's INF switched GPU preemption off for the whole machine
(`GraphicsDrivers\Scheduler\EnablePreemption = 0`) and told Windows never to recover from a
GPU timeout (`TdrDebugMode = 1`), because the driver's preemption entry point did nothing:
with preemption on, Windows would ask for one and wait forever for the answer.

The driver now answers. The host cannot stop a command buffer half way, so a preemption
request is reported done (`DXGK_INTERRUPT_DMA_PREEMPTED`) once everything submitted has
finished, with that last fence as the last completed one; if nothing is pending it is reported
at once (`VioGpuCommander::Preempt`). `DxgkDdiQueryCurrentFence`, which Windows reads when it
suspects a hang, returns the last finished fence instead of nothing. Both settings are gone
from the INF.

Load test 19 (fresh VM, this package): Windows' scheduler key holds neither value; the
Experience Index runs to the end (4.6, gaming graphics 5.6, frame rates as before); 32-bit and
64-bit checks, dxdiag and Chess Titans next to the compositor (also inside Flip 3D) right; no
display timeout or reset event in the System log; nothing in the host's log.

Still a stub then: recovery after a real timeout (`DxgkDdiResetFromTimeout`,
`DxgkDdiRestartFromTimeout`); done the same day, "Recovery after a GPU timeout" below.

## The hardware pointer (2026-11-15)

The pointer is now drawn by the host over the picture, as on a real card, instead of by
Windows into every frame. The driver offers colour pointers up to 64x64
(`DXGK_DRIVERCAPS.PointerCaps.Color`; the INF sets `HWCursor` to 1). `DxgkDdiSetPointerShape`
copies the shape into a 64x64 host image and sends `VIRTIO_GPU_CMD_UPDATE_CURSOR`;
`DxgkDdiSetPointerPosition` sends `MOVE_CURSOR`. The code follows virtio-win's 2D driver
(viogpudo, BSD-3-Clause). Monochrome and masked pointers, such as the text I-beam that inverts
what is under it, are refused, and Windows then draws those itself, exactly as before.

Two things learned on the way:

- Hiding the pointer by detaching its image (resource 0, as the 2D driver does) is not enough
  for every viewer: QEMU's VNC server ignores the hide and keeps showing the last image, with
  its hot spot moved to 0,0, next to the I-beam Windows now draws. The driver hides it by
  showing a transparent image instead, and puts the shape back when Windows shows it again.
- QEMU takes the control queue (the image upload) and the cursor queue (the switch to the
  image) in no fixed order, so a pointer command sent straight after the upload can show the
  previous shape. The driver waits for the upload to finish first (`UploadCursor`).

Checked in load test 21 (fresh VM, this package), with `scripts/screen-vnc.sh -c`, which also
saves the pointer image the host has: the arrow on the desktop (hot spot 3,3), a transparent
image while the pointer is over a text field (Windows' own I-beam in the frame), the arrow
again after, the resize arrow on a window edge (hot spot 14,7), Chess Titans with the pointer
over the board and none drawn into its frames. Nothing in Windows' System log or the host's.

## Recovery after a GPU timeout (2026-11-15)

When a command buffer takes the host more than about two seconds, Windows asks the driver to
preempt it, waits two more seconds, and then resets the driver ("Display driver stopped
responding and has recovered", event 4101): it collects debug data
(`DxgkDdiCollectDbgInfo`), calls `DxgkDdiResetFromTimeout`, frees what the lost devices
held, and calls `DxgkDdiRestartFromTimeout`. Applications get their device back as lost or
removed; the compositor makes a new one.

The host cannot be stopped half way through a buffer, so the driver's reset forgets instead
(`VioGpuCommander::Reset`): every buffer not yet sent to the host is dropped; the one the
host is working on no longer counts, and what it holds (its allocations' busy marks, the event
someone waits on) is given up at once, because Windows may free those allocations right after
the reset; and an epoch counter makes sure that the host's late answer for any buffer from
before the reset is never reported to Windows as finished.

Testing it found a bluescreen that was there before this work, in Windows itself: stop `0x7E`
(an access violation) in `VIDMM_DMA_POOL::WriteDmaBufferToMinidump`, while Windows writes its
report of the timeout and before it calls the driver. The report walks the adapter's whole
history of DMA buffers, but reads each one as if it lived where the pool being written keeps
its buffers. The driver had paging buffers in its aperture segment (`PagingBufferSegmentId` 1)
and command buffers in system memory (`DmaBufferSegmentSet` 0), so for a command buffer
Windows followed the paging pool's segment pointer, which is null there. Paging buffers are now
in system memory too (`PagingBufferSegmentId` 0); the driver does all paging work on the CPU,
so nothing else changes. (The dump said which pointer: `tools/win7-debug/kmem.py` reads the
exception and registers out of `MEMORY.DMP`.)

How it was tested: Windows 7's own way to fake a timeout (`D3DKMTEscape` with
`D3DKMT_ESCAPE_TDRDBGCTRL`, `tools/win7-hello/forcetdr.c`) answers "not supported" on a retail
system, and a real one would mean keeping the host GPU busy for seconds. So a test build of
the driver (`TEST_TDR=1 windows/kmd/build.sh`) takes a private escape that holds the next
finished buffer back, as a host that is slow with it, for up to the given time or until the
reset: `forcetdr stall 10000`, then move the pointer so that something is drawn. On the test
VM with Chess Titans open: the timeout comes about four seconds later, Windows recovers, the
desktop and Aero come back, Chess Titans plays on, and the System log has event 4101 and
nothing else; a second timeout a minute later recovers the same way. Nothing in the host's
log. If the buffer is held back much longer than the reset (ten seconds without ending at the
reset), Windows times out again during the recovery and stops with `0x116` ("recover from
timeout failed"), as it does with any GPU that stays hung.

Load test 22 (fresh VM, the regular build with these changes): 32-bit and 64-bit Direct3D 9
checks right, DirectDraw lists the hardware devices, dxdiag finds no problems, the Experience
Index as before (4.6, gaming graphics 5.6); nothing new in Windows' System log or the host's.

## Command buffers in flight (2026-11-15)

Every part of every command buffer (a SUBMIT of Direct3D's commands, a transfer) went to the
host fenced, and the driver waited for each answer before sending the next part or the next
buffer. QEMU answers a fenced command only once the host GPU has finished it and everything
before it, and it notices a finished fence only at the guest's next notification or at its own
10 ms poll; the Mac side found the same (`docs/re/perf-notes.md`). So the guest sat idle for a
round trip per part, and anything that waits for the GPU once a frame ran at about 100 frames
a second at most. The Experience Index's tests sat at 46-97 frames a second.

Now (`VioGpuCommander`, `VioGpuCommand::Run`):

- all parts of a buffer go to the host at once and in order; only the last is fenced and comes
  back. Up to eight buffers are with the host at a time; the host runs them in queue order.
- Windows hears of finished buffers strictly in submission order (a buffer without host work
  finishes at once, but waits for the ones before it).
- a sender that finds the virtio ring full waits for the host to give descriptors back. Before,
  the command was dropped without a word; with one buffer at a time that never happened.
- while buffers are with the host, the driver notifies the queue every millisecond without a
  new command, so QEMU looks at its fences then and not only every 10 ms. The system timer
  runs at 1 ms for it while there is work and goes back a second after the last buffer.

The Experience Index's Direct3D tests (frames a second, `dx`, 1280x1024 off screen):

| Test | before | buffers in flight | and the 1 ms notify |
|---|---|---|---|
| Direct3D 9 batch | 97 | 132 | 522 |
| Direct3D 9 alpha blend | 81 | 149 | 455 |
| Direct3D 9 ALU | 53 | 115 | 294 |
| Direct3D 9 texture load | 48 | 115 | 264 |
| Direct3D 10 batch | 96 | 100 | 137 |
| Direct3D 10 alpha blend | 96 | 100 | 149 |
| Direct3D 10 ALU | 61 | 64 | 89 |
| Direct3D 10 texture load | 46 | 51 | 85 |
| Direct3D 10 geometry (two) | 92, 31 | 100, 30 | 157, 59 |
| Direct3D 10 constant buffer | 49 | 50 | 84 |

The middle column is the same driver without the notify; both were measured on one VM,
replacing the driver in place. Direct3D 10's geometry-shader test (the second geometry
number) raises its load as frames get faster, so it reads 28 to 59 between runs and does not
compare.

Load test 23 (fresh VM, this package): the checks right (32-bit and 64-bit Direct3D 9,
DirectDraw's hardware devices, dxdiag), the Experience Index 6.1 for graphics and 6.1 for
gaming graphics (4.6 and 5.6 before), nothing in the host's log. With the desktop idle QEMU
uses as much of the host's processor as with the driver before (20-22% of one core over ten
seconds, both measured on the same VM). The timeout recovery above was run again with
buffers in flight and works the same.

## Speed, second round: where a frame's time goes (2026-11-15)

The Experience Index stood at 6.1 for graphics and 6.1 for gaming graphics. Its detailed
results (`C:\Windows\Performance\WinSAT\DataStore`) give a score per test: the Direct3D 9
tests count only up to 6.0, the gaming score comes from the Direct3D 10 tests, and of those the
weakest pull it down most. The same disk keeps a 2025 assessment on a passed-through Radeon of
the HD 5800 series: 7.8 and 7.8, Direct3D 10 batch 1751 frames a second, geometry 1391 and
2739.

Measured with the tools in `tools/win7-debug/rig/` and `tools/win7-debug/guest/` (described in
`tools/win7-debug/README.md`), during the Direct3D 10 batch test:

- The guest used one processor fully, but the samples of its instruction pointer showed a
  program polling: Windows' kernel waiting on an object (`KeWaitForSingleObject`, 27% of the
  busy samples, with the system call around it about two thirds), and in our driver
  `QueryGetData` → `virgl_get_query_result` → `virgl_fence_finish`. The assessment waits for an
  event query every frame.
- The kernel driver's timing build (`TRACE_TIMING=1 windows/kmd/build.sh`) showed each command
  buffer 6.2 to 6.7 ms with the host, the rest of its path under 0.1 ms.
- The host's Intel GPU was awake 94% of the time, but at its lowest clock, 350 of 1200 MHz: the
  rig's minimum clock had gone back to 350 at a host restart (the raise to 1200 is live only,
  `docs/re/perf-notes.md`). Raised to 1200 again: the batch test went from 150 to 344 frames a
  second.
- With the GPU at full clock, how soon the guest learns that work has finished counted again.
  QEMU looks at virglrenderer's fences when the guest notifies the device, or at its 10 ms
  poll. The kernel driver's notification while work is out now comes every 0.5 ms (was 1 ms:
  268 against 346 frames a second), and a program that polls a fence makes the driver notify
  at once, at most every 100 µs (`VIOGPU_NUDGE`, patch 0027): 446 frames a second, the GPU now
  98% busy. Patch 0027 also gives Windows the fence wait's timeout in milliseconds; it was
  passed in nanoseconds.

Tried and dropped: keeping constant buffers on the CPU and binding each change from a fresh
range of a stream buffer. Like for like (two runs each, alternating), the batch test 348-355
against 353-368 and the constant buffer test 248 against 248: no difference.

| Experience Index | before | GPU at 1200 MHz | and the nudges |
|---|---|---|---|
| Graphics | 6.1 | 6.5 | 6.7 |
| Gaming graphics | 6.1 | 6.5 | 6.7 |
| Direct3D 10 batch / alpha (frames a second) | 140 / 129 | 355 / 358 | 457 / 450 |
| Direct3D 10 texture / ALU | 89 / 85 | 239 / 237 | 265 / 266 |
| Direct3D 10 geometry F4 / V8 | 135 / 30 | 287 / 35 | 340 / 40 |
| Direct3D 10 constant buffer | 84 | 203 | 196 |

What holds the score now:

- The host GPU: in the batch tests it is 96-98% busy. A Direct3D 10 frame of that test is about
  22 draws with trivial shaders and keeps the GPU busy about 2.2 ms; the Direct3D 9 version of
  the same test needs about 1.5 ms. Not found yet why; the GPU runs one request a frame from
  QEMU and QEMU never waits on it.
- The second geometry test (V8, 5.5): a geometry shader with a three-step loop that writes an
  indexed temporary array, about 20 ms of GPU time a frame. On real hardware it is the fastest
  of the tests.
- The GPU clock: at a host restart the rig's minimum clock goes back to 350 MHz, and the
  scores with it (6.5 → about 6.1).

Load test 24 (fresh VM, the package built from scratch with patch 0027, host GPU at 1200 MHz):
the checks right (32-bit and 64-bit Direct3D 9, DirectDraw, dxdiag), Chess Titans right, the
Experience Index 6.6 and 6.6 (Direct3D 10 batch 455, geometry 312 and 43), nothing in the
host's log. Between runs on one install the index moves by 0.1.

### The same package on Hog (2026-11-15)

The package of load test 24 on Hog (`docs/windows/test-vms.md`, "On Hog": QEMU 8.2 with KVM in
WSL2, virglrenderer 1.0 on Mesa's Direct3D 12 driver, a GeForce RTX 4090): the desktop with
Aero at 1024x768, the 32-bit and 64-bit Direct3D 9 checks right, DirectDraw's hardware devices,
dxdiag "No problems found", and the Experience Index 7.3 for graphics and 7.3 for gaming
graphics. Direct3D 10 frames a second: batch 654, alpha 707, ALU 380, texture 637, geometry
749 and 808, constant buffer 1805; Direct3D 9 batch 769.

The second geometry test, 40 frames a second on the rig, runs at 808 here: its slowness is the
rig's GPU (or its driver), not our translation. The other scores are not bound by Hog's GPU,
which is mostly idle: see the next section.

Found on Hog (fixed 2026-11-16: `docs/windows/mesa-win7.md`, "Shaders that fit the host"; it was
the constant buffer test): the host renderer refused one shader program, "Too many vertex uniform
blocks (15/14)". Direct3D 10 has 14 constant buffer slots, and the Direct3D 10 driver binds
slot n as Gallium's constant buffer n + 1, so a shader using the last slot needs 15 uniform
blocks on the host; Mesa's Direct3D 12 GL driver offers 14, the rig's Intel driver more. After
such an error virglrenderer drops that context's later drawing, so the test it happened in
(not known yet which) may have counted frames it did not draw.

## Speed, third round: buffers reported once they are with the host (2026-11-16)

On Hog the host GPU is far from busy, yet the Direct3D 10 batch test stayed near 650 frames a
second. The timing build (`TRACE_TIMING=1`, which now also counts parts, bytes and how many
buffers are out at once) showed why: Windows' scheduler hands the driver one buffer, at most two,
and the next only when one is reported done. A buffer waited about 12 ms in the scheduler and
0.8 ms with the host, so each frame paid a full round trip to the host, which under nested
virtualization (a VM exit costs tens of microseconds) is long.

Three changes, measured on Hog with the regular build:

- A command buffer's parts are sent from the DMA buffer itself (or a reused copy, below)
  instead of a fresh copy each time: under nested virtualization each newly touched page of pool
  memory costs a VM exit, 75-100 µs a frame.
- The device is notified once a buffer, after all its parts are queued, not once a part.
  Batch test 642-663 → 714-739.
- Rendering buffers are reported done to Windows once all of their parts are with the host
  (the host runs them in order); paging and present buffers still once the host has run them,
  and reports always go out in submission order. Since Windows may then reuse the DMA buffer,
  these buffers' parts are copied into reused memory (a lookaside list). Everything that waits
  for the GPU for real still waits for the host: the completion event of a buffer, the
  allocations' busy marks (which the user-mode driver's waits use), and now also the
  destruction of an allocation. Batch test → 882-888.

The first try reported the preemption (Windows asks for one now and then; the driver answers
when it has nothing left with the host) before the last buffers' completion. Windows takes
every buffer not reported by then as preempted and submits it again, the same DMA buffer and
so the same command object, which was then run and freed twice: bugcheck 0xC2
(`BAD_POOL_CALLER`, 7, a block freed twice), found from QEMU's monitor (the stack of the one
processor not frozen, the block's pool tag, the driver's base). `build.sh` now writes a link
map (`viogpu3d.map`) to name addresses in such a dump.

| Hog, Experience Index | before | now |
|---|---|---|
| Graphics / gaming graphics | 7.3 / 7.3 | 7.5 / 7.5 |
| Direct3D 10 batch / alpha (frames a second) | 654 / 707 | 864 / 887 |
| Direct3D 10 texture / ALU | 637 / 380 | 843 / 596 |
| Direct3D 10 geometry F4 / V8 | 749 / 808 | 1031 / 940 |
| Direct3D 10 constant buffer | 1805 | 2150 |
| Direct3D 9 batch | 769 | 910 |

Chess Titans and the desktop with Aero look as before. Hog's GPU is still mostly idle: the
frames are still bound by round trips, now those of the present buffers and the programs' own
waits for the GPU.

The same driver on the rig (VM 220, host GPU at 1200 MHz): the Experience Index 6.9 and 6.9 (was
6.6 to 6.7); Direct3D 10 frames a second: batch 655 (457), alpha 629 (450), ALU 323 (266),
texture 324 (265), geometry 639 (340) and 59 (43), constant buffer 311 (196). So the rig's Intel
GPU was not the only limit either. Its weakest test is still the second geometry test.

## Speed, fourth round: where each host's frame goes (2026-11-16)

Measured again after the third round, with two new tools:

- `tools/win7-debug/rig/rcsbusy.sh SECONDS` on the VM host: the share of time the Intel GPU's
  render engine was executing requests, from the i915 driver's own counter (the PMU event
  `i915/rcs0-busy/`, read with `perf_event_open` from perl, so neither perf nor intel_gpu_top
  is needed). `gpubusy.sh` only says how long the GPU was awake.
- `RIPPLE_CMD_STATS` with `RIPPLE_UMD_LOG` in the guest (Mesa patch 0035): what the command
  streams hold, by virgl command, and forty submits in a row with their time and contents.

**The rig is bound by its GPU.** The render engine is 96 to 100% busy in every Direct3D 10 test
and in the Direct3D 9 ones (batch 659 frames a second, ALU 318, texture 336, constant buffer
321, second geometry test 809, alpha 639; Direct3D 9 batch 730, ALU 283, texture 303). The host
submits one GPU job a frame (i915 `request_add`: 646 a second at 642 frames a second) and never
waits on one. What is left there is how much GPU time a frame's drawing costs, not the path to
the GPU.

**Hog was bound by the trip to the host, and made it twice a frame.** Its GPU is about 45%
busy; QEMU's time is mostly the guest's processors, the thread that draws about a fifth of
one. The submit trace showed every frame followed by a second, empty submit:

    11195.633 ms:  5724 bytes, draws 31, ... copies in 31
    11195.666 ms:     8 bytes, draws 0

WinSAT ends two event queries a frame. Mesa's virgl driver flushes for each (`end_query` of a
`GPU_FINISHED` query asks for a fence), and a flush that asks for a fence was sent even with
nothing encoded since the last one: a second guest kick, host flush and completion interrupt.
Mesa patch 0034: every submit keeps its fence, and a flush that finds nothing new (and no
upload queued) returns that fence. An event query ended again also let go of its old fence
only when destroyed, one event handle a frame; fixed with it.

On one install on Hog, runs alternated (`RIPPLE_EMPTY_FLUSH` keeps the old behaviour): batch
872 and 895 frames a second with the empty submits, 1096 and 1114 without; the test's handle
count stays at 155 through a run.

| Hog, Experience Index | before | now |
|---|---|---|
| Graphics / gaming graphics | 7.5 / 7.5 | 7.7 / 7.7 |
| Direct3D 10 batch / alpha (frames a second) | 898 / 965 | 1125 / 1224 |
| Direct3D 10 texture / ALU | 847 / 656 | 1031 / 896 |
| Direct3D 10 geometry F4 / V8 | 1052 / 1056 | 1398 / 1398 |
| Direct3D 10 constant buffer | 721 | 902 |

The rig stays at 7.1 with the same frame rates, as a GPU-bound host should. Chess Titans and
the desktop with Aero look as before on Hog.

What the rig's GPU time is spent on, from the same tests at fewer pixels (render engine 96 to
100% busy each time):

| Frames a second on the rig | 1280x1024 | 640x512 | 320x256 |
|---|---|---|---|
| Direct3D 10 batch | 676 | 1656 | 2189 |
| Direct3D 10 ALU | 315 | 746 | 1054 |

A frame's GPU time is close to a fixed part plus a part per pixel: batch about 0.4 ms and
0.8 ms a megapixel, ALU about 0.75 ms and 1.9 ms a megapixel. At the assessment's 1280x1024
about three quarters of the time is pixel work, on a GPU running at its top clock. So these
tests are bound by how fast the UHD 630 fills pixels, and what the driver could still save is
in the fixed quarter. For scale: on real hardware Intel's HD 4600 (20 execution units) is
listed at 6.5 to 6.7 for gaming graphics and the Iris Pro 5200 (40) at the top, 7.9; the
UHD 630 (24) is at 7.1 here.

## Speed, fifth round: nudges only where they are cheap (2026-11-16)

A new line in the driver log (`RIPPLE_CMD_STATS`, Mesa patch 0036) says how the fences a
program polls went: how many nudges each took, how long after its submit it was found
signalled, and what a nudge costs. It corrects the fourth round's picture of Hog. WinSAT
keeps about ten frames in flight on both hosts: the fence it polls was submitted 8 ms earlier
on Hog (0.8 ms a frame) and 15 ms earlier on the rig (1.5 ms a frame). Neither host waits for
one frame's trip to the host and back. (What bounds Hog instead is in the sixth round: not the
host's work, as first written here.)

On Hog the nudges themselves were in the way. The batch test at 1280x1024, by nudge interval
(`RIPPLE_NUDGE_US`), one install, runs mixed:

| Interval | 5 µs | 20 µs | 100 µs (as shipped) | 400 µs | 800 µs | 1.6 ms | none |
|---|---|---|---|---|---|---|---|
| Frames a second | 1146 | 1088 | 1105 to 1126 | 1184 to 1202 | 1192 | 1227 to 1230 | 1266 to 1315 |

Why: QEMU's virtio-gpu device has `ioeventfd` off by default (`info qtree` on both hosts), so
a notification is handled by the guest processor that sent it, under QEMU's global lock. The
thread that draws holds that lock while it works, and the processor waits until it is between
jobs. Measured from the driver, a nudge takes 500 to 900 µs on Hog, where that thread is the
limit, and 12 to 100 µs on the rig, where the GPU is.

So the driver times each nudge, and the next one waits the square of what they have been
taking, in units of 100 µs (at least 100 µs, at most 20 ms): every 100 µs on the rig as
before, every few milliseconds on Hog, which then runs as without nudges.

| Batch test, frames a second (two rounds) | spaced by cost | every 100 µs | none |
|---|---|---|---|
| Hog | 1252, 1241 | 1100, 1125 | 1273, 1250 |
| Rig | 624, 641 | 633, 652 | 623, 621 |

| Hog, Experience Index | before | now |
|---|---|---|
| Graphics / gaming graphics | 7.7 / 7.7 | 7.8 / 7.8 |
| Direct3D 10 batch / alpha (frames a second) | 1125 / 1224 | 1298 / 1352 |
| Direct3D 10 texture / ALU | 1031 / 896 | 1195 / 881 |
| Direct3D 10 geometry F4 / V8 | 1398 / 1398 | 1558 / 1576 |
| Direct3D 10 constant buffer | 902 | 986 |

The rig stays at 7.1 / 7.1 (batch 643, alpha 622, ALU 325, texture 315, constant buffer 315).
No new renderer errors on either host.

Tried and dropped, both measured on the same install with runs alternated:

- A first nudge delayed by what earlier fences needed (learned per program): 1103 to 1148 on
  Hog, no better than every 100 µs. The cost of a nudge is what tells the hosts apart.
- `UpdateSubresource` of a whole buffer mapped with `PIPE_MAP_DISCARD_WHOLE_RESOURCE`, so virgl
  takes fresh storage instead of copying on the host (31 copies a frame in the batch test
  become none): 1128 and 1169 against 1115 and 1150 on Hog, 632 and 635 against 638 and 624 on
  a quiet rig. No gain, not kept.

For the next round, QEMU's drawing thread on Hog during the batch test (`perf trace -s` on the
thread, 4 seconds at about 1180 frames a second): 36,743 `ioctl` calls taking 1.0 s (7.8 a
frame, 28 µs each), then `ppoll`, `futex` and `getpid` at 0.13 to 0.17 s each, and one
`openat`, `eventfd2` and two `close` for every 1.6 frames.

## Speed, sixth round: notifications that do not hold up the guest (2026-11-17)

The fifth round's last sentence about Hog was wrong: its host was not working at its limit. A
profile of QEMU's drawing thread during the batch test (`perf record -e cpu-clock -g -t TID`,
frame-pointer stacks, call sites named from the libraries' relocations) showed:

- the thread on a processor only two thirds of the time (3.3 s of 5 s);
- a third of that inside `glFenceSync` (virglrenderer makes one fence a command buffer, and
  Mesa's Direct3D 12 driver flushes its batch for it), 19% of the thread's time in one
  three-instruction loop of NVIDIA's driver under that call;
- 23% in the kernel, a good part of it waking the guest processors that wait for QEMU's
  global lock.

So the host had time left and the frames did not arrive fast enough. What held them up is the
notification itself. With `ioeventfd` off (QEMU's default for this device) the guest processor
that writes the notification waits for QEMU's global lock, and the drawing thread holds that
lock for a whole command buffer. The kernel driver notified from the thread that queues the
command buffers: it queued one, then waited out the host's frame in progress, and host and
guest took turns.

Proof by one setting: the same guest with `-device virtio-vga-gl,...,ioeventfd=on` (the
notification becomes an event the kernel hands to QEMU, nobody waits) ran the batch test at
1531 to 1617 frames a second instead of about 1250, and the assessment gave 7.9 on every line.

The driver now gets there without the setting. The control queue has a thread for
notifications (`CtrlQueue::StartKicker`): the queueing thread asks it and goes on, only the
new thread waits, the host finds several buffers at a time, and requests made while it waits
become one notification. Where a notification is quick, waking a thread for it costs more
than it saves, so notifications are timed (one in four from the sender, all from the thread)
and the smoothed figure picks the way: by thread above 200 µs, from the sender below 100.
The figure goes up quickly and down slowly, because the thread's own notifications are a mix
of long waits and short ones.

Batch test on Hog, frames a second, three runs each (kernel drivers swapped in place with
`devcon restart`, the VM restarted to change the setting):

| Notifications | `ioeventfd` off (default) | `ioeventfd` on |
|---|---|---|
| from the sender (before) | 1274 to 1367 | 1540 to 1654 |
| always from the thread | 1569 to 1594 | 1312 to 1328 |
| chosen by their cost (now) | 1655 to 1671 | 1573 to 1626 |

| Hog, Experience Index, default settings | before | now |
|---|---|---|
| Graphics / gaming graphics / base score | 7.8 / 7.8 / 7.8 | 7.9 / 7.9 / 7.9 |
| Direct3D 10 batch / alpha (frames a second) | 1298 / 1352 | 1617 / 1806 |
| Direct3D 10 texture / ALU | 1195 / 881 | 1486 / 889 |
| Direct3D 10 geometry F4 / V8 | 1558 / 1576 | 2069 / 2161 |
| Direct3D 10 constant buffer | 986 | 1313 |

7.9 is where Windows 7's index ends.

The rig is bound by its GPU and does not change: batch 650, 641 and 650 before, 644, 648 and
646 after, on one boot. Its driver log shows the same choice being made there (notifications
of 600 µs at times, then back under 100). The full
assessment there gives 7.1 / 7.1 as before (batch 634, alpha 637, ALU 322, texture 324,
constant buffer 311), with no renderer errors, and the desktop with Aero looks as it did.

The nudges of the fifth round stay. With the thread a nudge costs the program under a
microsecond on Hog, so they come every 100 µs there again (4 to 6 for each polled fence), at
1633 frames a second.

In the UTM VM (emulated x86, 4 processors) the new kernel driver loads and the Direct3D 9 test
program draws, reads its picture back right and presents; no speed figures were taken there.

## The fine timer only for a stream of work (2026-11-17)

Noticed while another VM was being benchmarked on the same host: the rig's test VM, idle at its
desktop, kept QEMU's main thread at half a host processor or more. The guest's own counters showed it 100% idle. One second of KVM's trace
on the host (`kvm_pio` counted by port; the tracer has to be switched on first, `tracing_on` is
0 there) showed the guest's clock: 2048 reads of the real-time clock's ports a second, each a
trip to QEMU. `powercfg -energy` in the guest named the cause, "Outstanding Kernel Timer
Request, Requested Period 5000": ours.

The driver takes Windows' half-millisecond timer while command buffers are with the host (the
second round's nudges need it) and kept it for a second after the last one. A prompt window
with a blinking cursor sends two buffers 30 ms apart every half second, so the timer never went
back. On a VM whose clock is the emulated real-time clock that is 2048 interrupts a second for
an idle desktop; a VM with Hyper-V's timers (Hog) pays far less.

Now the timer is taken only for a stream of work: buffers with the host at moments spread over
60 ms or more with no pause over 100 ms (frames at 10 a second or faster, or one long job), and
it goes back 200 ms after the last buffer. A frame now and then is left to the ordinary timer
and QEMU's own 10 ms poll.

| Rig VM, idle, prompt window open | before | now |
|---|---|---|
| Guest clock interrupts a second | 2048 | 64 |
| QEMU process, share of one host processor | 66 to 80% | 24% |

No cost where it matters. Batch test, drivers swapped in place: Hog 1577 to 1599 frames a
second before, 1639 to 1657 after; the rig, four tests in two rounds each, within 3% either
way (ALU 317 and 324 before, 325 and 319 after; batch 642 and 653 before, 663 and 634 after).
The rig's Experience Index sits on the line between 7.0 and 7.1: two assessments with this
driver gave 7.0 and then 7.1 (gaming graphics tests 305 to 320 frames a second in the first,
320 to 330 in the second).

Hog with this driver: 7.9 on every line as before (two assessments; Direct3D 10 batch 1629 and
1640 frames a second, constant buffer 1319 and 1324), no renderer errors.

In the UTM VM the final driver (both changes) loads, the Direct3D 9 test program draws, reads
its picture back right and presents, and the idle guest keeps the ordinary timer: 0 of 315
samples of `NtQueryTimerResolution` over ten seconds were under 1 ms
(`tools/win7-debug/guest/timerres.ps1`). `powercfg -energy` did list a half-millisecond kernel
request there during its own five seconds; that was not followed up (its progress display in
the console may be the stream of work).

## A bluescreen on Hog, not explained (2026-11-17)

Once, during a test session on Hog: stop `0x4A` (`IRQL_GT_ZERO_AT_SYSTEM_SERVICE`, parameters
`7702989A, 2, 0, FFFFF880031C1CA0`): a system call returned to its program with the interrupt
level still at 2. It happened while a test script was installing a user-mode driver build
(the file fetched with BITS, the compositor stopped and started).

What the kernel dump says (`tools/win7-debug/kstack.py`, which names the code addresses left
on a thread's kernel stack, stale ones included): the thread belongs to `svchost.exe`, the
return address is in `ntdll`, and what ran last on its stack is NTFS (ten addresses) and the
kernel; nothing of `viogpu3d.sys`, `dxgkrnl.sys`, `dxgmms1.sys` or `win32k.sys`. That fits BITS
writing the downloaded file. It does not say what raised the level and left it there.

So there is no evidence that it is the display driver's, and none that it is not: code that
runs on a borrowed thread (an interrupt's deferred work) leaves no trace on that thread's
stack. Hog runs the guest under nested virtualization, where the interrupt level is one more
thing emulated. It has not happened on the rig. If it comes back, on either host, the first
thing to compare is what the display driver was doing at that moment (its serial log).

## UTM: the fine timer slows the guest clock (2026-11-17)

With Aero running under QEMU's TCG CPU emulation on Apple silicon, window operations made
Windows' tick clock advance at roughly a third of the performance counter's rate. The
driver's half-millisecond timer request was active almost throughout those operations.
After it released the request on an idle desktop, the clocks agreed again.

An isolated check (`tools/win7-hello/timerbench.c`) hides its console, requests each timer
resolution temporarily, measures three QPC seconds, then releases the request. On this VM:

| Requested interval | Actual interval | QPC elapsed | Tick-clock elapsed |
| --- | --- | --- | --- |
| ordinary | 15.625 ms | 3004 ms | 3000 ms |
| 4 ms | 3.9062 ms | 3004 ms | 2766 ms |
| 2 ms | 1.9531 ms | 3005 ms | 2562 ms |
| 1 ms | 0.9765 ms | 3027 ms | 1703 ms |
| 0.5 ms | 0.4882 ms | 3064 ms | 906 ms |
| ordinary again | 15.625 ms | 3012 ms | 3016 ms |

The command worker now recognizes QEMU's `TCGTCGTCGTCG` CPUID signature and leaves the timer
resolution alone there. The stream detection and fine-timer request remain unchanged for
other hypervisors. This avoids slowing all tick-based waits while trying to shorten GPU
fence waits. It cannot prevent another program from requesting a fine timer.

The x64 kernel driver builds, passes signature verification against the test certificate,
and loads after restarting the display device. With the same user-mode driver installed,
the ratio of tick time to QPC time changes from 0.357 to 1.000 during movement, 0.318 to 1.001
during repainting, 0.347 to 1.001 during resizing, and 0.363 to 0.999 during minimize/restore.
No fine-timer samples remain in any of the five modes (idle included). The 64-bit and 32-bit
texture regression tests still pass, and five more New-submenu cycles leave Explorer and
the compositor running with no new crash events.

These are clock measurements, not frame-rate claims. The early DWM timing queries returned
`DWM_E_COMPOSITIONDISABLED` from a worker thread even while glass was visible and
`DwmIsCompositionEnabled` returned true. Initializing the query on the window's UI thread
made the global query succeed; the probe now keeps its queries on that thread. The query for
the GDI window returns `E_PENDING`, and global submitted-frame identifiers must not be
mistaken for counts of presented frames. In-guest screen readback also took hundreds of
milliseconds per capture, so it cannot establish smooth animation. The UI probe now limits
requests by QPC as well as pumping messages, since a move/resize generates messages that can
otherwise make its message-aware wait return immediately. Exact frame pacing remains to be
measured without screen-readback stalls.

An attempted i386 kernel build still fails at the existing `InterlockedExchange64` call in
`viogpu_vidpn.cpp`; the kernel installed for these checks is x64. The 32-bit user-mode driver
and 32-bit regression executable both build and pass under that x64 guest.

## Refresh timing on the ordinary Windows 7 timer

The display driver advertises 60 Hz. Its flip thread slept for 16.67 ms after
every refresh. On the 15.625 ms Windows 7 timer in UTM, that usually cost two
ticks. In 180 consecutive waits on the display adapter, the measured rate
was 31.4 Hz and the median interval was 31.5 ms. Windows 7 detects timer
expiration on system-clock ticks. [Windows driver timer accuracy](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/timer-accuracy)

The flip thread now schedules successive deadlines against the interrupt
clock and skips expired deadlines after a stall. On the same x64 boot the
rate rose to 57.6 Hz, with a 16.3 ms median and a 30.9 ms 95th percentile.
The ordinary guest timer and Aero animations remained enabled. These rates
count refresh notifications. The number of distinct window frames also
depends on how fast Explorer and DWM can draw them.
