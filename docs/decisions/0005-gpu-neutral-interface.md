# 0005: A documented, GPU-neutral boundary between the GL bundle and the kernel

Date: 2026-11-10. Status: The owner's decision, passed on by the infrastructure agent. Applies
from M3 on.

## Decision

Design the interface between the GL bundle (Mesa) and the kernel side as documented and
GPU-neutral, not tied to QEMU's virtio-gpu. Goal: once the repo is public, a contributor can
add a real GPU family (for example AMD with Mesa's radeonsi, or Intel with iris) by writing
only a new kext and a Mesa winsys/backend for it, and reuse unchanged:
- the GL bundle (the `gld*` layer and the dispatch-table takeover, decision 0003),
- the Quartz Extreme / IOAccelerator surface side,
- the docs and the toolchain.

## In practice

- `docs/kext-uapi.md` (the user-client API) carries no virtio or virgl specifics: generic
  objects (device, context, buffer/resource, command submission, fences, surfaces) with a
  backend-defined payload where the GPU's own command format has to pass through.
- The GL bundle's non-Mesa code stays GPU-neutral; GPU specifics live in the Mesa
  winsys/backend.
- Where a virtio/virgl specific can't be avoided, note it in the uapi doc as such.

## Limits

This is an interface design rule, not extra scope: it must not slow M2 or add work beyond
designing the interface. If it would, flag it to the owner.
