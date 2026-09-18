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
import json
import os
import re
import shlex
import signal
import socket
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


def qmp_command(socket_path: Path, command: dict) -> dict:
    """One command through QEMU's QMP socket, answered as a dict.

    The conversation is newline-terminated JSON each way: the server's
    greeting, capabilities negotiation, then the command itself.
    """
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(30)
    client.connect(str(socket_path))
    try:
        stream = client.makefile("rw", encoding="utf-8", newline="\n")
        stream.readline()  # the greeting
        stream.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
        stream.flush()
        stream.readline()
        stream.write(json.dumps(command) + "\n")
        stream.flush()
        return json.loads(stream.readline())
    finally:
        client.close()


def send_key(socket_path: Path, key: str) -> None:
    """One keypress through QEMU's QMP socket: the acceptance check's finger."""
    qmp_command(
        socket_path,
        {"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": key}]}},
    )


def screen_dump(socket_path: Path, device: str, filename: str) -> str | None:
    """One console's screen, as a PPM QEMU writes: the acceptance check's eyes.
    None when the dump happened, QMP's error text when it did not."""
    answer = qmp_command(
        socket_path,
        {"execute": "screendump", "arguments": {"filename": filename, "device": device}},
    )
    if "error" in answer:
        return str(answer["error"])
    return None


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    """A binary PPM as (width, height, rgb bytes). QEMU's screendump writes
    exactly one shape: P6, decimal header, 255."""
    data = path.read_bytes()
    tokens: list[bytes] = []
    at = 0
    while len(tokens) < 4:
        while data[at : at + 1].isspace():
            at += 1
        if data[at : at + 1] == b"#":  # a comment runs to end of line
            while data[at : at + 1] != b"\n":
                at += 1
            continue
        end = at
        while not data[end : end + 1].isspace():
            end += 1
        tokens.append(data[at:end])
        at = end
    at += 1  # exactly one whitespace ends the header
    magic, width, height, ceiling = tokens[0], int(tokens[1]), int(tokens[2]), int(tokens[3])
    if magic != b"P6" or ceiling != 255:
        raise ValueError(f"{path}: not the P6/255 PPM a screendump writes")
    return width, height, data[at : at + width * height * 3]


