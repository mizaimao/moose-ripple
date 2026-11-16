#pragma once

#include "helper.h"

// Command buffers sent to the host and not yet finished there, at most
#define VIOGPU_MAX_RUNNING 8

class VioGpuAdapter;
class VioGpuDevice;
class VioGpuAllocation;
class VioGpuCommander;

// What a command buffer holds until it has finished: busy marks on its allocations and the
// event someone may wait on. Taken whole by whoever gives them up first (the command when it
// finishes, or a reset), so they are given up once.
struct VioGpuCommandHeld
{
    VioGpuAllocation **allocations;
    UINT length;
    PKEVENT event;
};

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
    // In the commander's list of buffers taken to run and not finished (under its lock)
    LIST_ENTRY active_entry;
    BOOLEAN active;
    // Finished, waiting for the buffers before it to be reported
    BOOLEAN done;
    // Reported to Windows as done. A rendering buffer is reported once all of it is with the
    // host (sent), the others (paging, present) once the host has run them (done); either way
    // in submission order (VioGpuCommander::ReportReady)
    BOOLEAN reported;
    BOOLEAN sent;
    BOOLEAN earlyOK;

    // Dropped by a reset: given up without telling Windows it finished
    void Abandon();

    UINT FenceId()
    {
        return m_FenceId;
    }
    LONG Epoch()
    {
        return m_epoch;
    }
    // Not pageable: a reset takes them under the queue lock
    void TakeHeld(VioGpuCommandHeld *held);
    static void ReleaseHeld(VioGpuCommandHeld *held);

  private:
    void Release();
    VioGpuAdapter *m_pAdapter;
    VioGpuCommander *m_pCommander;
    VioGpuDevice *m_pContext;

    UINT m_FenceId;
    LONG m_epoch;
    BOOLEAN m_queuedAll;
#ifdef RIPPLE_TRACE_TIMING
  public:
    // When the buffer was made (Render), handed over (SubmitCommand), taken to run, sent to
    // the host, answered by the host (interrupt), and finished
    LONGLONG t[6];

  private:
#endif

    char *m_pDmaBuffer;
    char *m_pCommand;
    char *m_pEnd;

    VioGpuAllocation **volatile m_allocations;
    UINT m_allocationsLength;

    PKEVENT volatile m_pCompletionEvent;
};

class VioGpuCommander
{
  public:
    VioGpuCommander(VioGpuAdapter *pAdapter);
    ~VioGpuCommander();

    NTSTATUS Start();
    void Stop();

    // A command buffer has finished: Windows is told, in submission order, unless a reset came
    // in between; the command is deleted
    void Complete(VioGpuCommand *cmd);
    // Out of the list of buffers taken to run
    void Retire(VioGpuCommand *cmd);
    void CommandFinished(void);
    // A rendering buffer is all with the host
    void Sent(VioGpuCommand *cmd);
    void ReportReady(void);
    // Copies of command data, reused: fresh pool memory costs a VM exit per page first touched
    // under nested virtualization
    PVOID AllocCopy(ULONG size);
    static void FreeCopy(void *ctx, void *data);
    // From pool: the structure is cache-aligned, more than the class may be
    PNPAGED_LOOKASIDE_LIST m_copyList;
    // Recovery after a GPU timeout (DxgkDdiResetFromTimeout): every command buffer not yet
    // run is dropped, and answers from the host for buffers sent before it are ignored
    // (they belong to an older epoch), so Windows never hears of a buffer from before the
    // reset. The host cannot stop a buffer it is running; what that buffer holds is given up
    // at once, since Windows may free its allocations right after the reset.
    void Reset();
    void Restart();
    LONG Epoch()
    {
        return m_epoch;
    }

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
    VioGpuCommand *DequeueSubmittedToRun();
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
    LIST_ENTRY m_ActiveQueue;

    KSPIN_LOCK m_Lock;

    UINT m_running;

    BOOLEAN IdleLocked();
    void ReportPreempted(UINT preemptionFenceId);
    volatile LONG m_lastCompletedFence;
    volatile LONG m_epoch;
    KGUARDED_MUTEX m_ResetMutex;
#ifdef RIPPLE_TRACE_TIMING
  public:
    void Timing(VioGpuCommand *cmd);
    UINT m_tParts, m_tBytes, m_tTransfers;
    UINT m_tDepthSum, m_tDepthMax, m_tWaitingMax;
    LONGLONG m_tCopy, m_tQueue;

  private:
    LONGLONG m_tSum[5], m_tMax[5];
    UINT m_tCount;
#endif
#ifdef RIPPLE_TEST_TDR
  public:
    // Test builds: the next finished buffer is held back this long (VIOGPU_TEST_STALL)
    volatile LONG m_testStallMs;

  private:
#endif
    BOOLEAN m_preemptPending;
    UINT m_preemptFenceId;
};
