# 0004: Display lag is measured and reported, not an acceptance requirement

Date: 2026-11-10. Status: The owner's decision, passed on by the infrastructure agent.

Display lag (how long a cursor move or a window update takes to reach the screen) is an
optional goal for moose-ripple. In M2 and M5, measure it, for example on the local QEMU
display and on the Proxmox console, and report the numbers in the milestone report, but don't
block a milestone on them. The owner's view: most retro users will be happy just to see the
Dashboard ripple and the translucent menu bar.
