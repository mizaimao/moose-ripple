# Performance stage (after M5)

Started 2026-11-11 ~01:10 by decision: measure first, then fix the biggest costs; the
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

## Host GPU minimum clock (02:40, live test only)

The rig's i915 minimum clock raised from 350 to 1200 MHz (`/sys/class/drm/card0/gt_min_freq_mhz`,
live, back to 350 at the next host reboot; making it permanent is a separate host change).
Same boot and scene, bundle 6554a67:

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
(another job was copying files on the rig at idle priority meanwhile):

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

## Second pass: how evenly frames arrive (2026-11-12)

The numbers above are rates. Stutter is about gaps, so this pass starts with tools for those:

- `scripts/pace-run.sh SCENARIO` gives two views of one run. Viewer: every screen update QEMU's
  VNC server delivers to a client on the VM host itself (`tools/vncgrab/vncrec.c`, timing only),
  against the time of each key press. Guest: every screen update the GL bundle sends to the
  kernel, by dtrace on WindowServer.
- `gl-gears` prints the longest frame of every 2 s, and `-s` runs it on Apple's software
  renderer as a reference.
- The GL bundle logs a present that spends over 60 ms in its three window-server calls
  ("slow present").

VM 210 had been booted with `ripple_qe=2` since M5, which makes the kext log every surface call
(hundreds of lines a second). It now runs with `ripple_qe=1`, as an installed system does. The
frame meter numbers did not change with it (Dashboard 26-55 / 15-32, Exposé 27-28 / 9-15
screens per second, peak / mean, two runs).

### Baseline (VM 210, 1280x800, one Finder window, no other VNC viewer)

Viewer side, ours against Apple's software renderer in the same VM (two to three runs each):

| | ours | software |
|---|---|---|
| Dashboard: key to screen | 45-110 ms | 70-145 ms |
| Dashboard: frames/s in the animation, longest gap | 29-30, 42-48 ms | 30-31, 41-90 ms |
| Exposé: key to screen | 40-130 ms | 95-340 ms |
| Exposé: frames/s, longest gap | 22-31, 48-210 ms | 19-21, 98-126 ms |
| Menus: click or move to screen | 25-170 ms (first click 100-560) | 40-135 ms (first click 125-340) |
| Window drag: frames/s, longest gap | 29-31, 37-240 ms | 32, 37-58 ms |

QEMU's VNC server sends a frame every 30 ms at most, so 30 frames a second is the ceiling for
any viewer, and a viewer on the host gets them evenly. The guest side is faster than that:
45-100 screen updates a second (Dashboard 52-63, Exposé 40-88, window drag 70-75, gl-gears
45-55), each taking 2-3 ms in the kernel call. So for the desktop effects the driver is no
longer what limits smoothness; the viewing path is. (The "100-300 updates a second" in an
earlier note was WindowServer's update requests, not screen updates.)

### The stall: about 170 ms, every few seconds

What does stand out in the guest is a stall of 140-200 ms, a few times every 10 s while
something animates. Counted with gl-gears over 60 s (two-second windows with a frame over
100 ms, of 29):

| WindowServer composites with | gl-gears renders with | windows with a stall |
|---|---|---|
| Apple's software compositor | Apple's software renderer | 5 |
| moose-ripple | Apple's software renderer | 16 |
| moose-ripple | moose-ripple | 21-24 |

What it is, by measurement:
- QEMU is not slow. Its own trace (`virtio_gpu_*` trace events, switched on over QMP for 20 s)
  shows every command answered within a fraction of a millisecond, also the one before each
  stall; then no command arrives from the guest for 155-180 ms.
- In the guest, the thread waiting for that answer does not continue. Timed inside the kext's
  wait (temporary instrumentation, not committed): either its sleep with a 1 ms deadline
  returns 140-190 ms late, or it wakes at once and then waits that long for the command lock,
  held by a thread that is itself not being run. The CPUs are mostly idle meanwhile.
- So a thread that has become runnable is sometimes not run for about 170 ms. That is below
  the driver, and it shows with Apple's renderer and compositor too (the kext's 2D path is
  still there), only less often: fewer commands, fewer chances.

