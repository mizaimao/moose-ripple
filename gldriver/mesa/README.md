# Mesa for the GL bundle (M4)

Mesa's GL implementation and its virgl Gallium driver, cross-built for Mac OS X 10.6 on dev.lan
(decision [0009](../../docs/decisions/0009-mesa-for-10.6.md)).

- `build.sh ARCH [--clean] [TARGETS]`: run on dev.lan; unpacks the pinned Mesa release into
  `.tools/mesa/`, applies `patches/`, copies `overlay/` over it and builds with meson + ninja.
- `patches/`: the few changes to Mesa's own files (no thread-local variables on 10.6, build hooks).
- `overlay/`: our files inside Mesa's tree. `src/gallium/winsys/virgl/ripple/` is the winsys
  that drives the virgl driver through our kext's user client (`docs/kext-uapi.md`).
- `overlay/include/ripple_compat.h`: libc functions 10.6 lacks, force-included into Mesa (patch 0004).

The meson venv lives in the repo's `.venv` on dev.lan (meson, mako, pyyaml, packaging).
