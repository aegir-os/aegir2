#!/usr/bin/env python3
"""Build and boot the upstream seL4 test suite on qemu-riscv-virt.

This is Aegir's end-to-end acceptance test for the vendored kernel: it drives
the real seL4 build system (kernel, ELF loader, OpenSBI, musllibc, sel4runtime,
libsel4) and then boots the result under QEMU and looks for the suite's
success marker. If this passes, the vendored tree, the pinned toolchain and the
chosen ABI all work together.

    python3 scripts/run_sel4test.py                 # configure if needed, build, boot
    python3 scripts/run_sel4test.py --reconfigure   # force a fresh configure
    python3 scripts/run_sel4test.py --timeout 300   # seconds of guest output

QEMU does not exit when the suite finishes, so this stops it once the marker
appears (or on timeout) and reports what it saw.

Exit status: 0 the marker was seen, 1 anything else.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
from pathlib import Path

import pins

BUILD_DIR = pins.ROOT / "out" / "sel4test"
ENV_SCRIPT = pins.ROOT / "scripts" / "env.sh"
MARKER = "All is well in the universe"
SUMMARY = re.compile(r"Test suite passed\.\s+(\d+) tests passed\.\s+(\d+) tests disabled\.")

# The vendored sel4test platform, our ABI, and our toolchain prefix. Each of
# these is pinned deliberately: the platform and ABI are recorded in
# specs/build.md, and the prefix must be explicit because seL4's gcc.cmake
# probe list is not the whole truth about the toolchain we use.
CONFIGURE_FLAGS = (
    "-DPLATFORM=qemu-riscv-virt",
    "-DCROSS_COMPILER_PREFIX=riscv64-unknown-elf-",
    "-DKernelRiscvExtD=ON",
    "-DSIMULATION=ON",
)


def bash(command: str, cwd: Path, timeout: int) -> None:
    """Run a shell command with Aegir's pinned tools on PATH."""
    print(f"INFO  (cd {cwd.relative_to(pins.ROOT)} && {command})", flush=True)
    subprocess.run(
        ["bash", "-c", f"set -euo pipefail; . {ENV_SCRIPT}; {command}"],
        cwd=str(cwd),
        check=True,
        timeout=timeout,
    )


def configure(timeout: int) -> None:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    flags = " ".join(CONFIGURE_FLAGS)
    bash(f"../../init-build.sh {flags}", BUILD_DIR, timeout)


def build(timeout: int) -> None:
    bash("ninja", BUILD_DIR, timeout)


def boot(timeout: int) -> tuple[bool, str]:
    """Boot the image, streaming output until the marker appears.

    Returns (saw_marker, last summary line).
    """
    command = ["bash", "-c", f"set -euo pipefail; . {ENV_SCRIPT}; exec ./simulate"]
    process = subprocess.Popen(
        command,
        cwd=str(BUILD_DIR),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    seen = False
    summary = ""
    try:
        assert process.stdout is not None
        for line in process.stdout:
            stripped = line.rstrip("\n")
            if stripped:
                print(f"    {stripped}", flush=True)
            match = SUMMARY.search(stripped)
            if match:
                summary = f"{match.group(1)} tests passed, {match.group(2)} disabled"
            if MARKER in stripped:
                seen = True
                break
    finally:
        # QEMU runs until something external stops it; it will not notice that
        # the suite is done. Take the whole process group down.
        if process.poll() is None:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        process.stdout.close() if process.stdout else None
    return seen, summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--timeout", type=int, default=900, help="seconds to wait for the suite (default 900)"
    )
    parser.add_argument(
        "--reconfigure", action="store_true", help="re-run cmake instead of reusing the build dir"
    )
    arguments = parser.parse_args(argv)

    if arguments.reconfigure or not (BUILD_DIR / "build.ninja").is_file():
        try:
            configure(arguments.timeout)
        except (OSError, subprocess.SubprocessError) as exc:
            pins.report(False, "sel4test configure failed", str(exc))
            return 1
    try:
        build(arguments.timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        pins.report(False, "sel4test build failed", str(exc))
        return 1

    try:
        seen, summary = boot(arguments.timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        pins.report(False, "sel4test boot failed", str(exc))
        return 1

    if not seen:
        pins.report(
            False,
            "sel4test did not report success",
            f"never saw {MARKER!r} within {arguments.timeout}s",
        )
        return 1
    pins.report(True, "sel4test passed on qemu-riscv-virt", summary or MARKER)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except pins.PinError as exc:
        pins.report(False, "sel4test run failed", str(exc))
        sys.exit(1)
