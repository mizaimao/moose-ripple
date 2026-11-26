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

### The README's numbers, measured again (2026-11-14, bundle of c4a87cf, kext 30ab66e)

Same methods as when each number was first taken, on the Proxmox host (1280x800, 4 CPUs,
GPU minimum clock 1200 MHz):

| | in the README | now |
|---|---|---|
| Quartz Composer, Arabesque (its own readout) | 60 frames/s | 60.2 |
| Angry Birds Rio, title screen | 59 frames/s | 59.1-60.0 |
| Flurry (ScreenSaverEngine) | 30 frames/s, 3% of a CPU | 30.0-30.1, 3.2-4.0% |
| Dashboard, screens per second, mean (three runs) | 13.6-14.2 | 14.3, 16.9, 15.9 |
| Exposé, screens per second, mean (three runs) | 7.8-11.2 | 14.7, 8.8, 9.4 |
| gl-gears, 800x600 | 57 frames/s | 58.7-58.8, 4.5 ms per present |
| gl-gears, 1280x720 | 55-58 | 58.7-58.9, 4.2 ms per present |
| the same window partly under the Dock (WindowServer draws it) | about 7 ms more | 11.8 ms against 4.4 |
| Core Image filters like software (`ci-effects filters`) | 23 of 24 | 24 of 24 |
| Photo Booth effects like software (`ci-effects booth`) | 55 of 61 | 57 of 61 |
| key press to the screen through VNC (`pace-run.sh`) | about 0.2 s | 32-167 ms for menus, 46-137 for Exposé and Dashboard |

The README has the new values. What moved and why, as far as known: CIEdges, Blue Print and
Comic Book now match (not looked into which change did it; the viewport fix of the same day
touches contexts without a drawable, which these use). The four effects left (Concert 99.6%
of pixels, Neon 21.5%, Pinch 4.2%, Pointillize 2.4%) differ on the GeForce 9600 GT too. The
0.2 s for a key press was the interrupt storm fixed in 14f325e. The software and 9600 GT
columns were not measured again: nothing of ours runs in them.

The UTM row, same day and build (UTM 5.0.8 on an M1 Max, Force Multicore on, 1280x800): gl-gears
55.6-56.9 frames/s at 400x400 and at 800x600 (README: about 55); Real Racing 2's menu 59.2
(57-59); its race, eight 5 s windows from the start with nobody driving: 33 36 29 30 30 32 30
26 frames/s, mean 30.7 (about 25-30). The README now says 56, 59 and about 30. The Windows
row (gl-gears at about 55) was not measured again: it needs the VM's window on that host.

### UTM: the whole guest stalls when a program starts with the driver on (2026-11-14, open)

Found while looking for the cause of sound drops at the start of a movie (#35). `tools/timer-late`
(four threads that sleep 1 ms at a time, and how late each sleep ends) in the UTM guest (UTM
5.0.8, M1 Max, 4 CPUs, Force Multicore on), no sound device involved:

| | wake-ups over 20 ms late | worst |
|---|---|---|
| idle desktop, 8 s | 0 | 12 ms |
| QuickTime Player starting on the Welcome movie, driver on (6 boots) | 15-24 in each | 186-193 ms |
| the same with `ripple_async=1` (one of those boots) | 24 | 189 ms |
| the same without `ripple_gl=1 ripple_qe=1` (software rendering, one boot) | 0 | 18 ms |
| `afplay` of a system sound, driver on | 8 | 48 ms |
| `afplay`, without the driver flags | 8 | 50 ms |

With the driver on, all four threads are late together four times within one second, 2 to
3 s after `open`: 70, 95, 70 and 190 ms. So the whole guest stands still, not one thread. It
happens at every start of the program, not when the movie is played again in it.

What it is not:
- Not the waiting for the host: the same with queued submits.
- Not the interrupt storm on a mirrored line (above, "The stall, found"): in the three boots
  checked every PCI device was on lines 16 to 23, none on 10 or 11.
- Not the host's command handling as such: in a profile of QEMU over such a start (`sample`,
  about 4800 samples in 9 s) virglrenderer's work on the main thread is 71 samples, about
  130 ms in all.

What the profile shows instead: the main thread waits for QEMU's big lock in about 170 to 200
samples and each of the four CPU threads in 130 to 470, and the USB controller's periodic
work (`ehci_work_bh`) takes 90 to 129. Who holds the lock, or whether the CPUs are stopped
for something else (a flush of translated code, for one), is not found yet. Two separate
things are mixed in the sound problem: a sound through the USB audio device stalls the guest
for 40 to 50 ms at its start and end with or without the driver, and a program that starts
drawing with the driver stalls it for up to 190 ms.

The same start on the Proxmox host (KVM), driver on, two runs: no wake-up over 20 ms late
(worst 17.9 and 5.2 ms). So the stall belongs to the emulated CPU in UTM, not to the driver's
work as such: the next place to look is what QEMU's code translator does when a program loads
the GL bundle (31 MB of code new to it).

### Core Image: textures kept for reuse (2026-11-25)

Where a Core Image render's time goes, counted with dtrace on `ci-effects filters` (480x300,
the Proxmox host, 1280x800, 4 CPUs, GPU minimum clock 1200 MHz). A filter with one pass makes
five kernel calls per render:

- one BUFFER_CREATE and one BUFFER_DESTROY: Core Image attaches a new texture of the same size
  to its buffer for every pass and deletes the old one (`fe_accel_buffer_attach`);
- two SUBMITs: Core Image's own flush when it unbinds the texture
  (`fe_gl_unbind_texture_object`), and the read-back of the result;
- one WAIT, for the read-back.

On average a SUBMIT takes 0.43 ms, a create 0.26, a destroy 0.27 and a WAIT 0.13. The filters
with several passes make 7 to 25 calls per render. These calls are 60 to 90% of a render's
time (ColorInvert: 1.4 of about 1.6 ms; ComicEffect: 8.9 of 13.3 ms with the tracer attached).
The driver's own CPU time is small.