def bands_at_posts(width: int, height: int, pixels: bytes) -> bool:
    """The driver's self-test pattern is three vertical bands, red, green and
    blue -- so a third into the bands, mid-height, must be exactly those."""

    def pixel(x: int, y: int) -> tuple[int, int, int]:
        at = (y * width + x) * 3
        return pixels[at], pixels[at + 1], pixels[at + 2]

    return (
        len(pixels) == width * height * 3
        and pixel(width // 6, height // 2) == (255, 0, 0)
        and pixel(width // 2, height // 2) == (0, 255, 0)
        and pixel(5 * width // 6, height // 2) == (0, 0, 255)
    )


def ensure_disk(build_dir: Path) -> None:
    """The machine's block device needs a disk to be a block device *of*. It is
    a GPT with three FAT partitions -- two holding a known file, one empty and
    writable -- built by make_disk.py in the build directory. Created once and
    left alone, and QEMU's -snapshot keeps even a writing run off it: a disk
    that changes between runs is not something to depend on. It lives in the
    build output rather than the repository, where scratch belongs."""
    disk = build_dir / "disk.img"
    if not disk.exists():
        subprocess.run(
            [sys.executable, str(Path(__file__).parent / "make_disk.py"), str(disk)],
            check=True,
        )


def boot_interactive(target: Target, build_dir: Path) -> int:
    """Boot the image with QEMU's own window on the displays: the user is the
    runner. The keys the acceptance check's script would press are theirs to
    press (the console says when, and which), the heads are the window's tabs,
    and QEMU stops when its window closes, not at a marker -- so nothing here
    watches, and nothing here is timed out but the user."""
    ensure_disk(build_dir)
    extra = " ".join(target.qemu_args)
    # -g/-s replace simulate's -nographic: a GTK window on the consoles, the
    # serial console on the terminal. Attached with `=`, for the same reason
    # --extra-qemu-args is: argparse reads a loose value starting with `-` as
    # an option of its own.
    command = (
        "./simulate --graphic='-display gtk' --serial='-serial stdio' --extra-qemu-args="
        + shlex.quote(extra)
    )
    return subprocess.call(
        ["bash", "-c", f"set -euo pipefail; . {ENV_SCRIPT}; exec {command}"],
        cwd=str(build_dir),
    )


def boot_and_watch(target: Target, build_dir: Path, timeout: int) -> tuple[bool, bool, str]:
    """Boot the image, streaming the console until the marker appears.

    Answers (marker seen, an acceptance check failed, test summary): a failed
    screen check is a failed run even when the marker arrived."""
    # The target's extra arguments belong to QEMU, not to the simulate script, so
    # they go through --extra-qemu-args as one string -- attached with `=` rather
    # than passed as a separate argument, because the value starts with `-bios`
    # and the script's argparse refuses a value that looks like an option (and
    # would read a loose `-bios` as its own `-b`).
    ensure_disk(build_dir)

    extra = " ".join(target.qemu_args)
    command = "./simulate --extra-qemu-args=" + shlex.quote(extra)
    # A leftover socket from a previous run would make QEMU's own bind fail.
    if target.qmp_socket is not None:
        (build_dir / target.qmp_socket).unlink(missing_ok=True)
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
    failed = False
    # The QMP script, in order: each step's trigger counts matches (two heads
    # say the same line, so a step can want two), and fires once.
    step_matches = [0] * len(target.qmp_steps)
    step_done = [False] * len(target.qmp_steps)
    step_dims: list[tuple[int, int]] = []
    summary = ""
    try:
        stream = process.stdout
        if stream is None:  # pragma: no cover - Popen above always pipes
            return False, True, ""
        for line in stream:
            stripped = line.rstrip("\n")
            if stripped:
                print(f"    {stripped}", flush=True)
            match = TEST_SUMMARY.search(stripped)
            if match:
                summary = f"{match.group(1)} tests passed, {match.group(2)} disabled"
            for index, step in enumerate(target.qmp_steps):
                if step_done[index] or re.search(step.trigger, stripped) is None:
                    continue
                step_matches[index] += 1
                if step_matches[index] < step.times:
                    continue
                step_done[index] = True
                socket_path = build_dir / str(target.qmp_socket)
                # The screens first, then the key: the key paces the guest's
                # next step, so everything this step checks must be read
                # before the guest moves on.
                for device in step.dumps:
                    dump = f"dump-{device}-{index}.ppm"
                    problem = screen_dump(socket_path, device, dump)
                    if problem is not None:
                        print(f"    runner: FAIL screendump of {device}: {problem}", flush=True)
                        failed = True
                        continue
                    try:
                        width, height, pixels = read_ppm(build_dir / dump)
                    except (ValueError, OSError) as reading:
                        print(f"    runner: FAIL the dump of {device}: {reading}", flush=True)
                        failed = True
                        continue
                    if not bands_at_posts(width, height, pixels):
                        print(
                            f"    runner: FAIL {device} shows {width}x{height} "
                            "without the bands at their posts",
                            flush=True,
                        )
                        failed = True
                        continue
                    print(f"    runner: {device} shows {width}x{height}, bands true", flush=True)
                    step_dims.append((width, height))
                if step.dumps and not failed:
                    if sorted(step_dims) != sorted(step.expect):
                        print(
                            f"    runner: FAIL the screens show {sorted(step_dims)}, "
                            f"expected {sorted(step.expect)}",
                            flush=True,
                        )
                        failed = True
                    else:
                        print(
                            f"    runner: the screens are {sorted(step.expect)} -- as cued",
                            flush=True,
                        )
                step_dims.clear()
                if step.press is not None:
                    # The guest said it is waiting: press the key. Events
                    # persist in the driver's posted buffers, so the press is
                    # not a race.
                    send_key(socket_path, step.press)
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
    return seen, failed, summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--target", required=True, choices=sorted(TARGETS))
    parser.add_argument("--timeout", type=int, default=900, help="seconds per step")
    parser.add_argument("--build-only", action="store_true", help="stop after building")
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="boot with a GTK window on the displays and no watching: the user "
        "presses the keys the acceptance script would, and QEMU stops when the "
        "window closes",
    )
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

    if arguments.interactive:
        return boot_interactive(target, build_dir)

    try:
        seen, failed, summary = boot_and_watch(target, build_dir, arguments.timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        pins.report(False, f"{target.name} boot failed", str(exc))
        return 1

    if not seen or failed:
        pins.report(
            False,
            f"{target.name} did not report success",
            f"never saw {target.marker!r} within {arguments.timeout}s"
            if not seen
            else "an acceptance check failed (see the runner's lines above)",
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