Ruled out, each by a run with only that changed:
- The kext's 20 ms polling fallback: with 1 ms polling the count is the same (21 of 29).
- The kext's per-call logging (`ripple_qe=2` against `1`) and the GL bundle's log folder.
- QEMU's synchronous handling of the notify write (`-global virtio-vga-gl.ioeventfd=on`: 13 of
  29 and 14 of 19).
- Idle CPUs: boot flag `idlehalt=0` (21 of 29), a 1 kHz timer tick on every CPU (24 of 29),
  four busy loops (7 of 29, stalls of 147 ms).
- The host GPU's runtime suspend (it stays active), new shaders or objects (WindowServer
  creates none in the steady state), disk writes by QEMU (64 KB in 20 s).

Open: what delays the runnable thread. Next checks: the same 60 s `gl-gears -s` count on a VM
without the kext, with one virtual CPU, and on the other hosts.

### Where a frame's time goes with one GL window animating

Per composited frame WindowServer makes 5 SUBMIT, 4 WAIT, 1 DISPLAY, 3 SURFACE_INFO and 2
surface-shape kernel calls: about 10 ms in all (SUBMIT 0.9 ms, WAIT 0.8 ms, DISPLAY 2.3 ms on
average), half of its wall time. gl-gears itself spends about 40% of its time waiting in two
window-server calls (`CGSLockWindowBits` and `CGSFlushSurfaceWithOptions`). Fewer waits and
submits per frame, and app windows kept on the GPU, are the driver's next steps.

### Fix: waiting for the GPU without a round trip when nothing was sent (2026-11-12)

Where the per-frame calls came from (dtrace on WindowServer, call sites): four of the five
submits and all four waits were `glTestObjectAPPLE`. Before WindowServer lets an app touch a
window's memory (`CGSLockWindowBits`), it tests the window's textures four times, and our
`glTestObjectAPPLE`, like the fence and finish calls of GL_APPLE_fence and
GL_APPLE_flush_render, flushed with a Mesa fence and waited on it: a submit even when the
buffer was empty, then a sync, two round trips to the host each time.

Now the flush sends only what is queued, and the wait is for the newest fence number the
kernel has given this process, which the winsys remembers as signalled once it has been waited
for. Repeated tests with nothing new cost no kernel call.

Same scene, VM 210, `ripple_qe=1`:

| | before | after |
|---|---|---|
| Kernel calls per composited frame (one GL window animating): submit / wait / display | 5 / 4 / 1 | 1 / 1 (no host round trip) / 1 |
| Time in them per frame | about 10 ms | about 5.4 ms |
| gl-gears in a window, 60 s | 44-52 frames/s | 58.5 frames/s |
| ...two-second windows with a frame over 100 ms | 21-24 of 29 | 3 of 29 |
| The same with Apple's software renderer drawing the window | 16 of 29, 53 frames/s | 1 of 29, 58.8 frames/s |
| Core Image filters, time per frame (24 filters) | 2-90 ms | 1.6-13.8 ms (median 2.9; software 2.0) |
| Dashboard, screens per second, peak / mean (three runs) | 26-55 / 15-32 | 29-36 / 11-16 |
| Exposé, screens per second, peak / mean | 27-28 / 9-15 | 22-25 / 10-11 |

Pictures unchanged: the same one Core Image filter (CIEdges, 3.9%) and the same six Photo
Booth effects differ from software rendering as before; desktop, Exposé, Dashboard and
gl-gears checked by eye.

