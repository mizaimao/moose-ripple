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

9600 GT (VM 211, Apple's NVIDIA driver, 1920x1080, two runs): Exposé 40-47 / 21-25, Dashboard
30-97 / 21-31, menus 19-22 / 11-12. It composites full screens at most 60 times a second, so
its frame counts are lower while its pixel rate is higher: screens/s peak 17 (Exposé) and 24
(Dashboard) of 2.07 Mpixels, i.e. about 35 and 50 Mpixels/s, against ours 10-15 screens/s of
1.02 Mpixels (10-15 Mpixels/s) and software's 9-19 (9-19 Mpixels/s). Frame counts aren't
comparable across these three; what the eye sees is smoothness at the display, which for ours
also goes through QEMU's display path.

## Client-storage uploads only when the memory changed (01:50)

dtrace during Dashboard: 5663 client-storage refreshes for 136 frames (about 40 per frame),
each a TRANSFER round trip through QEMU, plus CPU format conversion (the largest on-CPU item in
WindowServer). The emulation now keeps a copy of each texture's memory as last uploaded and
uploads only when it differs (memcmp, row by row; at most 128 MB of copies).

| Frame meter, peak / mean | Exposé | Dashboard | menus |
|---|---|---|---|
| ours before | 108-131 / 44-47 | 35-81 / 21-34 | 19 / 12-14 |
| ours now (two runs) | 138-146 / 52-73 | 66-85 / 37-40 | 15-19 / 10-12 |
| software | 110 / 55 | 133 / 58 | 22 / 14 |

Dashboard's pixel rate is now above software's (screens/s peak 19-22 against 18.8), its
frame count still lower. gl-gears 53-54 frames/s and animating on screen (pixels change
between captures), so changes still get through.

## Where a Dashboard frame's time goes (02:00-02:20)

Over the whole run nothing is saturated (`scripts/perf-run.sh dashboard`): QEMU's main thread
16%, host GPU render busy 7% (RC6 92%), WindowServer on CPU about 5% (dtrace profile; most
kernel samples are idle). Per animated frame, WindowServer's compositing thread (dtrace on its
IOKit calls between two presents): about 2.3 ms on the CPU, 1-2 ms in SUBMITs, about 7.4 ms
in each DISPLAY that reaches the kext (the host reads the whole screen out), and the rest
waiting in its server loop for the next update.

Correction: the per-selector timings earlier in this section's work counted every
`IOConnectCallMethod` with selector 9 as DISPLAY, but WindowServer's IOAccelerator surface
calls (`IOAccelSetSurfaceFramebufferShape`) use selector 9 too, so about two thirds of those
calls were cheap surface calls. Timings by our own functions instead (`ripple_cgl_device_display`,
`ripple_submit_cmd`): 184 display requests in 7 s, 694 ms in them.

**Tried and dropped: pipelined flush.** The kext queued RESOURCE_FLUSH and returned, with at
most one flush in flight. Worse: SUBMIT time went from about 0.2 s to 1 s per 7 s (max 197
ms) and fence WAITs from 1 ms to 248 ms. With the guest no longer waiting at the flush, Mesa
waits on fences when it reuses buffers, and QEMU only notices a retired fence at the next
guest notification or its 10 ms poll. Reverted (not committed).

**Kept: skip repeated displays (6554a67).** WindowServer flushes the screen (display), then
switches to another surface, which presented the screen again with nothing new drawn: 91 of
184 display requests. The winsys counts submits and uploads; a display of the buffer already
shown with the same count is skipped.

Same boot conditions, one Finder window, frame meter peak / mean (frames/s; screens/s):

| | Dashboard frames/s | Dashboard screens/s | Exposé screens/s |
|---|---|---|---|
| before (three runs) | 36-71 / 22-33 | 13.6-17.8 / 7.3-8.1 | 12-15 / 4.1-8.5 |
| skip repeated displays (three runs) | 65-76 / 29-39 | 23-24 / 8.6-12.8 | 14-19 / 5.5-8.0 |
| Apple's software renderer (two runs) | 90-92 / 38-42 | 18.7-20 / 7.4-7.9 | 10-11 / 3.6-5.3 |

