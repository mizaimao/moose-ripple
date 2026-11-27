# Development checks for the Snow Leopard driver

Two checks make a test or benchmark run fail on its own, without anyone looking at pictures
(rule of 2026-11-27). Both are for development and test guests; a release install stays quiet.

1. **What the driver leaves out.** The GL bundle counts every place where it skips part of
   what a program asked for or can't handle it (`ripple_gl_skip` in
   `gldriver/mesa/overlay/src/gallium/targets/ripple-gl/ripple_gl_target.c`): a GL call it
   doesn't implement, a pixel format attribute it doesn't offer, an IOSurface texture it can't
   make. The counts go to the process's debug log in `/Library/Logs/RippleGL` (only written
   when that folder exists): a `SKIP` line the first time, a `skips: N (...)` line with the
   frame statistics and when the process ends. Built with `-DRIPPLE_GL_CHECKS=0`, the bundle
   has none of it.
   `check-run.sh start LABEL` before a run and `check-run.sh end LABEL` after it (in the guest)
   print `checks LABEL: driver skips N, kext errors M: PASS` or FAIL, from the skip counts of
   every GL process during the run and the kext's errors in the kernel log.
   `host-errors.sh mark` and `host-errors.sh count MARK` (on the host) do the same for what the
   host's renderer reported as errors: the journal on a Proxmox host, or UTM's `debug.log`.
2. **The same pictures on Apple's software renderer.** `gl-feature-check` runs each case on
   ours and on Apple's software renderer and compares every pixel it reads (tolerance 3 of
   255); a mode fails when ours differs, apart from a short list of reads where Apple's
   renderer is the wrong one (`rec_known`, each with its reason).

Not covered yet: what Mesa itself leaves out below our code (formats and shader features it
falls back on), and fixed frames of benchmarks against the software renderer.