So the stall above goes with the sync round trips (a fenced empty submit and the wait for its
reply): with one per frame instead of four it is nearly gone, also for a window drawn by
Apple's software renderer. It is specific to the KVM host: the same 60 s count under UTM on
Apple silicon (emulated CPU, no KVM) shows no stall, with either renderer (issue 22). What
remains on the rig is about one stall in 20 s, in the one fenced command a frame still sends
(the display path's).

### After the fix: what is left, by measurement (2026-11-12)

Guest side, screen updates per second (`scripts/pace-run.sh`, VM 210): Dashboard 70, Exposé
74, menus 49, window drag 80, window resize 78, scrolling a long Finder list 70-127 (one
update per wheel notch), gl-gears 57. All well above what a VNC viewer can get.

What a viewer gets (`PACE_ENCODING=tight` makes the recorder ask for Tight encoding, as a web
console does; before that it measured Raw, the best case):

| | Raw, viewer on the host | Tight, as a web console asks |
|---|---|---|
| Dashboard | 29 frames/s, 114 MB/s | 15 frames/s, gaps of 61 ms, 15 MB/s |
| Exposé | 24-30 frames/s, 91 MB/s | 16 frames/s, gaps of 61 ms, 15 MB/s |
| Window drag | 31 frames/s, 42 MB/s | 28 frames/s, 6.5 MB/s |
| gl-gears (400x400) | 29 frames/s, 4.6 MB/s | 30 frames/s, 0.4 MB/s |

So through a web console a whole-screen effect arrives at about 15 frames a second and needs
15 MB/s of lossless data, before the console's own relay and the browser: that, not the
driver, is what limits how smooth the desktop looks there. It is QEMU's VNC server (30 ms
timer, Tight encoder); a display on the host GPU, or a viewer with a cheaper encoding on a
fast link, is the way around it.

An app's GL window by size (gl-gears `-w`, frames/s, and the bundle's time per present):
400x400 59 (8.8 ms), 800x600 50 (17.5 ms), 1280x720 42-57 (21.6 ms). The cost that grows
with the size is the copy path: the frame is read back from the host, copied twice in the
app and uploaded again by WindowServer. Keeping it on the GPU is the remaining driver step
for large windows (issue 10).

Also seen: for a window that reaches the Dock, WindowServer redraws the Dock's mirror in a
pass of its own, and each pass shows the screen: two or three screen updates (each a whole
screen read back by QEMU) and submits per app frame instead of one.

## App GL windows shown by the kernel (2026-11-12, f1e00d8)

From this commit every app's GL window is shown the way a hardware driver does it
(docs/re/ioaccelerator-interface.md, steps 1-9): the frame stays on the GPU, the kernel
copies it to the screen, and WindowServer draws the window from a texture of its surface only
while something is drawn over it. `RIPPLE_GL_DIRECT=0` or the file
`/Library/Logs/RippleGL/no-direct` brings the old path back for a comparison. VM 210,
1280x800, host GPU minimum clock at 1200 MHz again after a host restart.

gl-gears by window size, frames/s and the bundle's time per present, both paths on the same
day:

| | old path (read back) | kernel shows the frame |
|---|---|---|
| 400x400 | 58.7, 11.7 ms | 58.7, 4.5 ms |
| 800x600 | 48.1, 18.1 ms | 57.2, 4.7 ms |
| 1280x720 | 37.6, 24.8 ms | 55-58, 3.7-5.6 ms |

The cost no longer grows with the window. One start of the 1280x720 window in about thirty
stayed on the path through WindowServer (54 frames/s, 15.5 ms): that window ends a few pixels
above the Dock.

Real apps, default path against the new one: the table in ioaccelerator-interface.md, step 8
(Angry Birds Rio 32-39 to 58 frames/s; Flurry at its 30 frames/s cap with WindowServer's share
of a CPU going from 32% to 0%). Quartz Composer's own readout for Arabesque: 60 frames/s, as
before.

The numbers that had to hold:

| | before (earlier sections) | now |
|---|---|---|
| gl-gears in a window, 60 s | 58.5 frames/s | 57.0-57.6 (old path the same day: 56.9-57.9) |
| ...two-second windows with a frame over 100 ms | 3 of 29 | 5-8 of 29 (old path the same day: 5-9) |
| Screen updates per second, guest side: Dashboard | 52-70 | 50-59 |
| ...Exposé | 40-88 | 79 |
| ...menus | 49 | 32-56 |
| ...window drag | 70-80 | 60-84 |
| ...window resize | 78 | 67-74 |
| ...scrolling a long list | 70-127 | 39-107 |
| Each screen update, median | 2-3 ms | 1.9-3.0 ms |
| Viewer on the host (Raw): Dashboard, Exposé, drag, gl-gears | 29, 24-30, 31, 29 frames/s | 30, 29, 29, 30 |
| Frame meter, Dashboard, screens/s peak / mean | 29-36 / 11-16 | 23.5-35 / 9.6-14.8 |
| Frame meter, Exposé | 22-25 / 10-11 | 20-25 / 10.3-14.1 |

