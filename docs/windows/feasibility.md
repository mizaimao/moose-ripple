# Windows 7's Aero on virtio-gpu and virgl: feasibility study

2026-11-14. Written before any code. The question: can moose-ripple do for Windows 7's Aero
desktop what it did for Snow Leopard's Quartz Extreme, that is, put it on the host's GPU
through QEMU's virtio-gpu device and virglrenderer, with everything in this repository
staying under the MIT licence?

## Verdict

**Go, in stages, with three gates.** Nothing found rules it out, most of the pieces exist
under permissive licences, and none of them has been put together for Windows 7 yet:

- A full Windows display driver for virtio-gpu with 3D exists as an unmerged pull request
  to virtio-win (BSD), for Windows 10. It has to be taken down from driver model 1.3 to 1.1.
- Mesa (MIT) has a Direct3D 10 driver part that runs on its virgl driver in that work, and
  our own Mesa version (24.3.4) still carries it.
- The one part Aero cannot do without by Microsoft's rules, a Direct3D 9 driver part, is
  not in upstream Mesa. A new one under MIT exists in a fork since 2026; otherwise we write it.

The gates, each a point to stop and report:

1. **A Windows 7 desktop on our own kernel driver** (no 3D yet), built off Windows.
2. **Direct3D drawing on the host GPU** from a Windows 7 guest, on the test rig's host.
3. **The compositor starts on it**: Aero glass on screen.

The largest risks are the kernel driver's port to Windows 7's older driver model, the age of
virglrenderer on the Proxmox test host, and how much of Direct3D 9 the Windows 7 compositor
and its checks really ask for. Each is listed below with the experiment that settles it.

## What Aero needs from a driver

- **A WDDM driver.** Aero's compositor (the Desktop Window Manager, DWM) only runs on the
  Windows Display Driver Model that came with Vista: a kernel part (the display miniport) and
  a user-mode part that the Direct3D runtime loads. Windows 7 takes model version 1.0 or 1.1.
  Hardware bar: Direct3D 9 class with pixel shader 2.0 and 32 bits per pixel.
- **Direct3D 9 is the mandatory driver interface.** In WDDM only the Direct3D 9Ex driver
  interface is required; Direct3D 10 and later are optional.
- **Windows 7's DWM draws with Direct3D 10.1.** On a card with a Direct3D 10 driver part it
  uses that. On a Direct3D 9 card it runs through "10level9", a Microsoft layer that puts
  feature levels 9_1 to 9_3 on top of the Direct3D 9 driver part. (Vista's DWM uses
  Direct3D 9Ex directly.)
- **There is no display-only driver on Windows 7.** That model came with Windows 8 (WDDM
  1.2). virtio-win's display driver, `viogpudo`, is such a driver, for Windows 8 and later.
  So today Windows 7 on a virtio-gpu device stays on the Standard VGA adapter, with no Aero.
  That is our software-rendering baseline, as "boot without the driver" was on the Mac side.

## What exists

