# HANDOFF: Paravirtual GPU acceleration for Mac OS X 10.6 (Snow Leopard) guests

Working name: **snow-virgl** (rename freely)
Owner: MooseMan (human). Implementer: Claude Code agent.
Status: greenfield. Nothing exists yet but this document.

---

## 1. Goal

Give a Mac OS X 10.6.8 guest running in QEMU real GPU acceleration, meaning working **Quartz Extreme (QE)** and **Core Image (CI)**. Do it by writing a guest driver for QEMU's **virtio-gpu + virglrenderer** paravirtual GPU, so the guest's OpenGL calls run on the host's modern GPU.

Why: no VM product (VMware, Parallels, VirtualBox) has ever offered QE/CI to OS X guests. Without it, Snow Leopard is barely usable: many apps refuse to launch or draw blank areas (iMovie, Front Row, Core Animation-heavy UI, OpenGL games). The only alternative is keeping era-accurate GPUs or Macs alive forever. This project replaces that hardware dependency with software.

Success means a stock 10.6.8 install in QEMU where:
- `system_profiler SPDisplaysDataType` shows QE and CI as supported (or the equivalent check works; 10.6 may not show them, see M5).
- Exposé, Dashboard ripple and Core Animation run smoothly.
- Core Image filters work (Quartz Composer, Preview, Photo Booth).
- iMovie '09/'11 and a couple of period OpenGL games run with hardware rendering.

## 2. Non-goals (for now)