Two to three runs each; the guest-side rates swing by a third from run to run (scrolling gave
39 and 107 on the same build), so the rows overlap with the earlier ones and none of them
shows a slowdown that repeats. The desktop scenes do not use the new path at all (no app GL
window in them). The 170 ms stall (issue 22) was more frequent on this day on both paths.

Checks around the change: without Quartz Extreme work (`ripple_qe` off) a program on our
renderer falls back to the old path by itself; the off switches work; paired captures against
Apple's software rendering in the same VM (desktop and window shadow, a GL window, Exposé,
Dashboard, a covered GL window, a minimised one, a movie in QuickTime Player) show nothing
worse; and with the test switch the same build's path was checked by eye in UTM on Apple
silicon (no KVM): moving and covered gl-gears windows and a screen saver draw right.

## A game as the test case: Real Racing 2 on three hosts (2026-11-13)

The same scripted run on each host: start the game, wait for its menu, click into a quick
race on the first track (`tools/guest-input` clicks from inside the guest), then read the 5 s
windows of the bundle's `stats:` log lines.

| Host | Menu | Race start |
|---|---|---|
| Proxmox, KVM, Intel UHD 630 | 55-56 frames/s | 52-58 frames/s, the game at 15-40% of a CPU |
| UTM on an M1 Max, x86 emulation, one host thread for all four CPUs | 52-53 | 17-38 |
| the same with UTM's "Force Multicore" on | 58 | 17-34 (no real difference: see the aligned runs further down) |
| Windows 11, WSL2 with KVM, RTX 4090 through Direct3D 12 | 15 | not reached |

What each host is short of:

- **Proxmox:** nothing much. A profile of the game's own CPU time (dtrace `profile` probe, by
  program counter): 62% game, 26% driver, 12% system libraries. Its render thread spent 48% of
  its time waiting: 13% in the submit (the kernel waits for the host to run the commands), 19%
  in the present (the same, for the copies to the screen) and 16% in the bundle's own wait for
  the next refresh.
- **UTM:** the emulated CPU. With the VM's four CPUs on one host thread (UTM's default for an
  x86 guest on ARM) the scripted race start makes 17-38 frames/s. "Force Multicore" in the VM's System
  settings gives each CPU its own host thread, with no trouble in a 25 minute run, and with
  no gain either once the runs were lined up window by window (below: 29.7 against 28.2
  frames/s, the menu after 170 s against 175 s).
  (An earlier version of this note compared it with about 12 frames/s seen the day before;
  that was another moment of a longer race on an older build, not a like-for-like run.) UTM's documentation says QEMU
  does not guarantee correct emulation in this mode (x86 promises a stricter memory order than
  ARM gives). It is in the install guide as an option, not as the setting.
- **Windows 11 (WSL2):** the host's own CPU work. The game's menu settles at 15 frames/s
  (57 on Proxmox) while the game uses 7% of a guest CPU. On the host, QEMU's main thread, the
  one that turns our commands into Direct3D 12 calls through Mesa's d3d12 driver, is busy 76%
  of the time, nearly all of it in its own code (3.8 s of 5 s in user mode, 0.07 s in the
  kernel): about 50 ms of host CPU for each frame of that menu. QEMU's memory stayed flat
  (2.5 GB) over the run, and gl-gears, with a handful of draws a frame, runs at 55 frames/s
  there, so the cost is per draw or per upload, not per frame shown. What the thread does in
  those 50 ms needs a profile on the host (Mesa 25.2.8, virglrenderer 1.0.0, QEMU 8.2.2).
  The profile is further down ("Windows: the host converted the game's vertices on its CPU"):
  with patch 0012 the menu runs at 59 frames/s there.

