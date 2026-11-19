#!/usr/bin/env python3
"""Where the host's time goes while the UTM test VM moves and resizes a window.

Reads a folder written by tools/win7-debug/utm-hostprof.sh: one-second `sample` reports of the
VM's QEMU process (s-NNNN.txt) and the display draws from UTM's debug log (draws.txt).

    hostprof.py DIR [--phase NAME START END]... [--top N]

The workload is explorerrestore.exe's `motion` mode (or the probe built from it): every round
drags the window by its title bar for three seconds, then resizes it for three seconds, with
pauses of more than a second between. In the draw log every gesture is a run of draws no
further apart than 0.4 s that lasts 2.5 s or more; the runs are taken in order as move,
resize, move, resize. The draw rate does not tell the two apart on this host (20 to 40 draws
a second in both), so check the list of phases the report starts with against the number of
rounds. Chunks that end 12 s or more before the first gesture are "idle": start the sampling
that much before the workload. Only chunks lying wholly inside one phase are counted.
`--phase` names the phases by hand instead (START and END as HH:MM:SS.mmm on the host clock).

For every phase it prints, per thread, the share of samples in which the thread was running
(not parked in a wait), and for the threads that run guest code what they were running:
translated guest code itself, or the emulator's own work around it (memory slow path, device
access, interrupts, translating new code, waiting for QEMU's global lock). The names come from
`sample`; in a stripped library a static function carries the name of the nearest exported one
before it, so read the two raw lists under each phase before trusting a category.

MIT licence (LICENSE at the repository root).
"""
import argparse
import re
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

WAITS = {
    "__psynch_cvwait", "__psynch_mutexwait", "__psynch_rw_rdlock", "__psynch_rw_wrlock",
    "mach_msg2_trap", "mach_msg_trap", "mach_msg_overwrite_trap", "__select", "__select_nocancel",
    "poll", "__poll_nocancel", "ppoll", "kevent", "kevent64", "kevent_id", "kevent_qos",
    "__workq_kernreturn", "semaphore_wait_trap", "semaphore_timedwait_trap", "__ulock_wait",
    "__ulock_wait2", "__sigwait", "__semwait_signal", "read", "__read_nocancel", "__recvfrom",
    "__recvmsg", "__accept", "__sigsuspend", "nanosleep", "usleep",
}
LINE = re.compile(r"^([ +!:|]*)(\d+) (.+)$")
GLOBAL_LOCK = re.compile(r"qemu_mutex_lock_iothread|bql_lock")
CPU_THREAD = re.compile(r"(mttcg|rr)_cpu_thread_fn")
# The main loop runs device work (bottom halves, fd handlers, timers) with the global lock held.
DEVICE_WORK = re.compile(r"^(aio_bh_call|aio_dispatch_handler|timerlist_run_timers)$")
SYSTEM = re.compile(r"libsystem_|libdispatch|libobjc|CoreFoundation|^dyld$")
# Tried in this order on the emulator frames below the guest code; the first match names it.
HELPERS = [
    ("device access", re.compile(r"io_readx|io_writex|do_ld_mmio|do_st_mmio|memory_region_dispatch|"
                                 r"memory_region_(read|write)|address_space_(rw|read|write|ld|st)|flatview_|"
                                 r"cpu_(in|out)[bwl]|helper_(in|out)[bwl]")),
    ("TLB flush", re.compile(r"tlb_flush|helper_invlpg|helper_write_crN|cpu_x86_update_cr")),
    ("memory slow path", re.compile(r"tlb_fill|mmu_translate|probe_access|victim_tlb|tlb_set_page|"
                                    r"get_physical_address|mmu_lookup|helper_(ld|st)[a-z0-9_]*_mmu|"
                                    r"(load|store)_helper|do_(ld|st)\d")),
    ("interrupt or privilege switch", re.compile(r"do_interrupt|helper_iret|helper_sys(call|ret|enter|exit)|"
                                                 r"helper_raise|helper_l(call|jmp|ret)|helper_load_seg|"
                                                 r"raise_exception|raise_interrupt")),
    ("pause or halt", re.compile(r"helper_pause|helper_hlt|helper_mwait|do_pause|do_hlt")),
    ("translated-code lookup", re.compile(r"helper_lookup_tb_ptr|tb_lookup|tb_htable_lookup|qht_lookup")),
    ("floating point and SSE", re.compile(r"helper_f[a-z0-9_]+|float(32|64|x80|128)_|helper_[a-z0-9_]*(xmm|mmx|ymm)|"
                                          r"helper_[a-z]+[ps][sd]\b|softfloat|parts(64|128)?_")),
]
# For a guest-code thread that is in the emulator's loop, outside translated code.
LOOP = [
    ("translating new code", re.compile(r"tb_gen_code|tcg_gen_code|translator_loop|tcg_optimize|tcg_reg_alloc|"
                                        r"tb_link|tb_flush|tb_phys_invalidate|tb_invalidate|liveness_pass|"
                                        r"gen_intermediate")),
    ("translated-code lookup", re.compile(r"tb_lookup|tb_htable_lookup|qht_lookup|cpu_get_tb_cpu_state")),
    ("interrupt delivery", re.compile(r"cpu_handle_interrupt|cpu_handle_exception|x86_cpu_exec_interrupt|"
                                      r"do_interrupt|apic_")),
    ("work for other processors", re.compile(r"process_queued_cpu_work|async_safe|start_exclusive|end_exclusive|"
                                             r"cpu_exec_(start|end)|run_on_cpu")),
]
# For every other thread, on "function (library)".
OTHER = [
    ("virtio-gpu and virglrenderer", re.compile(r"virgl|vrend|virtio_gpu")),
    ("ANGLE and Metal", re.compile(r"\b(gl|rx|egl|angle|sh)::|GLES|libEGL|MTL|Metal|AGX|IOGPU|IOSurface|IOAccel")),
    ("display client", re.compile(r"[Ss]pice|red_|glz_|quic_|jpeg|lz4|display_channel|CSDisplay|CSRenderer")),
    ("main loop", re.compile(r"main_loop_wait|aio_|qemu_bh|timerlist|qemu_clock|os_host_main_loop|glib_pollfds")),
]


