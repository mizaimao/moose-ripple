/* kmod_info for RippleVirtioSound: an IOKit driver, so no start/stop routines of its own
 * (_start/_stop come from the SDK's libkmodc++.a and only run the C++ runtime setup).
 * Keep name and version equal to Info.plist. */
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(com.mooseman.ripple.VirtioSound, "0.0.1d1", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
