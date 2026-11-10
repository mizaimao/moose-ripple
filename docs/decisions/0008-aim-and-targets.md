# 0008: The project's aim and targets

Date: 2026-11-10. Status: The owner's words, passed on by the infrastructure agent after he weighed
the prior art (`docs/re/prior-art.md`): continue.

## Aim

System-wide acceleration for Mac OS X 10.6.8 guests (any app, Quartz Extreme, Core Image)
through the standard virtio-gpu/virgl card, GPU-agnostic, on any host that runs QEMU with virgl.

## Targets

- **Primary:** amd64 Linux/KVM hosts (the retro rig).
- **Later:** hardware acceleration on Apple silicon through QEMU + virgl on macOS (with an
  emulated x86 CPU), tested once M4 works.

## Change to the handoff

HANDOFF.md section 2 lists Apple silicon hosts as a non-goal. That changes to "later target".
The rest of section 2 stands.

## What stays ours

System-wide integration through Apple's GL plugin interface and WindowServer (M4, M5),
clean-room, GPU-agnostic. Prior projects did per-app 3D or Mac-host-only paths.
