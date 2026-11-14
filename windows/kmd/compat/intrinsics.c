/*
 * SPDX-License-Identifier: MIT
 *
 * Compiler intrinsics the Windows 7 kit's headers expect from Microsoft's compiler. The kit
 * only declares them; clang in cl mode would need its own intrin.h for them, which does not
 * go together with the kit's older headers. So the few the driver uses are real functions
 * here.
 */
unsigned char
__inbyte(unsigned short port)
{
    unsigned char value;
    __asm__ volatile("inb %w1, %b0" : "=a"(value) : "Nd"(port));
    return value;
}

unsigned short
__inword(unsigned short port)
{
    unsigned short value;
    __asm__ volatile("inw %w1, %w0" : "=a"(value) : "Nd"(port));
    return value;
}

unsigned long
__indword(unsigned short port)
{
    unsigned long value;
    __asm__ volatile("inl %w1, %k0" : "=a"(value) : "Nd"(port));
    return value;
}

void
__outbyte(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %b0, %w1" : : "a"(value), "Nd"(port));
}

void
__outword(unsigned short port, unsigned short value)
{
    __asm__ volatile("outw %w0, %w1" : : "a"(value), "Nd"(port));
}

void
__outdword(unsigned short port, unsigned long value)
{
    __asm__ volatile("outl %k0, %w1" : : "a"(value), "Nd"(port));
}

#if defined(_M_AMD64) || defined(__x86_64__)
/* The interrupt request level lives in CR8 on x64 (KeGetCurrentIrql and friends). */
unsigned __int64
__readcr8(void)
{
    unsigned __int64 value;
    __asm__ volatile("mov %%cr8, %0" : "=r"(value));
    return value;
}

void
__writecr8(unsigned __int64 value)
{
    __asm__ volatile("mov %0, %%cr8" : : "r"(value));
}
#endif
