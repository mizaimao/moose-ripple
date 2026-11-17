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
    m_epoch = 0;
    m_queuedAll = FALSE;
    done = FALSE;
    reported = FALSE;
    sent = FALSE;
    earlyOK = FALSE;
#ifdef RIPPLE_TRACE_TIMING
    RtlZeroMemory(t, sizeof(t));
    t[0] = KeQueryPerformanceCounter(NULL).QuadPart;
#endif
    m_pDmaBuffer = NULL;
    m_pCommand = NULL;
    m_pEnd = NULL;

    m_allocations = NULL;
    m_allocationsLength = 0;
    m_pCompletionEvent = NULL;

    list_entry.Blink = NULL;
    list_entry.Flink = NULL;
    active_entry.Blink = NULL;
    active_entry.Flink = NULL;
    active = FALSE;
};

void VioGpuCommand::PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_FenceId = pSubmitCommand->SubmissionFenceId;
    m_epoch = m_pCommander->Epoch();
    // Paging buffers are not reported early: Windows frees or moves memory once they are
    // done, and the host may still write into it. Presents neither: Windows times the screen
    // by them.
    earlyOK = !pSubmitCommand->Flags.Paging && !pSubmitCommand->Flags.Present && !pSubmitCommand->Flags.Flip;
#ifdef RIPPLE_TRACE_TIMING
    t[1] = KeQueryPerformanceCounter(NULL).QuadPart;
