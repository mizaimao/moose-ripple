# Reference copies (read-only)

Other projects' source kept for reading, never committed. They live in the build
host's copy of the repo under `.tools/src/` (git-ignored). Copy no code from projects whose
license doesn't allow it; cite file and line in `docs/re/` instead.

| Copy | What | Version | License note |
|---|---|---|---|
| `xnu-xnu-1504.15.3/` | Apple's kernel source, = 10.6.8 | tag xnu-1504.15.3, tarball SHA-256 `05cc3e96...2c58` | APSL 2.0 |
| `IOHIDFamily-IOHIDFamily-315.7.16/` | Apple's HID family, = 10.6.8's IOHIDFamily 1.6.6 | tarball SHA-256 `cb033840...2d2f` | APSL 2.0 |
| `linux-uapi/` | Linux virtio headers (vendored copies in `third_party/virtio`) | v6.12 | BSD (stated in each file) |
| `vmsvga2-r166/` | VMsvga2, VMware SVGA driver for OS X guests | SourceForge svn r166 | Zenith432's code "all rights reserved"; VMware parts MIT. Read only |
| `VirtualIntelMacOnAppleSilicon/` | Virtual Intel Mac: Tiger to High Sierra guests on macOS hosts, OpenGL forwarded to the host through QEMU's apple-gfx-pci and Apple's ParavirtualizedGraphics (`Source/PassthroughOpenGL`), guest drivers derived from VMsvga2 | commit `12fe65e839cb573be0a37de9f3aa5415d221640f` (2026-11-10, v2.0 line), shallow clone of https://github.com/nfzerox/VirtualIntelMacOnAppleSilicon | Repo says MIT, but the VMsvga2-derived parts carry VMsvga2's license problem. Read for design only (2026-11-10); no full study now |

Full SHA-256s: xnu `05cc3e9617581f8f9bfa434901fb902761c89bbcf770d822e84a31dd625c2b58`, IOHIDFamily
`cb033840edb81a07694793242ebe1a356fb7da205079a861b09457e58df92d2f`.