- PowerPC / Rosetta apps, OpenCL, hardware video decode.
- 10.5 or 10.7+ (possible later; design so they aren't blocked).
- Metal (doesn't exist on 10.6).
- Multi-monitor (single head first).
- Performance on Apple Silicon hosts. A 10.6 guest there runs under full x86 CPU emulation. The dev host is **x86_64 Linux + KVM**.
- Modifying QEMU or virglrenderer. The host side is treated as fixed. If a host change looks necessary, stop and write it up for the owner.

## 3. Architecture

```
 Guest: Mac OS X 10.6.8 (i386/x86_64)
 ┌──────────────────────────────────────────────────────────┐
 │ Apps / WindowServer / Core Image / Core Animation        │
 │        │ OpenGL / CGL                                    │
 │ OpenGL.framework  ──loads──►  SnowVirglGLDriver.bundle   │  (userspace, NEW)
 │                                 └─ Mesa Gallium virgl    │
 │                                    driver + macOS winsys │
 │                                        │ IOKit user client│
 ├────────────────────────────────────────┼─────────────────┤
 │ SnowVirglGPU.kext  (NEW, kernel)        ▼                 │
 │  - virtio-pci transport, virtqueues (control + cursor)   │
 │  - IOFramebuffer subclass (2D scanout, modes, cursor)    │
 │  - IOAccelerator-compatible surfaces (for WindowServer)  │
 │  - 3D: capsets, ctx create, resources, SUBMIT_3D, fences │
 └────────────────────────────────────────┬─────────────────┘
                                          │ virtio-gpu protocol
 Host: Linux x86_64, QEMU (KVM) ──────────▼───────────────────
   -device virtio-vga-gl  +  virglrenderer  →  host OpenGL  →  host GPU
```

There are three new components:
1. **SnowVirglGPU.kext**: Kernel driver. Must be a universal i386 + x86_64 build, because the 10.6 kernel boots 32-bit by default on most configurations and 64-bit with `arch=x86_64`.
2. **SnowVirglGLDriver.bundle**: Userspace OpenGL renderer plugin, loaded by OpenGL.framework from `/System/Library/Extensions/`. Universal i386 + x86_64.
3. **Tooling**: Build, deploy and test scripts, plus reverse-engineering notes.

## 4. What exists and what is unknown

### Public and documented (low risk)
- **virtio-gpu protocol**: the virtio spec, the GPU device section. Commands needed: `GET_DISPLAY_INFO`, `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`, `SET_SCANOUT`, `TRANSFER_TO_HOST_2D`, `RESOURCE_FLUSH`, `UPDATE_CURSOR`/`MOVE_CURSOR`, `GET_CAPSET_INFO`/`GET_CAPSET`, `CTX_CREATE`/`CTX_ATTACH_RESOURCE`, `RESOURCE_CREATE_3D`, `TRANSFER_TO/FROM_HOST_3D`, `SUBMIT_3D`. Reference implementations: the Linux `drivers/gpu/drm/virtio/` driver and QEMU's `hw/display/virtio-gpu*.c`.
- **virgl guest-side reference**: Mesa's Gallium `virgl` driver (MIT). On Linux it sits on top of DRM; we write a macOS "winsys" that talks to our kext instead.
- **IOFramebuffer**: Publicly documented IOKit family, with headers in the 10.6 SDK.
- **virtio PCI IDs**: vendor `0x1AF4`, modern virtio-gpu device `0x1050`. virtio-vga also exposes a VGA class code. *(Verify against QEMU source before relying on it.)*

### Private and undocumented (the real reverse-engineering risk)
- **OpenGL.framework renderer plugin interface**: the ABI between OpenGL.framework/libGL and renderer bundles (e.g. the software renderer `GLRendererFloat.bundle`, `ATIRadeonX2000GLDriver.bundle`, `GeForceGLDriver.bundle`). Exported entry points, dispatch tables and context and drawable lifecycle. **This is the go/no-go item.**
- **IOAccelerator family user-client protocol**: how GL bundles and WindowServer allocate and share accelerated surfaces with the kernel driver. QE depends on it.
- **What WindowServer and Core Image check** to decide whether QE/CI are available (renderer flags, required GL extensions such as ARB fragment program and GLSL, VRAM reporting, and so on).

### Prior art to study first
- **VMsvga2** (SourceForge, `vmsvga2`): An old community VMware SVGA driver for OS X guests, including 10.6. Reportedly fakes enough accelerator interface that apps believe hardware acceleration exists. Read its source for the IOAccelerator and surface work.
- **The open-source IOFramebuffer kext for VMware/QEMU OS X guests**, mentioned in the MattKC forum thread below. Find it and evaluate it as a base for M2.
- **MattKC forum thread "VirGL for old versions of Mac OS X?"**: the idea has been discussed but never built. It also warns that kexts can depend on a specific kernel build. For us that's fine, because 10.6.8 is frozen.
- **softGPU** (Win9x paravirtual GPU project): useful as a pattern for paravirtual GPU support on a legacy OS. Reportedly has virgl ambitions.
- **Darling** (macOS compatibility layer on Linux): may have reverse-engineered notes on OpenGL.framework internals. Check, and don't assume.

## 5. Environment

### Host
- x86_64 Linux with KVM, a reasonably modern GPU and Mesa or proprietary drivers.
- QEMU built with virglrenderer and OpenGL display support. Display device `-device virtio-vga-gl`, display `-display gtk,gl=on` or `sdl,gl=on`.
- Typical guest settings: `-machine q35` (or `pc` if 10.6 objects; agent to determine), a Penryn-class `-cpu`, 2–4 vCPUs, 4 GB RAM, AHCI disk, an e1000-family NIC that 10.6 has a driver for, and `isa-applesmc` (the OSK string is **supplied by the owner** and must not be committed).
- **Bootloader**: to be determined (Chameleon/Clover vs OpenCore). Pick whichever boots 10.6.8 most reliably in QEMU and document why.

### Guest
- Mac OS X 10.6.8, install media supplied by the owner. **Licensing is the owner's call.** Do not download Apple software from unofficial sources, and do not commit Apple binaries, SDKs or install media to the repo.
- Enable Remote Login (SSH) so builds, deploys, kext loads and log collection are scripted.
- Boot args for development: `-v debug=0x144 keepsyms=1`, plus `arch=i386` or `arch=x86_64` to test both kernels.
- Take a VM snapshot before every kext load experiment. Script it.

### Debugging
- **First choice:** QEMU's gdbstub (`-s -S`) with the 10.6.8 kernel symbols. Check whether a Kernel Debug Kit for 10.6.8 is obtainable through the owner's Apple Developer account; ask the owner.
- **Fallback:** Apple two-machine kdp over the emulated NIC (verify NIC support for kdp polled mode).
- **Logs:** `/var/log/kernel.log` and `system.log` inside the guest, plus panic reports. Pull them over SSH after every test.

### Toolchain
- **Reference build:** Xcode 3.2.6 with the 10.6 SDK, inside a guest VM. Slow but authoritative.
- **Preferred fast path:** cross-compile from the host with modern clang targeting `-mmacosx-version-min=10.6`, using the 10.6 SDK the owner supplies via osxcross or a similar setup. Kexts need kernel-compatible flags (no red zone and so on); copy them from Xcode 3.2.6's kext build settings.
- 10.6 has **no libc++**. Write C (preferred for the kext and the Mesa parts) or link libstdc++ statically.
- **Mesa version:** virgl landed in Mesa around 11.x (verify). Choose the oldest Mesa that has a reasonably complete virgl driver and still builds for 10.6 with your toolchain. Record the choice and why in `docs/decisions/`.

## 6. Milestones

Each milestone has acceptance criteria. Stop and report to the owner at each milestone boundary.

### M0: Environment and loop
- 10.6.8 boots in QEMU/KVM on the Linux host with `virtio-vga-gl` attached. It will run on the VGA fallback or stdvga at first.
- Scripts: `scripts/build.sh`, `scripts/deploy.sh` (scp to guest, set ownership `root:wheel`, `kextutil`/`kextload`), `scripts/logs.sh`, `scripts/snapshot.sh`.
- A hello-world kext loads and unloads on both the i386 and x86_64 kernels. A hello-world dylib runs.
- Kernel debugging works with symbols (breakpoint in the hello kext).
- **Accept when** one command builds, deploys, loads and pulls logs in under a minute.

### M1: Reverse-engineering spike (go/no-go)
- Inventory the OpenGL renderer plugin interface:
  - exports, imports and Objective-C/C symbols of `GLRendererFloat.bundle` and one hardware GL driver bundle
  - how OpenGL.framework discovers and loads renderer bundles (Info.plist keys, IOKit matching, renderer IDs)
  - the dispatch-table layout and the context, drawable and surface lifecycle
- Inventory the IOAccelerator user-client selectors used by a hardware GL bundle and by WindowServer (disassemble the bundle and the corresponding kext; trace with dtrace if useful, since dtrace exists on 10.6).
- Write `docs/re/gl-plugin-interface.md` and `docs/re/ioaccelerator-interface.md`.
- **Accept when** there's a written estimate: Is the GL plugin ABI small enough to implement? Can WindowServer be satisfied without a full IOAccelerator reimplementation? Give a recommendation: go, go with reduced scope, or stop.

### M2: virtio-gpu kext, 2D only
- PCI match, virtio transport (modern virtio 1.0 PCI), control and cursor queues, interrupts (MSI if 10.6 cooperates, otherwise legacy).
- IOFramebuffer subclass: modes from `GET_DISPLAY_INFO`, arbitrary resolutions including the host window size, 32-bit color, hardware cursor via the cursor queue, damage tracking with `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`.
- **Accept when** 10.6.8 runs at native host resolution (for example 2560×1600), resolution changes in System Preferences work, the hardware cursor works, and it runs 24 hours stable on both kernels. This is useful on its own; tag a release.

### M3: 3D transport
- Capset query, `CTX_CREATE`, 3D resource create and destroy, `ATTACH_BACKING`, `SUBMIT_3D`, fences and completion interrupts.
- An IOKit user client exposing these to userspace, with a minimal, versioned API documented in `docs/kext-uapi.md`.
- **Accept when** a guest command-line test tool creates a context, submits a virgl clear command, reads the result back and verifies the pixels.

### M4: GL renderer bundle
- Port Mesa's virgl Gallium driver with a macOS winsys on top of the M3 user client.
- Implement the GL plugin interface from M1 well enough that OpenGL.framework enumerates and loads the renderer.
- **Accept when** a CGL/NSOpenGLView test app (spinning gears or similar) renders through the virgl path. The `GL_RENDERER` string identifies our driver. It works in both 32-bit and 64-bit processes. OpenGL 2.1 is reported.

### M5: Quartz Extreme
- Implement whatever IOAccelerator surface support WindowServer needs for GPU compositing.
- **Accept when** WindowServer composites with the GPU: Exposé and Dashboard ripple work, there's no cursor tearing, and apps that require QE launch. Document how QE status was verified, since 10.6's System Profiler may not show it.

### M6: Core Image and real apps
- Expose the extensions and renderer capabilities Core Image requires.
- **Accept when** CI filters run on the GPU (Quartz Composer, Photo Booth effects), and iMovie and Front Row launch and work. Record performance numbers for 2–3 period OpenGL games against the software renderer.

## 7. Repo layout (suggested)

```
/kext/            SnowVirglGPU.kext sources (C/C++ subset allowed by IOKit Embedded C++)
/gldriver/        SnowVirglGLDriver.bundle: plugin glue + macOS winsys
/third_party/mesa/  vendored Mesa subset (virgl + gallium aux), pinned version
/tools/           guest-side test tools (virgl clear test, CGL gears, capability dumper)
/scripts/         build / deploy / logs / snapshot / qemu launch
/docs/re/         reverse-engineering notes (see rules below)
/docs/decisions/  short ADRs: bootloader, Mesa version, toolchain, etc.
/docs/kext-uapi.md
HANDOFF.md        this file
```

## 8. Rules for the agent

1. **Never invent private ABI details.** Every struct layout, selector number, vtable offset or function signature for Apple private interfaces must be backed by evidence in `docs/re/`: binary name, build/version, SHA-256, and address or symbol. If you can't verify it, mark it `UNVERIFIED` and test it before depending on it. LLM recollections of 2009-era Apple internals are unreliable.
2. **Snapshot before every kernel experiment.** Assume panics.
3. **Small, reviewable commits.** One concern per commit. Keep the build green on both architectures.
4. **Keep a lab notebook** in `docs/re/notebook.md`: dated entries for each experiment with hypothesis, what was done, result and next step. This is how the owner follows progress asynchronously.
5. **Don't touch QEMU or virglrenderer.** If you think host changes are required, write it up and stop.
6. **No Apple binaries, SDKs, keys (including the SMC OSK) or install media in the repo.** Reference them by path and version only.
7. **Licensing:** Mesa and virglrenderer are MIT. Check VMsvga2's license before copying any of its code; reading it is fine. Project license: to be decided by the owner (suggest MIT).
8. **Ask the owner before:** buying anything, choosing the project license, publishing or pushing anywhere public, or changing scope or milestones.
9. At the end of each milestone, write a short report: what works, what doesn't, risks found, and a recommended next step.

## 9. Known risks

| Risk | Impact | Mitigation |
|---|---|---|
| GL plugin ABI too large or too weird to implement | Project stalls at M4 | M1 spike decides early; fallback is a 2D-only release (M2) |
| WindowServer needs deep IOAccelerator semantics | QE (M5) stalls | Study VMsvga2; dtrace WindowServer on real hardware if the owner can provide a period Mac |
| Core Image needs features virgl GL can't provide on the host | CI (M6) partial | Check host virgl capsets early (GLSL version, extensions) |
| 32-bit vs 64-bit kernel and userspace doubles the work | Slower progress | Universal builds from day one; CI tests both |
| Kernel debugging friction | Slow iteration | Invest in M0 scripts and gdbstub with symbols |
| Old Mesa won't build for 10.6 | M4 delay | Cross-compile with modern clang; strip Mesa to the virgl driver and gallium aux only |

## 10. Open questions for the owner

1. Install media and licensing setup for the 10.6.8 guest: what do you have?
2. Can you get the 10.6.8 Kernel Debug Kit and the 10.6 SDK / Xcode 3.2.6 through your Apple Developer account?
3. Host machine for development: which x86 Linux box, and which GPU?
4. Is a period Mac available for ground-truth tracing (dtrace on real QE/CI hardware)? This would help a lot with M1 and M5.
5. Project license and whether it will be public.

## 11. References

- Linux virtio-gpu DRM driver: `drivers/gpu/drm/virtio/` (kernel source)
- QEMU virtio-gpu docs: https://qemu.readthedocs.io/en/master/system/devices/virtio/virtio-gpu.html
- virglrenderer: https://gitlab.freedesktop.org/virgl/virglrenderer/
- Mesa (Gallium virgl driver): `src/gallium/drivers/virgl/`, `src/gallium/winsys/virgl/`
- KVM Forum 2024, "The many faces of virtio-gpu": https://kvm-forum.qemu.org/2024/The_many_faces_of_virtio-gpu_F4XtKDi.pdf
- VMsvga2: https://sourceforge.net/projects/vmsvga2/
- MattKC forum, "VirGL for old versions of Mac OS X?": https://forum.mattkc.com/viewtopic.php?p=1843
- Apple IOKit Fundamentals and IOFramebuffer headers (10.6 SDK)
