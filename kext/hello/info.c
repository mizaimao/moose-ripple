/*
 * kmod_info and entry points for RippleHello. Xcode generates this file for a kext; the
 * names it must define are the ones xnu-1504.15.3 libkern/kmod/c_start.c and c_stop.c
 * reference: kmod_info (KMOD_INFO_NAME in osfmk/mach/kmod.h), _realmain and _antimain.
 * Keep the name and version equal to CFBundleIdentifier and CFBundleVersion in Info.plist.
 */
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t ripple_hello_start(kmod_info_t *ki, void *data);
extern kern_return_t ripple_hello_stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(com.mooseman.ripple.Hello, "0.0.1d1", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = ripple_hello_start;
__private_extern__ kmod_stop_func_t *_antimain = ripple_hello_stop;
