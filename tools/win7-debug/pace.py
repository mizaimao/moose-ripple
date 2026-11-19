"""What paces the compositor: DWM-Core events from an English tracerpt XML, per marked phase.

    pace.py TRACE.xml

The trace is the one `guest/pace.cmd` records around the motion probe, whose marks open and
close phases named drag and resize. For each kind of phase: frames rendered and presented per
second, the gaps between presents, how many display refreshes the compositor gives a frame,
and where its thread spent the time, by the Start and Stop events of its own stages (waiting
for the vertical blank, waiting for work, rendering, presenting).
MIT licence (LICENSE at the repository root).
"""
import sys
from collections import Counter, defaultdict
from datetime import datetime
from statistics import median
from xml.etree import ElementTree as ET

DWM_CORE = "{8c9dd1ad-e6e5-4b07-b455-684a9d879900}"
PROBE_MARKS = "{6c38bba1-1768-48c6-9c07-40dadba4b38f}"
STAGES = [
    "SCHEDULE_VBLANK_LOOP", "SCHEDULE_WFVB", "SCHEDULE_WFW", "SCHEDULE_GETPRESENTSTATS",
    "SCHEDULE_PROCESS_FRAME", "MILEVENT_MEDIA_UCE_PROCESSQUEUEEVENT",
    "MILEVENT_MEDIA_UCE_PROCESSPRESENTHISTORY", "SCHEDULE_RENDER",
    "MILEVENT_MEDIA_UCE_RENDEREVENT", "SCHEDULE_PRESENT", "MILEVENT_MEDIA_UCE_PRESENTEVENT",
]


def clock(text):
    head, _, fraction = text.rstrip("Z").partition(".")
    return datetime.fromisoformat(head).timestamp() + float("0." + (fraction or "0"))


def read(path):
    marks, events, providers = [], [], Counter()
    for _, e in ET.iterparse(path, events=("end",)):
        if not e.tag.endswith("}Event"):
            continue
        system = e.find("{*}System")
        guid = system.find("{*}Provider").get("Guid", "").lower()
        providers[guid] += 1
        t = clock(system.find("{*}TimeCreated").get("SystemTime"))
        if guid == PROBE_MARKS:
            marks.append((t, e.findtext("{*}EventData/{*}Data", "") or e.findtext("{*}Data", "")))
        elif guid == DWM_CORE:
            task = e.findtext("{*}RenderingInfo/{*}Task") or "?"
            opcode = (e.findtext("{*}RenderingInfo/{*}Opcode") or "").strip()
            thread = int(system.find("{*}Execution").get("ThreadID"))
            data = {d.get("Name"): (d.text or "").strip() for d in e.findall("{*}EventData/{*}Data")}
            events.append((t, thread, task, opcode, data))
        e.clear()
    marks.sort()
    events.sort(key=lambda event: event[0])
    return marks, events, providers


def main():
    marks, events, providers = read(sys.argv[1])
    print("events by provider:", dict(providers))
    for kind in ("drag", "resize"):
        starts = [t for t, name in marks if name == kind + "-start"]
        stops = [t for t, name in marks if name == kind + "-stop"]
        # the first quarter second and the last tenth of a gesture are its start and its end
        spans = [(a + 0.25, b - 0.1) for a, b in zip(starts, stops) if b - a > 0.5]
        total = sum(b - a for a, b in spans)
        if not total:
            continue
        spent, lengths = defaultdict(float), defaultdict(list)
        presents = renders = 0
        gaps, refreshes = [], Counter()
        for a, b in spans:
            opened, last_present = {}, None
            for t, thread, task, opcode, data in events:
                if t < a or t > b:
                    continue
                if opcode == "Start":
                    opened[(thread, task)] = t
                elif opcode == "Stop" and (thread, task) in opened:
                    length = t - opened.pop((thread, task))
                    spent[task] += length
                    lengths[task].append(length)
                    if task == "MILEVENT_MEDIA_UCE_PRESENTEVENT":
                        presents += 1
                        if last_present is not None:
                            gaps.append(t - last_present)
                        last_present = t
                    elif task == "SCHEDULE_RENDER":
                        renders += 1
                if task == "SCHEDULE_FRAMEINFO" and "cRefreshesPerFrame" in data:
                    refreshes[data["cRefreshesPerFrame"]] += 1
        gaps.sort()
        print("\n%s: %d phases, %.1f s" % (kind, len(spans), total))
        print("  rendered %.1f frames a second, presented %.1f" % (renders / total, presents / total))
        if gaps:
            print("  between presents: median %.1f ms, 95th percentile %.1f, longest %.1f; %d of %d over 25 ms"
                  % (1000 * median(gaps), 1000 * gaps[int(len(gaps) * 0.95)], 1000 * gaps[-1],
                     sum(gap > 0.025 for gap in gaps), len(gaps)))
        print("  refreshes the compositor gives a frame:", dict(refreshes))
        for task in STAGES:
            if lengths[task]:
                ordered = sorted(lengths[task])
                print("  %-42s %5d times, %5.1f%% of the time, median %6.2f ms, 95th percentile %6.2f ms"
                      % (task, len(ordered), 100 * spent[task] / total, 1000 * median(ordered),
                         1000 * ordered[int(len(ordered) * 0.95)]))


if __name__ == "__main__":
    main()