def unknown(frame):
    """A frame in memory that belongs to no library: translated guest code."""
    return frame[0] == "???" or "unknown binary" in frame[1]


def thread_name(rest):
    found = re.match(r"Thread_(\S+)\s*:?\s*(.*)$", rest)
    name = re.sub(r"\s+\((serial|concurrent)\)$", "", found.group(2)).strip()
    return f"{name} [{found.group(1)}]" if name else f"[{found.group(1)}]"


def close(stack, rows, col):
    """Take the frames at column `col` or deeper off the stack. What a frame's callees did not
    account for are the samples in which it was the one executing."""
    while stack and stack[-1][0] >= col:
        _, label, left = stack.pop()
        if left > 0 and stack:
            rows.append((stack[0][1], tuple(node[1] for node in stack[1:]) + (label,), left))
        elif left > 0:
            rows.append((label, (), left))


def read_sample(path):
    """One `sample` report: (start as epoch seconds, UTC offset, [(thread, frames, samples)]).
    frames are (function, library) pairs from the thread's first frame to the one executing."""
    start = offset = None
    rows, stack, graph = [], [], False
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("Date/Time:"):
            when = datetime.strptime(line.split(None, 1)[1].strip(), "%Y-%m-%d %H:%M:%S.%f %z")
            start, offset = when.timestamp(), when.utcoffset()
        elif line.startswith("Call graph:"):
            graph = True
        elif graph:
            found = LINE.match(line)
            if not found:
                if line.strip().startswith("Total number in stack"):
                    break
                continue
            col, count, rest = len(found.group(1)), int(found.group(2)), found.group(3)
            if rest.startswith("Thread_"):
                close(stack, rows, 0)
                stack.append([col, thread_name(rest), count])
            elif stack:
                close(stack, rows, max(col, stack[0][0] + 1))
                stack[-1][2] -= count
                library = re.search(r"\(in ([^)]*)\)", rest)
                stack.append([col, (rest.split("  (in ")[0].strip(), library.group(1) if library else ""), count])
    close(stack, rows, 0)
    return start, offset, rows


