/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

#include <d3dkmthk.h>

#pragma pack(1)
typedef struct _VIOGPU_BOX
{
    ULONG x;
    ULONG y;
    ULONG z;
    ULONG width;
    ULONG height;
    ULONG depth;
} VIOGPU_BOX;
#pragma pack()

// ================= QueryAdapterInfo UMDRIVERPRIVATE
#define VIOGPU_IAM 0x56696f475055 // Identified for queryadapterinfo (VioGPU as hex)

typedef struct _VIOGPU_ADAPTERINFO
{
    ULONGLONG IamVioGPU; // Should be set by driver to VIOGPU_IAM
    struct
    {
        UINT Supports3d : 1;
        UINT Reserved : 31;
    } Flags;
    ULONGLONG SupportedCapsetIDs;
} VIOGPU_ADAPTERINFO;

// ================= ESCAPES
#define VIOGPU_GET_DEVICE_ID         0x000
#define VIOGPU_GET_CUSTOM_RESOLUTION 0x001
#define VIOGPU_GET_CAPS              0x002

#define VIOGPU_RES_INFO              0x100
#define VIOGPU_RES_BUSY              0x101

#define VIOGPU_CTX_INIT              0x200

// Have the device look for finished work now (a notification of the control queue), asked by
// a program that polls a fence
#define VIOGPU_NUDGE                 0x301

// Test builds only (RIPPLE_TEST_TDR): hold the next finished command buffer back Id ms, so
// that Windows sees a GPU timeout and the driver's recovery runs
#define VIOGPU_TEST_STALL            0x300

#pragma pack(1)
typedef struct _VIOGPU_DISP_MODE
{
    USHORT XResolution;
    USHORT YResolution;
} VIOGPU_DISP_MODE, *PVIOGPU_DISP_MODE;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_PARAM_REQ
{
    ULONG ParamId;
    UINT64 Value;
} VIOGPU_PARAM_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CAPSET_REQ
{
    ULONG CapsetId;
    ULONG Version;
    ULONG Size;
    // The caller's buffer, eight bytes in every build: a 32-bit program and a 64-bit kernel
    // driver have to agree on where it is and on how long the packet is. A 32-bit caller
    // clears the packet first, so its upper half is zero. (moose-ripple)
    union {
        UCHAR *Capset;
        ULONGLONG Capset64;
    };
} VIOGPU_CAPSET_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_INFO_REQ
{
    D3DKMT_HANDLE ResHandle;
    ULONG Id;
} VIOGPU_RES_INFO_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_BUSY_REQ
{
    D3DKMT_HANDLE ResHandle;
    BOOL Wait;
    BOOL IsBusy;
} VIOGPU_RES_BUSY_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CTX_INIT_REQ
{
    UINT CapsetID;
} VIOGPU_CTX_INIT_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_ESCAPE
{
    USHORT Type;
    USHORT DataLength;
    union {
        ULONG Id;
        VIOGPU_DISP_MODE Resolution;
        VIOGPU_PARAM_REQ Parameter;
        VIOGPU_CAPSET_REQ Capset;

        VIOGPU_RES_INFO_REQ ResourceInfo;
        VIOGPU_RES_BUSY_REQ ResourceBusy;

        VIOGPU_CTX_INIT_REQ CtxInit;
    } DUMMYUNIONNAME;
} VIOGPU_ESCAPE, *PVIOGPU_ESCAPE;
#pragma pack()

// ================= CreateResource
#pragma pack(1)
typedef struct _VIOGPU_RESOURCE_OPTIONS
{
    ULONG target;
    ULONG format;
    ULONG bind;
    ULONG width;
    ULONG height;
    ULONG depth;
    ULONG array_size;
    ULONG last_level;
    ULONG nr_samples;
    ULONG flags;
} VIOGPU_RESOURCE_OPTIONS;

#pragma pack(1)
typedef struct _VIOGPU_CREATE_RESOURCE_EXCHANGE
{
    ULONG magic;
} VIOGPU_CREATE_RESOURCE_EXCHANGE;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CREATE_ALLOCATION_EXCHANGE
{
    VIOGPU_RESOURCE_OPTIONS ResourceOptions;
    ULONGLONG Size;
} VIOGPU_CREATE_ALLOCATION_EXCHANGE;
#pragma pack()

// Vista only: the private data of an allocation that is to be a display primary, the
// allocation exchange followed by VIOGPU_VISTA_PRIMARY_TAG. Vista refuses a primary that can
// live only in the aperture segment; the kernel driver puts an allocation sent with this tag
// in its desktop segment. GetStandardAllocationDriverData tags the desktop's primary, the
// user-mode driver the primaries of Direct3D programs (the compositor's, full-screen games').
// The user-mode driver's copy of this header has the same definition.
typedef struct _VIOGPU_VISTA_PRIMARY
{
    VIOGPU_CREATE_ALLOCATION_EXCHANGE allocation;
    ULONG tag;
} VIOGPU_VISTA_PRIMARY;
#define VIOGPU_VISTA_PRIMARY_TAG 0x6D507256UL

// ================= COMMAND BUFFER
#define VIOGPU_CMD_NOP                0x0
#define VIOGPU_CMD_SUBMIT             0x1 // Submit Command to virgl
#define VIOGPU_CMD_TRANSFER_TO_HOST   0x2 // Transfer resource to host
#define VIOGPU_CMD_TRANSFER_FROM_HOST 0x3 // Transfer resource to host
// Written by the kernel driver only, after a copy into a surface: this box of the resource has
// changed, show it if the resource is the one on screen. The body is a VIOGPU_TRANSFER_CMD.
#define VIOGPU_CMD_SCREEN_CHANGED     0x100
// From user mode: set this event of the calling process when the commands of this buffer have run
// on the host. The body is a VIOGPU_SIGNAL_CMD. Windows 7 has nothing of its own for this; later
// versions do it with a synchronization object that signals a CPU event.
#define VIOGPU_CMD_SIGNAL_EVENT       0x4

#pragma pack(1)
typedef struct _VIOGPU_COMMAND_HDR
{
    UINT type;
    UINT size;
} VIOGPU_COMMAND_HDR;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_TRANSFER_CMD
{
    ULONG res_id;

    VIOGPU_BOX box;

    ULONGLONG offset;
    ULONG level;
    ULONG stride;
    ULONG layer_stride;
} VIOGPU_TRANSFER_CMD;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_SIGNAL_CMD
{
    ULONGLONG hEvent; // an event handle of the calling process
} VIOGPU_SIGNAL_CMD;
#pragma pack()

#define BASE_NAMED_OBJECTS    L"\\BaseNamedObjects\\"
#define GLOBAL_OBJECTS        L"Global\\"
#define RESOLUTION_EVENT_NAME L"VioGpuResolutionEvent"
