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
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

import pins
from targets import TARGETS, Target

ENV_SCRIPT = pins.ROOT / "scripts" / "env.sh"
TEST_SUMMARY = re.compile(r"Test suite passed\.\s+(\d+) tests passed\.\s+(\d+) tests disabled\.")
# The guest's own verdict: a check that fails prints "test: FAIL ..." and the
# summary counts them ("N checks FAILED"). The marker is the boot's last line,
# not the verdict -- a failing test still reaches it -- so a run whose checks
# failed is a failed run however the marker arrived.
GUEST_FAILURE = re.compile(r"test: FAIL|checks FAILED")


def preflight(target: Target) -> list[str]:
    """What this machine still needs before the target can build or boot, as a
    list of "what (which make command fixes it)". Checked here because the
    failure otherwise surfaces as `bash returned exit status 1`: the vendored
    tree's absence is a missing init-build.sh -- it is placed by `make deps`
    and gitignored -- and a missing toolchain is cmake's least readable
    error."""
    missing: list[str] = []
    if not (pins.ROOT / "init-build.sh").is_file() or not (pins.ROOT / "kernel").is_dir():
        missing.append("the vendored seL4 tree (make deps)")
    if not (pins.ROOT / "third_party/tools/venv/bin/cmake").is_file():
        missing.append("the pinned host tools (make tools)")
    if not any(pins.ROOT.glob("third_party/toolchain/*/shims/riscv64-unknown-elf-gcc")):
        missing.append("the pinned RISC-V toolchain (make tools)")
    if shutil.which("qemu-system-riscv64") is None:
        # QEMU is the one piece taken from the host (specs/build.md's host
        # prerequisites). Needed even for --build-only: configure extracts the
        # machine's device tree by running it
        # (kernel/src/plat/qemu-riscv-virt/config.cmake:133).
        missing.append("qemu-system-riscv64 (a host package)")
    return missing


def bash(command: str, cwd: Path, timeout: int) -> None:
    """Run a shell command with Aegir's pinned tools on PATH."""
    print(f"INFO  (cd {cwd.relative_to(pins.ROOT)} && {command})", flush=True)
    # The marker after sourcing env.sh is a diagnostic: a `set -e` death inside
    # the source prints nothing, and without the marker a silent failure cannot
    # be told apart from the command itself failing to start.
    subprocess.run(
        [
            "bash",
            "-c",
            f"set -euo pipefail; . {ENV_SCRIPT}; "
            f"echo 'INFO  environment ready (scripts/env.sh)' >&2; {command}",
        ],
        cwd=str(cwd),
        check=True,
        timeout=timeout,
        # Build steps never read the terminal. Handing them /dev/null instead
        # also takes the tty away from anything (qemu's -nographic stdio setup
        # is the known offender) that would poke it from the background process
        # group `timeout` puts this pipeline in -- that poke is a SIGTTOU stop,
        # which looks exactly like a configure that hangs forever.
        stdin=subprocess.DEVNULL,
    )


def configure(target: Target, build_dir: Path, timeout: int, extra_flags: str = "") -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    flags = " ".join(target.configure_flags)
    if extra_flags:
        flags = f"{flags} {extra_flags}".strip()
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


def hosted_cxx() -> bool:
    """Whether the hosted C++ runtime is wanted for this build.

    An environment switch rather than a target property, because it selects a
    runtime, not a machine: `AEGIR_HOSTED_CXX=1 make build`. It becomes a cmake
    cache option of the same name, and the runtime bootstrap below keys off it.
    """
    value = os.environ.get("AEGIR_HOSTED_CXX", "").strip().lower()
    return value not in ("", "0", "off", "no", "false")


def build_runtimes(target: Target, timeout: int) -> None:
    """Build the hosted runtime's two vendored pieces for this target.

    Run before configure because cmake imports them (libs/aegir-musl,
    libs/aegir-libcxx) and refuses to configure without them. Both scripts are
    idempotent: an existing install is left alone, so this is cheap after the
    first build of a target (and `make clean` is what forces a rebuild).
    """
    root = ENV_SCRIPT.parent.parent
    bash(f"bash scripts/build_musl.sh {target.name}", root, timeout)
    bash(f"bash scripts/build_libcxx.sh {target.name}", root, timeout)


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


# The characters the acceptance script types, as QEMU's qcodes: a lowercase
# letter or a digit is its own name, and the two whitespaces are QEMU's.
# Anything else would need a shift chord, and no step types one yet.
_PRESS_QCODES = {"\t": "tab", "\n": "ret"}


