#include "viogpu_command.h"
#include "viogpu_device.h"
#include "viogpu_adapter.h"
#include "baseobj.h"

#pragma code_seg(push)
#pragma code_seg()

VioGpuCommand::VioGpuCommand(VioGpuAdapter *adapter)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_pAdapter = adapter;
    m_pCommander = &adapter->commander;
    m_pContext = NULL;

    m_FenceId = 0;
    m_pDmaBuffer = NULL;
    m_pCommand = NULL;
    m_pEnd = NULL;

    m_allocations = NULL;
    m_allocationsLength = 0;
    m_pCompletionEvent = NULL;

    list_entry.Blink = NULL;
    list_entry.Flink = NULL;
};

void VioGpuCommand::PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_FenceId = pSubmitCommand->SubmissionFenceId;
    if (m_pDmaBuffer)
    {
        m_pCommand = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionStartOffset;
        m_pEnd = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionEndOffset;
    }
    m_pContext = reinterpret_cast<VioGpuDevice *>(pSubmitCommand->hContext);
}

#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

void VioGpuCommand::Run()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    while (m_pCommand < m_pEnd)
    {

        VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)m_pCommand;
        m_pCommand += sizeof(VIOGPU_COMMAND_HDR);

        void *cmdBody = m_pCommand;
        m_pCommand += cmdHdr->size;

        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s fence_id=%d running command=%d", __FUNCTION__, m_FenceId, cmdHdr->type));

        switch (cmdHdr->type)
        {
            case VIOGPU_CMD_SUBMIT:
                {
                    PBYTE submitCmd = new (NonPagedPoolNx) BYTE[cmdHdr->size];
                    RtlCopyMemory(submitCmd, cmdBody, cmdHdr->size);

                    m_pAdapter->ctrlQueue.SubmitCommand(submitCmd,
                                                        cmdHdr->size,
                                                        m_pContext->GetId(),
                                                        VioGpuCommand::QueueRunningCb,
                                                        this);
                    return;
                }

            case VIOGPU_CMD_TRANSFER_TO_HOST:
            case VIOGPU_CMD_TRANSFER_FROM_HOST:
                {
                    VIOGPU_TRANSFER_CMD *transferCmd = (VIOGPU_TRANSFER_CMD *)cmdBody;

                    m_pAdapter->ctrlQueue.TransferHostCmd(cmdHdr->type == VIOGPU_CMD_TRANSFER_TO_HOST,
                                                          m_pContext->GetId(),
                                                          transferCmd,
                                                          VioGpuCommand::QueueRunningCb,
                                                          this);
                    return;
                }

            case VIOGPU_CMD_SCREEN_CHANGED:
                {
                    VIOGPU_TRANSFER_CMD *changed = (VIOGPU_TRANSFER_CMD *)cmdBody;
                    m_pAdapter->vidpn.ScreenChanged(changed->res_id, &changed->box);
                    break;
                }

            case VIOGPU_CMD_NOP:
            default:
                {
                    // DO NOTHING
                    break;
                }
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("%s finished fence_id=%d", __FUNCTION__, m_FenceId));

    if (m_allocations)
    {
        for (UINT i = 0; i < m_allocationsLength; i++)
        {
            if (m_allocations[i])
            {
                m_allocations[i]->UnmarkBusy();
            }
        }
        delete m_allocations;
    }

    if (m_pCompletionEvent)
    {
        KeSetEvent(m_pCompletionEvent, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(m_pCompletionEvent);
        m_pCompletionEvent = NULL;
    }

    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt;
    interrupt.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    interrupt.DmaCompleted.SubmissionFenceId = m_FenceId;
    interrupt.DmaCompleted.NodeOrdinal = 0;
    interrupt.DmaCompleted.EngineOrdinal = 0;
    m_pAdapter->NotifyInterrupt(&interrupt, true);

    m_pCommander->CommandFinished(m_FenceId);

    delete this;
}