| Piece | What it is | Licence | State |
|---|---|---|---|
| virtio-win `viogpudo` | Display-only driver for virtio-gpu, Windows 8 and later | BSD-3-Clause | Shipped. No Windows 7 build; virtio-win stopped building for Windows 7 after 0.1.173 (2019), for signing reasons |
| virtio-win pull request 943, `viogpu3d` | Full WDDM driver for virtio-gpu with 3D: kernel miniport, plus user-mode parts built from Mesa (an OpenGL driver and a Direct3D 10 driver) on virgl | BSD-3-Clause (kernel part), MIT (Mesa part) | Open since July 2023, not merged, last rebased September 2025. Built and tested on Windows 10 only. Registers as WDDM 1.3 (Windows 8.1). Its author doubts it works on Windows 7. Known gaps: no preemption (it turns preemption off for the whole system), drawing glitches, reports of low speed |
| Mesa merge request 24223 | The Mesa side of the above: a Windows "winsys" for the virgl driver and changes to the Direct3D 10 part | MIT | Not merged |
| virglrenderer changes for it | Seven commits: drawing without vertex elements, a vertex-id variant, two shader fixes, a clear-surface command, primitive restart | MIT | Merged: four in 1.0.0 (September 2023), three in 1.0.1 |
| Mesa `d3d10umd` | Direct3D 10 driver part (driver interface versions 10.0, 10.1 and 11.0) on Gallium, from VMware's work | MIT | In Mesa from 2021 until it was removed before 26.3. Present in 24.3.4, the version this repository already builds. Shader path is the older TGSI one |
| Mesa "nine" | Direct3D 9 as an API for Wine, on Gallium. Not a Windows driver part, but it holds a Direct3D 9 shader translator and fixed-function emulation | MIT | In our Mesa |
| Mesa `wgl` | OpenGL for Windows (the installable client driver) | MIT | In our Mesa |
| Yttrium (2026) | A fork of pull request 943 for Windows 10 x64 on WDDM 2. Draws through Venus (Vulkan on the host) with shared "blob" memory, not through virgl. Its Mesa fork adds a **Direct3D 9 driver part** inside `d3d10umd`, built on nine's shader translator | Kernel part: BSD-3-Clause with MPL-2.0 for its changes. The Direct3D 9 files: MIT | Active, alpha |
| vmlab-win3d (2026) | A package of Yttrium for Windows 10 and 11. Builds everything, the kernel driver included, **on Linux** with the llvm-mingw toolchain, Microsoft's headers fetched as NuGet packages, and signs with osslsigncode | MIT (its own scripts) | Proof that a WDDM driver can be built and test-signed off Windows |
| Microsoft's driver samples | `KMDOD`, a display-only sample (WDDM 1.2). No full WDDM 1.x sample with 3D | **MS-PL, not MIT** | See "Licences" |

Read for understanding only, never copied (GPL or LGPL): VirtualBox's Windows guest display
driver (a WDDM driver that runs Aero on Windows 7), ReactOS, Wine's Direct3D, qemu-3dfx.

## Can Mesa's Direct3D pieces cover what Aero needs?

Mostly yes for drawing, no for the kernel side, and the Direct3D 9 part is the open piece.

| Needed for Aero on Windows 7 | Where it would come from | Gap |
|---|---|---|
| Kernel miniport, WDDM 1.1 | Pull request 943's kernel driver | Port from 1.3 down to 1.1: drop the entry points Windows 7 does not have (boot-display handover, engine status and reset, command cancel, node metadata), take the display over from VGA the Windows 7 way and give it back for crash screens, check memory segments, command buffers, fences and timeouts against the 1.1 rules, and settle preemption. This is the largest piece of new work and the least documented |
| Direct3D 10 driver part | Mesa `d3d10umd` on virgl, with the winsys from merge request 24223 | Build Mesa 24.3 for Windows 7 (upstream's default minimum is Windows 8 since 22.2; there is a build option for older, and third-party builds kept Windows 7 working through 24.x). Whether the Windows 7 DWM is content with it has never been tried by anyone |
| Direct3D 9 driver part | Not in upstream Mesa | Three ways: (a) a stub that offers no 3D, enough to bring the kernel driver up; (b) the MIT files from Yttrium's Mesa fork, moved back to our Mesa version; (c) our own on nine's translator. Needed for certain: Vista's DWM, Direct3D 9 programs, Windows' own capability checks. Needed for Windows 7's DWM only if the Direct3D 10 part alone does not satisfy it |
| OpenGL in the guest | Mesa `wgl` on virgl | Comes with the same winsys |
| Screen modes, pointer, presenting a frame | `viogpudo` and pull request 943 | Small |
| Host side | QEMU's `virtio-vga-gl` and virglrenderer, unchanged | The Direct3D 10 part relies on the virglrenderer changes of 1.0.0 and 1.0.1. The Proxmox test host has **0.10.4** (January 2023), which has none of them |

## Risks and unknowns, and what settles each

1. **The kernel driver on WDDM 1.1.** Nobody has published a permissive WDDM 1.0 or 1.1
   driver with 3D; the only open one that runs Aero on Windows 7 is VirtualBox's (GPL: study
   only). Settled by gate 1 and gate 2.
2. **virglrenderer on the Proxmox host is older than the Direct3D 10 work.** Either the guest
   works around the missing pieces (bind a dummy vertex element, lower the vertex-id variant,
   clear through a draw), or the host gets a newer virglrenderer library, which is a change
   to the host and not ours to make. UTM and the Windows host carry newer versions already.
   Settled early in gate 2 by a test program, before DWM is involved.
3. **Does Windows 7's DWM accept a Direct3D 10 driver part without a real Direct3D 9 one?**
   Unknown. If not, the Direct3D 9 part moves from "later" to "before gate 3".
