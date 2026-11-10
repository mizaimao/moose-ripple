# Future ideas

Not scheduled and not researched; recorded so they aren't lost. The current plan
(HANDOFF.md milestones, M2 next) comes first.

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