Change: the winsys's buffer cache, which held vertex, index and staging buffers and plain
render targets as Mesa's DRM winsys does, now holds textures too. A released texture waits
up to a second for a request of the same size and format. Limits: 64 MB of textures, the ones
waiting longest destroyed first; a buffer whose handle went to the kernel (a window's frame,
the screen, WindowServer's texture of a surface) is never cached, because the kernel may
still show it. `RIPPLE_GL_TEXCACHE=0` in a program's environment, or the file
`/Library/Logs/RippleGL/no-texcache`, brings the old behaviour back.

One run each on the same boot and build, cache off against on:

| | cache off | cache on | Apple's software renderer |
|---|---|---|---|
| 24 Core Image filters, sum of the times per render | 98.4 ms | 78.6 ms | 51-52 ms |
| ...fastest and slowest filter | 1.4 and 13.2 ms | 1.2 and 12.0 ms | 0.8 and 4.5 ms |
| 61 Photo Booth effects, sum | 83.1 ms | 62.7 ms | not summed |
| BUFFER_CREATE / BUFFER_DESTROY calls in the filter run | 467 / 439 | 373 / 234 | |
| SUBMIT / WAIT calls | 421 / 99 | 421 / 99 | |

The test is short (the 96 renders take about half a second), so 189 of its texture requests
are served from the cache and the rest are Core Image filling its own store of intermediate
images, which it only starts to give back late in the run. A program that renders for longer
reuses more.

Pictures: every filter and every Photo Booth effect compares with software rendering exactly
as with the cache off (25 runs each way for the filters). Desktop, Exposé, Dashboard and a
gl-gears window (59.5 frames/s, longest frame 17 ms) looked at on the same build; no error
from the host's renderer.

Seen on the way, not from this change:

- The first run within two minutes of a boot is three to four times slower on the multi-pass
  filters (ComicEffect 41.8 ms): WindowServer is still drawing at 33 frames/s then. Wait until
  it is idle before timing.
- In about one run in ten the reference picture of CISepiaTone (Apple's software renderer) has
  four wrong pixels in a row at a random place; with the cache off 2 of 25 runs, with it on 4
  of 25. Ours is right in those pixels. `CI_EFFECTS_WHERE=1` makes `ci-effects` print where
  the differing pixels are.
- CIEdges differs from software on 3.9% of the pixels and 55 of the 61 Photo Booth effects
  match (Blue Print 4.3% and Comic Book 3.4% differ next to the four known ones), with the
  cache off and on. The table of 2026-11-14 says 24 of 24 and 57 of 61. Looked for the change
  that did it, one run per step on the same VM: the 0.2.1 release's GL bundle (built that day)
  on today's kext, then that release's bundle and kext together. Both give 23 of 24 and 55 of
  61, so no driver change since then is the cause, and the three values are the ones first
  measured on 2026-11-11. The guest system is unchanged since 2026-11-10 and no debug switch
  was changed around that day. The driver log of the run of 2026-11-14 shows Core Image
  using three 517x323 pbuffers where it uses one now; what made it do so is not found.
  `RIPPLE_FLOAT_PIXELS=1` does not bring it back (CIEdges stays, and a second filter differs).
  Until the run of 2026-11-14 can be repeated, 23 of 24 and 55 of 61 are the numbers that hold.

Left for Core Image: the two SUBMITs per pass, each of which waits for the host.

Queued submits (`ripple_async=1`) on the same two tests, one run on a boot with the flag,
texture cache on:

| | waiting for the host | queued |
|---|---|---|
| 24 Core Image filters, sum | 78.6 ms | 74.1 ms (0.8 to 10.3 a filter) |
| 61 Photo Booth effects, sum | 62.7 ms | 563.6 ms |

The filters gain little, and the Photo Booth compositions (drawn through QCRenderer, with a
`glFinish` after every frame) take about nine times as long: 20 of the 61 need 5 to 15 ms a
frame instead of about 1. Same pictures. So the queued mode has a slow path of its own, in
the wait that follows a queued submit, and it has to be found before the mode can become the
default.

The slow wait, found (2026-11-25). Times of the kernel calls in the Photo Booth test on a
boot with `ripple_async=1`: of 552 WAITs, 259 took 9 to 10 ms and the rest under 3 ms. A WAIT
that has to wait sends an empty fenced command (`VirtioGPUDevice::sync`), and QEMU answers a
fenced command when it next looks at its fences and finds that one retired. It looks right
after handling a batch of commands, and otherwise on a 10 ms timer. With submits that wait
for the host, the GPU has usually finished by the time the fenced command arrives, and the
first look passes it (0.13 ms a WAIT). With queued submits the fenced command arrives right
behind the work, the GPU is still busy at the first look, and the reply comes with the timer.

Fix, in the kext: while it waits for a fenced reply, the waiter sends QEMU an empty command
that nobody waits for, after 0.2 ms and then at doubling intervals up to 2 ms. Handling it
makes QEMU look at its fences again, so the reply comes when the GPU is done instead of on
the timer. One run each on the same VM:

| | old kext | with the prod |
|---|---|---|
| 61 Photo Booth effects, sum, queued submits | 563.6 ms | 39.2 ms |
| ...submits that wait for the host (the default) | 62.7 ms | 47.8 ms (45.1, 50.6 in two more runs) |
| 24 Core Image filters, sum, queued | 74.1 ms | 77.5 ms |
| ...the default | 78.6 ms | 85.3 ms (86.0, 80.1, 78.7, 82.3 in four more runs) |
| gl-gears 400x400, the default | 59.5 frames/s | 59.5; 59.3 with Dashboard open over it |

The filters do not move: their totals spread from 78.7 to 86.0 ms between runs of one boot
and build. Same pictures in every run; desktop, Dashboard and the gl-gears window looked at
on the new kext; no error from the kext or the host's renderer.

This also bounds every other wait for the GPU (`glFinish`, fences, a read-back while the GPU
is busy) at the GPU's own time plus a fraction of a millisecond, where it could be 10 ms.
Queued submits themselves stay behind the flag: this was the one measured reason against
them on the Core Image tests, and the checks on the other hosts are still to do.

Queued submits with the test programs (2026-11-25, the Proxmox host, the kext with the prod):

| | waiting for the host | queued (`ripple_async=1`) |
|---|---|---|
| OpenArena 0.8.8, timedemo of `demo088-test1`, 1024x768 window (3398 frames) | 99.8 and 100.7 frames/s | 102.5 and 102.0 |
| ...frame times, shortest / mean / longest | 7.0 / 10.0 / 52 ms | 7.0 / 9.8 / 52-58 ms |

Command: `openarena.ub +set fs_basepath DIR +set r_fullscreen 0 +set r_mode 6 +set s_initsound 0
+set com_introplayed 1 +timedemo 1 +demo demo088-test1 +set nextdemo quit` (with `+set timedemo
1` in place of `+timedemo 1` the result line is not printed). The picture is right in both
modes. Queued submits are worth about 2% here, so on this host the game is not held up by
waiting for the host. No frame is shorter than 7 ms in either mode. The 1024x768 window
reaches under the Dock at 1280x800, so WindowServer draws it (about 7 ms more a present, see
above); a window clear of the Dock is the next thing to time.

Cinebench R11.5's OpenGL test gives no score with the driver yet: it refuses its own picture
("OpenGL Reference Matching Test Failure", issue 154), with no GL error in the driver's log.

The Dock's share, same boot, default mode, same 1024x768 window: 100.8 frames/s with the Dock
shown (frame times 7.0 / 9.9 / 50 ms), 140.8 with the Dock hidden (4.0 / 7.1 / 50 ms; the
bundle counts 3.0 ms per present). So a window that something is drawn over costs this game
about 3 ms a frame, 40% of its frame rate.

Correction to "Two per-draw trims" (2026-11-13), found 2026-11-25: the second trim (Mesa
patch 0011, sampler-state binds that repeat what is bound are not sent) drew wrong textures.
The host renderer needs that bind after every change of a program's textures
(docs/re/m6-notes.md, Cinebench). The patch now sends it again in that case, so most of the
62,114 to 12,115 saving in sampler binds is gone. On the Proxmox host nothing measured moved
(OpenArena 99.6 against 99.8 frames/s, Core Image and Photo Booth within their spread); the
UTM race numbers of that day were taken with the faulty trim and have to be measured again.

### Covered windows: the surface flush on a thread of its own (2026-11-25)

A window that something is drawn over (the Dock, another window, Exposé) is drawn by
WindowServer, and the driver tells it about each frame with a surface flush. The call returns
when WindowServer has redrawn the window, and the program waited for that at every swap. A
helper thread makes the call now; the program waits only while the flush for its previous
frame is still on its way, so it is never more than one frame ahead. `RIPPLE_GL_FLUSH_THREAD=0`
or the file `/Library/Logs/RippleGL/no-flush-thread` bring the old behaviour back.

Same boot, the Proxmox host, OpenArena's timedemo in a 1024x768 window that reaches under the
Dock:

| | flush on the program's thread | on the helper thread |
|---|---|---|
| OpenArena timedemo, window under the Dock | 100.4 frames/s (7.0 / 10.0 / 54 ms) | 143.8 (4.0 / 7.0 / 48 ms); 141.5 in a second run |
| the same with the Dock hidden (earlier boot) | 140.8 | |
| gl-gears under the Dock with 10 ms of work a frame | 57.1, 59.5, 59.5 frames/s | 59.4, 59.5, 59.5 |
| gl-gears crossed by another window | | 59.5 throughout, longest frame 17 ms |

So a covered window now costs this game nothing. Picture looked at: the game under the Dock
with the Dock drawn over it.

### UTM: what limits the frame rate (2026-11-25, issue 109)

The Mac OS X part of issue 109's measurement: UTM 5 on an M1 Max, the guest with four emulated
CPUs (multi-threaded translation), release 0.2.2 installed, 1280x800. Sampled on the host over
60 s: CPU time of each QEMU thread (`ps -M`, the difference of two readings) and the GPU's
`Device Utilization %` (`ioreg -c IOAccelerator`, every 5 s).

| | guest frames/s | host GPU busy | QEMU, all threads | the four CPU threads | QEMU's main thread |
|---|---|---|---|---|---|
| idle desktop | | 0-1% | 25% of a core | 4-6% each | 7% |
| gl-gears 1280x720 | 56.8 | 15-19% | 55% | 11-13% each | 5% |
| OpenArena timedemo, 1024x768 | 14.9 (141 on the KVM host) | 5-7% | 108% | 23-27% each | 5% |

gl-gears is no load test: it draws on a 60 Hz timer and reaches it. The timedemo is: it runs
as fast as it can, and the four CPU threads together use one core's worth, which is one guest
thread kept busy and moved between the guest's CPUs. The host's side of the renderer (QEMU's
main thread) and the GPU are nearly idle. So of the issue's three candidates it is the first,
the emulated CPU: a frame takes 67 ms of guest time where the KVM host needs 7.

Where those 67 ms go, from a 10 s `sample` of the game in the guest (3485 samples of its main
thread): 30% in the swap, nearly all of that blocked in a kernel call, waiting for the host;
10% in the driver's draw path; the rest in the game's own code (its vertex maths, emulated).
So the third candidate, waiting on round trips, is about a third of a frame here, and it is
the part a driver can change. Release 0.2.2 does not have the fix for the wait on QEMU's
10 ms fence timer (above, "The slow wait, found"), which is the likely cause.

The same run with the drivers of this day in the guest (the kext that prods QEMU while it
waits, the flush thread for covered windows, the texture cache, the corrected sampler binds):

| OpenArena timedemo in UTM | release 0.2.2 | today's drivers |
|---|---|---|
| frames/s | 14.9 | 22.9 (22.2 in the run that was profiled) |
| frame times, shortest / mean / longest | 37 / 66.9 / 240 ms | 18 / 43.7 / 214 ms |
| the game's main thread: share in the swap | 30% | 4% |
| ...in the driver's draw calls | 10% | 16% |
| host GPU busy | 5-7% | 6-11% |
| QEMU, all threads; the four CPU threads | 108%; 23-27% each | 145%; 31-36% each |

The wait in the swap is gone and the game is half as fast again. Its window reaches under the
Dock in both runs, so the flush thread has a part in that next to the kext's shorter wait.
What is left on the game's thread is its own drawing code (three quarters of the samples) and
kernel calls inside the frame (a fifth, in `mach_msg_trap`): the emulated CPU is the limit
now, and queued submits are the next thing to try there.

A trap met on the way: a UTM guest that is restarted while it is still rebuilding its kext
caches hangs in the boot loader at Extensions.mkext. After installing kexts there, wait until
`ps ax | grep [k]extcache` prints nothing. The guest was repaired from a second guest (its disk
attached over QMP as a USB disk, `kextcache -u` on it), as docs/install.md describes for a new
disk.

Queued submits in UTM (2026-11-25, same guest, the drivers of this day, one boot each):
OpenArena's timedemo 22.9 frames/s with submits that wait for the host (frame times 19.0 /
43.6 / 195 ms) and 24.8 with `ripple_async=1` (16.0 / 40.4 / 199 ms), 8% more; no error from
the kext. The picture was not looked at in UTM (a capture inside the guest was not possible
in this session); on the KVM host the queued mode draws the game right.

### Core Image after the texture cache: what a render still costs (2026-11-25)

Traced again on the Proxmox host with the drivers of this day (`ci-effects filters`, per
render, mean of three):

| filter | wall | buffer creates | submits | wait | outside kernel calls |
|---|---|---|---|---|---|
| ColorInvert (one pass) | 1.01 ms | none | 2, 0.69 ms | 0.07 ms | 0.26 ms |
| ZoomBlur | 7.86 ms | 6, 1.50 ms | 6, 2.68 ms | 0.07 ms | 3.6 ms |
| EdgeWork | 7.01 ms | 4, 1.64 ms | 4.7, 2.50 ms | 0.15 ms | 2.7 ms |
| ComicEffect | 11.63 ms | 10, 3.18 ms | 12, 4.09 ms | 0.08 ms | 4.3 ms |
| UnsharpMask | 7.67 ms | 0.7, 0.33 ms | 3, 2.19 ms | 0.15 ms | 5.0 ms |

Three parts of about the same size in the filters with several passes:

- **Submits**, one per pass: Core Image calls `glFlushRenderAPPLE` when it unbinds a texture
  (545 times in the run of 96 renders). Each is a trip to the host.
- **Programs.** Core Image makes a new program object for every render, gives it its text and
  deletes it again (96 `glGenProgramsARB`, 226 `glProgramStringARB`, 96 deletes in 96
  renders), and 188 of the 226 texts are different from every other: values are written into
  the text. So every render compiles one to three ARB programs, in Mesa in the guest (the time
  outside kernel calls) and again as a shader on the host (inside the submits). A cache by
  program text would not help this test.
- **Texture creates** that the cache does not serve in this short run, while Core Image is
  still filling its own store of intermediate pictures.

What could still be done: hold back the flushes of a context that has no window and send
them with its next real submit, keeping their order among the process's contexts. That would
take a filter with ten passes from twelve trips to one or two, about a quarter of its time.
It changes when WindowServer's and Core Image's flushes reach the host, so it needs the full
picture checks, and it does nothing for the compile. Not done; the one-pass filters are
within 0.2 ms of the software renderer, and Photo Booth's effects are faster than it.

Queued submits on the Proxmox host with the drivers of this day (one boot with
`ripple_async=1`, against the runs of the same day without it): Cinebench R11.5's OpenGL test
39.55 frames/s against 33.23 (the same 3,625 pixels off its reference, test successful);
OpenArena's timedemo under the Dock 138.7 against 143.8 and 141.5. No error from the kext.

### Queued submits are the default (2026-11-25)

The last check before the change, Real Racing 2 on the Proxmox host with the drivers of this
day (the scripted quick race, two minutes of it, one boot each):

| | waiting for the host | queued |
|---|---|---|
| race, frames/s (24 windows of 5 s) | 60.1 (60.0-60.2) | 60.1 (60.0-60.2) |
| time per present | 4.39 ms | 3.22 ms |

The race now runs at the display's 60 frames/s in both modes (it was 52-58 on 13 Nov), so the
gain shows only in the time a present takes. The picture is right in both, and neither the
kext nor the host's renderer logged an error.

The kext now queues unless the boot flag `ripple_async=0` is given, and the registry property
`RippleSubmits` shows `queued` or `waiting`. With that kext and no flag, same host: OpenArena's
timedemo 147.5 frames/s, Cinebench's OpenGL test 40.16 frames/s (the same 3,625 pixels off
its reference, test successful); desktop with the menu bar, a window with its shadow, Exposé
and Dashboard looked at. What the mode gave in today's like-for-like runs: Cinebench 33.2 to
39.6 frames/s, OpenArena in UTM 22.9 to 24.8, the 61 Photo Booth effects 47.8 to 39.2 ms,
OpenArena on the Proxmox host about the same (141.5 and 143.8 waiting, 138.7 and 147.5
queued, each on its own boot).