Two savings in the driver's CPU time, from the profiles (worth about a tenth of the frame rate
where the CPU is the limit, 28 to 31 frames/s in UTM's race start):

- The list of buffers a command buffer uses was looked up through a 512-slot hash that
  collides all the time in a game with thousands of buffers; each collision walked the list
  (14% of the driver's time, now 5%). Each buffer now remembers where it was listed last.
- Mesa's `simple_mtx_init` went through `call_once` (no futexes on 10.6: a `pthread_once` and
  two `pthread_setspecific` calls), and virgl makes a mutex for every buffer transfer, about
  90 a frame in the game. Patch 0009 makes the mutex directly.

### Not waiting for the host: works, but off for now (`ripple_async=1`)

A submit used to return when the host had run its commands, and a present when the host had
copied the frame: the program and the host took turns. With the boot flag `ripple_async=1`
both are queued and the program goes on (two command buffers per connection, used in turn; a
present waits only for the previous present's copies, so a program is never more than one
frame ahead). On Proxmox the game's render thread then waits 17% of the time instead of 48%
(1% in the submit, 6% in the present, 8% for the next refresh), and its menu runs at 59.4-59.8
frames/s instead of 55-56.

It is not the default because of the stall this document already describes (a thread that
should run is not run for about 0.19 s). With the flag on it shows up several times a second
instead of once every few seconds: gl-gears, which draws from a run loop timer, drops from 58
to 26-45 frames/s with a 190 ms frame in every 2 s window, and Exposé from about 10 screens/s
to 5. Samples show the program waiting in its run loop for the timer and WindowServer idle,
so it is the wake-up that is late, not our code holding anything. The waiting in the old path
(a kernel sleep with a 20 ms deadline and an interrupt, several times a frame) apparently kept
the CPUs awake often enough to hide it. That stall is fixed now (next section); with the fix
the flag no longer costs anything measured: gl-gears 58.8 frames/s, and 58.6 instead of 54.5 for a
window WindowServer draws, Exposé and Dashboard in their usual ranges. It stays a flag until
more programs have run with it.

### The stall, found: an interrupt storm on a mirrored line (2026-11-13)

`tools/timer-late` made it measurable without any drawing of its own: threads that sleep 1 ms
at a time, and how late each sleep ends. Idle desktop: never more than 1 ms late. With gl-gears
running: one thread at a time 190-210 ms late, a few times in 20 s, the others on time. The
CPUs agree on the time (`tools/tsc-skew`), and Apple's CPU power management kext is not
involved (same stalls with it moved away).

KVM's own trace events on the host (`/sys/kernel/tracing/events/kvm`, nothing to install)
showed what the first guest CPU does in those 200 ms: it takes about 10000 interrupts on one
vector, and each time the guest only reads the status of four USB controllers and the disk
controller and acknowledges. The cause, step by step:

- In QEMU's Q35 machine a PCI device's interrupt line arrives at two inputs of the interrupt
  controller: its own (21 for the GPU) and one of the old PC interrupts (10 or 11), because the
  firmware leaves that old routing on. 10.6 uses input 21 for our device, and input 10 for the
  USB and disk controllers in most boots (in some boots it gives those 16 and 17 instead, and
  input 10 is off: then there is no stall, which is why some runs were clean).
- Our interrupt handler was the usual IOKit pair: a filter that says "mine", then an action on
  a work loop thread. For a level-triggered line IOKit masks the line in between.
- If the device raised its line again in that gap (the next command finishing), input 21 was
  masked but input 10 was not: the USB and disk handlers ran, found nothing, acknowledged, and
  the interrupt came straight back, since only our handler can quiet the device and it was
  waiting behind the very CPU that was busy with this.
- After 10000 rounds KVM pauses such an interrupt for 10 ms ("delayed EOI injection", its
  brake for interrupt storms). In that pause the work loop ran and everything went on. 10000
  rounds of about 20 microseconds are the 0.2 s.