def send_key(socket_path: Path, keys: str) -> bool:
    """Keypresses through QEMU's QMP socket, one per character of `keys`: the
    acceptance check's fingers. False -- and nothing sent -- when a character
    has no qcode, because half a typed password is worse than none."""
    qcodes: list[str] = []
    for char in keys:
        qcode = _PRESS_QCODES.get(char, char)
        if not ((len(qcode) == 1 and (qcode.islower() or qcode.isdigit())) or
                qcode in _PRESS_QCODES.values()):
            return False
        qcodes.append(qcode)
    for qcode in qcodes:
        qmp_command(
            socket_path,
            {"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}},
        )
        # The guest's input queue is eight descriptors deep
        # (libs/aegir-virtio's kQueueSize) and QEMU drops what does not fit:
        # sixteen keys sent back to back arrive as a press burst at QMP
        # speed, and the tail is lost. A typist's pace is what a queue
        # without flow control is given.
        time.sleep(0.05)
    return True


def input_send_event(socket_path: Path, events: tuple[dict, ...]) -> str | None:
    """Pointer motion and clicks through QEMU's QMP socket: the acceptance
    check's hand on the mouse or tablet. No device is named: with no console
    bound (the display is `none`), events fall through to the unbound input
    handlers -- abs lands on the tablet, rel on the mouse, btn on whichever
    registered first (ui/input.c's qemu_input_find_handler). The events are
    QMP's own InputEvent dicts ({type: abs/rel/btn, ...}). One command per
    event, at the typist's pace send_key already keeps: abs rides the
    tablet's queue and btn the mouse's, and a burst that wakes the console
    once for both queues is drained mouse-first whatever the send order --
    a click meant for where the motion went lands where the pointer stood.
    Returns None on success, QMP's error text when it refuses."""
    for event in events:
        answer = qmp_command(
            socket_path,
            {"execute": "input-send-event", "arguments": {"events": [event]}},
        )
        if "error" in answer:
            return str(answer["error"])
        time.sleep(0.05)
    return None


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
    # The QMP script, in order: each step's trigger counts matches and fires
    # on every one, up to `times` (0: no cap -- a cue that repeats once per
    # session gets an answer per session).
    step_matches = [0] * len(target.qmp_steps)
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
            if GUEST_FAILURE.search(stripped):
                failed = True
            for index, step in enumerate(target.qmp_steps):
                if (step.times != 0 and step_matches[index] >= step.times) or re.search(
                    step.trigger, stripped
                ) is None:
                    continue
                step_matches[index] += 1
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
                    if device in step.bands and not bands_at_posts(width, height, pixels):
                        print(
                            f"    runner: FAIL {device} shows {width}x{height} "
                            "without the bands at their posts",
                            flush=True,
                        )
                        failed = True
                        continue
                    for at in step.pixels:
                        named, x, y, r, g, b = at
                        if named != device:
                            continue
                        offset = (y * width + x) * 3
                        shown = pixels[offset], pixels[offset + 1], pixels[offset + 2]
                        if shown != (r, g, b):
                            print(
                                f"    runner: FAIL {device} at ({x},{y}) shows "
                                f"{shown}, expected ({r},{g},{b})",
                                flush=True,
                            )
                            failed = True
                    if failed:
                        continue
                    print(f"    runner: {device} shows {width}x{height}, true to its checks", flush=True)
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
                if step.events:
                    # The guest said it is waiting: move or click the
                    # pointer. Events persist in the driver's posted buffers,
                    # so the send is not a race -- but the order the console
                    # drains two devices' queues is, so the send is one
                    # command per event, paced (input_send_event's
                    # docstring). A refused send would let the guest wait
                    # forever, so the answer is checked.
                    problem = input_send_event(socket_path, step.events)
                    if problem is not None:
                        print(
                            f"    runner: FAIL input-send-event: {problem}",
                            flush=True,
                        )
                        failed = True
                if step.press is not None:
                    # The guest said it is waiting: type the keys. Events
                    # persist in the driver's posted buffers, so the presses
                    # are not a race.
                    if not send_key(socket_path, step.press):
                        print(
                            f"    runner: FAIL a character of '{step.press}' has no qcode",
                            flush=True,
                        )
                        failed = True
            if target.marker in stripped:
                seen = True
            # The run is done when the marker has printed and the script is
            # played out -- the business may continue past the boot marker (a
            # login on the greeter, the bureau's backdrop), and a cue that
            # never comes is the timeout's and the gate's to report.
            if seen and all(played > 0 for played in step_matches):
                break
        # A step whose cue never printed is a check that never ran: the run
        # does not get to pass on evidence that was never taken.
        for index, step in enumerate(target.qmp_steps):
            if step_matches[index] == 0:
                print(
                    f"    runner: FAIL the cue never printed: {step.trigger}",
                    flush=True,
                )
                failed = True
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

    missing = preflight(target)
    if missing:
        pins.report(False, f"{target.name} cannot build yet", "missing: " + "; ".join(missing))
        return 1

    wanted_flags = " ".join(target.configure_flags)
    hosted = hosted_cxx()
    # Always passed explicitly, ON or OFF: the cmake cache keeps a value set by
    # a previous build, so omitting the flag would leave a hosted tree hosted
    # when the environment says otherwise (and vice versa).
    extra_flags = f"-DAEGIR_HOSTED_CXX={'ON' if hosted else 'OFF'}"
    wanted_flags = f"{wanted_flags} {extra_flags}".strip()
    try:
        if hosted:
            build_runtimes(target, arguments.timeout)
        stamp = configured_flags(build_dir)
        if (
            arguments.reconfigure
            or not (build_dir / "build.ninja").is_file()
            or (stamp != "" and stamp != wanted_flags)
        ):
            print(f"INFO  (re)configuring {target.name} as: {wanted_flags or 'defaults'}", flush=True)
            configure(target, build_dir, arguments.timeout, extra_flags)
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
