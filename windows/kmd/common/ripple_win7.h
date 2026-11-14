/*
 * SPDX-License-Identifier: MIT
 *
 * Declarations the driver needs on Windows 7 that the Windows 7 kit does not have.
 * Included from helper.h after the kit's display headers.
 */
#pragma once

/* The driver keeps each mode, and the picture it starts from, in this structure. Display
 * driver model 1.2 declares it for handing the boot display over, which Windows 7 does not
 * do; the driver's own bookkeeping still uses it, so it is declared here with the same
 * fields. */
typedef struct _DXGK_DISPLAY_INFORMATION
{
    UINT Width;
    UINT Height;
    UINT Pitch;
    D3DDDIFORMAT ColorFormat;
    PHYSICAL_ADDRESS PhysicAddress;
    D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId;
    ULONG AcpiId;
} DXGK_DISPLAY_INFORMATION, *PDXGK_DISPLAY_INFORMATION;

/* Entries in a context's allocation list and patch location list. The Windows 10 driver took
 * a constant of newer kits for both; Windows 7's kit has none, so the size is named here. */
#define RIPPLE_ALLOCATION_LIST_SIZE 256
