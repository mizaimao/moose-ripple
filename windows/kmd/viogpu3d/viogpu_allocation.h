#pragma once
#include "helper.h"
#include "virgl_hw.h"
#include "viogpum.h"

class VioGpuAdapter;

class VioGpuResource
{
  public:
  private:
};

class VioGpuAllocation
{
  public:
    VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_OPTIONS *options);
    ~VioGpuAllocation(void);

    UINT GetId(void)
    {
        return m_Id;
    }

    void MarkBusy();
    void UnmarkBusy();
    void WaitIdle();

    void SetDxPhysicalAddress(size_t DxPhysicalAddress)
    {
        m_DxPhysicalAddress = DxPhysicalAddress;
    };

    size_t GetDxPhysicalAddress()
    {
        return m_DxPhysicalAddress;
    };

    // Bytes from one row to the next in the allocation's own memory. The surfaces Windows
    // draws into with the CPU are all 32 bits a pixel.
    UINT GetPitch()
    {
        return m_options.width * 4;
    }

    UINT GetWidth()
    {
        return m_options.width;
    }
    UINT GetHeight()
    {
        return m_options.height;
    }
    UINT GetFormat()
    {
        return m_options.format;
    }
    BOOL IsCoherent()
    {
        return (m_options.flags & VIRGL_RESOURCE_FLAG_MAP_COHERENT) != 0;
    }

    void AttachBacking(MDL *pMdl, size_t pageCount, size_t pageOffset);
    void DetachBacking();

    void FlushToScreen(UINT scan_id);

    static NTSTATUS GetStandardAllocationDriverData(DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation);
    static NTSTATUS DxgkCreateAllocation(VioGpuAdapter *adapter, DXGKARG_CREATEALLOCATION *pCreateAllocation);

    NTSTATUS DescribeAllocation(DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation);
    NTSTATUS MapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);
    NTSTATUS UnmapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer);

    NTSTATUS EscapeResourceInfo(VIOGPU_RES_INFO_REQ *resInfo);
    NTSTATUS EscapeResourceBusy(VIOGPU_RES_BUSY_REQ *resBusy);

#if DXGKDDI_INTERFACE_VERSION < DXGKDDI_INTERFACE_VERSION_WIN7
    // Vista: a primary drawn by the GPU (VIOGPU_VISTA_DRAWN_PRIMARY_TAG), and the copy of its
    // picture into its own memory in the desktop segment, where Windows reads screen captures
    // (viogpu_vidpn.cpp, VioGpuVidPN::Flip)
    BOOLEAN IsDrawnPrimary() const
    {
        return m_drawnPrimary;
    }
    void MirrorToMemory(PHYSICAL_ADDRESS memory);
#endif

  private:
    VioGpuAdapter *m_adapter;

    VIOGPU_RESOURCE_OPTIONS m_options;
    UINT m_Id;

    MDL *m_pMDL;
    size_t m_pageCount;
    size_t m_pageOffset;

    size_t m_DxPhysicalAddress;

#if DXGKDDI_INTERFACE_VERSION < DXGKDDI_INTERFACE_VERSION_WIN7
    BOOLEAN m_drawnPrimary;
    BOOLEAN m_mirrorAttached;
#endif

    KEVENT m_busyNotification;
    volatile LONG m_busy;
};