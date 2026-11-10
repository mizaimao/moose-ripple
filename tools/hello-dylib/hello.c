/*
 * hello.dylib: the M0 userspace check. Prints from a constructor when loaded, so a stock
 * program can load it with DYLD_INSERT_LIBRARIES in a 32-bit and a 64-bit process.
 */
#include <stdio.h>
#include <unistd.h>

__attribute__((constructor))
static void ripple_hello_dylib(void)
{
    printf("hello.dylib: loaded in pid %d, %d-bit process\n", getpid(), (int)(sizeof(void *) * 8));
}