Not done: a long run (hours) in the queued mode, and the Windows host. The long run is due
before the next release.

### Is the Proxmox host at its limit? (2026-11-25)

The two test programs with the kext that queues by default, sampled on the host while they
ran (read-only: the i915 busy time of QEMU's render node, CPU time of QEMU's threads), and
one run of each on the same host's GeForce 9600 GT, passed through to a second 10.6.8 VM
with the same CPU model and count and Apple's own driver:

| | ours (Intel UHD 630 through virgl) | GeForce 9600 GT, Apple's driver |
|---|---|---|
| OpenArena timedemo, 1024x768 window | 147.5 frames/s | 351.1 frames/s |
| ...host GPU busy | 47-51%, at its top clock (1200 MHz) | |
| ...QEMU's main thread (the host's renderer) | 31-40% of a core | |
| ...the guest's CPU threads, sum | 65-82% of a core | |
| Cinebench R11.5 OpenGL test | 40.2 frames/s, window 1280x708 (37.7 in a second run) | 33.0 frames/s, window 1920x967 |
| ...reference check | 99.38%, 3,625 pixels off | 99.41%, 3,442 pixels off |
| ...host GPU busy | 65-88%, about 70% | |
| ...QEMU's main thread | 14-22% | |
| ...the guest's CPU threads, sum | 135-177% | |