The fix is in the kext's interrupt filter: it tells the device not to interrupt until the
action has emptied the queues (the virtio "no interrupt" flag), reads the status register once
more for a raise that slipped in, schedules the action itself (`signalInterrupt`) and returns
false, so IOKit never masks our line. Measured in boots with input 10 active, same test (4
sleepers, gl-gears):

| | line 10 raised longer than 100 ms | KVM's storm brake | sleeps over 20 ms late | gl-gears |
|---|---|---|---|---|
| before, 18 s | 2 (201 ms) | 2 | 0-5 in a run | 58.7 frames/s, a 190 ms frame every few seconds |
| after, 40 s | 0 (longest 4.4 ms) | 0 | 0 | 58.7 frames/s, longest frame 17-19 ms |

Not specific to our device: any driver on that machine type whose handler waits with its line
masked can be stormed the same way through the mirrored input.

### Queued submits with real apps (2026-11-13, after the stall fix)

The same launches with `ripple_async=1` and without, same boot conditions (interrupt input 10
active), on the Proxmox host:

| | waiting for the host (default) | queued (`ripple_async=1`) |
|---|---|---|
| gl-gears 800x600 | 58.7 frames/s | 58.8 |
| gl-gears 1280x720 (WindowServer draws it: just above the Dock) | 54.5 | 58.6 |
| Real Racing 2, menu | 55-56 | 58.4-59.7 |
| Real Racing 2, race | 52-58 at the start | 55.6 mean over 5 minutes (38-60) |
| Angry Birds Rio | 59.6-59.8 | 59.6-60.0 |
| Quartz Composer, Arabesque, viewer behind the editor window | 23-36 | 35-38 |
| Flurry screen saver | 30 (its cap) | 30 |
| Exposé / Dashboard, screens per second | 8-13 / 11-15 | 9-14 / 11-16 |

Looked at with it on: each of these apps' pictures, gl-gears moving, covered by a floating
window, minimised and restored, and under the Dock, Dashboard with widgets, a movie in
QuickTime Player. Five minutes of the race: no error from the host's renderer, none from the
kext. Nothing got slower, and what was held up by waiting got faster; where an app already
runs at the display's 60 frames/s there is nothing to gain on this host. Still to do before it
becomes the default: the same on the UTM and Windows hosts, where the host's share of a frame
is larger.

### Where the driver's time per draw goes, and two things that do not help (2026-11-13)

