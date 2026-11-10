# 0009: Mesa 24.3.4 for the GL bundle, built against the 10.6 SDK's own C++ library

Date: 2026-11-10. Status: in progress (M4). Routine technical choice, made by ripple.

## Decisions

1. **Mesa 24.3.4** (last 24.x release, archive.mesa3d.org, SHA-256
   `e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025`, same as in the release
   notes on docs.mesa3d.org). Pinned in `gldriver/mesa/build.sh`, unpacked under `.tools/` on
   dev.lan, never committed. Recent enough that virgl uses NIR and the current state tracker;
   any newer virgl Mesa works with the rig's virglrenderer 0.10.4, which adapts through caps.
2. **C++: the 10.6 SDK's libstdc++ 4.2.1 headers and the system's `libstdc++.6.dylib`**, with
   clang in C++17 mode. The GL path of Mesa barely uses the C++ library (`<vector>`,
   `<type_traits>`, one `<thread>` in a fallback we leave out); C++17 code using `std::vector`
   compiles against those headers for i386 and x86_64. No modern C++ runtime to build or ship.
3. **No thread-local variables.** 10.6's dyld has none (they came in 10.7) and clang refuses
   `__thread` for a 10.6 target, emulated TLS included. Patches keep Mesa's current context and
   dispatch in pthread keys made at load time (`patches/0001`), and drop the other two
   thread-locals in `util/` (`patches/0002`). Mesa's GL entry points then reach the context
   through `_glapi_get_context()`, as on Windows.
4. **Missing libc functions** (`clock_gettime`, `strnlen`, `strndup`, `memmem`,
   `open_memstream`) come from `gldriver/mesa/overlay/include/ripple_compat.h`, force-included
   (patch 0004, as project arguments so meson's own checks see the plain SDK) and all
   `static inline`, so the bundle exports nothing extra.
5. **Our code lives beside Mesa, not in a fork:** `gldriver/mesa/overlay/` is copied over the
   unpacked tree (the winsys in `src/gallium/winsys/virgl/ripple`, later the CGL frontend and
   the bundle target); `patches/` holds the few edits to Mesa's own files. Meson cross-builds one
   architecture at a time with clang, the 10.6 SDK and cctools-port ld64; the two are joined
   with lipo.

## Build notes

- flex and bison come from Debian on dev.lan (installed by the owner, 2026-11-10, after Claude
  Code's safety check refused building them from source).
- clang gets target, SDK and ld64 from a config file: meson's checks fail on
  unused-argument errors when `--ld-path` is on the command line of a compile-only run.