#endif
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

    // Sent before a reset (the host's late answer, or the rest of the buffer): dropped
    if (m_epoch != m_pCommander->Epoch())
    {
        Abandon();
        return;
    }

    if (!m_queuedAll)
    {
#ifdef RIPPLE_TRACE_TIMING
        t[2] = KeQueryPerformanceCounter(NULL).QuadPart;
#endif
        // Every part goes to the host at once and in order, the next buffer right after: the host
        // runs them in queue order. Only the last part sent to the host is fenced and comes back
        // here, once the host GPU has done it and everything before it. Waiting for each part,
        // as before, left the guest idle for a host round trip per part, and QEMU notices a
        // finished fence only at the guest's next notification or its 10 ms poll.
        char *last = NULL;
        for (char *p = m_pCommand; p < m_pEnd; p += sizeof(VIOGPU_COMMAND_HDR) + ((VIOGPU_COMMAND_HDR *)p)->size)
        {
            UINT type = ((VIOGPU_COMMAND_HDR *)p)->type;
            if (type == VIOGPU_CMD_SUBMIT || type == VIOGPU_CMD_TRANSFER_TO_HOST || type == VIOGPU_CMD_TRANSFER_FROM_HOST)
            {
                last = p;
            }
        }
        m_queuedAll = TRUE;

        m_pAdapter->ctrlQueue.DeferKicks(TRUE);
        while (m_pCommand < m_pEnd)
        {
            VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)m_pCommand;
            BOOLEAN isLast = (m_pCommand == last);
            m_pCommand += sizeof(VIOGPU_COMMAND_HDR);

            void *cmdBody = m_pCommand;
            m_pCommand += cmdHdr->size;

            DbgPrint(TRACE_LEVEL_VERBOSE, ("%s fence_id=%d running command=%d", __FUNCTION__, m_FenceId, cmdHdr->type));

            switch (cmdHdr->type)
            {
                case VIOGPU_CMD_SUBMIT:
                    {
#ifdef RIPPLE_TRACE_TIMING
                        LONGLONG c0 = KeQueryPerformanceCounter(NULL).QuadPart;
#endif
                        // A buffer reported early goes back to Windows before the host has read it,
                        // so its commands are copied (into reused memory: fresh memory costs a VM
                        // exit a page under nested virtualization). The others are sent from the
                        // DMA buffer itself, which Windows keeps until they are reported.
                        PBYTE submitCmd;
                        BOOLEAN borrowed = !earlyOK;
                        if (earlyOK)
                        {
                            submitCmd = (PBYTE)m_pCommander->AllocCopy(cmdHdr->size);
                            if (submitCmd)
                            {
                                RtlCopyMemory(submitCmd, cmdBody, cmdHdr->size);
                            }
                            else
                            {
                                submitCmd = (PBYTE)cmdBody;
                                borrowed = TRUE;
                                earlyOK = FALSE;
                            }
                        }
                        else
                        {
                            submitCmd = (PBYTE)cmdBody;
                        }
#ifdef RIPPLE_TRACE_TIMING
                        LONGLONG c1 = KeQueryPerformanceCounter(NULL).QuadPart;
#endif

                        m_pAdapter->ctrlQueue.SubmitCommand(submitCmd,
                                                            cmdHdr->size,
                                                            m_pContext->GetId(),
                                                            isLast ? VioGpuCommand::QueueRunningCb : NULL,
                                                            isLast ? this : NULL,
                                                            isLast,
                                                            borrowed,
                                                            borrowed ? NULL : VioGpuCommander::FreeCopy,
                                                            m_pCommander);
#ifdef RIPPLE_TRACE_TIMING
                        m_pCommander->m_tCopy += c1 - c0;
                        m_pCommander->m_tQueue += KeQueryPerformanceCounter(NULL).QuadPart - c1;
                        m_pCommander->m_tParts++;
                        m_pCommander->m_tBytes += cmdHdr->size;
#endif
                        break;
                    }

                case VIOGPU_CMD_TRANSFER_TO_HOST:
                case VIOGPU_CMD_TRANSFER_FROM_HOST:
                    {
                        VIOGPU_TRANSFER_CMD *transferCmd = (VIOGPU_TRANSFER_CMD *)cmdBody;
#ifdef RIPPLE_TRACE_TIMING
                        m_pCommander->m_tTransfers++;
#endif

                        m_pAdapter->ctrlQueue.TransferHostCmd(cmdHdr->type == VIOGPU_CMD_TRANSFER_TO_HOST,
                                                              m_pContext->GetId(),
                                                              transferCmd,
                                                              isLast ? VioGpuCommand::QueueRunningCb : NULL,
                                                              isLast ? this : NULL,
                                                              isLast);
                        break;
                    }

                case VIOGPU_CMD_SCREEN_CHANGED:
                    {
                        // Only noted here; the flush that shows it is queued after this buffer's parts
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

        m_pAdapter->ctrlQueue.DeferKicks(FALSE);
        if (earlyOK)
        {
            m_pCommander->Sent(this);
        }
#ifdef RIPPLE_TRACE_TIMING
        t[3] = t[4] = KeQueryPerformanceCounter(NULL).QuadPart;
#endif

        if (last != NULL)
        {
            // Back here when the host has done the last part
            return;
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("%s finished fence_id=%d", __FUNCTION__, m_FenceId));

#ifdef RIPPLE_TEST_TDR
    LONG stall = InterlockedExchange(&m_pCommander->m_testStallMs, 0);
    if (stall > 0)
    {
        // As a host that is slow with this buffer: it finishes late, at the latest when Windows
        // has given up on it and reset
        LARGE_INTEGER delay;
        delay.QuadPart = Int32x32To64(100, -10000);
        DbgPrint(TRACE_LEVEL_ERROR, ("%s test: fence_id=%u held back %d ms\n", __FUNCTION__, m_FenceId, stall));
        for (LONG waited = 0; waited < stall && m_epoch == m_pCommander->Epoch(); waited += 100)
        {
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
    }
#endif

#ifdef RIPPLE_TRACE_TIMING
    t[5] = KeQueryPerformanceCounter(NULL).QuadPart;
    m_pCommander->Timing(this);
#endif
    Release();
    // Reported to Windows in order, which may be later, and deleted then
    m_pCommander->Complete(this);
}

void VioGpuCommand::Abandon()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_WARNING, ("%s fence_id=%d dropped by a reset\n", __FUNCTION__, m_FenceId));
    Release();
    m_pCommander->Retire(this);
    delete this;
}

void VioGpuCommand::Release()
{
    PAGED_CODE();

    VioGpuCommandHeld held;
    TakeHeld(&held);
    ReleaseHeld(&held);
}

void VioGpuCommand::ReleaseHeld(VioGpuCommandHeld *held)
{
    PAGED_CODE();

    if (held->allocations)
    {
        for (UINT i = 0; i < held->length; i++)
        {
            if (held->allocations[i])
            {
                held->allocations[i]->UnmarkBusy();
            }
        }
        delete held->allocations;
        held->allocations = NULL;
    }
    if (held->event)
    {
        KeSetEvent(held->event, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(held->event);
        held->event = NULL;
    }
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

void VioGpuCommand::TakeHeld(VioGpuCommandHeld *held)
{
    held->length = m_allocationsLength;
    held->allocations = (VioGpuAllocation **)InterlockedExchangePointer((PVOID volatile *)&m_allocations, NULL);
    held->event = (PKEVENT)InterlockedExchangePointer((PVOID volatile *)&m_pCompletionEvent, NULL);
}

void VioGpuCommand::QueueRunning()
{
    m_pCommander->QueueRunning(this);
}

void VioGpuCommand::QueueRunningCb(void *cmd)
{
#ifdef RIPPLE_TRACE_TIMING
    ((VioGpuCommand *)cmd)->t[4] = KeQueryPerformanceCounter(NULL).QuadPart;
#endif
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
    InitializeListHead(&m_ActiveQueue);
    KeInitializeSpinLock(&m_Lock);

    m_running = 0;
    m_lastCompletedFence = 0;
    m_epoch = 0;
    KeInitializeGuardedMutex(&m_ResetMutex);
    m_copyList = (PNPAGED_LOOKASIDE_LIST)ExAllocatePoolWithTag(NonPagedPool, sizeof(NPAGED_LOOKASIDE_LIST), 'cGIV');
    if (m_copyList)
    {
        ExInitializeNPagedLookasideList(m_copyList, NULL, NULL, 0, 64 * 1024, 'cGIV', 0);
    }
#ifdef RIPPLE_TEST_TDR
    m_testStallMs = 0;
#endif
#ifdef RIPPLE_TRACE_TIMING
    RtlZeroMemory(m_tSum, sizeof(m_tSum));
    RtlZeroMemory(m_tMax, sizeof(m_tMax));
    m_tCount = 0;
    m_tParts = m_tBytes = m_tTransfers = 0;
    m_tCopy = m_tQueue = 0;
    m_tDepthSum = m_tDepthMax = m_tWaitingMax = 0;
#endif
    m_preemptPending = FALSE;
    m_preemptFenceId = 0;
}

VioGpuCommander::~VioGpuCommander()
{
    PAGED_CODE();
    if (m_copyList)
    {
        ExDeleteNPagedLookasideList(m_copyList);
        ExFreePoolWithTag(m_copyList, 'cGIV');
        m_copyList = NULL;
    }
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

    // While buffers are with the host, the queue is nudged every half millisecond (Windows'
    // finest timer): QEMU notices a finished fence at the guest's next notification or its own
    // 10 ms poll, so a program that waits for the GPU every frame was held to about 100 frames a
    // second. A program polling a fence also asks for a notification (VIOGPU_NUDGE).
    //
    // The system timer runs that fine only for a stream of work: buffers with the host at
    // moments spread over 60 ms or more, with no pause over 100 ms between them (frames at 10
    // a second or faster, or one long job). It goes back 200 ms after the last buffer. A frame
    // now and then is left to the ordinary timer and QEMU's poll, 10 ms late at worst: with
    // the fine timer taken for any buffer and kept for a second, a blinking text cursor (two
    // buffers 30 ms apart every half second) held the whole guest at 2048 clock interrupts a
    // second, two thirds of a host processor for an idle desktop.
    // The times are the interrupt time, which moves in steps of the timer (15.6 ms when
    // coarse), so the cursor's 30 ms can read as 47: hence 60.
    const ULONGLONG streamGap = 100 * 10000, streamMin = 60 * 10000;
    ULONGLONG streamStart = 0, lastWork = 0;
    BOOLEAN fineTimer = FALSE;
    for (;;)
    {
        LARGE_INTEGER nudge, idle;
        nudge.QuadPart = -5000;
        idle.QuadPart = Int32x32To64(200, -10000);
        BOOLEAN busy = m_running > 0;
        if (busy)
        {
            ULONGLONG now = KeQueryInterruptTime();
            if (now - lastWork > streamGap)
            {
                streamStart = now;
            }
            lastWork = now;
            if (!fineTimer && now - streamStart >= streamMin)
            {
                ExSetTimerResolution(5000, TRUE);
                fineTimer = TRUE;
            }
        }
        NTSTATUS wait = KeWaitForSingleObject(&m_QueueEvent,
                                              Executive,
                                              KernelMode,
                                              FALSE,
                                              busy ? &nudge : (fineTimer ? &idle : NULL));

        if (m_bStopWorkThread)
        {
            if (fineTimer)
            {
                ExSetTimerResolution(0, FALSE);
            }
            PsTerminateSystemThread(STATUS_SUCCESS);
            break;
        }
        if (wait == STATUS_TIMEOUT)
        {
            if (busy)
            {
                m_pAdapter->ctrlQueue.Nudge();
            }
            else if (m_running == 0)
            {
                ExSetTimerResolution(0, FALSE);
                fineTimer = FALSE;
            }
        }

        while (true)
        {
            VioGpuCommand *command = DequeueSubmittedToRun();
            if (command == NULL)
            {
                break;
            }
            QueueRunning(command);
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

// A buffer has finished on the host (or never needed it): what it held is given back already
// (Release); here it counts as done, is reported if not yet, and is deleted once it and all
// before it are done and reported. Under the reset lock, so that a reset cannot come between.
void VioGpuCommander::Complete(VioGpuCommand *cmd)
{
    PAGED_CODE();

    KIRQL irql;
    KeAcquireGuardedMutex(&m_ResetMutex);
    if (cmd->Epoch() != m_epoch)
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("%s fence_id=%u from before a reset, not reported\n", __FUNCTION__, cmd->FenceId()));
        Retire(cmd);
        KeReleaseGuardedMutex(&m_ResetMutex);
        delete cmd;
        return;
    }
    LockQueue(&irql);
    cmd->done = TRUE;
    UnlockQueue(irql);
    // Reported before the count drops: CommandFinished may report a preemption, and Windows takes
    // every buffer not reported done by then as preempted and submits it again (the same DMA
    // buffer, so the same command object, run and freed twice)
    ReportReady();
    CommandFinished();

    for (;;)
    {
        VioGpuCommand *head = NULL;
        LockQueue(&irql);
        if (!IsListEmpty(&m_ActiveQueue))
        {
            head = CONTAINING_RECORD(m_ActiveQueue.Flink, VioGpuCommand, active_entry);
            if (head->done && head->reported)
            {
                RemoveEntryList(&head->active_entry);
                head->active = FALSE;
            }
            else
            {
                head = NULL;
            }
        }
        UnlockQueue(irql);
        if (head == NULL)
        {
            break;
        }
        delete head;
    }
    KeReleaseGuardedMutex(&m_ResetMutex);
}

void VioGpuCommander::Sent(VioGpuCommand *cmd)
{
    PAGED_CODE();

    KIRQL irql;
    KeAcquireGuardedMutex(&m_ResetMutex);
    if (cmd->Epoch() == m_epoch)
    {
        LockQueue(&irql);
        cmd->sent = TRUE;
        UnlockQueue(irql);
        ReportReady();
    }
    KeReleaseGuardedMutex(&m_ResetMutex);
}

// Windows hears of buffers in submission order: each one, once it and all before it are either
// done on the host or, for rendering buffers, all with the host. A rendering buffer's DMA buffer
// can then be reused at once and the scheduler hands over the next buffers, instead of two at a
// time each waiting for a round trip to the host. What Windows and programs wait on for real
// (completion events, allocations' busy marks) still waits for the host. Under the reset lock.
void VioGpuCommander::ReportReady(void)
{
    PAGED_CODE();

    for (;;)
    {
        UINT fences[16];
        UINT n = 0;
        KIRQL irql;
        LockQueue(&irql);
        for (PLIST_ENTRY e = m_ActiveQueue.Flink; e != &m_ActiveQueue && n < ARRAYSIZE(fences); e = e->Flink)
        {
            VioGpuCommand *c = CONTAINING_RECORD(e, VioGpuCommand, active_entry);
            if (c->reported)
            {
                continue;
            }
            if (!c->done && !(c->earlyOK && c->sent))
            {
                break;
            }
            c->reported = TRUE;
            fences[n++] = c->FenceId();
        }
        UnlockQueue(irql);
        if (n == 0)
        {
            break;
        }
        for (UINT i = 0; i < n; i++)
        {
            DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt;
            RtlZeroMemory(&interrupt, sizeof(interrupt));
            interrupt.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
            interrupt.DmaCompleted.SubmissionFenceId = fences[i];
            InterlockedExchange(&m_lastCompletedFence, (LONG)fences[i]);
            m_pAdapter->NotifyInterrupt(&interrupt, true);
        }
        if (n < ARRAYSIZE(fences))
        {
            break;
        }
    }
}

void VioGpuCommander::Reset()
{
    PAGED_CODE();

    LIST_ENTRY dropped;
    KIRQL irql;
    UINT count = 0;

    InitializeListHead(&dropped);
    KeAcquireGuardedMutex(&m_ResetMutex);
    LockQueue(&irql);
    InterlockedIncrement(&m_epoch);
    while (!IsListEmpty(&m_SubmittedQueue))
    {
        InsertTailList(&dropped, RemoveHeadList(&m_SubmittedQueue));
    }
    while (!IsListEmpty(&m_RunningQueue))
    {
        InsertTailList(&dropped, RemoveHeadList(&m_RunningQueue));
    }
    // A buffer still with the host no longer counts: its answer will be dropped
    m_running = 0;
    m_preemptPending = FALSE;
    UnlockQueue(irql);

    // What the buffers taken to run hold, a few at a time (the queue lock is a spin lock and
    // giving them up is pageable)
    for (;;)
    {
        VioGpuCommandHeld held[4];
        VioGpuCommand *done[4];
        UINT n = 0;
        LockQueue(&irql);
        while (n < 4 && !IsListEmpty(&m_ActiveQueue))
        {
            VioGpuCommand *cmd = CONTAINING_RECORD(RemoveHeadList(&m_ActiveQueue), VioGpuCommand, active_entry);
            cmd->active = FALSE;
            cmd->TakeHeld(&held[n]);
            // Done and only waiting for its turn: nothing else refers to it
            done[n++] = cmd->done ? cmd : NULL;
        }
        UnlockQueue(irql);
        if (n == 0)
        {
            break;
        }
        for (UINT i = 0; i < n; i++)
        {
            VioGpuCommand::ReleaseHeld(&held[i]);
            delete done[i];
        }
    }
    KeReleaseGuardedMutex(&m_ResetMutex);

    while (!IsListEmpty(&dropped))
    {
        CONTAINING_RECORD(RemoveHeadList(&dropped), VioGpuCommand, list_entry)->Abandon();
        count++;
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("%s GPU timeout: reset, %u command buffers dropped\n", __FUNCTION__, count));
}

#ifdef RIPPLE_TRACE_TIMING
// Test builds (build.sh TRACE_TIMING=1): where a command buffer's time goes, averaged and
// at most over 2000 buffers, in microseconds
void VioGpuCommander::Timing(VioGpuCommand *cmd)
{
    LARGE_INTEGER freq;
    KeQueryPerformanceCounter(&freq);
    for (int i = 0; i < 5; i++)
    {
        LONGLONG d = cmd->t[i + 1] - cmd->t[i];
        if (cmd->t[i] == 0 || cmd->t[i + 1] == 0 || d < 0)
            d = 0;
        d = d * 1000000 / freq.QuadPart;
        m_tSum[i] += d;
        if (d > m_tMax[i])
            m_tMax[i] = d;
    }
    if (++m_tCount == 2000)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("T us avg/max: render-submit %lld/%lld submit-run %lld/%lld run-sent %lld/%lld host %lld/%lld answer-finish %lld/%lld\n",
                  m_tSum[0] / 2000, m_tMax[0], m_tSum[1] / 2000, m_tMax[1], m_tSum[2] / 2000, m_tMax[2],
                  m_tSum[3] / 2000, m_tMax[3], m_tSum[4] / 2000, m_tMax[4]));
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("T per buffer: %u submits (%u bytes), %u transfers; copy %lld us, queue %lld us\n",
                  m_tParts / 2000, m_tBytes / 2000, m_tTransfers / 2000, m_tCopy * 1000000 / freq.QuadPart / 2000,
                  m_tQueue * 1000000 / freq.QuadPart / 2000));
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("T with the host when one more is taken: avg %u.%02u, max %u; handed over and waiting, max %u\n",
                  m_tDepthSum / 2000, (m_tDepthSum % 2000) / 20, m_tDepthMax, m_tWaitingMax));
        m_tDepthSum = m_tDepthMax = m_tWaitingMax = 0;
        m_tParts = m_tBytes = m_tTransfers = 0;
        m_tCopy = m_tQueue = 0;
        RtlZeroMemory(m_tSum, sizeof(m_tSum));
        RtlZeroMemory(m_tMax, sizeof(m_tMax));
        m_tCount = 0;
    }
}
#endif