Real Racing 2's race on the Proxmox host, counted with dtrace over 5 s: 96 draws a frame.
There is no single hot spot (the largest function is 7% of the driver's time). What stands
out is uploading: of all draws, 80% upload the constant vertex attributes again (the state
tracker's "current values"), 62% upload vertex arrays the game keeps in its own memory, and
24% upload index arrays from its memory; each upload maps a piece of an upload buffer anew
(44 maps a frame) and unmaps it before the draw. The host sees little of it, the transfers
are merged into about two a frame. Trimming this is worth a few percent of the frame rate
where the guest's CPU is the limit, not more.

Mesa's threaded mode (glthread), which would move the driver's work off the program's drawing
thread, is not available: it needs a driver whose buffer maps are safe from a second thread
(`PIPE_CAP_MAP_UNSYNCHRONIZED_THREAD_SAFE`) and that can draw while buffers are mapped
(`PIPE_CAP_ALLOW_MAPPED_BUFFERS_DURING_EXECUTION`). virgl has neither, `_mesa_glthread_init`
returns at once; only iris, radeonsi, zink and asahi have them.

Core Image's single filters at 480x300, re-measured after the stall fix: 1.6-12.8 ms a frame
against 1.0-5.5 ms for Core Image's software renderer, as before; the picture comparison is
unchanged (23 of 24 the same).

### Two per-draw trims, and the UTM runs lined up (2026-11-13 evening)

Two Mesa patches against what the game does on every draw:

- 0010: the constant vertex attributes ("current values") are put together first and uploaded
  only if they differ from what was uploaded last; otherwise the draw uses that buffer again.
- 0011: virgl does not send a sampler state binding that is the one already bound. The state
  tracker binds the samplers again whenever a texture changes, mostly the same ones.

Counted on the Proxmox host over 10 s of the same race scene (388 draws a frame): buffer maps
47 560 before, 12 516 after; sampler bindings sent 62 114 before, 12 115 after. The frame rate
there does not move (43-49 frames/s in that scene: the guest is waiting for the host, not
short of CPU).

Under UTM the guest's CPU is the limit, and the same scripted race shows it. Each run: the 5 s
windows from the start of the race, lined up (an earlier comparison in these notes took
windows from different moments of the race, which made Force Multicore look like a gain):

| Run (UTM, M1 Max) | frames/s in the first eight windows | mean |
|---|---|---|
| before the trims, Force Multicore on | 16.8 22.4 17.4 20.4 22.6 28.2 29.8 30.2 | 23.5 |
| with the trims, Force Multicore on | 17.8 28.6 21.6 26.4 26.0 30.2 45.2 41.8 | 29.7 |
| the same, interrupt handling from before the stall fix | 17.0 23.6 18.0 22.4 24.1 28.0 47.2 45.8 | 28.3 |
| with the trims, Force Multicore off | 19.0 26.2 23.6 23.8 25.8 29.2 39.2 38.6 | 28.2 |
| the same with `ripple_async=1` | 19.4 29.0 23.4 26.2 26.2 31.0 51.8 50.4 | 32.2 |

So: the trims are worth about a quarter there; the stall fix costs nothing; Force Multicore
makes no real difference (the game's menu after 170 s with it, 175 s without); queued submits
add about a seventh. Two runs of the same setup differed by a few percent.

### Windows: the host converted the game's vertices on its CPU (2026-11-14)

A profile of QEMU on the Windows host while the game's menu ran at 15 frames/s (`perf record
-e cpu-clock -g` inside WSL2 for 10 s; function names from Ubuntu's debug symbol server, as
the packages are stripped). QEMU's main thread had 75% of the samples:

| Share of QEMU's samples | Where |
|---|---|
| 51% | `u_vbuf_translate_begin`: `util_format_r16g16_sscaled_unpack_rgba_float` and `util_format_r16g16b16_sscaled_unpack_rgba_float` |
| 11% | `u_vbuf_get_minmax_index_mapped` |
| 7% | NVIDIA's driver, made to flush by the buffer maps of the same path |
| 18% | the guest's own CPUs |

All of the first three sit under `vrend_draw_vbo`. The game gives positions and texture
coordinates as 16-bit integers that the GPU is to read as floats without normalising
(`GL_SHORT`). Direct3D 12 has no vertex format for that, so Mesa's D3D12 driver leaves it to
Mesa's fallback: at every draw it looks for the lowest and highest index, then converts that
range of vertices on the CPU, reading them out of memory mapped from the GPU. Intel's Linux
driver and ANGLE on Metal take the format as it is, so only this host was slow.

Patch 0012 moves the conversion into the guest: when the host's renderer name says Direct3D
12, our virgl driver reports these formats as missing and Mesa in the guest writes floats
before the data is sent (most of the game's vertex data is client arrays, copied for each
draw anyway). `RIPPLE_FLOAT_VERTICES=0` or `=1` in a program's environment overrides the
choice. The patch also lets a read of a buffer the host has not written skip the submit and
the wait: the guest's copy is the current one, and the conversion reads vertex and index
buffers at every draw.

Same boot and build, the switch off and on (scripted: 120 s to the menu, two clicks into a
quick race, then the 5 s windows from the start of the race):

| Windows 11, WSL2, RTX 4090 | Menu | Presents in the first eight 5 s windows of the race | Mean |
|---|---|---|---|
| `RIPPLE_FLOAT_VERTICES=0` | 15.0 frames/s | 29 28 57 36 34 41 58 120 | 9.9 frames/s |
| default (conversion on) | 59.4 | 215 295 296 293 291 298 299 299 | 57.2 |

QEMU's main thread went from 76% busy at the menu to 15% in the race, and the conversion
functions are gone from its profile. The picture is the same (frames dumped with `swapdump`).

These runs had no window: over ssh WSL has no desktop (its display server crashes in a loop
there), so QEMU ran with `SDL_VIDEODRIVER=offscreen`, which keeps the GPU path and drops only
the window. The menu made the same 15 frames/s with the window the day before, so the window
was not the limit then; what it costs at 60 frames/s is not measured yet.

On the Proxmox host the default stays off (Intel has the format). Forced on there, the same
race draws correctly and is a little slower (first eight windows 47.1 against 47.6 frames/s,
the next eight 43.8 against 51.1), which is why the conversion is tied to the host's renderer.

Queued submits (`ripple_async=1`) on the same host, same script after a restart: no gain to
see, as the race is at the 60 frames/s limit either way. Presents per 5 s in race windows 2
to 8: 299 280 301 279 279 290 290 with it, 295 296 293 291 298 299 299 without. A second
run without it dipped to 275-285 later in the race, so the few percent between them are
within what two runs differ by. No errors in the guest's kernel log. It stays opt-in: it
helps where the guest's CPU is the limit (UTM), and nowhere else so far.

### The frame meter counted about twice the passes (2026-11-14)

After the fix for GL errors left set in WindowServer (`m6-notes.md`, "GL errors nobody asked
about"), the frame meter's frames/s reads lower while the work is the same. Same bundle, the
old errors switched on and off (`cs-error`), `scripts/frame-meter-run.sh expose dashboard`
with dtrace counting WindowServer's `glFlush` calls and our screen updates over both runs:

| | errors as before | now |
|---|---|---|
| passes (`glFlush`) and screen updates | 392 and 392 | 378 and 378 |
| Exposé frames/s, peak / mean (three runs) | 137-179 / 56-78 | 56-75 / 33-37 |
| Dashboard frames/s, peak / mean (three runs) | 71-185 / 32-75 | 35-114 / 23-36 |
| Exposé screens/s, mean | 8.8-10.3 | 8.6-14.6 |
| Dashboard screens/s, mean | 11.9-18.6 | 13.6-16.8 |

So `CGSGetPerformanceData`'s frame count rose with the dropped programs and not only with the
passes (how it counts was not looked into). The frames/s figures above this point were taken with the
errors and are not comparable with later ones; screens/s, the pacing run and counts of
`glFlush` are. Pacing before and after is in `m6-notes.md`.

### Video: the Welcome movie on both hosts (2026-11-14)

Reported in testing: with UTM's acceleration off the Welcome movie plays choppy. The same
movie with our driver, on both hosts. Setup Assistant's `intro.mov` (H.264, 1008x630, 800
frames in 33.3 s: 24 frames/s, no sound track) in QuickTime Player, screen 1280x800, measured
over 20 s of playback (frame meter, `top`, our bundle's present counts, dtrace on the screen
updates):

| | UTM (emulated CPU), our driver | same VM, Apple's software drawing | Proxmox (KVM), our driver, two runs |
|---|---|---|---|
| frames QuickTime Player presents per second | 23.8-24.1 | not counted | 23.6-25.0 |
| frame meter, screens/s, mean | 22.9 | 2.4 | 23.9-24.8 |
| update requests/s, mean | 49 | 11 | 51-53 |
| CPU, QuickTime Player (percent of one core) | 90-114 | 141-164 | 41-48 |
| CPU, WindowServer | 30-33 | 40-41 | 12-15 |
| CPU, kernel_task | 9-15 | 55-57 | 6-8 |

With the driver the movie runs at its own 24 frames/s on both hosts. In UTM no gap between
screen updates was over 50 ms (953 updates in 20 s), and a frame read back from the display
shows the right picture. The software column is the same VM booted without `ripple_gl=1
ripple_qe=1`: about a tenth of the pixels reach the screen, at more CPU. So the macOS host
(ANGLE on Metal) does not behave differently from Mesa on Linux here; what differs is the
price of the emulated CPU (QuickTime Player about twice the CPU time, mostly decoding). One
run each in UTM.
