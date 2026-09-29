#!/usr/bin/env python3
"""Check a heap dispatcher trace: mallocng's releases against its mappings.

    python3 scripts/heap_trace.py <console-log>

The heap logs every `mmap`, `munmap` and refusal when it is built with
`-DAEGIR_HEAP_TRACE` (libs/hosted/aegir-heap/src/heap.cc), each line tagged
with the process's badge because two processes with the same image use the same
addresses. This reads such a log back and answers the question a leak or a
corruption turns on: does every `munmap(addr, len)` name a range that is
currently mapped by an `mmap(addr, len)`, in the same process?

It reports, per process:

  - exact releases (the healthy case),
  - sub-range releases (a release of part of a live mapping),
  - unknown releases (the address is not live at all -- a double release, or a
    mapping this log lost),
  - mappings that overlap a live one (the heap handing the same address twice),
  - regions still live at the end (a mapping never released).

A line the console split mid-write is skipped and counted, because it drops an
event and can make an unpaired release look real.

Exit status: 0 when every release pairs and nothing overlaps, 1 otherwise.
"""

from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

PAGE = 4096
TRACE = re.compile(r"DISPATCHER_TRACE (0x[0-9a-f]+) (\S+)(.*)$")


def align(value: int) -> int:
    return (value + PAGE - 1) & ~(PAGE - 1)


def parse_tail(kind: str, tail: str) -> tuple[int, int] | None:
    """The operands a kind carries, or None when the line is incomplete.

    A line the console split mid-write keeps its `DISPATCHER_TRACE` and loses
    an operand; treating that as malformed drops the whole event instead of
    inventing a zero length, which is what makes an unpaired release look real.
    """
    fields = tail.split()
    wanted = 1 if kind == "mmap-refused" else 2
    if len(fields) != wanted:
        return None
    try:
        values = [int(field, 16) for field in fields]
    except ValueError:
        return None
    return (values[0], values[1] if len(values) > 1 else 0)


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    log = Path(argv[0])
    if not log.is_file():
        print(f"heap_trace: {log} is not a file", file=sys.stderr)
        return 2

    # A process's events are split at its `region` line: a badge is unique among
    # live processes, so a region line is a new process even when a badge is
    # reused later.
    streams: dict[int, list[dict]] = defaultdict(list)
    skipped = 0
    for raw in log.read_text(errors="replace").splitlines():
        at = raw.find("DISPATCHER_TRACE")
        if at < 0:
            continue
        match = TRACE.match(raw[at:].strip())
        if not match:
            skipped += 1
            continue
        badge = int(match.group(1), 16)
        kind = match.group(2)
        values = parse_tail(kind, match.group(3))
        if values is None:
            skipped += 1
            continue
        first, second = values
        if kind == "region":
            streams[badge].append({"base": first, "limit": second, "events": []})
        else:
            if not streams[badge]:
                streams[badge].append({"base": 0, "limit": 0, "events": []})
            streams[badge][-1]["events"].append((kind, first, second))

    totals = defaultdict(int)
    problems: list[str] = []
    processes = 0
    for badge, runs in streams.items():
        for run in runs:
            if not run["events"]:
                continue
            processes += 1
            live: dict[int, int] = {}
            for kind, first, second in run["events"]:
                if kind == "mmap":
                    length = align(second)
                    for base, held in live.items():
                        if first < base + held and base < first + length:
                            totals["overlap"] += 1
                            problems.append(
                                f"badge 0x{badge:x}: mmap 0x{first:x}+0x{length:x} "
                                f"overlaps live 0x{base:x}+0x{held:x}"
                            )
                    live[first] = length
                elif kind == "mmap-refused":
                    totals["refused"] += 1
                elif kind == "munmap":
                    length = align(second)
                    if first in live and live[first] == length:
                        del live[first]
                        totals["exact"] += 1
                    elif any(base <= first and first + length <= base + held
                             for base, held in live.items()):
                        totals["sub"] += 1
                        problems.append(
                            f"badge 0x{badge:x}: munmap 0x{first:x}+0x{length:x} "
                            "releases part of a live mapping"
                        )
                    else:
                        totals["unknown"] += 1
                        problems.append(
                            f"badge 0x{badge:x}: munmap 0x{first:x}+0x{length:x} "
                            "names nothing live (a double release, or a lost mmap)"
                        )
            if live:
                totals["leftover"] += len(live)
                totals["leftover_bytes"] += sum(live.values())

    print(f"processes {processes}, split log lines {skipped}")
    print(
        f"releases: exact {totals['exact']}, sub-range {totals['sub']}, "
        f"unknown {totals['unknown']}; mappings overlapping {totals['overlap']}"
    )
    print(
        f"still live at the end: {totals['leftover']} regions, "
        f"{totals['leftover_bytes']} bytes"
    )
    for problem in problems[:20]:
        print(f"  {problem}")
    # An unpaired release is explained when a split log line dropped its
    # mapping; one beyond that count is a real double release.
    unexplained = totals["unknown"] - skipped
    if totals["sub"] or totals["overlap"] or unexplained > 0:
        print(
            f"FAIL  {totals['unknown']} unpaired ({skipped} log lines split, "
            f"{unexplained} unexplained), {totals['sub']} sub-range, "
            f"{totals['overlap']} overlapping"
        )
        return 1
    print(
        "PASS  every release pairs with a mapping and nothing overlaps "
        f"({totals['unknown']} unpaired, all {skipped} split log lines)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
