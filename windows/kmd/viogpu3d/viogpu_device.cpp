#include "viogpu_device.h"
#include "viogpu_adapter.h"
#include "baseobj.h"
#include "virgl_hw.h"

PAGED_CODE_SEG_BEGIN

VioGpuDevice::VioGpuDevice(VioGpuAdapter *pAdapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s", __FUNCTION__));

    m_pAdapter = pAdapter;
    m_id = pAdapter->ctxIdr.GetId();
    pAdapter->ctrlQueue.CreateCtx(m_id, 0);
}

VioGpuDevice::~VioGpuDevice()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s", __FUNCTION__));

    m_pAdapter->ctrlQueue.DestroyCtx(m_id);
    m_pAdapter->ctxIdr.PutId(m_id);
}

NTSTATUS VioGpuDevice::Init(VIOGPU_CTX_INIT_REQ *pOptions)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s", __FUNCTION__));

    UINT context_init = 0;

    context_init |= pOptions->CapsetID;

    m_pAdapter->ctrlQueue.DestroyCtx(m_id); // Destroy old viogpu context
    m_pAdapter->ctrlQueue.CreateCtx(m_id, context_init);

    return STATUS_SUCCESS;
}

// A transfer between a surface's own memory and its copy on the host, for one rectangle. The
// host takes "offset" as the place of the rectangle's first pixel in that memory, so it has to
// be worked out from the rectangle's position; left at zero, every rectangle was filled with
// what is in the surface's top left corner.
static UCHAR *PutTransfer(UCHAR *dmaBuf, UINT type, UINT resId, const RECT *rect, INT dx, INT dy, UINT pitch = 0)
{
    VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
    cmd_hdr->type = type;
    cmd_hdr->size = sizeof(VIOGPU_TRANSFER_CMD);
    dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

    VIOGPU_TRANSFER_CMD *cmdBody = (VIOGPU_TRANSFER_CMD *)dmaBuf;
    dmaBuf += sizeof(VIOGPU_TRANSFER_CMD);

    cmdBody->res_id = resId;

    cmdBody->box.x = rect->left + dx;
    cmdBody->box.y = rect->top + dy;
    cmdBody->box.z = 0;
    cmdBody->box.width = rect->right - rect->left;
    cmdBody->box.height = rect->bottom - rect->top;
    cmdBody->box.depth = 1;

    cmdBody->layer_stride = 0;
    cmdBody->stride = pitch;
    cmdBody->level = 0;
    cmdBody->offset = (ULONGLONG)cmdBody->box.y * pitch + (ULONGLONG)cmdBody->box.x * 4;

    return dmaBuf;
}

