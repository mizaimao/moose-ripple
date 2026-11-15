# windows/kmd: the kernel driver

The kernel half of the Windows display driver for QEMU's virtio-gpu device (see
`docs/windows/feasibility.md`, milestone W1). **Not working yet:** it builds for Windows 7 x64
(`build.sh`, on Linux with clang and the Windows 7 kit's headers), loads and talks to the
device, and Windows 7 does not keep it started so far (`docs/windows/kmd-port-notes.md`, "Load
tests"). This folder started as an unchanged copy of other people's work and is changed from
there.

## Where the code comes from

| Folder | Source | Licence |
|---|---|---|
| `VirtIO/` | virtio-win's virtio library: `VirtIO/` of `kvm-guest-drivers-windows` | BSD-3-Clause, `VirtIO/LICENSE` |
| `common/`, `shared/` | The same repository's `viogpu/common` and `viogpu/shared/viogpum.h` | BSD-3-Clause, `LICENSE` |
| `viogpu3d/` | The 3D display driver of virtio-win pull request 943, "Virtio GPU 3D acceleration for windows" | BSD-3-Clause, `LICENSE` |

All of it was taken from the pull request's branch,
<https://github.com/max8rr8/kvm-guest-drivers-windows>, branch `viogpu3d`, commit
`9ed3aab11fb46e55dc835ff008008623b290a6cf` (2025-09-05). The upstream project is
<https://github.com/virtio-win/kvm-guest-drivers-windows>; the pull request is
<https://github.com/virtio-win/kvm-guest-drivers-windows/pull/943>. Not copied: the Visual
Studio project files, the WDF variant of the virtio library, and the helper programs.

One file is left out: `common/viogpu_pci.cpp` (the driver's glue between the virtio library
and the PCI device). Its header says GPL version 2, also in the upstream project today,
although that project's licence file is BSD-3-Clause. This repository takes no GPL code, so
the file is not here and its job is written anew (`common/ripple_pci.cpp`). Every
other file was checked for its licence header: BSD-3-Clause text, or none (covered by the
licence file).

The copyright notices in the files and the two licence texts stay as they are. Changes made
here are under the same licence as the file they are in; files that are new here are MIT
like the rest of this repository and say so in their header.

## What changes

That driver was written for Windows 10 and registers for display driver model 1.3. Windows 7
has 1.1. `docs/windows/kmd-port-notes.md` lists what has to change; the git history of this
folder after the first commit is the port.
