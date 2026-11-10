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

.PHONY: all hello hellodylib cglinfo clean
all: hello hellodylib cglinfo

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