- **OpenArena is not at the host's limit.** No part is busy all the time: the GPU half, the
  host's renderer a third, the guest's CPUs three quarters of one core. With the same CPUs the
  game runs 2.4 times as fast on the 9600 GT, so a frame of the game itself needs under 2.9 ms
  and ours takes 6.8 ms. The difference is time in our path on the CPU (the driver in the
  guest, the translation on the host) and waiting between the two, not the Intel GPU.
- **Cinebench is close to the Intel GPU's limit**: about 70% busy, up to 88%. The two frame
  rates are not like for like: the window on the 9600 GT has twice the pixels, because its
  monitor offers no 1280x800 and a switch of our VM to 1920x1080 crashed WindowServer
  (docs/known-issues.md, KI-11). Both pictures differ from Cinebench's reference by nearly the
  same number of pixels.
- Cinebench shows its licence window over the test window in every run on both VMs; the four
  runs on the 9600 GT were 32.94 to 32.99 frames/s.

### Where a frame of OpenArena goes, and the display flush held to the refresh rate (2026-11-26)

The 9600 GT reference runs the timedemo 2.4 times as fast with the same CPUs, so the frame
was profiled (`sample` on the game for 9 s, the kext that queues, the window under the Dock;
other VMs were running on the host, so these are shares, not timings). The game's main thread,
6.9 ms a frame:

