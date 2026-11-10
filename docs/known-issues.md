# Known issues

Things we found that others running Mac OS X 10.6 in QEMU may run into too.

## The pointer doesn't follow QEMU's USB tablet exactly (10.6 + `usb-tablet`)

**Symptom.** With QEMU's virtual USB tablet (`-device usb-tablet`, Proxmox `tablet: 1`), 10.6
places the pointer about 1.18x too far from the screen centre: the centre is exact, the error
grows towards the edges, and the pointer clamps at the screen edge early.

**Why it often goes unnoticed.** With the default display (Apple's boot framebuffer, no
hardware cursor) the guest draws its own pointer into the picture. Clicks always match that
pointer, so everything looks right; the pointer just doesn't sit under your real mouse and
runs ahead of it towards the edges. With a driver that has a hardware cursor (ours), a VNC
console such as Proxmox's draws the pointer under your real mouse, and the guest's error then
shows up as clicks landing somewhere else (2026-11-10: pointer at the centre, click on
the left half).

**Measurement** (2026-11-10, 10.6.8 10K549, QEMU 9.2, Proxmox 8.4). Absolute positions were
injected with QMP `input-send-event` (range 0..32767) and the guest's pointer position read
with `tools/ptrpos` (`CGEventGetLocation`):

| Injected (x and y) | Expected at 1280x800 | Guest pointer | With Apple's boot framebuffer at 1024x768 |
|---|---|---|---|
| 0 | 0, 0 | 0.1, 0.4 | |
| 4096 | 160, 100 | 75.3, (y alike) | |
| 8192 | 320, 200 | 263.6, 164.7 | 210.8, 158.1 (expected 256, 192) |
| 12288 | 480, 300 | 451.8, (y alike) | |
| 16384 | 640, 400 | 640.0, 400.0 | 512.0, 384.0 |
| 24576 | 960, 600 | 1016.5, 635.3 | 813.2, 609.9 (expected 768, 576) |
| 32767 | 1280, 800 | 1279.9, 799.6 | |

That is a linear map stretched by 1.176 (about 1/0.85) around the centre, then clamped. It is
repeatable and doesn't depend on the previous position, so it isn't pointer acceleration, and
it is the same with or without our framebuffer, so it comes from 10.6's HID stack, not the
display.

**Cause.** Apple's `IOHIDEventDriver` (IOHIDFamily 1.6.6, open source as IOHIDFamily-315.7.16)
trims 15% off the range of every absolute pointing device, half at each end
(`IOHIDEventDriver.cpp`: `absoluteAxisRemovalPercentage = 15`), then scales the remaining 85%
to the screen: 1/0.85 = 1.176. The driver reads an override from its own property
`AbsoluteAxisBoundsRemovalPercentage`.

**Fix.** [`kext/qemutablet`](../kext/qemutablet/Info.plist), a codeless kext (only an
Info.plist): a personality that has Apple's `IOHIDEventDriver` drive QEMU's tablet (vendor
0x0627, product 1, Generic Desktop / Mouse) with `AbsoluteAxisBoundsRemovalPercentage` = 0.
Its vendor/product keys outscore Apple's generic personality (7200 vs 1200). After installing
it in `/System/Library/Extensions` and rebuilding the kext cache:

| Injected | Guest pointer at 1280x800 |
|---|---|
| 0 | 0, 0 |
| 4096 | 160, 100 |
| 8192 | 320, 200 |
| 16384 | 640, 400 |
| 24576 | 960, 600 |
| 32767 | 1279, 799 |

It works with any display driver, not only ours, so it is useful on its own for 10.6 in QEMU
(and likely 10.5, which has the same HID code; untested).

## 10.6.8 can't use a PCI BAR above 4 GB (OVMF)

Found while setting up OpenCore (decision 0007). Under OVMF, QEMU's virtio-gpu 64-bit BAR 2 can
land above 4 GB (0x800100000 from OVMF, or 0xff10100000 after 10.6 moves it), and the first access
to it from a driver never completes (`vm_fault` again and again, kernel_task at 100%, no
display). Keep every BAR below 4 GB: `-fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0` for OVMF and
the boot flag `npci=0x2000` for 10.6. SeaBIOS places them below 4 GB already. Since
2026-11-10 RippleVirtioGPU refuses a BAR above 4 GB with a log line instead of hanging, so the
boot framebuffer keeps the display.
