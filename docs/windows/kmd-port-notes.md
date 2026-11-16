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
