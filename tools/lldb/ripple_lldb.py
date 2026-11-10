"""lldb helpers for debugging the 10.6.8 guest through QEMU's gdbstub.

Loaded by scripts/debug.sh with `command script import`. Adds:

  ripple-continue [N]   Resume until a breakpoint is hit, at most N times (default 5).
                        Right after attaching, the first resume often comes back at once
                        with a stray SIGTRAP from another vCPU (seen 2026-11-10 at
                        mach_kernel`legacy_write+18 on thread #4); this skips such stops.
"""
import lldb


def _hit_breakpoint(process):
    target = process.GetTarget()
    for thread in process:
        if thread.GetStopReason() == lldb.eStopReasonBreakpoint:
            return thread
        pc = thread.GetFrameAtIndex(0).GetPC()
        if target.ResolveLoadAddress(pc).IsValid():
            for bp in target.breakpoint_iter():
                for loc in bp:
                    if loc.GetLoadAddress() == pc:
                        return thread
    return None


def ripple_continue(debugger, command, result, internal_dict):
    tries = int(command) if command.strip() else 5
    debugger.SetAsync(False)
    process = debugger.GetSelectedTarget().GetProcess()
    for attempt in range(1, tries + 1):
        error = process.Continue()
        if error.Fail():
            result.SetError("continue failed: %s" % error.GetCString())
            return
        thread = _hit_breakpoint(process)
        if thread is not None:
            process.SetSelectedThread(thread)
            result.AppendMessage("breakpoint hit on thread #%d after %d resume(s)"
                                 % (thread.GetIndexID(), attempt))
            return
        t = process.GetSelectedThread()
        result.AppendMessage("stray stop %d: thread #%d, %s, pc 0x%x"
                             % (attempt, t.GetIndexID(), t.GetStopDescription(80),
                                t.GetFrameAtIndex(0).GetPC()))
    result.SetError("no breakpoint hit after %d resumes" % tries)


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand(
        "command script add -f ripple_lldb.ripple_continue ripple-continue")
