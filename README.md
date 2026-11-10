# moose-ripple

GPU acceleration for Mac OS X 10.6.8 (Snow Leopard) running in QEMU: Quartz Extreme and
Core Image through a guest driver for QEMU's virtio-gpu + virglrenderer. Named after the
Dashboard ripple, the classic sign that Quartz Extreme works.

- **The plan:** [HANDOFF.md](HANDOFF.md) (written under the working name "snow-virgl").
- **Status:** M0 (build, test and debug loop), M1 (go/no-go spike, [report](docs/re/m1-report.md)), M2 (2D
  display driver, [report](docs/re/m2-report.md)) and M3 (3D transport, [report](docs/re/m3-report.md)) done
  2026-11-10. Next: M4, the GL bundle, once the owner says go.
- **Visibility:** private until it reaches a meaningful stage; The owner decides when it opens.
- **Known issues** (useful beyond this project): [docs/known-issues.md](docs/known-issues.md).
- **Ideas for later:** [docs/future-ideas.md](docs/future-ideas.md) (not scheduled).
- **Issues:** Linear, project moose-ripple.

## Machines

- **Retro rig** (`ssh retro`): the QEMU/KVM host. Keep it on Proxmox 8.4 (the owner's rule). Its
  GeForce 9600 GT passes through to a real 10.6 VM (VMs 200/201) as the reference. The virgl
  host GPU will be the i9-9900K's built-in UHD 630, once it's switched on in the BIOS.
- **Mac mini, Mid 2010:** a real 10.6.8 Mac for ground truth (new RAM on order).
- **dev.lan:** compiles. Nothing runs on the owner's laptop.