def category(frames, guest_thread):
    """(running?, what it was doing) for one sampled stack."""
    if not frames:
        return True, "no frames"
    names = [frame[0] for frame in frames]
    if names[-1] in WAITS:
        if any(GLOBAL_LOCK.search(name) for name in names):
            return False, "waiting for QEMU's global lock"
        if any(DEVICE_WORK.search(name) for name in names):
            return False, "waiting inside device work, global lock held"
        if guest_thread and "qemu_wait_io_event" in names:
            return False, "waiting: guest processor idle"
        return False, "waiting"
    if guest_thread:
        first = next((i for i, frame in enumerate(frames) if unknown(frame)), None)
        if first is not None:
            below = [frame[0] for frame in frames[first:] if not unknown(frame)]
            if not below:
                return True, "translated guest code"
            for label, pattern in HELPERS:
                if any(pattern.search(name) for name in below):
                    return True, "helper: " + label
            return True, "helper: other"
        for label, pattern in LOOP:
            if any(pattern.search(name) for name in names):
                return True, "loop: " + label
        return True, "loop: other"
    texts = ["%s (%s)" % frame for frame in frames]
    for label, pattern in OTHER:
        if any(pattern.search(text) for text in texts):
            return True, label
    return True, "other"


def gestures(draws):
    """Starts of the runs of draws no further apart than 0.4 s that last 2.5 s or more."""
    runs, first, last = [], None, None
    for t in draws + [None]:
        if first is not None and (t is None or t - last > 0.4):
            if last - first >= 2.5:
                runs.append(first)
            first = None
        if t is None:
            break
        if first is None:
            first = t
        last = t
    return runs


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("folder", type=Path)
    parser.add_argument("--phase", nargs=3, action="append", default=[], metavar=("NAME", "START", "END"))
    parser.add_argument("--top", type=int, default=25)
    args = parser.parse_args()

    seconds = 1.0
    run = args.folder / "run.txt"
    if run.exists():
        words = run.read_text().split()
        if "chunk_seconds" in words:
            seconds = float(words[words.index("chunk_seconds") + 1])
    chunks = []
    for path in sorted(args.folder.glob("s-*.txt")):
        start, offset, rows = read_sample(path)
        if start is None or not rows:
            print(f"{path.name}: no call graph, left out", file=sys.stderr)
            continue
        chunks.append((start, offset, rows))
    if not chunks:
        sys.exit(f"no sample reports with a call graph in {args.folder}")
    zone = timezone(chunks[0][1])
    day = datetime.fromtimestamp(chunks[0][0], zone)

    def clock(text):
        when = datetime.strptime(text, "%H:%M:%S.%f")
        return day.replace(hour=when.hour, minute=when.minute, second=when.second,
                           microsecond=when.microsecond).timestamp()

    def stamp(t):
        return datetime.fromtimestamp(t, zone).strftime("%H:%M:%S.%f")[:-3]

    draws = []
    log = args.folder / "draws.txt"
    if log.exists():
        for line in log.read_text(errors="replace").splitlines():
            try:
                draws.append(datetime.strptime(line[:23], "%Y-%m-%d %H:%M:%S,%f").replace(tzinfo=zone).timestamp())
            except ValueError:
                pass
    draws.sort()

    phases = []
    if args.phase:
        phases = [(name, clock(a), clock(b)) for name, a, b in args.phase]
    else:
        for i, first in enumerate(gestures(draws)):
            # three seconds from the first draw, less the ends
            phases.append(("resize" if i % 2 else "move", first + 0.3, first + 2.8))
    quiet = min(a for _, a, _ in phases) - 12 if phases else None
    print("phases on the host clock:")
    for name, a, b in phases:
        print(f"  {name:7} {stamp(a)} to {stamp(b)}  {sum(a <= t <= b for t in draws) / (b - a):5.1f} draws/s")
    if not phases:
        print("  none: the draw log has no gesture; name the phases with --phase")

    # The threads that run guest code: QEMU's processor threads (failing those names, the
    # threads whose stacks pass through memory that belongs to no library).
    guest = {thread for _, _, rows in chunks for thread, frames, _ in rows
             if any(CPU_THREAD.search(frame[0]) for frame in frames)}
    if not guest:
        guest = {thread for _, _, rows in chunks for thread, frames, _ in rows
                 if any(unknown(frame) for frame in frames)}

    totals = defaultdict(lambda: defaultdict(Counter))  # phase, thread, category: samples
    leaves = defaultdict(Counter)                        # phase, executing function: running samples
    callers = defaultdict(Counter)                       # phase, first emulator function under guest code
    lockers = defaultdict(Counter)                       # phase, who asked for the global lock: waiting samples
    holders = defaultdict(Counter)                       # phase, device work that waits with the lock held
    used = Counter()
    for start, _, rows in chunks:
        end = start + seconds
        kind = next((name for name, a, b in phases if a <= start and end <= b), None)
        if kind is None and quiet is not None and end < quiet:
            kind = "idle"
        if kind is None:
            continue
        used[kind] += 1
        for thread, frames, count in rows:
            running, what = category(frames, thread in guest)
            totals[kind][thread][what] += count
            if what == "waiting inside device work, global lock held":
                at = next(i for i, frame in enumerate(frames) if DEVICE_WORK.search(frame[0]))
                inner = [frame for frame in frames[at + 1:] if not SYSTEM.search(frame[1])]
                steps = [frame[0] for frame in inner if re.match(r"vrend_(decode|renderer)_", frame[0])][:2]
                holders[kind][" > ".join(steps + [inner[-1][0]] if inner else ["?"])] += count
            if what == "waiting for QEMU's global lock":
                at = next(i for i, frame in enumerate(frames) if GLOBAL_LOCK.search(frame[0]))
                lockers[kind]["%s (%s)" % frames[at - 1] if at else "?"] += count
            if running and frames:
                leaves[kind]["%s (%s)" % frames[-1]] += count
                first = next((i for i, frame in enumerate(frames) if unknown(frame)), None)
                named = [frame for frame in frames[first:] if not unknown(frame)] if first is not None else []
                if named:
                    callers[kind]["%s (%s)" % named[0]] += count

    for kind in ["move", "resize", "idle"] + sorted(set(used) - {"move", "resize", "idle"}):
        if not used[kind]:
            continue
        print(f"\n== {kind}: {used[kind]} chunks of {seconds:g} s ==")
        threads = totals[kind]

        def ran(thread):
            return sum(n for what, n in threads[thread].items() if not what.startswith("waiting"))

        for thread in sorted(threads, key=ran, reverse=True):
            cats = threads[thread]
            total = sum(cats.values())
            if ran(thread) < 0.01 * total and "waiting for QEMU's global lock" not in cats:
                continue
            mark = " (runs guest code)" if thread in guest else ""
            print(f"  {thread}{mark}: running {100 * ran(thread) / total:.0f}% of {total} samples")
            for what, n in cats.most_common():
                if what != "waiting" and n >= 0.005 * total:
                    print(f"      {100 * n / total:5.1f}%  {what}")
        together = Counter()
        for thread in threads:
            if thread in guest:
                together.update(threads[thread])
        if together:
            total = sum(together.values())
            print(f"  all {sum(t in guest for t in threads)} guest-processor threads together, {total} samples:")
            for what, n in together.most_common():
                print(f"      {100 * n / total:5.1f}%  {what}")
        for title, table in (("executing function, running samples of all threads", leaves[kind]),
                             ("first emulator function called from guest code", callers[kind]),
                             ("who asked for the global lock, waiting samples", lockers[kind]),
                             ("device work that waits with the global lock held", holders[kind])):
            total = sum(table.values())
            if total:
                print(f"  -- {title} --")
                for what, n in table.most_common(args.top):
                    print(f"      {100 * n / total:5.1f}%  {what}")


if __name__ == "__main__":
    main()
