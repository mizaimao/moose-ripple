#pragma once

#include "helper.h"

#define VIOGPU_MAX_RUNNING 1

class VioGpuAdapter;
class VioGpuDevice;
class VioGpuAllocation;
class VioGpuCommander;

class VioGpuCommand
{
  public:
    VioGpuCommand(VioGpuAdapter *adapter);

    void Run();

    void PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand);
    void QueueRunning();
    static void QueueRunningCb(void *cmd);

    void SetDmaBuf(char *pDmaBuffer)
    {
        m_pDmaBuffer = pDmaBuffer;
    }

    void AttachAllocations(DXGK_ALLOCATIONLIST *allocationList, UINT allocationListLength);

    // An event to set when the command has run; the reference to it is given to the command.
    void SetCompletionEvent(PKEVENT pEvent)
    {
        m_pCompletionEvent = pEvent;
    }

    LIST_ENTRY list_entry;

  private:
    VioGpuAdapter *m_pAdapter;
    VioGpuCommander *m_pCommander;
    VioGpuDevice *m_pContext;

    UINT m_FenceId;

    char *m_pDmaBuffer;
    char *m_pCommand;
    char *m_pEnd;

    VioGpuAllocation **m_allocations;
    UINT m_allocationsLength;

    PKEVENT m_pCompletionEvent;
};

class VioGpuCommander
{
  public:
    VioGpuCommander(VioGpuAdapter *pAdapter);

    NTSTATUS Start();
    void Stop();

    void CommandFinished(UINT fenceId);

    // Preemption as this driver can do it: the host cannot stop a command buffer half way, so
    // a preemption request is reported done once everything submitted has finished
    // (DxgkDdiPreemptCommand). And the fence of the last finished buffer
    // (DxgkDdiQueryCurrentFence).
    void Preempt(UINT preemptionFenceId);
    UINT LastCompletedFence()
    {
        return (UINT)m_lastCompletedFence;
    }

    NTSTATUS Patch(const DXGKARG_PATCH *pPatch);
    NTSTATUS SubmitCommand(const DXGKARG_SUBMITCOMMAND *pSubmitCommand);

    _IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql, Irql) _IRQL_raises_(DISPATCH_LEVEL) void LockQueue(
                                                                                                        KIRQL *Irql);
    _IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void UnlockQueue(KIRQL Irql);

    VioGpuCommand *DequeueRunning();
    void QueueRunning(VioGpuCommand *cmd);

    VioGpuCommand *DequeueSubmitted();
    void QueueSubmitted(VioGpuCommand *cmd);

  private:
    static void ThreadWork(PVOID Context);
    void ThreadWorkRoutine(void);

    VioGpuAdapter *m_pAdapter;

    KEVENT m_QueueEvent;
    PETHREAD m_pWorkThread;
    BOOLEAN m_bStopWorkThread;

    LIST_ENTRY m_SubmittedQueue;
    LIST_ENTRY m_RunningQueue;

    KSPIN_LOCK m_Lock;

    UINT m_running;

    BOOLEAN IdleLocked();
    void ReportPreempted(UINT preemptionFenceId);
    volatile LONG m_lastCompletedFence;
    BOOLEAN m_preemptPending;
    UINT m_preemptFenceId;
};
