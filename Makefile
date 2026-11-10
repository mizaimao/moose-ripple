# moose-ripple build. Runs on dev.lan (Linux): clang, cctools-port ld64 and Apple's 10.6 SDK
# (from Xcode 3.2.6), all under .tools/ (git-ignored). scripts/build.sh runs this there.
# See docs/decisions/0001-toolchain.md.
#
#   make             everything
#   make hello       build/RippleHello.kext (universal i386 + x86_64)
#   make hellodylib  build/hello.dylib (universal i386 + x86_64)

TOOLS  ?= $(CURDIR)/.tools
SDK    ?= $(TOOLS)/sdk/MacOSX10.6.sdk
CCT    ?= $(TOOLS)/cctools/bin/x86_64-apple-darwin10-
CC     := clang
LD     := $(CCT)ld
LIPO   := $(CCT)lipo
VTOOL  := $(CCT)vtool
OUT    := build
target  = -target $(1)-apple-darwin10

# Kernel code flags, from xnu-1504.15.3: makedefs/MakeInc.def (CFLAGS_GEN, CFLAGS_X86_64)
# and libkern/kmod/Makefile.kmod (-Os -mkernel). Headers: the SDK's Kernel.framework.
KCFLAGS := -mmacosx-version-min=10.6 -static -nostdinc -fno-builtin -finline -fno-common \
           -msoft-float -fsigned-bitfields -fno-stack-protector -mkernel -DKERNEL \
           -Os -Wall -Wno-deprecated-non-prototype \
           -I$(SDK)/System/Library/Frameworks/Kernel.framework/Headers

# 10.6's own kexts are x86_64 "kext bundle" (MH_KEXT_BUNDLE) + i386 "object" (MH_OBJECT):
# `file` on AppleAPIC, AppleSMBusPCI and others in the guest, 2026-11-10.
# 10.6's kxld rejects any other load command in an i386 MH_OBJECT kext ("Invalid segment
# type in MH_OBJECT kext: 36", LC_VERSION_MIN_MACOSX, seen 2026-11-10), so the i386 link
# drops LC_DATA_IN_CODE and vtool strips the version command clang put in each object.
# libkmod.a (the SDK's _start/_stop glue) is linked in as Xcode does.
KLIBS       := -L$(SDK)/usr/lib -lkmod
link_x86_64 = $(LD) -arch x86_64 -kext -macosx_version_min 10.6 -o $(1) $(2) $(KLIBS)
link_i386   = $(LD) -arch i386 -r -no_data_in_code_info -o $(1).r $(2) $(KLIBS) && \
              $(VTOOL) -remove-build-version macos -output $(1) $(1).r && rm -f $(1).r

# Userspace: compile and link against the SDK. Like the i386 kexts, userspace binaries leave
# out the load commands 10.6 predates (LC_FUNCTION_STARTS, LC_DATA_IN_CODE,
# LC_VERSION_MIN_MACOSX): Apple's own 10.6 programs have none, and 10.6's dtrace could not
# see any module of a program that had them (pid probes matched nothing; 2026-11-10).
UCFLAGS    := -mmacosx-version-min=10.6 -isysroot $(SDK) -O2 -Wall
ULD         = $(LD) -arch $(1) -macosx_version_min 10.6 -syslibroot $(SDK) \
              -no_function_starts -no_data_in_code_info
strip_vmin  = $(VTOOL) -remove-build-version macos -output $(1) $(1).v && rm -f $(1).v
link_dylib  = $(call ULD,$(1)) -dylib -install_name /tmp/ripple/$(notdir $(2)) \
              -o $(2).v $(3) -lSystem && $(call strip_vmin,$(2))

.PHONY: all hello hellodylib cglinfo accelprobe glshim virtiogpu ptrpos setmode clean
all: hello hellodylib cglinfo accelprobe glshim virtiogpu ptrpos setmode

clean:
	rm -rf $(OUT)

# --- RippleHello -------------------------------------------------------------------------
HELLO_OBJ = $(OUT)/$(1)/hello/hello.o $(OUT)/$(1)/hello/info.o

$(OUT)/i386/hello/%.o: kext/hello/%.c
	@mkdir -p $(dir $@)
	$(CC) $(call target,i386) $(KCFLAGS) -c $< -o $@
$(OUT)/x86_64/hello/%.o: kext/hello/%.c
	@mkdir -p $(dir $@)
	$(CC) $(call target,x86_64) $(KCFLAGS) -c $< -o $@

$(OUT)/i386/RippleHello: $(call HELLO_OBJ,i386)
	$(call link_i386,$@,$^)
$(OUT)/x86_64/RippleHello: $(call HELLO_OBJ,x86_64)
	$(call link_x86_64,$@,$^)