| | share | per frame |
|---|---|---|
| the game's own code | 46% | 3.2 ms |
| inside OpenGL calls (the driver's work on the CPU; `glDrawElements` 14%) | 21% | 1.4 ms |
| swap: waiting until the previous frame has been shown | 16% | 1.1 ms |
| swap: handing the frame's commands to the kernel | 11% | 0.7 ms |
| swap: the present call and the rest | 5% | 0.3 ms |

The thread that shows the frames (the flush thread) was inside `CGSFlushSurfaceWithOptions`
95% of the time: showing a frame took as long as drawing one, and the game is never allowed
more than one frame ahead. So the pace was set by showing, not by drawing.

What makes showing slow on this host: every display flush makes QEMU's headless display
(Proxmox uses it for `vga: virtio-gl`) read the whole screen back from the GPU, on the thread
that also runs the guest's OpenGL commands, and the guest asked for a flush at every present,
146 times a second here.

**Change.** The kext asks for a flush at most 66 times a second (boot flag `ripple_flush_hz`,
0 for the old behaviour). A frame that comes sooner is held: the buffer on the host is
current already, and a timer flushes it when the gap is over, so the last frame always
reaches the screen. 66 is just above the 62.5 Hz of the driver's refresh tick, so a program
that draws once per tick is never held.

Same kext, the host to itself, one boot each:

| OpenArena timedemo, window under the Dock | `ripple_flush_hz=0` | default (66) |
|---|---|---|
| frames/s | 147.8 | 192.4 |
| frame times, shortest / mean / longest | 4.0 / 6.8 / 49 ms | 2.0 / 5.2 / 52 ms |
| time per present (the driver's count) | 1.2-2.5 ms | 0.51 ms |
| host GPU busy | 46-50% | 43-48% |
| QEMU's main thread | 31-35% | 33-40% |
| the guest's CPU threads, sum | 63-90% | 91-101% |

With the limit the guest's CPUs are fully used: the game's thread is the limit now, and what
is left to gain is the driver's CPU time per frame (1.4 ms in OpenGL calls, 0.7 ms handing
over the commands). Not slower elsewhere, same boot: gl-gears 59.5 frames/s with a longest
frame of 17 ms, Cinebench's OpenGL test 40.7 frames/s (reference check passes; it presents
fewer than 66 times a second, so nothing is held). Looked at: the game, the desktop, Exposé
and Dashboard in the guest, and through VNC that the host's display shows the last frame
after Dashboard opens and closes. No error from the kext or the host's renderer.

The gain is for hosts whose display reads the screen back (QEMU's headless display). Not
measured in UTM, where the display works differently.

### The hand-off to the kernel, the draws, and covered windows again (2026-11-26)

With the display limit in, the Proxmox host to itself, OpenArena's timedemo (423 draws a
frame, one submit of 73 KB naming 104 buffers), 1024x768 window:

| run | frames/s |
|---|---|
| window under the Dock (as in every run above) | 188.0, 194.0, 198.2 on three boots |
| the Dock hidden, so nothing covers the window | 263.5 |
| window under the Dock, draws skipped (`RIPPLE_GL_SKIP_DRAWS=1`) | 299.2 |

- **A covered window costs a quarter of the frame rate again.** At 140 frames/s it made no
  difference (the display flush set the pace then); at these rates WindowServer drawing every
  frame of the covered window is what the game waits for. Open: let a program that does not
  wait for the refresh hand WindowServer only its newest frame.
- **The draws** cost 2.0 ms of a 5.3 ms frame in all. Of that, 0.8 ms is the driver's work in
  `glDrawElements` on the CPU (1.9 µs a draw: uploading the client arrays 0.19 ms, texture and
  sampler state 0.2 ms, the rest spread thin); the remainder is the host running them.
- **The submit call.** The kext now times its four parts per connection (registry property
  `RippleSubmitStats` on each `RippleGPUClient`, every 512 submits): checking the 104 handles
  46 µs, waiting for a free command buffer 0.1 µs, copying 73 KB 9 µs, and queueing the
  command 298 µs. The last is the write that tells the host to look at the queue: it leaves
  the guest and returns only when QEMU's main thread lets go of its lock, which it holds
  while it runs GL commands. A program pays it twice a frame (submit and present).

Tried: making that write from a kernel helper thread for queued commands (boot flag
`ripple_kick=1`). The queueing step falls to 34 µs. One boot each, same kext:

| | write from the caller | from the helper thread |
|---|---|---|
| window under the Dock | 194.0, 198.2 | 183.9, 186.3, 185.6 |
| the Dock hidden | 263.5 | 290.9, 289.5 |

