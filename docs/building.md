# Building moose-ripple from source

The guest files build on Linux, cross-compiled for Mac OS X 10.6:

```
make package                    # build/moose-ripple-VERSION.zip, the files for step 1 of install.md
make virtiogpu glbundle gaplugin qemutablet qecheck     # only the files, in build/
```

The results land in `build/`.
What the build needs, under the repository's git-ignored `.tools/` and `.venv/`:

- clang, targeting `i386-apple-darwin10` and `x86_64-apple-darwin10`. Tested: clang 19.1.7 on
  Debian 13.
- [cctools-port](https://github.com/tpoechtrager/cctools-port), branch `973.0.1-ld64-609`, built
  into `.tools/cctools` (its `ld`, `lipo` and `vtool`).
- `MacOSX10.6.sdk` from Xcode 3.2.6 in `.tools/sdk/`, from your own copy.
- For the OpenGL driver: meson, ninja, and Python with mako, pyyaml and packaging in `.venv`.
  `gldriver/mesa/build.sh` downloads Mesa 24.3.4, applies [gldriver/mesa/patches/](../gldriver/mesa/patches/)
  and builds both architectures ([gldriver/mesa/](../gldriver/mesa/README.md)).

The OpenCore boot disk has its own build: [boot/opencore/](../boot/opencore/README.md). Why the
toolchain looks like this: [decision 0001](decisions/0001-toolchain.md) and
[decision 0009](decisions/0009-mesa-for-10.6.md).

Our own development loop is in `scripts/`. It builds on a Linux host over ssh and installs into a
Proxmox VM, taking a snapshot first (`build.sh`, `install-kext.sh`, `install-glbundle.sh`,
`bootargs.sh`, `qe-check.sh`). Its settings (host, VM ID, guest user and key) are at the top of
[scripts/lib.sh](../scripts/lib.sh).