void VioGpuCommand::AttachAllocations(DXGK_ALLOCATIONLIST *allocationList, UINT allocationListLength)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_allocations = new (NonPagedPoolNx) VioGpuAllocation *[allocationListLength];
    m_allocationsLength = allocationListLength;
    for (UINT i = 0; i < allocationListLength; i++)
    {
        VioGpuDeviceAllocation *deviceAllocation = reinterpret_cast<VioGpuDeviceAllocation *>(allocationList[i].hDeviceSpecificAllocation);
        if (deviceAllocation)
        {
            m_allocations[i] = deviceAllocation->GetAllocation();
            m_allocations[i]->MarkBusy();
        }
        else
        {
            m_allocations[i] = NULL;
        }
    }
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()

void VioGpuCommand::QueueRunning()
{
    m_pCommander->QueueRunning(this);
}

void VioGpuCommand::QueueRunningCb(void *cmd)
{
    ((VioGpuCommand *)cmd)->QueueRunning();
}

#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

VioGpuCommander::VioGpuCommander(VioGpuAdapter *pAdapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_pAdapter = pAdapter;
    m_bStopWorkThread = FALSE;
    m_pWorkThread = NULL;

    KeInitializeEvent(&m_QueueEvent, SynchronizationEvent, FALSE);

    InitializeListHead(&m_SubmittedQueue);
    InitializeListHead(&m_RunningQueue);
    KeInitializeSpinLock(&m_Lock);

    m_running = 0;
    m_lastCompletedFence = 0;
    m_preemptPending = FALSE;
    m_preemptFenceId = 0;
}

NTSTATUS VioGpuCommander::Start()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    HANDLE threadHandle = 0;

    m_bStopWorkThread = FALSE;
    NTSTATUS status = PsCreateSystemThread(&threadHandle,
                                           (ACCESS_MASK)0,
                                           NULL,
                                           (HANDLE)0,
                                           NULL,
                                           VioGpuCommander::ThreadWork,
                                           this);

    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to create command worker thread, status %x\n", __FUNCTION__, status));
        VioGpuDbgBreak();
        return status;
    }

    ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)(&m_pWorkThread), NULL);

    ZwClose(threadHandle);

    return status;
}

void VioGpuCommander::Stop()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    // Not started, or stopped already (the device can be removed without having been stopped)
    if (m_pWorkThread == NULL)
    {
        return;
    }

    // The thread runs driver code: it has to be gone before the driver can be unloaded, however
    // long that takes.
    m_bStopWorkThread = TRUE;
    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(m_pWorkThread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(m_pWorkThread);
    m_pWorkThread = NULL;
}

void VioGpuCommander::ThreadWork(PVOID Context)
{
    PAGED_CODE();

    VioGpuCommander *pdev = reinterpret_cast<VioGpuCommander *>(Context);
    pdev->ThreadWorkRoutine();
}

void VioGpuCommander::ThreadWorkRoutine(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    for (;;)
    {
        KeWaitForSingleObject(&m_QueueEvent, Executive, KernelMode, FALSE, NULL);

        if (m_bStopWorkThread)
        {
            PsTerminateSystemThread(STATUS_SUCCESS);
            break;
        }

        while (m_running < VIOGPU_MAX_RUNNING)
        {
            VioGpuCommand *command = DequeueSubmitted();
            if (command == NULL)
            {
                break;
            }
            QueueRunning(command);
            m_running++;
        }

        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s Running command\n", __FUNCTION__));
        while (true)
        {
            VioGpuCommand *command = DequeueRunning();
            if (command == NULL)
            {
                break;
            }
            command->Run();
        }
    }
}

NTSTATUS VioGpuCommander::Patch(const DXGKARG_PATCH *pPatch)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s \n", __FUNCTION__));

    UNREFERENCED_PARAMETER(pPatch);

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()
// Under the queue lock: nothing submitted that has not finished
BOOLEAN VioGpuCommander::IdleLocked()
{
    return m_running == 0 && IsListEmpty(&m_SubmittedQueue) && IsListEmpty(&m_RunningQueue);
}

