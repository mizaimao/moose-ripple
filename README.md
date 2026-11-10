# moose-ripple

GPU acceleration for Mac OS X 10.6.8 (Snow Leopard) running in QEMU: Quartz Extreme and
Core Image through a guest driver for QEMU's virtio-gpu + virglrenderer. Named after the
Dashboard ripple, the classic sign that Quartz Extreme works.

- **The plan:** [HANDOFF.md](HANDOFF.md) (written under the working name "snow-virgl").
- **Status:** not started. Next is M0, the build and test loop.
- **Visibility:** private until it reaches a meaningful stage; The owner decides when it opens.
- **Issues:** Linear, project moose-ripple.

## Machines

- **Retro rig** (`ssh retro`): the QEMU/KVM host. Keep it on Proxmox 8.4 (the owner's rule). Its
  GeForce 9600 GT passes through to a real 10.6 VM (VMs 200/201) as the reference. The virgl
  host GPU will be the i9-9900K's built-in UHD 630, once it's switched on in the BIOS.
- **Mac mini, Mid 2010:** a real 10.6.8 Mac for ground truth (new RAM on order).
- **dev.lan:** compiles. Nothing runs on the owner's laptop.