4. **Mesa on Windows 7.** Needs the older-Windows build option and a toolchain that still
   targets Windows 7. Third-party builds show it works for 24.x.
5. **Speed.** Pull request 943 was reported slow on Windows 10 (single-digit frames per
   second in one report). Blob memory, the usual cure, needs WDDM 2 and so is out for
   Windows 7. Our Mac-side experience applies: count round trips to the host, measure, and
   compare with the software baseline and the real card.
6. **Signing.** A 64-bit Windows 7 loads only signed kernel drivers. Ours will be
   test-signed: the user turns on test mode (`bcdedit /set testsigning on`) or picks "disable
   driver signature enforcement" at boot. An unpatched Windows 7 SP1 cannot check SHA-256
   signatures (it needs update KB3033929 or KB4474419), so the test signature should be
   SHA-1 or both.
7. **Stability.** A display driver that hangs takes the desktop with it; Windows resets a
   driver that does not answer for two seconds. Test VMs get snapshots before every install.

## What Vista and XP would add

- **Vista (WDDM 1.0).** Nearly the same kernel driver, minus a few 1.1 additions. But
  Vista's DWM is Direct3D 9Ex only, so Aero there needs the full Direct3D 9 driver part.
  Vista follows once that part is solid; it is not a way in.
- **XP (the older XPDM model).** A different kind of driver altogether: a video miniport
  and a display driver that both live in the kernel, no compositor and so no Aero. What it
  can gain is OpenGL (a client driver in user mode, reached through the display driver) and,
  much harder, Direct3D 8 and 9, whose driver interface sits in the kernel on XP and would
  have to hand its work to Mesa in user mode. Nothing permissive exists for that; the
  nearest are a Windows 9x driver stack under MIT and an XP display driver whose OpenGL side
  comes from a GPL project. Mesa itself left XP behind with 17.2. XP would be a project of its own, OpenGL first.

## Toolchain, and where it runs

(The detailed check is a separate document, `toolchain.md`.) What the research says:

- **Off Windows is possible.** vmlab-win3d builds a WDDM kernel driver, Mesa's user-mode
  parts and the driver package on Linux: llvm-mingw (clang) as the cross compiler,
  Microsoft's SDK and WDK headers downloaded as NuGet packages at build time, osslsigncode
  for the test signature, a small tool for the catalog file. That fits this project's habit:
  builds on the Linux build host, tools and downloads under `.tools/`, nothing of
  Microsoft's in the repository.
- **Targeting Windows 7.** Microsoft's kits stopped targeting Windows 7 with WDK
  10.0.22621 (Windows 11 22H2); earlier kits still do, and Microsoft keeps the Windows 10
  2004 kit available for that purpose. WDK 7.1.0 is the period kit: it brings
  its own compiler and targets XP to Windows 7.
- **Fallback:** a Windows build VM with WDK 7.1.0 or a newer kit, if the cross build hits a
  wall (kernel import libraries and `displib.lib`, which every WDDM miniport links, are the
  likely place). Installing a kit anywhere needs the owner's word first.

## Licences

Everything written here stays MIT. Sources that may be copied from, each keeping its notice:

| Source | Licence | Use |
|---|---|---|
| virtio-win, and pull request 943's kernel driver | BSD-3-Clause | Base of the kernel driver |
| Mesa, including merge request 24223 and `d3d10umd` | MIT | User-mode parts |
| The Direct3D 9 files in Yttrium's Mesa fork | MIT (per file header) | Candidate base for the Direct3D 9 part; check every file's header |
| virglrenderer | MIT | Host side, unchanged |

Three points that need a decision:

- **Microsoft's driver samples are under MS-PL, not MIT.** MS-PL lets code be used and
  changed, but source copies must stay under MS-PL. So sample code could not sit in an
  all-MIT tree. Proposal: read them, copy nothing.
- **Yttrium's changes to the kernel driver are under MPL-2.0** where marked. Proposal: base
  the kernel driver on virtio-win and pull request 943 only, and read Yttrium's for ideas.
- **Any QEMU or virglrenderer patch**: none is planned. The rule from the Mac side stands:
  the host stays stock.

Tools that only run at build time (a signing tool, a catalog tool, compilers) are not
copied into the repository, whatever their licence.

## Milestones

