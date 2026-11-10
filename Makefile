# moose-ripple build. Runs on dev.lan (Linux): clang, cctools-port ld64 and the open-source
# xnu-1504.15.3 headers, all under .tools/ (git-ignored). scripts/build.sh runs this there.
#
#   make             everything
#   make hello       build/RippleHello.kext (universal i386 + x86_64)

TOOLS  ?= $(CURDIR)/.tools
XNU    ?= $(TOOLS)/src/xnu-xnu-1504.15.3
CCT    ?= $(TOOLS)/cctools/bin/x86_64-apple-darwin10-
CC     := clang
LD     := $(CCT)ld
LIPO   := $(CCT)lipo
VTOOL  := $(CCT)vtool
OUT    := build
ARCHS  := i386 x86_64

# Kernel code flags, from xnu-1504.15.3: makedefs/MakeInc.def (CFLAGS_GEN, CFLAGS_X86_64)
# and libkern/kmod/Makefile.kmod (-Os -mkernel).
KINC    := -I$(XNU)/EXTERNAL_HEADERS -I$(XNU)/bsd -I$(XNU)/osfmk -I$(XNU)/libkern -I$(XNU)/iokit -I$(XNU)/osfmk/libsa
KCFLAGS := -mmacosx-version-min=10.6 -static -nostdinc -fno-builtin -finline -fno-common \
           -msoft-float -fsigned-bitfields -fno-stack-protector -mkernel -DKERNEL \
           -Os -Wall -Wno-deprecated-non-prototype $(KINC)
target   = -target $(1)-apple-darwin10

# 10.6's own kexts are x86_64 "kext bundle" (MH_KEXT_BUNDLE) + i386 "object" (MH_OBJECT):
# `file` on AppleAPIC, AppleSMBusPCI and others in the guest, 2026-11-10.
# 10.6's kxld rejects any other load command in an i386 MH_OBJECT kext ("Invalid segment
# type in MH_OBJECT kext: 36", LC_VERSION_MIN_MACOSX, seen 2026-11-10), so the i386 link
# drops LC_DATA_IN_CODE and vtool strips the version command clang put in each object.
link_x86_64 = $(LD) -arch x86_64 -kext -macosx_version_min 10.6 -o $(1) $(2)
link_i386   = $(LD) -arch i386 -r -no_data_in_code_info -o $(1).r $(2) && \
              $(VTOOL) -remove-build-version macos -output $(1) $(1).r && rm -f $(1).r

.PHONY: all hello hellodylib clean
all: hello hellodylib

clean:
	rm -rf $(OUT)

# kmod glue (_start/_stop calling _realmain/_antimain), built from xnu's own source.
KMOD_SRC := c_start c_stop
$(OUT)/%/kmod/c_start.o $(OUT)/%/kmod/c_stop.o: $(XNU)/libkern/kmod/c_start.c $(XNU)/libkern/kmod/c_stop.c
	@mkdir -p $(dir $@)
	$(CC) $(call target,$*) $(KCFLAGS) -c $(XNU)/libkern/kmod/c_start.c -o $(OUT)/$*/kmod/c_start.o
	$(CC) $(call target,$*) $(KCFLAGS) -c $(XNU)/libkern/kmod/c_stop.c -o $(OUT)/$*/kmod/c_stop.o

# --- RippleHello -------------------------------------------------------------------------
HELLO_OBJ = $(OUT)/$(1)/hello/hello.o $(OUT)/$(1)/hello/info.o $(OUT)/$(1)/kmod/c_start.o $(OUT)/$(1)/kmod/c_stop.o

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

# --- hello.dylib (userspace) --------------------------------------------------------------
# Until the 10.6 SDK arrives, userspace links against the guest's own libSystem, copied into
# .tools/guest-root (never committed; SHA-256 in docs/decisions/0001-toolchain.md).
GUESTROOT ?= $(TOOLS)/guest-root
UCFLAGS   := -mmacosx-version-min=10.6 -O2 -Wall -fno-stack-protector
link_dylib = $(LD) -arch $(1) -dylib -macosx_version_min 10.6 -syslibroot $(GUESTROOT) \
             -install_name /tmp/ripple/$(notdir $(2)) -o $(2) $(3) -lSystem

$(OUT)/%/hello-dylib/hello.o: tools/hello-dylib/hello.c
	@mkdir -p $(dir $@)
	$(CC) $(call target,$*) $(UCFLAGS) -c $< -o $@
$(OUT)/%/hello.dylib: $(OUT)/%/hello-dylib/hello.o
	$(call link_dylib,$*,$@,$^)

hellodylib: $(OUT)/hello.dylib
$(OUT)/hello.dylib: $(OUT)/i386/hello.dylib $(OUT)/x86_64/hello.dylib
	$(LIPO) -create $^ -output $@

hello: $(OUT)/RippleHello.kext
$(OUT)/RippleHello.kext: $(OUT)/i386/RippleHello $(OUT)/x86_64/RippleHello kext/hello/Info.plist
	rm -rf $@
	mkdir -p $@/Contents/MacOS
	cp kext/hello/Info.plist $@/Contents/Info.plist
	$(LIPO) -create $(OUT)/i386/RippleHello $(OUT)/x86_64/RippleHello -output $@/Contents/MacOS/RippleHello