NTSTATUS VioGpuDevice::GenerateBltPresent(DXGKARG_PRESENT *pPresent, VioGpuAllocation *src, VioGpuAllocation *dst)
{
    UCHAR *dmaBuf = (UCHAR *)pPresent->pDmaBuffer;

    // The pieces to copy, and one rectangle that covers them all
    const RECT *rects = pPresent->pDstSubRects;
    UINT rectCnt = pPresent->SubRectCnt;
    if (rectCnt == 0 || rects == NULL)
    {
        rects = &pPresent->DstRect;
        rectCnt = 1;
    }

    RECT coverRect = rects[0];
    for (UINT i = 1; i < rectCnt; i++)
    {
        coverRect.top = min(coverRect.top, rects[i].top);
        coverRect.left = min(coverRect.left, rects[i].left);
        coverRect.right = max(coverRect.right, rects[i].right);
        coverRect.bottom = max(coverRect.bottom, rects[i].bottom);
    }

    INT dx = pPresent->SrcRect.left - pPresent->DstRect.left;
    INT dy = pPresent->SrcRect.top - pPresent->DstRect.top;

    // Everything but the copies: up to two transfers and the note that the screen changed
    UINT sizeOfOneRect = 4 * (VIRGL_CMD_RESOURCE_COPY_REGION_SIZE + 1);
    UINT sizeOfRest = 4 * sizeof(VIOGPU_COMMAND_HDR) + 3 * sizeof(VIOGPU_TRANSFER_CMD);
    if (pPresent->DmaSize < sizeOfRest + sizeOfOneRect)
    {
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }

    // More pieces than the buffer holds: copy the covering rectangle in one piece instead
    if (rectCnt > (pPresent->DmaSize - sizeOfRest) / sizeOfOneRect)
    {
        rects = &coverRect;
        rectCnt = 1;
    }

    // If source requires coherency (staging or shadow surface) then emit transfer
    if (src->IsCoherent())
    {
        dmaBuf = PutTransfer(dmaBuf, VIOGPU_CMD_TRANSFER_TO_HOST, src->GetId(), &coverRect, dx, dy, src->GetPitch());
    }

    {
        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_SUBMIT;
        cmd_hdr->size = rectCnt * sizeOfOneRect;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        for (UINT i = 0; i < rectCnt; i++)
        {
            UINT *cmdBody = (UINT *)dmaBuf;
            dmaBuf += sizeOfOneRect;

            RECT rect = rects[i];

            cmdBody[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
            cmdBody[1] = dst->GetId();
            cmdBody[2] = 0;
            cmdBody[3] = rect.left;
            cmdBody[4] = rect.top;
            cmdBody[5] = 0;

            cmdBody[6] = src->GetId();
            cmdBody[7] = 0;
            cmdBody[8] = rect.left + dx;
            cmdBody[9] = rect.top + dy;
            cmdBody[10] = 0;
            cmdBody[11] = rect.right - rect.left;
            cmdBody[12] = rect.bottom - rect.top;
            cmdBody[13] = 1;
        }
    }

    if (dst->IsCoherent())
    {
        dmaBuf = PutTransfer(dmaBuf, VIOGPU_CMD_TRANSFER_FROM_HOST, dst->GetId(), &coverRect, 0, 0, dst->GetPitch());
    }

    // Without a compositor nothing flips: Windows draws into a surface of its own and copies the
    // changed pieces to the surface on screen. The host shows them only when told to, so the copy
    // is followed by a note of what changed (VioGpuVidPN::ScreenChanged).
    dmaBuf = PutTransfer(dmaBuf, VIOGPU_CMD_SCREEN_CHANGED, dst->GetId(), &coverRect, 0, 0);

    pPresent->pDmaBuffer = dmaBuf;

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDevice::Present(_Inout_ DXGKARG_PRESENT *pPresent)
{
    PAGED_CODE();

    if (pPresent->Flags.Flip)
    {
        // Nothing to run for a flip. The private data may still hold the pointer of an earlier
        // use of this buffer, which SubmitCommand would take for a command.
        if (pPresent->pDmaBufferPrivateData)
        {
            *(VioGpuCommand **)pPresent->pDmaBufferPrivateData = NULL;
        }
        return STATUS_SUCCESS;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s Flags=%s %s %s %s %s %s %s %s)\n",
              __FUNCTION__,
              pPresent->Flags.Blt ? "Blt" : "",
              pPresent->Flags.ColorFill ? "ColorFill" : "",
              pPresent->Flags.Flip ? "Flip" : "",
              pPresent->Flags.FlipWithNoWait ? "FlipWithNoWait" : "",
              pPresent->Flags.SrcColorKey ? "SrcColorKey" : "",
              pPresent->Flags.DstColorKey ? "DstColorKey" : "",
              pPresent->Flags.LinearToSrgb ? "LinearToSrgb" : "",
              pPresent->Flags.Rotate ? "Rotate" : ""));

    VioGpuCommand *cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
    if (pPresent->pDmaBuffer)
    {
        VioGpuCommand **privateData = (VioGpuCommand **)pPresent->pDmaBufferPrivateData;
        *privateData = cmd;
    }

    cmd->SetDmaBuf((char *)pPresent->pDmaBuffer);

    DXGK_ALLOCATIONLIST *dxgk_src = &pPresent->pAllocationList[DXGK_PRESENT_SOURCE_INDEX];
    DXGK_ALLOCATIONLIST *dxgk_dst = &pPresent->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX];

    VioGpuAllocation *src = NULL;
    VioGpuAllocation *dst = NULL;
    ;

    if (dxgk_src->hDeviceSpecificAllocation != NULL)
    {
        src = reinterpret_cast<VioGpuDeviceAllocation *>(dxgk_src->hDeviceSpecificAllocation)->GetAllocation();
        if (pPresent->pDmaBuffer)
        {
            pPresent->pPatchLocationListOut->AllocationIndex = DXGK_PRESENT_DESTINATION_INDEX;
            pPresent->pPatchLocationListOut->AllocationOffset = 0;
            pPresent->pPatchLocationListOut->DriverId = 1;
            pPresent->pPatchLocationListOut->SlotId = 1;
            pPresent->pPatchLocationListOut->PatchOffset = 0;
            pPresent->pPatchLocationListOut->SplitOffset = 0;

            pPresent->pPatchLocationListOut += 1;
        }
    }

    if (dxgk_dst->hDeviceSpecificAllocation != NULL)
    {
        dst = reinterpret_cast<VioGpuDeviceAllocation *>(dxgk_dst->hDeviceSpecificAllocation)->GetAllocation();
        if (pPresent->pDmaBuffer)
        {
            pPresent->pPatchLocationListOut->AllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
            pPresent->pPatchLocationListOut->AllocationOffset = 0;
            pPresent->pPatchLocationListOut->DriverId = 2;
            pPresent->pPatchLocationListOut->SlotId = 2;
            pPresent->pPatchLocationListOut->PatchOffset = 0;
            pPresent->pPatchLocationListOut->SplitOffset = 0;

            pPresent->pPatchLocationListOut += 1;
        }
    }

    if (pPresent->Flags.Blt)
    {
        if (pPresent->pDmaBuffer && dst && src)
        {
            NTSTATUS Status = GenerateBltPresent(pPresent, src, dst);
            if (!NT_SUCCESS(Status))
            {
                *(VioGpuCommand **)pPresent->pDmaBufferPrivateData = NULL;
                delete cmd;
                return Status;
            }
        }
    }
    else
    {
        if (pPresent->pDmaBuffer)
        {
            VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)pPresent->pDmaBuffer;
            cmd_hdr->type = VIOGPU_CMD_NOP;
            cmd_hdr->size = 0;
            pPresent->pDmaBuffer = (char *)pPresent->pDmaBuffer + sizeof(VIOGPU_COMMAND_HDR);
        }

        DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s Unsupported PRESENT\n", __FUNCTION__));
    }

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDevice::Render(DXGKARG_RENDER *pRender)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    char *pDmaBufStart = (char *)pRender->pDmaBuffer;
    PKEVENT pCompletionEvent = NULL;

    // The commands are copied as they are, and one output patch entry is written per input
    // entry: neither may be more than Windows gave room for.
    if (pRender->CommandLength > pRender->DmaSize || pRender->PatchLocationListInSize > pRender->PatchLocationListOutSize)
    {
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }

    __try
    {
        pRender->PatchLocationListOutSize = pRender->PatchLocationListInSize;
        for (UINT i = 0; i < pRender->PatchLocationListOutSize; i++)
        {
            pRender->pPatchLocationListOut->AllocationIndex = pRender->pPatchLocationListIn[i].AllocationIndex;
            pRender->pPatchLocationListOut->AllocationOffset = 0;
            pRender->pPatchLocationListOut->PatchOffset = 0;
            pRender->pPatchLocationListOut->SplitOffset = 0;
            pRender->pPatchLocationListOut->SlotId = i;

            pRender->pPatchLocationListOut++;
        }

        unsigned char *dmaBuf = (unsigned char *)pRender->pDmaBuffer;
        unsigned char *cmdBuf = (unsigned char *)pRender->pCommand;
        unsigned char *endBuf = cmdBuf + pRender->CommandLength;
        while (cmdBuf < endBuf)
        {
            if (cmdBuf + sizeof(VIOGPU_COMMAND_HDR) > endBuf)
            {
                if (pCompletionEvent)
                {
                    ObDereferenceObject(pCompletionEvent);
                }
                return STATUS_INVALID_USER_BUFFER;
            }

            memcpy(dmaBuf, cmdBuf, sizeof(VIOGPU_COMMAND_HDR));
            VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
            cmdBuf += sizeof(VIOGPU_COMMAND_HDR);
            dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

            if (cmdBuf + cmdHdr->size > endBuf)
            {
                if (pCompletionEvent)
                {
                    ObDereferenceObject(pCompletionEvent);
                }
                return STATUS_INVALID_USER_BUFFER;
            }

            // Copy command body
            memcpy(dmaBuf, cmdBuf, cmdHdr->size);

            // An event to set when this buffer has run. The handle belongs to the calling process,
            // which is the one we are in now and not the one the commands run in later, so the
            // event is looked up here, and the command itself is not passed on.
            if (cmdHdr->type == VIOGPU_CMD_SIGNAL_EVENT)
            {
                if (cmdHdr->size >= sizeof(VIOGPU_SIGNAL_CMD) && pCompletionEvent == NULL)
                {
                    HANDLE hEvent = (HANDLE)(ULONG_PTR)((VIOGPU_SIGNAL_CMD *)dmaBuf)->hEvent;
                    PVOID pEvent = NULL;
                    if (NT_SUCCESS(ObReferenceObjectByHandle(hEvent,
                                                             EVENT_MODIFY_STATE,
                                                             *ExEventObjectType,
                                                             UserMode,
                                                             &pEvent,
                                                             NULL)))
                    {
                        pCompletionEvent = (PKEVENT)pEvent;
                    }
                }
                cmdHdr->type = VIOGPU_CMD_NOP;
            }
            dmaBuf += cmdHdr->size;
            cmdBuf += cmdHdr->size;
        }
        pRender->pDmaBuffer = dmaBuf;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<---> %s Usermode copy exception", __FUNCTION__));
        if (pCompletionEvent)
        {
            ObDereferenceObject(pCompletionEvent);
        }
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuCommand *cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
    if (pRender->pDmaBuffer)
    {
        VioGpuCommand **privateData = (VioGpuCommand **)pRender->pDmaBufferPrivateData;
        *privateData = cmd;
    }
    cmd->SetDmaBuf(pDmaBufStart);
    cmd->SetCompletionEvent(pCompletionEvent);
    cmd->AttachAllocations(pRender->pAllocationList, pRender->AllocationListSize);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
};

