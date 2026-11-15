/*
 * SPDX-License-Identifier: MIT
 *
 * Kernel names that newer kits have and the Windows 7 kit (WDK 7.1) does not, mapped to what
 * Windows 7 offers. Included before everything else (build.sh, /FI).
 */
#ifndef RIPPLE_WIN7_H
#define RIPPLE_WIN7_H

/* Pool that cannot hold code came with Windows 8. */
#define NonPagedPoolNx NonPagedPool

/* Newer spelling of the same allocation call. */
#define ExAllocatePoolUninitialized ExAllocatePoolWithTag

#endif
