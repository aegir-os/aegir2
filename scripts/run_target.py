#!/usr/bin/env python3
"""Configure, build and boot one of Aegir's targets.

QEMU does not stop when a target has finished saying what it has to say, so this
streams the guest console, stops QEMU once the target's success marker appears,
and reports what it saw. Every step runs under a timeout, per the project rule
that long-running processes must not be able to wedge a session.

    python3 scripts/run_target.py --target aegir
    python3 scripts/run_target.py --target aegir --build-only
    python3 scripts/run_target.py --target sel4test --reconfigure

Exit status: 0 the marker was seen (or the build succeeded with --build-only).
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import signal
import subprocess
import sys
from pathlib import Path

import pins
from targets import TARGETS, Target

ENV_SCRIPT = pins.ROOT / "scripts" / "env.sh"
TEST_SUMMARY = re.compile(r"Test suite passed\.\s+(\d+) tests passed\.\s+(\d+) tests disabled\.")


def bash(command: str, cwd: Path, timeout: int) -> None:
    """Run a shell command with Aegir's pinned tools on PATH."""
    print(f"INFO  (cd {cwd.relative_to(pins.ROOT)} && {command})", flush=True)
    subprocess.run(
        ["bash", "-c", f"set -euo pipefail; . {ENV_SCRIPT}; {command}"],
        cwd=str(cwd),
        check=True,
        timeout=timeout,
    )


def configure(target: Target, build_dir: Path, timeout: int) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    flags = " ".join(target.configure_flags)
    root = ENV_SCRIPT.parent.parent
    if target.source_dir == ".":
        relative = os.path.relpath(root, build_dir)
        bash(f"{relative}/init-build.sh {flags}".strip(), build_dir, timeout)
        return

    # A project inside the tree: the root's init-build.sh decides which project to
    # configure by looking for a CMakeLists.txt next to itself
    # (tools/seL4/cmake-tool/init-build.sh:41-54), and next to *our* root there is
    # one -- so it would configure Aegir instead. Name the project directly, with
    # the command init-build.sh's easy-settings path uses.
    source = os.path.relpath(root / target.source_dir, build_dir)
    cache = os.path.relpath(root / ".sel4_cache", build_dir)
    bash(
        f"cmake -G Ninja {flags} -DSEL4_CACHE_DIR={cache} "
        f"-C {source}/settings.cmake {source}",
        build_dir,
        timeout,
    )


def build(target: Target, build_dir: Path, timeout: int) -> None:
    bash("ninja", build_dir, timeout)


def configured_flags(build_dir: Path) -> str:
    """The configure flags a build directory was last configured with.

    The memory and core count a target asks for are configure-time values (the
    device tree is dumped with them, scripts/targets.py), so a build directory
    that was configured for a different machine has to be reconfigured rather
    than reused -- ninja alone would happily keep building the old one.
    """
    stamp = build_dir / ".aegir-configure"
    return stamp.read_text(encoding="utf-8") if stamp.is_file() else ""


def record_flags(build_dir: Path, flags: str) -> None:
    (build_dir / ".aegir-configure").write_text(flags, encoding="utf-8")


def boot_and_watch(target: Target, build_dir: Path, timeout: int) -> tuple[bool, str]:
    """Boot the image, streaming the console until the marker appears."""
    # The target's extra arguments belong to QEMU, not to the simulate script, so
    # they go through --extra-qemu-args as one string -- attached with `=` rather
    # than passed as a separate argument, because the value starts with `-bios`
    # and the script's argparse refuses a value that looks like an option (and
    # would read a loose `-bios` as its own `-b`).
    # The machine's block device needs a disk to be a block device *of*. It is a
    # GPT with three FAT partitions -- two holding a known file, one empty and
    # writable -- built by make_disk.py in the build directory. Created once and
    # left alone, and QEMU's -snapshot keeps even a writing run off it: a disk
    # that changes between runs is not something to depend on. It lives in the
    # build output rather than the repository, where scratch belongs.
    disk = build_dir / "disk.img"
    if not disk.exists():
        subprocess.run(
            [sys.executable, str(Path(__file__).parent / "make_disk.py"), str(disk)],
            check=True,
        )

    extra = " ".join(target.qemu_args)
    command = "./simulate --extra-qemu-args=" + shlex.quote(extra)
    process = subprocess.Popen(
        ["bash", "-c", f"set -euo pipefail; . {ENV_SCRIPT}; exec {command}"],
        cwd=str(build_dir),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        # The console is a byte stream: a kernel abort or a driver writing raw
        # bytes is still output we want to see, not a reason to crash.
        errors="replace",
        start_new_session=True,
    )
    # A `timeout` around *this* script kills us directly, and Python's default handler
    # for SIGTERM exits without unwinding -- so the `finally` below never runs, the
    # process group is never taken down, and QEMU keeps running for ever. That is how
    # 47 machines accumulated on one host during a long session. Becoming an exception
    # is what makes the cleanup run; nothing else about it changes.
    def _stop(signum: int, _frame: object) -> None:
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    seen = False
    summary = ""
    try:
        stream = process.stdout
        if stream is None:  # pragma: no cover - Popen above always pipes
            return False, ""
        for line in stream:
            stripped = line.rstrip("\n")
            if stripped:
                print(f"    {stripped}", flush=True)
            match = TEST_SUMMARY.search(stripped)
            if match:
                summary = f"{match.group(1)} tests passed, {match.group(2)} disabled"
            if target.marker in stripped:
                seen = True
                break
        stream.close()
    finally:
        # Take the whole process group down: QEMU is a child of the shell, and
        # neither notices that the target is finished.
        if process.poll() is None:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(os.getpgid(process.pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
    return seen, summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--target", required=True, choices=sorted(TARGETS))
    parser.add_argument("--timeout", type=int, default=900, help="seconds per step")
    parser.add_argument("--build-only", action="store_true", help="stop after building")
    parser.add_argument(
        "--reconfigure", action="store_true", help="re-run cmake instead of reusing the build dir"
    )
    arguments = parser.parse_args(argv)

    target = TARGETS[arguments.target]
    build_dir = pins.ROOT / target.build_dir

    wanted_flags = " ".join(target.configure_flags)
    try:
        stamp = configured_flags(build_dir)
        if (
            arguments.reconfigure
            or not (build_dir / "build.ninja").is_file()
            or (stamp != "" and stamp != wanted_flags)
        ):
            print(f"INFO  (re)configuring {target.name} as: {wanted_flags or 'defaults'}", flush=True)
            configure(target, build_dir, arguments.timeout)
            record_flags(build_dir, wanted_flags)
        elif stamp == "":
            # An existing build directory from before this record existed: adopt
            # it as configured with what the target now asks for, so a later
            # change is still noticed.
            record_flags(build_dir, wanted_flags)
        build(target, build_dir, arguments.timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        pins.report(False, f"{target.name} build failed", str(exc))
        return 1

    if arguments.build_only:
        pins.report(True, f"{target.name} built", target.description)
        return 0

    try:
        seen, summary = boot_and_watch(target, build_dir, arguments.timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        pins.report(False, f"{target.name} boot failed", str(exc))
        return 1

    if not seen:
        pins.report(
            False,
            f"{target.name} did not report success",
            f"never saw {target.marker!r} within {arguments.timeout}s",
        )
        return 1
    pins.report(True, f"{target.name} booted", summary or target.marker)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except pins.PinError as exc:
        pins.report(False, "run failed", str(exc))
        sys.exit(1)
