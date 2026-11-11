# Prior art

Recorded 2026-11-10. Read-only: copy no code. Reading
copies, where we have them: [references.md](references.md).

| Project | What it does | Where it differs from moose-ripple |
|---|---|---|
| **VMsvga2** (SourceForge, Zenith432) | VMware SVGA driver for OS X guests incl. 10.6: framebuffer, accelerator user clients, a GL bundle that forwards `gld*` calls to Apple's GMA 950 and software renderers | VMware only; its own code is all-rights-reserved (read only; copy at `.tools/src/vmsvga2-r166`) |
| **Virtual Intel Mac** (github.com/nfzerox/VirtualIntelMacOnAppleSilicon, commit `12fe65e8`) | Tiger to High Sierra guests with OpenGL forwarded to a macOS host's OpenGL through QEMU's apple-gfx-pci and Apple's ParavirtualizedGraphics (`Source/PassthroughOpenGL`: a GLD engine, shared-memory render targets, IOSurface publication), guest drivers derived from VMsvga2 | macOS hosts only; MIT as stated, but the VMsvga2-derived parts carry VMsvga2's license problem. Copy at `.tools/src/VirtualIntelMacOnAppleSilicon`; not studied in depth |
| **startergo/VMQemuVGA** | Reported: a virtio-gpu/QXL kext for 10.6.8 under QEMU/UTM with virgl transport, Mesa cross-built for 10.6 x86_64 with a `virgl_iokit_winsys` over ten user-client selectors; clears and textured triangles read back byte-exact. 3D was per-app, through a substitute OpenGL.framework, not system-wide (no WindowServer / Quartz Extreme). Known bug: the cursor fails with GL scanout on virtio-vga-gl | **UNVERIFIED**: the repo returns 404 on GitHub now; this is from a search-engine summary only. Closest in transport to our M3; differs in integration (ours goes through Apple's GL plugin interface and WindowServer) |
| startergo/Mesa-VirGL | Mesa fork (presumably the above's Mesa side) | Public; not examined |
| startergo/VMsvga2-modern | VMsvga2 variant | Public; not examined |
| startergo/homebrew-qemu-virgl-kosmickrisp, startergo/homebrew-virglrenderer | QEMU with virgl on macOS hosts | Relevant to the later Apple silicon target (decision 0008) |

What stays ours: system-wide integration through Apple's GL plugin interface and WindowServer
(M4/M5), clean-room, GPU-agnostic, on any host with QEMU + virgl.