hello: $(OUT)/RippleHello.kext
$(OUT)/RippleHello.kext: $(OUT)/i386/RippleHello $(OUT)/x86_64/RippleHello kext/hello/Info.plist
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	cp kext/hello/Info.plist $@/Contents/Info.plist
	$(LIPO) -create $(OUT)/i386/RippleHello $(OUT)/x86_64/RippleHello -output $@/Contents/MacOS/RippleHello

# --- hello.dylib (userspace) --------------------------------------------------------------
$(OUT)/%/hello-dylib/hello.o: tools/hello-dylib/hello.c
	@mkdir -p $(dir $@)
	$(CC) $(call target,$*) $(UCFLAGS) -c $< -o $@
$(OUT)/%/hello.dylib: $(OUT)/%/hello-dylib/hello.o
	$(call link_dylib,$*,$@,$^)

hellodylib: $(OUT)/hello.dylib
$(OUT)/hello.dylib: $(OUT)/i386/hello.dylib $(OUT)/x86_64/hello.dylib
	$(LIPO) -create $^ -output $@

# --- cgl-info (userspace tool) -----------------------------------------------------------
link_tool = $(call ULD,$(1)) -o $(2).v $(SDK)/usr/lib/crt1.10.6.o $(3) $(4) -lSystem && \
            $(call strip_vmin,$(2))

$(OUT)/%/cgl-info/cgl-info.o: tools/cgl-info/cgl-info.c
	@mkdir -p $(dir $@)
	$(CC) $(call target,$*) $(UCFLAGS) -c $< -o $@
$(OUT)/%/cgl-info/cgl-info: $(OUT)/%/cgl-info/cgl-info.o
	$(call link_tool,$*,$@,$^,-framework OpenGL)

cglinfo: $(OUT)/cgl-info
$(OUT)/cgl-info: $(OUT)/i386/cgl-info/cgl-info $(OUT)/x86_64/cgl-info/cgl-info
	$(LIPO) -create $^ -output $@

# --- C++ kexts ---------------------------------------------------------------------------
# xnu-1504.15.3 makedefs/MakeInc.def CXXFLAGS_GEN; libkmodc++.a's _start runs the C++ runtime
# setup (OSRuntimeInitializeCPP, xnu libkern/kmod/cplus_start.c).
CXX       := clang++
# -std=gnu++98: the SDK headers are C++98 (kern/queue.h uses `register`). Modern clang refuses
# -fapple-kext for i386 ("not supported for C++ on Darwin/i386"), so C++ kexts are x86_64-only
# here; an i386 slice would need Xcode 3.2.6's gcc-4.2 (docs/decisions/0001-toolchain.md).
# Global destructors must go in __mod_term_func: the 10.6 kernel has no __cxa_atexit, and
# LLVM lowers them to __cxa_atexit calls on Mach-O unless told not to.
KCXXFLAGS := $(filter-out -Wno-deprecated-non-prototype,$(KCFLAGS)) -std=gnu++98 \
             -fapple-kext -fno-rtti -fno-exceptions -fcheck-new -fno-use-cxa-atexit \
             -mllvm -disable-atexit-based-global-dtor-lowering \
             -Wno-mismatched-tags -Wno-overloaded-virtual
KLIBSXX   := -L$(SDK)/usr/lib -lkmodc++ -lkmod
linkxx_x86_64 = $(LD) -arch x86_64 -kext -macosx_version_min 10.6 -o $(1) $(2) $(KLIBSXX)
linkxx_i386   = $(LD) -arch i386 -r -no_data_in_code_info -o $(1).r $(2) $(KLIBSXX) && \
                $(VTOOL) -remove-build-version macos -output $(1) $(1).r && rm -f $(1).r
# make_kext NAME PLIST ARCHS: kext bundle from $(OUT)/<arch>/NAME for each arch in ARCHS
make_kext = rm -rf $@ && mkdir -p $@/Contents/MacOS && cp $(2) $@/Contents/Info.plist && \
            $(LIPO) -create $(foreach a,$(3),$(OUT)/$(a)/$(1)) -output $@/Contents/MacOS/$(1)

# --- RippleAccelProbe (M1 loader experiment) ---------------------------------------------
PROBE_OBJ = $(OUT)/$(1)/accelprobe/RippleAccelProbe.o $(OUT)/$(1)/accelprobe/info.o
$(OUT)/%/accelprobe/RippleAccelProbe.o: kext/accelprobe/RippleAccelProbe.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(call target,$*) $(KCXXFLAGS) -c $< -o $@
$(OUT)/%/accelprobe/info.o: kext/accelprobe/info.c
	@mkdir -p $(dir $@)
	$(CC) $(call target,$*) $(KCFLAGS) -c $< -o $@
$(OUT)/x86_64/RippleAccelProbe: $(call PROBE_OBJ,x86_64)
	$(call linkxx_x86_64,$@,$^)