Faster by 10% where nothing covers the window, slower by 6% where WindowServer draws it, so
it stays off by default until the covered case is understood. QEMU's main thread during the
covered run with the helper, sampled 300 times on the host: idle in its event loop 36%,
waiting for the GPU 26%, running 19%, in other kernel calls 16%. Neither side is busy all the
time; they wait for each other.

For scale: the GeForce 9600 GT reference ran this timedemo at 351 frames/s (its window not
covered, on a 1920x1080 screen).

### A covered window: WindowServer gets the newest frame (2026-11-25, late)

While something is drawn over a program's window, WindowServer draws the window, and the
program's flush thread asks it to at every frame. The program waited whenever the flush for
its previous frame was still on its way, which at these frame rates was most of the time.
Now a program with swap interval 0 does not wait: it leaves word and draws on, and when
WindowServer has finished one redraw the flush thread asks for one more, which shows the
newest frame (the frames are in the surface's texture already). A program that waits for the
refresh keeps the old behaviour. `RIPPLE_GL_FLUSH_WAIT=1` in the environment brings the wait
back for every program.

OpenArena's timedemo, 1024x768 window with the Dock drawn over its lower edge, same boot,
switched by that variable, each host to itself:

| | waiting (as before) | newest frame | the Dock hidden |
|---|---|---|---|
| Proxmox host (QEMU 9.2, virglrenderer 0.10.4) | 190.0, 186.5 | 198.1 | 268.5 |
| second Intel host (QEMU 10.0, virglrenderer 1.1.0), a copy of the same guest | 189.9 | 225.7 | 268.6 |

The two hosts are not to be compared with each other; each row is its own before and after.
On the Proxmox host the covered runs scatter by about 5% from run to run (all of that evening,
several boots: waiting 180.1, 183.7, 186.5, 190.0; newest frame 182.4, 196.9, 198.1, 200.6,
201.1, 202.9), so the gain there is about 6%; a first pair of 180.1 and 201.1 overstated it.
A covered window still costs a quarter to a sixth of the frame rate: WindowServer's drawing
shares the host's renderer with the game. gl-gears, which waits for the refresh, in a covered
window: 59.5 frames/s, longest frame 17 ms, as before. Looked at: the game's picture advancing
under the Dock in the guest, two captures a second apart.

The helper-thread notify (`ripple_kick=1`) again with this change in, the Proxmox host to
itself, one boot each: with nothing over the window 293.5 frames/s against 268.5 (290.9 and
289.5 against 263.5 and 264.3 earlier that evening), so 9 to 10% there; with the Dock over the
window 202.9 and 196.9 against 198.1 and 200.6, no difference left. The loss it showed for
covered windows came from the waiting that is gone now.

**The helper thread is the default** (same evening; `ripple_kick=0` switches it off). The two
checks that were open, one boot each way on the Proxmox host: WindowServer's frame meter over
Exposé and Dashboard, screens per second, mean of two runs each: 1.0 and 15.6 without the
helper, 1.1 and 15.6 with it (no window was open, so Exposé moves little); gl-gears 59.5
frames/s with a longest frame of 17 ms both ways, in a window of its own and in a covered one.
With the kext that has it as its default, no flag: OpenArena's timedemo 273 to 279 frames/s
with the Dock hidden (five runs on two boots; the three runs with the flag earlier gave 290 to
294, and 264 to 269 without the helper), 203.6 with the Dock over the window, and Cinebench's
OpenGL test 45.7 frames/s against 40.2 and 40.7 before (reference check passes, the same 3,625
pixels off). The per-submit queueing step is 12 to 17 µs with it.

### The same drivers in UTM, checked without timing (2026-11-26)

UTM 5 on an M1 Max, other guests running, so no frame rate is quoted as a measurement. With
the kext and bundle of 2026-11-25 (queued submits, the display limit, the helper thread,
covered windows without the wait, the accelerator reference):

- The guest restarts with them (kext caches rebuilt and waited for first), Quartz Extreme on.
- The picture in the queued mode is right: the desktop, and OpenArena's timedemo at two moments
  with the Dock over its window (captures of the guest's own screen).
- Changing the resolution while the system runs works (1280x800 to 1024x768 and back,
  WindowServer keeps running), as on the Proxmox host since the fix for KI-11.
- gl-gears, which waits for the refresh, 56.5 frames/s with a longest frame of 19 ms, as before
  the display limit: nothing is held back at the display's rate.
- No error from the kext. The timedemo ran at 24.5 frames/s next to the other guests (22.9 to
  24.8 alone on 2026-11-25).

### The per-draw CPU cost, and a trial of a second GL thread (2026-11-26)

Looked at first on the kept profile of the timedemo (5,463 samples of the game's thread):
locks 1.3%, thread-local lookups under 1%, so nothing to win there. The 1.2 ms a frame in
OpenGL calls is spread: copying the game's vertex arrays 2%, the bookkeeping of each upload
(map, transfer, unmap) 3%, texture and sampler state per draw 5%, the fixed-function program
lookup 2%, encoding the draw 3%. No single piece can be told from run-to-run scatter.

Tried instead, as a feasibility build that is not in the tree: Mesa's own second GL thread
("glthread"), which hands the GL calls to a worker thread. It needs two things virgl does not
offer: buffer maps that are safe from the application's thread, and buffers that stay mapped
while they are drawn from. Both were given for the trial: the upload buffer is mapped straight
from its guest memory, Mesa's upload function tells the driver how far it has written, and
before each draw the driver puts a transfer command for the new range into the stream. In the
frontend the entries that touch the context directly (about sixty overrides, the swap, the
drawable changes) wait for the worker first.

Result on the second Intel host, OpenArena's timedemo, alternating runs on one boot, another
guest running (so not a clean timing): with the Dock over the window 224 frames/s against 211,
with the Dock hidden 302 and 321 against 282 and 303. About 6%. The picture is right in both
(a first build that drew garbage ran at 370 and is not a result). gl-gears runs with it too.

Why not more: the worker takes the driver's work (it is busy 15% of the time), but the game's
thread keeps the copy of the vertex arrays (0.2 ms a frame), waits for the worker at every
`glClear`, and then waits in the present for the host, which becomes the pacer at 300
frames/s. Not pursued for now: a correct version means auditing every override for state it
reads, for a gain of this size on these hosts. It may be worth more under emulation (UTM),
where the game's thread is the limit and the host has time to spare; not tried there. The two
patch files of the trial are kept outside the tree.

### A covered window asks WindowServer at most 66 times a second (2026-11-26)

After the change above, a program that does not wait for the refresh left word with
WindowServer after every frame, and WindowServer redrew the covered window as often as it
could, about 200 times a second, on the host renderer the program itself draws with. The flush
thread now waits out a gap of 15 ms between two requests (`RIPPLE_GL_FLUSH_HZ` in the
environment, default 66, 0 for no limit); a frame that comes meanwhile takes the place of the
queued one, so WindowServer always gets the newest. A program that waits for the refresh is
not held.

OpenArena's timedemo, 1024x768 window with the Dock over its lower edge, alternating on one
boot, switched by that variable:

| | no limit | 66 a second | for scale: the Dock hidden |
|---|---|---|---|
| second Intel host (another guest was running) | 191.2, 192.9 | 277.6, 275.7 | about 300 |
| Proxmox host, to itself | 198.3, 192.2 | 264.7, 267.5 | 285.2 |

gl-gears, which waits for the refresh, in a covered window: 59.8 frames/s, longest frame
17 ms, as before. Looked at: the game's picture advancing under the Dock, two captures a
second apart.

A covered window now costs about 7% of the frame rate on the Proxmox host, where it cost a
quarter the evening before.

### A two-hour stability run (2026-11-26)

On the Proxmox host, to itself, GPU clock at 1200 MHz, with the kext and bundle of 2026-11-25
(queued submits, the display limit, the helper thread, covered windows without the wait, the
accelerator reference; the redraw limit above came after). `tools/soak/soak.sh` ran detached
in the guest for 122 minutes, 22 rounds of: OpenArena's timedemo in a covered and in an
uncovered window, Cinebench's OpenGL test with its reference check, Real Racing 2 started and
taken into a quick race for two minutes, the two Core Image comparisons, gl-gears in a covered
window, Exposé and Dashboard.

| step, 22 times each | result |
|---|---|
| OpenArena, 1024x768 window under the Dock | 185.8 to 207.4 frames/s, mean 193.5 |
| OpenArena, 800x600 window, nothing over it | 288.4 to 307.1, mean 299.8 |
| Cinebench OpenGL test | 34.9 to 39.2 frames/s; reference check passed every time, the same 3,625 pixels off |
| Real Racing 2, two minutes of a race | 60.1 frames/s every time, no 5 s window under 60.0; menu reached after 65 to 70 s |
| Core Image: 24 filters, 61 Photo Booth effects | 1 and 6 differ from the software renderer, every time |
| gl-gears, covered | 59.4 frames/s or more, longest frame 20 ms |

No drift in any of them from the first round to the last. Health line after each round: no
error from the kext, no crash report, no command over 100 ms; WindowServer 58 to 62 MB, wired
memory 732 to 734 MB. On the host: no message from the renderer in the journal, QEMU's memory
3.4 GB after five minutes and 3.6 GB at most. The guest shut itself down cleanly at the end.

Cinebench is slower inside the run (36 frames/s) than started alone after a boot (46); the
order of the programs was not varied, so this is noted, not explained.

### The changes of 2026-11-25 timed in UTM (2026-11-26)

UTM 5 on an M1 Max, no other guest running (a disk tool was busy on the host, and the level
is lower than the 22.9 to 24.8 frames/s of the day before, so only this table's rows compare
with each other). OpenArena's timedemo, window under the Dock, one run each, the kext changes
switched by boot flag and the bundle changes by environment variable on the same builds:

| display limit and helper thread (kext) | newest frame and redraw limit (bundle) | frames/s |
|---|---|---|
| off | off | 19.8 |
| off | on | 19.6 |
| on | off | 20.0 |
| on | on | 19.9 |
| on | on, the Dock hidden | 20.8 |

No difference in UTM, as expected: the emulated CPU is the limit there, the program presents
far fewer than 66 times a second, and nothing waits for the host. Nothing got slower either.
What would help under emulation is less work on the program's own thread.

### Core Image: the gap to software is in a filter's first two renders (2026-11-26)

`ci-effects filters` prints one time per filter, the mean of renders two to four of a
filter. With `CI_EFFECTS_PASSES=8 CI_EFFECTS_EACH=1` (new) it prints every render, ours and
the software renderer's. Sums over the 24 filters, 480x300, on the second Intel host (other
guests were running; ours / software):

| | first render | second render | each later render |
|---|---|---|---|
| all 24 filters | 353 / 84 ms | 119 / 54 ms | 32.6 / 34.8 ms |
| one colour filter (ColorInvert) | 6.3 / 1.2 | 0.9 / 0.8 | 0.82 / 0.77 |
| GaussianBlur | 26.9 / 6.1 | 6.2 / 3.3 | 1.72 / 1.12 |
| ComicEffect | 43.8 / 7.9 | 18.7 / 4.3 | 3.80 / 3.62 |
| UnsharpMask | 17.3 / 3.4 | 18.2 / 3.3 | 1.16 / 1.46 |

From the third render on, ours is as fast as the software renderer over the 24 filters (the
distortions and UnsharpMask faster, the blurs slower). The 1.2-12 ms against 0.8-4.5 quoted
before came from the second render, which the tool's mean includes.

This corrects the section "Core Image after the texture cache" above: Core Image does not
compile programs in every render. The call trace of a run shows them only in a filter's
first render and, for most filters with several passes, in its second, where Core Image
builds new, joined programs; later renders set none. 188 of the 226 texts are different, so
a cache by text has nothing to find in this run.

Where the first two renders go (the test sampled while it does only those, 1 ms steps, three
runs): of about 470 ms in ours, 142 ms are in `glProgramStringARB` (0.63 ms a text: parsing
15, Mesa's NIR passes and the translation to TGSI about 95, the TGSI text for the host 21),
about 100 ms in creating textures (a call to the kernel that waits for the host, which is
busy compiling what was just sent), 42 ms in the read-back.

**Change: an ARB program's shader is made at the first draw with it.** Mesa built a shader
for every program when its text was set. Core Image sets three programs for a kernel and
draws with two at most: of the 226 texts, 81 never see a draw (27 of 77 vertex programs, 54
of 149 fragment programs), and each cost a compile in the guest and another on the host.
Patch `0013` leaves the shader of an ARB program to its first draw (GLSL programs keep theirs
at the link); `RIPPLE_GL_EAGER_PROGRAMS=1` in the environment brings the old behaviour back.
Same host, one boot, alternating by that variable, sums over the 24 filters:

| | first render | second render | each later render |
|---|---|---|---|
| shader when the text is set (before) | 352.7, 344.6 ms | 119.0, 117.1 ms | 32.6, 32.4 ms |
| shader at the first draw (after) | 314.6, 314.6 | 94.5, 94.0 | 31.5, 32.0 |
| the same pair later, the host quiet, creates queued (below): before | 324.6, 325.0 | 102.9, 104.3 | 27.6, 27.4 |
| after | 268.2, 278.0 | 81.5, 82.1 | 27.1, 27.3 |

The first showing of a filter (two renders) goes from 467 to 409 ms over the 24, 12% less,
and from 428 to 355 ms (17% less) in the second pair.
The picture comparison is unchanged (1 of 24 filters and 6 of 61 Photo Booth effects differ,
the same ones). Looked at after a restart with the new bundle: the desktop with the menu bar,
a window with its shadow, Exposé, Dashboard. OpenArena's timedemo 284.6 frames/s with the
change and 280.5 with the variable set, gl-gears 59.7 frames/s; no error from the kext.

Left in the first two renders: the compile itself for the programs that are drawn with (Mesa
runs its full set of NIR passes on programs of twenty instructions), and the texture creates.

### Core Image's flush per pass costs nothing any more (2026-11-26)

Core Image calls `glFlushRenderAPPLE` when it unbinds a texture, once or twice a pass, and
each is a submit. The idea on the list was to hold these back until the context's next real
submit. Its upper bound, tried with a switch that drops the call altogether (a trial build,
not in the tree; the read-back at the end of a render still submits everything): the 24
filters, alternating on one boot on the second Intel host, each later render in sum 34.4 and
30.7 ms with the flushes, 33.6 and 33.5 ms without; first and second renders the same within
their scatter; the picture comparison unchanged. With queued submits a flush is a call into
the kernel that returns at once, so there is nothing left to gain here. Not built.

### A buffer is created without waiting for the host (2026-11-26)

Timers in the kext's buffer create (`RippleCreateStats` on the connection: calls, bytes, time
getting the memory, in the host's create, in the host's attach) for the filter test's first
two renders, 288 buffers: 4 ms for the memory, 127 ms waiting for the host's answer to the
create and 25 ms for the attach. The create itself is nothing to the host; the answer comes
when the host has worked through what was queued before it, the programs it was just sent
among them. With queued submits these were the waits left in a render besides its read-back.

The kext now puts a 3D client's create, attach and context attach in the queue and returns
(`ripple_create=0` at boot: wait for each as before). The queue keeps the order, so a later
command finds the buffer. The attach's list of memory runs goes into the request itself, up
to 254 runs; a longer list still waits. A refusal by the host is no longer returned to the
caller: it shows in the kernel log ("queued command failed"). The framebuffer's and
WindowServer's own surfaces are created as before.

Second Intel host, no other guest busy, the same kext and bundle, one boot each, `ci-effects`
with eight renders a filter, sums over the 24 filters (ours; the software renderer's are 84,
54 and 34 ms):

| | first render | second render | each later render |
|---|---|---|---|
| creates waited for (`ripple_create=0`) | 330.8, 306.4, 314.7 ms | 98.3, 93.9, 95.9 ms | 33.5, 31.2, 31.8 ms |
| creates queued | 263.2, 271.6, 287.9, 277.7, 268.3 | 77.3, 77.6, 80.7, 79.7, 79.5 | 25.4, 25.8, 26.8, 26.7, 27.2 |

The first showing of the 24 filters goes from 413 to 353 ms (15% less), each later render
from 32.2 to 26.4 ms (18% less; a later render still creates the buffers the texture cache
does not hold). With the queue the timers show 0.7 ms in the creates and 37 to 52 ms in the
attaches: when the host is behind, the sixteen command slots fill and the next command waits
for one, so part of the wait has only moved. OpenArena's timedemo on the same two boots:
276.3, 275.6, 267.4 frames/s waited for, 283.0, 269.1, 288.4 queued, no difference.

The picture comparison is unchanged in both modes (1 of 24 filters, 6 of 61 effects, the same
ones). Looked at with the queue: desktop with the menu bar, a window with its shadow, Exposé,
Dashboard; gl-gears 58.7 frames/s; no error from the kext and no refused command.

Two traps of this measurement, for the next one. A run while another guest used the host's
GPU came out five times slower (530 ms for the first renders), and the software renderer's
column, which only needs the CPU, hardly showed it: on a shared host, look at the other
guests' GPU use before and after a run. And the first "before" was no before: the restart
that should have set the boot flag had been skipped, so both runs were on one boot (the five
values of the queued row). The kernel's own `kern.bootargs` and the boot time are checked
after each restart now.

The same pair in UTM on an M1 Max (no other guest; a different picture, a scaled system
desktop picture, so these compare only with each other; six renders a filter): first render
7,282 and 7,217 ms waited for, 8,013 and 7,254 queued; second 1,910 and 1,908 against 2,074
and 1,903; each later render 147.7 and 147.4 against 137.0 and 137.5 ms (7% less). The first
two renders are the emulated CPU compiling and do not change. The software renderer there
needs 3,330 ms for a later render of the 24, ours 137. With that picture 7 of the 24 filters
differ from the software renderer in UTM, with either kext mode and with the shader made at
load or at the first draw alike; which filters and why is not looked into yet.