#pragma code_seg(push)
#pragma code_seg()
#define VIOGPU_COPY_SMALL (64 * 1024)
PVOID VioGpuCommander::AllocCopy(ULONG size)
{
    // A header in front says which way it was allocated
    PULONG p;
    if (m_copyList && size + 16 <= VIOGPU_COPY_SMALL)
    {
        p = (PULONG)ExAllocateFromNPagedLookasideList(m_copyList);
        if (p)
        {
            p[0] = 1;
            return (PBYTE)p + 16;
        }
    }
    p = (PULONG) new (NonPagedPoolNx) BYTE[size + 16];
    if (!p)
        return NULL;
    p[0] = 0;
    return (PBYTE)p + 16;
}

void VioGpuCommander::FreeCopy(void *ctx, void *data)
{
    VioGpuCommander *self = (VioGpuCommander *)ctx;
    PULONG p = (PULONG)((PBYTE)data - 16);
    if (p[0] == 1)
        ExFreeToNPagedLookasideList(self->m_copyList, p);
    else
        delete[] (PBYTE)p;
}
#pragma code_seg(pop)

void VioGpuCommander::Restart()
{
    PAGED_CODE();
    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
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
// One buffer fewer with the host
void VioGpuCommander::CommandFinished(void)
{
    KIRQL irql;
    BOOLEAN report = FALSE;
    UINT preemptionFenceId = 0;

    LockQueue(&irql);
    m_running--;
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

// Out of the list of buffers taken to run, unless a reset took it out already
void VioGpuCommander::Retire(VioGpuCommand *cmd)
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    if (cmd->active)
    {
        RemoveEntryList(&cmd->active_entry);
        cmd->active = FALSE;
    }
    UnlockQueue(oldIrql);
}

// The next buffer to run, if fewer than the most allowed are running; it counts as running
// from here, in the same lock as a reset's recount
VioGpuCommand *VioGpuCommander::DequeueSubmittedToRun()
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    PLIST_ENTRY result = NULL;
    if (m_running < VIOGPU_MAX_RUNNING && !IsListEmpty(&m_SubmittedQueue))
    {
        result = RemoveHeadList(&m_SubmittedQueue);
        m_running++;
#ifdef RIPPLE_TRACE_TIMING
        {
            UINT waiting = 0;
            for (PLIST_ENTRY e = m_SubmittedQueue.Flink; e != &m_SubmittedQueue; e = e->Flink)
                waiting++;
            m_tDepthSum += m_running;
            m_tDepthMax = max(m_tDepthMax, m_running);
            m_tWaitingMax = max(m_tWaitingMax, waiting);
        }
#endif
        VioGpuCommand *cmd = CONTAINING_RECORD(result, VioGpuCommand, list_entry);
        InsertTailList(&m_ActiveQueue, &cmd->active_entry);
        cmd->active = TRUE;
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