NTSTATUS VioGpuDevice::OpenAllocation(_In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    for (UINT i = 0; i < pOpenAllocation->NumAllocations; i++)
    {
        DXGK_OPENALLOCATIONINFO *openAllocationInfo = &pOpenAllocation->pOpenAllocation[i];
        VioGpuAllocation *allocation = m_pAdapter->AllocationFromHandle(openAllocationInfo->hAllocation);
        openAllocationInfo->hDeviceSpecificAllocation = new (NonPagedPoolNx) VioGpuDeviceAllocation(this, allocation);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

CtrlQueue *VioGpuDevice::GetCtrlQueue()
{
    PAGED_CODE();

    return &m_pAdapter->ctrlQueue;
}

VioGpuDeviceAllocation::VioGpuDeviceAllocation(VioGpuDevice *device, VioGpuAllocation *allocation)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s res_id=%d ctx_id=%d\n", __FUNCTION__, allocation->GetId(), device->GetId()));

    m_pAllocation = allocation;
    m_pDevice = device;

    m_pDevice->GetCtrlQueue()->CtxResource(true, m_pDevice->GetId(), m_pAllocation->GetId());
}

VioGpuDeviceAllocation::~VioGpuDeviceAllocation()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s res_id=%d ctx_id=%d\n", __FUNCTION__, m_pAllocation->GetId(), m_pDevice->GetId()));

    m_pDevice->GetCtrlQueue()->CtxResource(false, m_pDevice->GetId(), m_pAllocation->GetId());
}

VioGpuAllocation *VioGpuDeviceAllocation::GetAllocation()
{
    PAGED_CODE();

    return m_pAllocation;
}

PAGED_CODE_SEG_END