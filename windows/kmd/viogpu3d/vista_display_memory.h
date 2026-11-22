#pragma once
#include "helper.h"

#if DXGKDDI_INTERFACE_VERSION < DXGKDDI_INTERFACE_VERSION_WIN7
class VistaDisplayMemory
{
  public:
    static const UINT SegmentId = 2;
    VistaDisplayMemory() : m_base(NULL), m_size(0) {}
    ~VistaDisplayMemory();
    NTSTATUS Initialize();
    void Describe(DXGK_SEGMENTDESCRIPTOR *segment) const;
    NTSTATUS Page(DXGKARG_BUILDPAGINGBUFFER *paging);

  private:
    BYTE *m_base;
    SIZE_T m_size;
    BYTE *Range(ULONGLONG offset, SIZE_T size) const;
};
#endif
