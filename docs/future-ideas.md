# Future ideas

Not scheduled; recorded so they aren't lost. The README's "Ideas for later" lists them in short.

## A second performance pass

Guided by what the real apps and games show. Done on 2026-11-12: apps' OpenGL windows stay on
the GPU (showing a frame takes 4-5 ms at any size, where a 1280x800 window took 20-30 ms), and
a composited frame needs one submit, one wait without a host round trip and one display call
(it was five, four and one). Since 2026-11-26 the host is asked to show the screen at most 66 times
a second (it reads the whole screen back each time): OpenArena's timedemo went from 148 to 192
frames/s on the Proxmox host. With queued submits, the notify from a helper thread and covered
windows that no longer wait (all 2026-11-25) the same timedemo runs at about 280 frames/s with
nothing over its window; the same host's GeForce 9600 GT with Apple's driver does 351. Left:
- A window with something drawn over it (the Dock, another window) still costs a program a
  quarter of its frame rate (about 200 frames/s against 280): WindowServer draws such a window,
  and its drawing shares the host's renderer with the program.
- At about 300 frames/s the host is the pacer on the Intel hosts: QEMU's main thread runs the
  guest's GL commands and waits for the GPU in between. In the guest the driver's own work is
  1.2 ms a frame, spread thin; putting it on a second thread (Mesa's glthread, a trial that is
  not in the tree) gave about 6% there. It may be worth more under emulation, where the
  program's thread is the limit ([re/perf-notes.md](re/perf-notes.md), "a second GL thread").
- In UTM none of the changes of 2026-11-25 is timed yet; they are only checked to work.
- A single Core Image filter is still slower than software rendering (1.2-12 ms against 0.8-4.5
  for a 480x300 picture). Its textures are reused and its submits are queued since 2026-11-25;
  what is left is one trip to the host per pass (Core Image's flush when it unbinds a
  texture), a new program compiled for every render, and the read-back
  ([re/perf-notes.md](re/perf-notes.md), "Core Image after the texture cache").
- Frames reach a VNC viewer unevenly (see [re/perf-notes.md](re/perf-notes.md)). Every open
  VNC viewer (a Proxmox web console, for example) makes QEMU encode each frame again, before the
  next viewer gets it: with one console open, a second viewer got whole-screen frames every
  65-190 ms instead of every 31 ms.

## Watching on a real monitor instead of VNC

The host GPU that runs the guest's OpenGL can also show it: QEMU's `-display gtk,gl=on` or
`sdl,gl=on` puts the VM in an OpenGL window on a monitor attached to that GPU (for an Intel host,
the motherboard's video output), with no VNC encoding in between. A passed-through card such as
the 9600 GT can't do this: it belongs to its own VM. On a Proxmox host this needs a graphical
session or SDL on the console, and a QEMU built with GTK or SDL display support (to be checked
for Proxmox's `pve-qemu-kvm`).

## Native context instead of virgl

virglrenderer's native context mode passes the GPU's own command stream between guest and host
instead of translating OpenGL calls, with much less CPU overhead. It exists for AMD (amdgpu) and
Intel hosts, needs a host kernel from Linux 6.13 on and a newer Mesa (25.x), and the guest would
need a Mesa driver for that GPU family built for 10.6. That trades the project's GPU-neutral
design (decision 0005) for speed on those GPUs, so it would sit next to virgl, not replace it.
The test rig's kernel is pinned to 6.5, so it can't be tried there as is.

## Apple silicon hosts

Snow Leopard emulated by QEMU (x86 on ARM) on a Mac, with virtio-gpu and virgl drawing on the
Mac's GPU, for example through UTM. Our guest files are x86 and run inside the emulated machine
unchanged; the CPU emulation makes the system slow, but drawing would no longer run on that
slow emulated CPU. A later target in decision 0008.

Tested since 2026-11-11 in UTM 5.0.8 on an M1 Max: Quartz Extreme, the translucent menu bar and
gl-gears at about 55 frames/s work; the pointer is invisible there (KI-8). Setup:
[install.md](install.md#utm-on-an-apple-silicon-mac).

## Other Mac OS X versions

10.5, 10.7 and 10.8. The OpenCore boot disk already starts 10.7 to 10.9, but the driver is built
and tested only for 10.6.8: each version changes the OpenGL plugin interface and WindowServer's
expectations, so each needs its own reverse engineering and testing.

## A software backend

Mesa's llvmpipe (a fast, multi-threaded CPU renderer) behind the same OpenGL plugin, for hosts
without a usable GPU. It would give Quartz Extreme and Core Image on the CPU, at a cost in speed.

## Emulate an Intel GMA 950 in QEMU (2026-11-10)

A possible side project, difficulty unknown. Idea: emulate an Intel GMA 950 as a QEMU device,
in the spirit of 86Box's Voodoo emulation, so Apple's own GMA 950 drivers give Quartz Extreme
and Core Image with no Mac-side driver from us.

Facts so far:
- Apple shipped GMA 950 drivers for 10.4 to 10.7, with Quartz Extreme and Core Image.
- In 10.6.8 (10K549) `AppleIntelGMA950.kext` is i386 only, so the guest would need the 32-bit
  kernel; `AppleIntelGMA950GLDriver.bundle` and `AppleIntelGMA950GA.plugin` are i386 + x86_64
  ([re/binaries.md](re/binaries.md)).
- The emulator would still have to run the chip's shader programs (on the host GPU or CPU) and
  match the chip closely enough for Apple's driver.
- It would mean changing QEMU, which this project otherwise treats as fixed (HANDOFF.md
  section 2), so it would be a separate project.
