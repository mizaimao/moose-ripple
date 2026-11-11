# Performance stage (after M5)

Started 2026-11-11 ~01:10 on the owner's decision: measure first, then fix the biggest costs; the
visual parity rule still holds. Tool: `scripts/perf-run.sh SCENARIO` (idle, expose, dashboard,
menu, drag, gears) drives VM 210 through QMP and samples, over the same interval: guest
WindowServer CPU, our bundle's present stats, QEMU's main thread (virglrenderer runs there)
and vCPU threads, the host GPU's render-engine busy time (i915 fdinfo of QEMU's renderD128),
RC6 residency (GPU idle) and the actual GPU clock.

## First measurements, our stack (VM 210, build 9e55017, 1280x800, 2026-11-11 01:15)

| Scenario (about 6 s) | Frames/s while moving | Guest CPU per frame | WindowServer CPU | QEMU main | vCPUs | Host GPU busy | RC6 | GPU clock |
|---|---|---|---|---|---|---|---|---|
| idle | - | - | 0% | 13% | 4% | 0% | 100% | 0 MHz |
| Exposé (F9 x4) | 46 | 3.8 ms | 7% | 16% | 17% | 4% | 94% | 113 MHz |
| Dashboard (F12 x4) | 41 | 4.9 ms | 8% | 16% | 15% | 6% | 91% | 113 MHz |
| menus (File across to Window and back) | 33-48 | 3.9-5.1 ms | 5% | 16% | 12% | 2% | 96% | 84 MHz |
| window drag (40 moves, 10/s) | 29 | 6.5 ms | 4% | 22% | 20% | 3% | 93% | 308 MHz |
| gl-gears running | 25 (gl-gears' own count) | - | 15% | 32% | 31% | 12% | 68% | 350 MHz |

CPU columns are percent of one core; the GPU's top clock is 1200 MHz. Nothing is near its
limit: the GPU idles most of the time at a low clock, QEMU's main thread uses under a third of
a core. So frames wait rather than work: synchronous steps between the guest and the host
(GPU readbacks of app GL windows, fences, the present path) are the suspects. gl-gears was 36
frames/s at the M5 check (2026-11-10); 25 now.

References (Apple's software renderer on 210, the 9600 GT on 211) still to measure.

## Where the time went, and the first fix (01:20-01:30)

dtrace in the guest (off-CPU time of WindowServer's threads during a window drag): the one
wait in our code was `ripple_submit_cmd` (the kext's SUBMIT), about 11 ms per frame. The kext
marked every SUBMIT and TRANSFER with `VIRTIO_GPU_FLAG_FENCE`, and QEMU answers a fenced
command only once the host GPU has finished everything before it: each `glFlush` and each
texture upload waited for the GPU to go idle.

| Kext | gl-gears | Exposé | Dashboard | menus | per present (guest CPU) |
|---|---|---|---|---|---|
| fenced (before) | 25/s | 46/s | 41/s | 48/s | 3.8-6.5 ms |
| transfers unfenced only | 25/s | about the same | | | |
| no fences at all (experiment, unsafe) | 48-55/s | 51-57/s | 85/s | 82/s | 4.3-5.7 ms |
| submits unfenced, one GPU sync per displayed frame (now) | 34-39/s | 43-60/s | 58-63/s | about 58/s | 6.6-11.7 ms |

Now: SUBMIT returns once QEMU has taken the commands; fences are numbered in queue order and
pass when an empty fenced SUBMIT (`VirtioGPUDevice::sync`) returns; `WAIT` with a timeout does
such a sync if needed; `DISPLAY` syncs once before showing a frame (docs/kext-uapi.md). The
remaining gap to the experiment is that per-frame sync; next: make it not block (queue the
fenced empty submit before the scanout flush without waiting for its reply).

## Second fix: the per-frame wait without blocking (01:35)

`DISPLAY` now queues the fenced empty submit (`VirtioGPUDevice::syncAsync`) ahead of the
scanout flush and doesn't wait for its reply: QEMU handles it before the flush (making the
fence pushes the host GPU's queued work out), and the reply passes the fence later in the
interrupt's drain, which also lets resource fences pass without anyone waiting.

| Kext | gl-gears | Exposé | Dashboard | menus | per present (guest CPU) |
|---|---|---|---|---|---|
| fenced (before) | 25/s | 46/s | 41/s | 48/s | 3.8-6.5 ms |
| one blocking sync per frame | 34-39/s | 43-60/s | 58-63/s | about 58/s | 6.6-11.7 ms |
| one non-blocking sync per frame (now) | 43-56/s | 55-75/s | 86/s | 70/s | 4.2-5.6 ms |

Screens checked (desktop, Exposé with a window, menus): nothing new wrong. The menu glitch
reported in testing shows in [this capture](img/perf-menu-glitch-edit.png) while moving
from File to Edit: the Edit menu's area shows the window behind it without the menu items,
and shrunken copies of a menu appear further down. It predates these kext changes.

## Menu glitch fixed (01:37)

Cause (GL trace of a File -> Edit switch): WindowServer blurs a menu's background through a
framebuffer object between `glPushAttrib(GL_VIEWPORT_BIT)` and `glPopAttrib()`, then binds the
screen again. Mesa restored the viewport on the pop, but our window offset keeps the program's
viewport itself and re-applied the blur pass's 193x196 one on the bind: the menu was drawn
squeezed and misplaced (the Edit menu's area showed the window behind it, shrunken copies of
the menu stayed elsewhere on screen). Push/pop now save and restore that copy too
(`ripple_cgl_push_attrib`/`pop_attrib`). Moving across File, Edit, View, Go: every menu right
([Edit menu](img/perf-menu-edit-fixed.png)).

## WindowServer's own frame rate, ours against Apple's software compositor (01:45-01:55)

`tools/frame-meter` reads `CGSGetPerformanceData` (Quartz Debug's frame meter) during
`scripts/frame-meter-run.sh` (6 s each: F9 x4, F12 x4, menu bar by keyboard), same VM, one
Finder window open. Frames composited per second, peak / mean:

| Scenario | Ours (kext 89f1356, two runs) | Software (210 without our driver) |
|---|---|---|
| Exposé | 108-131 / 44-47 | 110 / 55 |
| Dashboard | 35-81 / 21-34 | 133 / 58 |
| menus (keyboard) | 19 / 12-14 | 22 / 14 |

Exposé and menus are on a par with software now; Dashboard (Core Animation widgets, dimming)
composites about half as many frames. Suspect: client-storage textures are uploaded again at
their first use in every frame (the emulation), and Dashboard has many.