void VioGpuCommander::ReportPreempted(UINT preemptionFenceId)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt;
    RtlZeroMemory(&interrupt, sizeof(interrupt));
    interrupt.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    interrupt.DmaPreempted.PreemptionFenceId = preemptionFenceId;
    interrupt.DmaPreempted.LastCompletedFenceId = (UINT)m_lastCompletedFence;
    interrupt.DmaPreempted.NodeOrdinal = 0;
    interrupt.DmaPreempted.EngineOrdinal = 0;
    m_pAdapter->NotifyInterrupt(&interrupt, true);
}

// Not pageable: it takes the queue's spin lock.
void VioGpuCommander::CommandFinished(UINT fenceId)
{
    KIRQL irql;
    BOOLEAN report = FALSE;
    UINT preemptionFenceId = 0;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s fence_id=%u", __FUNCTION__, fenceId));

    LockQueue(&irql);
    m_running--;
    InterlockedExchange(&m_lastCompletedFence, (LONG)fenceId);
    if (m_preemptPending && IdleLocked())
    {
        report = TRUE;
        preemptionFenceId = m_preemptFenceId;
        m_preemptPending = FALSE;
    }
    UnlockQueue(irql);

    if (report)
    {
        ReportPreempted(preemptionFenceId);
    }
    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
}

void VioGpuCommander::Preempt(UINT preemptionFenceId)
{
    KIRQL irql;
    BOOLEAN now;

    LockQueue(&irql);
    now = IdleLocked();
    if (!now)
    {
        m_preemptPending = TRUE;
        m_preemptFenceId = preemptionFenceId;
    }
    UnlockQueue(irql);

    if (now)
    {
        ReportPreempted(preemptionFenceId);
    }
}

NTSTATUS VioGpuCommander::SubmitCommand(
const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    VIOGPU_ASSERT(pSubmitCommand != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s fence_id=%d\n", __FUNCTION__, pSubmitCommand->SubmissionFenceId));

    VioGpuCommand *cmd = NULL;
    if (pSubmitCommand->pDmaBufferPrivateData)
    {
        VioGpuCommand **priv = (VioGpuCommand **)pSubmitCommand->pDmaBufferPrivateData;
        if (*priv != NULL)
        {
            cmd = *priv;
        }
    }

    if (!cmd)
    {
        cmd = new (NonPagedPoolNx) VioGpuCommand(m_pAdapter);
    }

    cmd->PrepareSubmit(pSubmitCommand);
    QueueSubmitted(cmd);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql,
                                                        Irql) _IRQL_raises_(DISPATCH_LEVEL) void VioGpuCommander::
                                                                                                    LockQueue(KIRQL *Irql)
{
    KIRQL SavedIrql = KeGetCurrentIrql();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, SavedIrql));

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_Lock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_Lock);
    }
    else
    {
        VioGpuDbgBreak();
    }
    *Irql = SavedIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

_IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void VioGpuCommander::UnlockQueue(KIRQL Irql)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, Irql));

    if (Irql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_Lock, Irql);
    }
    else
    {
        KeReleaseSpinLockFromDpcLevel(&m_Lock);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuCommand *VioGpuCommander::DequeueRunning()
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    PLIST_ENTRY result = NULL;
    if (!IsListEmpty(&m_RunningQueue))
    {
        result = RemoveHeadList(&m_RunningQueue);
    }
    UnlockQueue(oldIrql);
    if (!result)
    {
        return NULL;
    }
    return CONTAINING_RECORD(result, VioGpuCommand, list_entry);
}

void VioGpuCommander::QueueRunning(VioGpuCommand *cmd)
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    InsertTailList(&m_RunningQueue, &cmd->list_entry);
    UnlockQueue(oldIrql);

    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
}

VioGpuCommand *VioGpuCommander::DequeueSubmitted()
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    PLIST_ENTRY result = NULL;
    if (!IsListEmpty(&m_SubmittedQueue))
    {
        result = RemoveHeadList(&m_SubmittedQueue);
    }
    UnlockQueue(oldIrql);
    if (!result)
    {
        return NULL;
    }
    return CONTAINING_RECORD(result, VioGpuCommand, list_entry);
}

void VioGpuCommander::QueueSubmitted(VioGpuCommand *cmd)
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    InsertTailList(&m_SubmittedQueue, &cmd->list_entry);
    UnlockQueue(oldIrql);

    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
}

#pragma code_seg(pop)