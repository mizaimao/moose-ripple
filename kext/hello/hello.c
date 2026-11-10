/*
 * RippleHello: the M0 hello-world kext. Logs on load and unload, nothing else.
 * Proves the toolchain (clang + cctools-port ld64 + xnu-1504.15.3 headers) makes kexts
 * that 10.6.8 loads on both the i386 and the x86_64 kernel.
 */
#include <mach/mach_types.h>
#include <libkern/libkern.h>

kern_return_t ripple_hello_start(kmod_info_t *ki, void *data);
kern_return_t ripple_hello_stop(kmod_info_t *ki, void *data);

kern_return_t ripple_hello_start(kmod_info_t *ki, void *data)
{
    (void)data;
    printf("RippleHello: start (%s %s, %lu-bit kernel)\n",
           ki->name, ki->version, (unsigned long)(sizeof(void *) * 8));
    return KERN_SUCCESS;
}

kern_return_t ripple_hello_stop(kmod_info_t *ki, void *data)
{
    (void)ki;
    (void)data;
    printf("RippleHello: stop\n");
    return KERN_SUCCESS;
}