The reference for "right" is the same Windows 7 on the GeForce 9600 GT (the passthrough VM,
never while a virgl VM draws), and the baseline is Windows 7 on the Standard VGA adapter.
Each milestone ends with a report and, from W3 on, with paired screenshots against both.

| | Goal | Done when |
|---|---|---|
| W0 | Ground: test VM, baseline, toolchain | A Windows 7 SP1 x64 test VM on `virtio-vga-gl` with snapshots; baseline recorded (driver in use, Aero unavailable, scores, screenshots); a "hello" kernel driver and a "hello" DLL built off Windows, test-signed, loaded in the VM |
| W1 | Our kernel driver owns the display (gate 1) | Windows 7 boots to its desktop on a WDDM 1.1 miniport for virtio-gpu: mode changes, pointer, a clean crash screen, sleep refused or survived; drawing still on the CPU; Direct3D 9 part is the stub |
| W2 | Direct3D on the host GPU (gate 2) | A Direct3D 10 test program in the guest draws through Mesa on virgl and the picture is right, on the Proxmox host; the answer to "does 0.10.4 on the host do" is known |
| W3 | Aero (gate 3) | DWM starts and stays up: glass, live thumbnails, Flip 3D, window animations, next to the 9600 GT's pictures; measured against the baseline |
| W4 | Direct3D 9 | The Direct3D 9 part is real: DirectX's own diagnostics pass, a few period programs and games run; Windows' experience index runs to the end |
| W5 | The rest | OpenGL in the guest, 32-bit Windows 7, an installer, the other two hosts (UTM, Windows), performance pass |
| later | Vista, then XP | Separate decisions |

W0 is under way. W1 is the first place the project can fail outright; W3 is the first place
it shows.

## Decisions asked of the owner

1. **Go on to W1?** (Recommended: yes.)
2. **The Proxmox host's virglrenderer** (0.10.4). Recommended: decide after W2's first test.
   If guest-side workarounds are not enough, the choice is a newer library on that host or
   doing W2 and W3 on another host first.
3. **Microsoft's MS-PL samples:** read only, as proposed?
4. **Yttrium's MIT Direct3D 9 files** as a base for our Direct3D 9 part, although they are
   not in upstream Mesa: allowed?
5. **Microsoft headers and import libraries** downloaded at build time into `.tools/` (never
   committed), and, if needed, a WDK on a Windows build VM: allowed?

## Sources

- virtio-win pull request 943: <https://github.com/virtio-win/kvm-guest-drivers-windows/pull/943>
  (its description, its `driver.cpp`, `viogpu_adapter.cpp` and `viogpu3d.inx`, and the
  discussion, including the author's answer on Windows versions, 2023-08-13)
- Mesa merge request 24223 and virglrenderer merge request 1185 (named in that pull
  request); virglrenderer's history for the seven commits and the releases that hold them
- Mesa 24.3.4, `src/gallium/frontends/d3d10umd` (`Adapter.cpp` for the interface versions)
  and `meson.build` (`min-windows-version`)
- Yttrium: <https://github.com/arehnman/yttrium-virtio-gpu> and
  <https://github.com/arehnman/virtio-win-mesa> (branch `yttrium-experimental`)
- vmlab-win3d: <https://github.com/VMLabDev/vmlab-win3d>
- Microsoft: "What's new for Windows 7 display drivers (WDDM 1.1)",
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/display/what-s-new-for-windows-7-display-drivers--wddm-1-1->;
  "Supported and other WDK download versions",
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads>;
  Windows-driver-samples, <https://github.com/microsoft/Windows-driver-samples>
- OSR, "The Windows Driver Kit and Visual Studio 2022",
  <https://www.osr.com/blog/2022/09/21/the-windows-driver-kit-and-visual-studio-2022/>
- mesa-dist-win on Windows 7: <https://github.com/pal1000/mesa-dist-win/discussions/112>
- virtio-win and Windows 7: <https://github.com/virtio-win/virtio-win-pkg-scripts/issues/40>
- XP: <https://github.com/JHRobotics/softgpu/issues/12>, <https://github.com/JHRobotics/vmhal9x>,
  <https://github.com/f1nalspace/vmdispxp>
- The test host: Proxmox VE 8.4 with QEMU 9.2.0 and virglrenderer 0.10.4; Windows 7, Vista
  and XP images and a GeForce 9600 GT for passthrough are there