accelprobe: $(OUT)/RippleAccelProbe.kext
$(OUT)/RippleAccelProbe.kext: $(OUT)/x86_64/RippleAccelProbe kext/accelprobe/Info.plist
	$(call make_kext,RippleAccelProbe,kext/accelprobe/Info.plist,x86_64)

# --- RippleGLDriver shim (M1 loader experiment, x86_64 only) -----------------------------
SHIM := $(OUT)/x86_64/shim
$(SHIM)/trampolines.s $(SHIM)/gld-names.h: gldriver/shim/gld-entry-points.txt tools/gen-gld-trampolines.sh
	@mkdir -p $(SHIM)
	sh tools/gen-gld-trampolines.sh $< $(SHIM)/trampolines.s $(SHIM)/gld-names.h
$(SHIM)/trampolines.o: $(SHIM)/trampolines.s
	$(CC) $(call target,x86_64) -c $< -o $@
$(SHIM)/shim.o: gldriver/shim/shim.c $(SHIM)/gld-names.h
	$(CC) $(call target,x86_64) $(UCFLAGS) -I$(SHIM) -c $< -o $@
$(OUT)/x86_64/RippleGLDriver: $(SHIM)/shim.o $(SHIM)/trampolines.o
	$(call ULD,x86_64) -bundle -o $@.v $^ -lSystem && $(call strip_vmin,$@)
glshim: $(OUT)/RippleGLDriver.bundle
$(OUT)/RippleGLDriver.bundle: $(OUT)/x86_64/RippleGLDriver gldriver/shim/Info.plist
	rm -rf $@ && mkdir -p $@/Contents/MacOS && cp gldriver/shim/Info.plist $@/Contents/Info.plist && \
	cp $(OUT)/x86_64/RippleGLDriver $@/Contents/MacOS/RippleGLDriver

# --- RippleVirtioGPU (M2) ---------------------------------------------------------------
VGPU_INC := -Ithird_party/virtio -Ikext/virtiogpu
VGPU_OBJ  = $(OUT)/x86_64/virtiogpu/VirtioGPUDevice.o $(OUT)/x86_64/virtiogpu/RippleFramebuffer.o \
            $(OUT)/x86_64/virtiogpu/virtqueue.o \
            $(OUT)/x86_64/virtiogpu/info.o
$(OUT)/x86_64/virtiogpu/%.o: kext/virtiogpu/%.cpp kext/virtiogpu/*.h
	@mkdir -p $(dir $@)
	$(CXX) $(call target,x86_64) $(KCXXFLAGS) $(VGPU_INC) -c $< -o $@
$(OUT)/x86_64/virtiogpu/%.o: kext/virtiogpu/%.c kext/virtiogpu/*.h
	@mkdir -p $(dir $@)
	$(CC) $(call target,x86_64) $(KCFLAGS) $(VGPU_INC) -c $< -o $@
$(OUT)/x86_64/RippleVirtioGPU: $(VGPU_OBJ)
	$(call linkxx_x86_64,$@,$^)
virtiogpu: $(OUT)/RippleVirtioGPU.kext
$(OUT)/RippleVirtioGPU.kext: $(OUT)/x86_64/RippleVirtioGPU kext/virtiogpu/Info.plist
	$(call make_kext,RippleVirtioGPU,kext/virtiogpu/Info.plist,x86_64)

# --- ptrpos (userspace tool) -------------------------------------------------------------
$(OUT)/%/ptrpos/ptrpos.o: tools/ptrpos/ptrpos.c
	@mkdir -p $(dir $@)
	$(CC) $(call target,$*) $(UCFLAGS) -c $< -o $@
$(OUT)/%/ptrpos/ptrpos: $(OUT)/%/ptrpos/ptrpos.o
	$(call link_tool,$*,$@,$^,-framework ApplicationServices -framework CoreFoundation)
ptrpos: $(OUT)/ptrpos
$(OUT)/ptrpos: $(OUT)/i386/ptrpos/ptrpos $(OUT)/x86_64/ptrpos/ptrpos
	$(LIPO) -create $^ -output $@

# --- setmode (userspace tool) ------------------------------------------------------------
$(OUT)/%/setmode/setmode.o: tools/setmode/setmode.c
	@mkdir -p $(dir $@)
	$(CC) $(call target,$*) $(UCFLAGS) -c $< -o $@
$(OUT)/%/setmode/setmode: $(OUT)/%/setmode/setmode.o
	$(call link_tool,$*,$@,$^,-framework ApplicationServices -framework CoreFoundation)
setmode: $(OUT)/setmode
$(OUT)/setmode: $(OUT)/i386/setmode/setmode $(OUT)/x86_64/setmode/setmode
	$(LIPO) -create $^ -output $@