gl-gears in a window: 40-45 frames/s with the change, 39-47 with the bundle before it (A/B on
the same scene; the 53-54 noted at 01:50 was with a different scene). Animates (18 of 90
points on the red gear's teeth differ between two captures 0.5 s apart).

## Paired captures after the performance steps (02:05-02:18)

Same scene on both (fresh boot, one Finder window at the same place, the same input script):
desktop with the menu bar, File menu open, Exposé, Dashboard. Ours (kext 89f1356, bundle
6554a67) and Apple's software renderer on VM 210 (`img/perf-pair-qe-*.png`,
`img/perf-pair-sw-*.png`). Nothing looks worse than software: identical layout, translucent
menu bar and menus with the blurred backdrop on both; in Exposé our scaled windows are
smoother (software's scaled text is jagged). Moving File -> Edit -> View: each menu right.

## What's left

- Each real display costs about 7.4 ms on the host: QEMU (display `egl-headless` for the
  Proxmox console) reads the whole 1280x800 screen back after the frame's GPU work, with the
  GPU mostly in RC6 at a low clock. That is QEMU's display path, the same for any guest
  driver. Raising the host GPU's minimum clock might shorten it; that is a host change
  (the owner's call), not tried.
- Dashboard composites fewer, larger frames than software (more pixels per second, fewer
  frames); WindowServer spends the rest of each frame waiting for its next update.
- CPU colour conversion on uploads: not worth it now (WindowServer's CPU is about 5%).

## Host GPU minimum clock (02:40, the owner's decision: live test only)

The rig's i915 minimum clock raised from 350 to 1200 MHz (`/sys/class/drm/card0/gt_min_freq_mhz`,
live, back to 350 at the next host reboot; making it permanent is a separate host change for
The owner). Same boot and scene, bundle 6554a67:

| | 350 MHz (before) | 1200 MHz |
|---|---|---|
| real screen flush (Dashboard, dtrace on `ripple_cgl_device_display`) | 7.2-7.3 ms | 2.5-2.6 ms |
| Dashboard frames/s peak / mean | 48-64 / 25-26 | 64-67 / 31-35 |
| Dashboard screens/s peak / mean | 21-23.8 / 8.2-9.5 | 24.7-30.5 / 11-15.3 |
| Exposé screens/s peak / mean | 18.6-21 / 9.5-10.2 | 18-27 / 9.3-15.2 |
| host GPU render busy, RC6 | 8%, 90% | 3%, 93% |

The GPU still sleeps in RC6 most of the time; when it wakes it now runs at full clock, so
QEMU's read-out of the screen finishes about three times sooner. Kept live.

## One display update per frame (2026-11-11 11:20, with the widget flicker fix 4292cff)

The display was updated at every flush of WindowServer's context, three to four times a frame
during Dashboard effects (see m6-notes, "a widget being added flickered"). Now once, at the
frame's end. Same scene, 1200 MHz, frame meter peak / mean, two Dashboard and five Exposé runs
(files were being copied on the rig at idle priority meanwhile):

| | before (bundle 6554a67) | now (4292cff) |
|---|---|---|
| Dashboard frames/s | 64-67 / 31-35 | 71-74 / 37-44 |
| Dashboard screens/s | 24.7-30.5 / 11-15.3 | 29-31 / 13.4-17.1 |
| Exposé screens/s | 18-27 / 9.3-15.2 | 21-28 / 8.6-13.4 |
| gl-gears in a window, frames/s | 40-45 | 45-52 |

## After the audit and glitch fixes (2026-11-11 13:45, d9e4bd2)

Same scene and method; nothing slower:

| | before (bundle 6554a67) | now (d9e4bd2) |
|---|---|---|
| Dashboard frames/s | 64-67 / 31-35 | 80 / 35-38 |
| Dashboard screens/s | 24.7-30.5 / 11-15.3 | 29-30 / 13.6-14.2 |
| Exposé screens/s | 18-27 / 9.3-15.2 | 22-24 / 7.8-11.2 |
| gl-gears in a window, frames/s | 40-45 | 40-50 |

For the second performance pass: a GL window's present spends about 14 ms of its 20 ms in
three window-server calls (`CGSFlushSurfaceWithOptions` 8 ms, `CGSLockWindowBits` 6-7 ms),
on top of the read-back. Showing app surfaces through the kernel surface instead of a private
window would remove both.
