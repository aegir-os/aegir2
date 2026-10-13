#!/usr/bin/env python3
"""Prove the POSIX conformance surface is defined and covered.

    python3 scripts/check_posix_coverage.py

Cross-reads three things -- the dispatcher's answered syscalls
(`libs/hosted/aegir-heap/src/heap.cc`), the surface manifest
(`manifests/posix-surface.toml`), and the suite's cases
(`apps/hosted/aegir-posix-conformance/src/`) -- and fails, naming the interface,
when an area is still `todo`, when an interface's state disagrees with the
dispatcher, or when an interface has no case in the suite. It *is* the worklist:
while the suite is incomplete it prints what is left and exits non-zero, so
"the suite is complete" is this check passing and never a claim in prose
(specs/posix-conformance.md).

Host tools only (python3); it gates `make posix-test`.
Exit status: 0 the surface is enumerated and covered, 1 otherwise.
"""

from __future__ import annotations

import re
import tomllib
from pathlib import Path

import pins

DISPATCHER = pins.ROOT / "libs" / "hosted" / "aegir-heap" / "src" / "heap.cc"
MANIFEST = pins.ROOT / "manifests" / "posix-surface.toml"
SOURCES = pins.ROOT / "manifests" / "sources.toml"
SUITE = pins.ROOT / "apps" / "hosted" / "aegir-posix-conformance" / "src"


def answered_syscalls() -> set[int]:
    """The riscv64 syscalls the dispatcher's `vsyscall` switch answers.

    Parsed from the switch itself, never from a list beside it: the switch is
    the truth for "does Aegir answer this call", and the manifest is checked
    against it."""
    text = DISPATCHER.read_text()
    start = text.index("long ret = -ENOSYS;")
    end = text.index("default:", start)
    return {int(number) for number in re.findall(r"case (\d+):", text[start:end])}


def suite_interfaces() -> dict[str, int]:
    """{interface: number of cases} the suite declares. Every case names its
    interface as the first string literal -- through `check`, `unavailable`,
    `skip` and `deviation`, or through the two shaping helpers
    (`conforms_or_deviation`, `answers_or_hole`)."""
    seen: dict[str, int] = {}
    pattern = (r'\b(?:check|unavailable|skip|deviation|conforms_or_deviation|answers_or_hole)'
               r'\("([A-Za-z0-9_]+)"')
    for path in sorted(SUITE.glob("*.cc")):
        for match in re.finditer(pattern, path.read_text()):
            name = match.group(1)
            seen[name] = seen.get(name, 0) + 1
    return seen


def pinned_spec_pages() -> set[str]:
    """The Issue 8 pages `manifests/sources.toml` actually pins.

    A case may only cite a clause the build fetches: writing `open: ...` from
    memory of the standard and labelling it `open.html` is the failure this
    refuses (specs/posix-conformance.md)."""
    with SOURCES.open("rb") as handle:
        sources = tomllib.load(handle).get("source", [])
    for source in sources:
        if source.get("name") == "posix-spec":
            return {entry["name"] for entry in source.get("files", [])}
    return set()


def main() -> int:
    with MANIFEST.open("rb") as handle:
        surface = tomllib.load(handle)

    answered = answered_syscalls()
    cases = suite_interfaces()
    pinned = pinned_spec_pages()
    areas = surface.get("area", [])
    interfaces = surface.get("interface", [])

    todo = [area["name"] for area in areas if area["status"] != "covered"]
    problems: list[str] = []
    known: set[str] = set()

    for interface in interfaces:
        name = interface["name"]
        state = interface["state"]
        syscall = interface.get("syscall", 0)
        known.add(name)
        if state in ("implemented", "partial"):
            if syscall and syscall not in answered:
                problems.append(
                    f"{name} ({interface['area']}): marked {state}, but the "
                    f"dispatcher answers no syscall {syscall}")
        elif state == "unimplemented":
            if syscall and syscall in answered:
                problems.append(
                    f"{name} ({interface['area']}): marked unimplemented, but the "
                    f"dispatcher answers syscall {syscall} -- flip it and give it a case")
        else:
            problems.append(f"{name}: unknown state {state!r}")
        if name not in cases:
            problems.append(f"{name} ({interface['area']}): no case in the suite")

        # The clause must be a page the build fetches. This is the check that a
        # case is derived from the standard rather than from memory.
        clause = interface.get("clause", "")
        if clause.endswith(".html") and clause not in pinned:
            problems.append(
                f"{name} ({interface['area']}): clause {clause} is not a pinned page "
                f"in manifests/sources.toml -- the case cites a standard that was "
                f"never fetched"
            )

    for name in sorted(cases):
        if name not in known:
            problems.append(f"{name}: a case in the suite, but not in the manifest")

    for name in todo:
        print(f"    area {name}: todo -- its interfaces are not enumerated yet", flush=True)
    for problem in problems:
        print(f"    {problem}", flush=True)

    covered = sum(1 for interface in interfaces if interface["name"] in cases)
    ok = not todo and not problems
    pins.report(
        ok,
        "the POSIX conformance surface is enumerated and every interface has a case",
        f"{covered}/{len(interfaces)} interfaces, {len(todo)} area(s) todo",
    )
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
