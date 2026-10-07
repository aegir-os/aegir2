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
import queue
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pins
from targets import TARGETS, Target

ENV_SCRIPT = pins.ROOT / "scripts" / "env.sh"
TEST_SUMMARY = re.compile(r"Test suite passed\.\s+(\d+) tests passed\.\s+(\d+) tests disabled\.")
# The guest's own verdict: a check that fails prints "test: FAIL ..." and the
# summary counts them ("N checks FAILED"). The marker is the boot's last line,
# not the verdict -- a failing test still reaches it -- so a run whose checks
# failed is a failed run however the marker arrived. A smoke that reports its
# own failure the same way ("NAME_SMOKE_FAIL") counts too. A boot service's
# own refusal counts as well: `auth: FAIL ...` is a session that came up
# wrong, and `vfs: no room ...` is a table that filled -- the two that let a
# session whose namespace would not bind pass unnoticed.
GUEST_FAILURE = re.compile(r"test: FAIL|checks FAILED|_SMOKE_FAIL|auth: FAIL|vfs: no room")
# The supervisor's fault report (specs/director.md). `hello` is the one boot
# service that faults on purpose, and it does so before the boot marker prints;
# any report *after* the marker is a service nobody asked to die, which the run
# must fail on -- the console was suspended mid-drag this way once and the run
# still passed, because `hello`'s fault is the only one the marker tolerates.
SUPERVISOR_FAULT = re.compile(r"supervisor:.*faulted")
# The boot's ready/faulted summary. The ready count moves with the manifest;
# the fault count is the one that must stay at hello's single deliberate fault.
BOOT_SUMMARY = re.compile(r"(\d+) ready, (\d+) faulted")
# The guest's own rectangles, so a click can follow the layout rather than a
# pinned pixel: a line `rect <name> <x> <y> <w> <h>`, screen pixels. A step's
# `clicks` names one and the runner lands on it wherever the widget is.
RECT_CUE = re.compile(r"\brect (\S+) (\d+) (\d+) (\d+) (\d+)")


def preflight(target: Target) -> list[str]:
    """What this machine still needs before the target can build or boot, as a
    list of "what (which make command fixes it)". Checked here because the
    failure otherwise surfaces as `bash returned exit status 1`: a missing
    vendored tree or toolchain is cmake's least readable error."""
    missing: list[str] = []
    if not (pins.ROOT / "kernel" / "CMakeLists.txt").is_file() or not (
        pins.ROOT / "tools/seL4/cmake-tool"
    ).is_dir():
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
    if shutil.which("qemu-img") is None:
        # A run's disk is answered through an overlay this script makes
        # (with_overlay), not QEMU's -snapshot: QEMU creates its own in a
        # directory compiled into the binary, so a host that mounts that one
        # read-only could not start a guest at all.
        missing.append("qemu-img (a host package; the run's disk overlay)")
    return missing


def stop_group(process: subprocess.Popen) -> None:
    """Take a `start_new_session` command and its children down.

    A signal reaches only the process it was sent to, and a build's cmake/ninja
    children are the shell's, not its caller's: killing the shell alone leaves
    them running with nobody to stop them (AGENTS.md). The shell leads its own
    process group by construction, so signalling the group reaches the tree --
    SIGTERM first, SIGKILL if it will not go.
    """
    if process.poll() is not None:
        return
    try:
        os.killpg(os.getpgid(process.pid), signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def bash(command: str, cwd: Path, timeout: int) -> None:
    """Run a shell command with Aegir's pinned tools on PATH.

    The shell leads its own process group and the group is taken down whichever
    way the command ends -- its own timeout, the outer `timeout` ending this
    script, or a Ctrl-C -- so a long build's children cannot outlive the runner
    (stop_group).
    """
    print(f"INFO  (cd {cwd.relative_to(pins.ROOT)} && {command})", flush=True)
    # The marker after sourcing env.sh is a diagnostic: a `set -e` death inside
    # the source prints nothing, and without the marker a silent failure cannot
    # be told apart from the command itself failing to start.
    process = subprocess.Popen(
        [
            "bash",
            "-c",
            f"set -euo pipefail; . {ENV_SCRIPT}; "
            f"echo 'INFO  environment ready (scripts/env.sh)' >&2; {command}",
        ],
        cwd=str(cwd),
        # Build steps never read the terminal. Handing them /dev/null instead
        # also takes the tty away from anything (qemu's -nographic stdio setup
        # is the known offender) that would poke it from the background process
        # group this pipeline runs in -- that poke is a SIGTTOU stop, which
        # looks exactly like a configure that hangs forever.
        stdin=subprocess.DEVNULL,
        start_new_session=True,
    )
    try:
        returncode = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        stop_group(process)
        raise
    except BaseException:
        # The outer `timeout` (or the user) ended us: main turns that signal
        # into an exception, and the build tree has to come down with it.
        stop_group(process)
        raise
    if returncode != 0:
        raise subprocess.CalledProcessError(returncode, process.args)


def configure(target: Target, build_dir: Path, timeout: int, extra_flags: str = "") -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    flags = " ".join(target.configure_flags)
    if extra_flags:
        flags = f"{flags} {extra_flags}".strip()
    root = ENV_SCRIPT.parent.parent
    # Configure directly, not through the root init-build.sh: that script pins
    # kernel/gcc.cmake, and seL4 keeps a toolchain file it is handed instead of
    # choosing one from TRIPLE (kernel/configs/seL4Config.cmake:244-267), so the
    # pinned file would win and TRIPLE in configs/ would be ignored. The command
    # below is what init-build.sh itself runs, so TRIPLE selects
    # kernel/llvm.cmake and the build is clang (specs/build.md).
    #
    # It is also what an in-tree project needs: the root init-build.sh decides
    # which project to configure by looking for a CMakeLists.txt next to itself
    # (tools/seL4/cmake-tool/init-build.sh:41-54), and next to *our* root there
    # is one, so it would configure Aegir instead.
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
    runtime, not a machine. It defaults ON; `AEGIR_HOSTED_CXX=0` selects the
    lean freestanding build (the root task and services, with the greeter and
    bureau as placeholders). It becomes a cmake cache option of the same name,
    and the runtime bootstrap below keys off it.
    """
    value = os.environ.get("AEGIR_HOSTED_CXX", "").strip().lower()
    return value not in ("0", "off", "no", "false")


def toolkit() -> bool:
    """Whether the GUI toolkit is wanted (requires the hosted runtime).

    Defaults ON, and is forced OFF when the hosted runtime is: the toolkit
    cannot be built without it, and the cmake option's guard would otherwise be
    the only thing saying so. `AEGIR_TOOLKIT=0` leaves it out while keeping the
    hosted runtime, which is how the runtime is proven without the toolkit.
    """
    if not hosted_cxx():
        return False
    value = os.environ.get("AEGIR_TOOLKIT", "").strip().lower()
    return value not in ("0", "off", "no", "false")


def build_builtins(target: Target, timeout: int) -> None:
    """Build compiler-rt's builtins and publish them where clang finds them.

    Every link needs these, freestanding or hosted: seL4's user-mode link rule
    injects `-lgcc` and the crt objects (projects/musllibc/Findmusllibc.cmake),
    and with libgcc gone those names must resolve to compiler-rt
    (scripts/build_compiler_rt.sh). Idempotent.
    """
    root = ENV_SCRIPT.parent.parent
    bash(f"bash scripts/build_compiler_rt.sh {target.name}", root, timeout)


def build_runtimes(target: Target, timeout: int) -> None:
    """Build the hosted runtime's vendored pieces for this target.

    Full musl, libc++/libcxxabi/libunwind, FreeType (the font service's
    rasterizer), and zlib+libpng and libjpeg-turbo (the PNG and JPEG classes'
    decoders). Run before configure because cmake imports them (libs/aegir-musl,
    libs/aegir-libcxx, libs/aegir-freetype, libs/aegir-libpng,
    libs/aegir-libjpeg) and refuses to configure without them. Each script is
    idempotent: an existing install is left alone, so this is cheap after the
    first build of a target (and `make clean` is what forces a rebuild).
    """
    root = ENV_SCRIPT.parent.parent
    bash(f"bash scripts/build_musl.sh {target.name}", root, timeout)
    bash(f"bash scripts/build_libcxx.sh {target.name}", root, timeout)
    bash(f"bash scripts/build_freetype.sh {target.name}", root, timeout)
    bash(f"bash scripts/build_zlib.sh {target.name}", root, timeout)
    bash(f"bash scripts/build_libpng.sh {target.name}", root, timeout)
    bash(f"bash scripts/build_libjpeg.sh {target.name}", root, timeout)
    bash(f"bash scripts/build_llvm.sh {target.name}", root, timeout)


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


def configured_with_clang(build_dir: Path) -> bool:
    """Whether a build directory was configured with the pinned clang.

    seL4 picks its toolchain at configure time from `TRIPLE`
    (configs/riscv64-qemu-virt.cmake): with it, kernel/llvm.cmake and clang;
    without, `${CROSS_COMPILER_PREFIX}g++`. `TRIPLE` is a cache variable, so a
    directory configured before it was set keeps GCC in its `CMakeCache.txt`,
    and neither a re-run of cmake nor a matching flag stamp switches it -- the
    build then fails in our own sources on a warning only GCC raises
    (aegir-trinket's `-Walloc-size-larger-than`). A cache with no compiler line
    is treated as usable: the caller only asks about a directory that already
    has a build.ninja.
    """
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return True
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_C_COMPILER:") or line.startswith("CMAKE_CXX_COMPILER:"):
            if "clang" not in line:
                return False
    return True


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
# letter or a digit is its own name, and the three whitespaces are QEMU's.
# A shifted character is a chord (_PRESS_CHORDS) or a shift plus its base key.
_PRESS_QCODES = {"\t": "tab", "\n": "ret", " ": "spc", "-": "minus",
                 "/": "slash", ".": "dot", ",": "comma", "=": "equal"}

# A character that needs a shift chord: QEMU's send-key holds the whole list
# down together, so a chord is one call. Only what a step types is here.
# `>` and `<` are the redirection operators a shell line can carry, `$` names a
# variable and `"` groups a word (specs/shell.md).
_PRESS_CHORDS = {":": ("shift", "semicolon"),
                 ">": ("shift", "dot"),
                 "<": ("shift", "comma"),
                 "$": ("shift", "4"),
                 "\"": ("shift", "apostrophe"),
                 "|": ("shift", "backslash"),
                 # The file requester's Pattern box types an AmigaDOS wildcard
                 # (`#?`, specs/pattern.md), so the two keys it needs are here.
                 "#": ("shift", "3"),
                 "?": ("shift", "slash")}

# The keys that are not characters, spelled between angle brackets in a step's
# `press` -- the editor's arrows and editing keys (specs/terminal.md). A token
# is one key; `<up><backspace>6\n` is up, backspace, 6, Enter.
_PRESS_KEYS = {
    "up": "up",
    "down": "down",
    "left": "left",
    "right": "right",
    "home": "home",
    "end": "end",
    "ret": "ret",
    "enter": "ret",
    "tab": "tab",
    "spc": "spc",
    "space": "spc",
    "esc": "esc",
    "backspace": "backspace",
    "delete": "delete",
    # The editor's mode toggle (specs/trinket/editor.md): insert mode draws the
    # block cursor, overwrite the underline.
    "insert": "insert",
    # A screen shortcut (specs/workbench.md): a chord, so the modifier is held
    # with the key in one send-key. Super+Space is the bureau's Execute.
    "win-space": ("meta_l", "spc"),
    # The console's Break (specs/process.md): Ctrl-C, held with the key, so the
    # terminal sees the control modifier and sets **C** on the foreground line.
    "ctrl-c": ("ctrl", "c"),
}


def send_key(socket_path: Path, keys: str, delay: float = 0.05) -> bool:
    """Keypresses through QEMU's QMP socket, one per key of `keys`: the
    acceptance check's fingers. A printable character is itself, and a key
    with no character -- an arrow, Backspace -- is `<name>` (_PRESS_KEYS).
    False -- and nothing sent -- when a key has no qcode, because half a typed
    password is worse than none. `delay` is the typist's pace: a widget that
    repaints a whole window and waits on the GPU per key is not the terminal's
    grid, and QEMU drops what the guest's input ring cannot take."""
    qcodes: list[tuple[str, ...]] = []
    index = 0
    while index < len(keys):
        if keys[index] == "<":
            end = keys.find(">", index + 1)
            if end == -1:
                return False
            name = keys[index + 1 : end]
            if name not in _PRESS_KEYS:
                return False
            entry = _PRESS_KEYS[name]
            qcodes.append(entry if isinstance(entry, tuple) else (entry,))
            index = end + 1
            continue
        char = keys[index]
        if char in _PRESS_CHORDS:
            qcodes.append(_PRESS_CHORDS[char])
            index += 1
            continue
        if char.isupper():
            qcodes.append(("shift", char.lower()))
            index += 1
            continue
        qcode = _PRESS_QCODES.get(char, char)
        if not ((len(qcode) == 1 and (qcode.islower() or qcode.isdigit())) or
                qcode in _PRESS_QCODES.values()):
            return False
        qcodes.append((qcode,))
        index += 1
    for qcode in qcodes:
        # The pace is kept *before* every key, the first included. A step that
        # clicks the field it then types into sends the click and the keys with
        # no gap otherwise, and the click's focus and the first key arrive
        # close enough that the key is routed before the field is focused: the
        # first character is lost (file_requester.cc's note is the same race).
        # The guest's input queue is eight descriptors deep
        # (libs/aegir-virtio's kQueueSize) and QEMU drops what does not fit:
        # sixteen keys sent back to back arrive as a press burst at QMP speed,
        # and the tail is lost. A typist's pace is what a queue without flow
        # control is given.
        time.sleep(delay)
        qmp_command(
            socket_path,
            {
                "execute": "send-key",
                "arguments": {"keys": [{"type": "qcode", "data": q} for q in qcode]},
            },
        )
    return True


def input_send_event(socket_path: Path, events: tuple[dict, ...]) -> str | None:
    """Pointer motion and clicks through QEMU's QMP socket: the acceptance
    check's hand on the mouse or tablet. No device is named: with no console
    bound (the display is `none`), events fall through to the unbound input
    handlers -- abs lands on the tablet, rel on the mouse, btn on whichever
    registered first (ui/input.c's qemu_input_find_handler). The events are
    QMP's own InputEvent dicts ({type: abs/rel/btn, ...}). One command per
    event, at the typist's pace send_key already keeps: abs rides the
    tablet's queue and btn the mouse's, and the console drains the tablet
    before the mouse, so a click meant for where the motion went lands there.
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


def axis_value(pixel: int, span: int) -> int:
    """A screen pixel as the tablet's 0..32767 axis -- the console maps it back
    with the inverse (apps/.../aegir-console/src/main.cc)."""
    if span <= 0:
        return 0
    value = int(pixel * 32767 / span)
    return 0 if value < 0 else (32767 if value > 32767 else value)


def input_send_clicks(socket_path: Path, clicks: tuple, anchors: dict,
                      screen: tuple[int, int]) -> str | None:
    """Click each named rectangle at the fraction `(rx, ry)` of it. The guest
    reported the rectangle (a `rect <name> ...` line, RECT_CUE), so the click
    follows the layout -- a font or metric change moves it with the widget --
    instead of a pinned coordinate. Returns QMP's error text when a click is
    refused, or a message when a name was never reported."""
    for name, rx, ry in clicks:
        box = anchors.get(name)
        if box is None:
            return f"no rect cue for {name!r}"
        x = box[0] + int(box[2] * rx)
        y = box[1] + int(box[3] * ry)
        problem = input_send_event(socket_path, (
            {"type": "abs", "data": {"axis": "x", "value": axis_value(x, screen[0])}},
            {"type": "abs", "data": {"axis": "y", "value": axis_value(y, screen[1])}},
            {"type": "btn", "data": {"button": "left", "down": True}},
            {"type": "btn", "data": {"button": "left", "down": False}},
        ))
        if problem is not None:
            return problem
    return None


def input_send_drags(socket_path: Path, drags: tuple, anchors: dict,
                     screen: tuple[int, int]) -> str | None:
    """Drag from one named rectangle to another: move to the fraction of the
    first, press, move to the fraction of the second, release. A widget's thumb
    or knob is carried this way -- the press grabs it, the motion moves it and
    the release ends the gesture -- where a `click` (press and release in place)
    cannot. The events are paced (input_send_event), and the press is its own
    command, so the console's drain delivers the pointer to the thumb before
    the button goes down. Returns QMP's error text, or a message when a name was
    never reported."""
    for from_name, frx, fry, to_name, trx, try_ in drags:
        from_box = anchors.get(from_name)
        to_box = anchors.get(to_name)
        if from_box is None:
            return f"no rect cue for {from_name!r}"
        if to_box is None:
            return f"no rect cue for {to_name!r}"
        from_x = from_box[0] + int(from_box[2] * frx)
        from_y = from_box[1] + int(from_box[3] * fry)
        to_x = to_box[0] + int(to_box[2] * trx)
        to_y = to_box[1] + int(to_box[3] * try_)
        problem = input_send_event(socket_path, (
            {"type": "abs", "data": {"axis": "x", "value": axis_value(from_x, screen[0])}},
            {"type": "abs", "data": {"axis": "y", "value": axis_value(from_y, screen[1])}},
            {"type": "btn", "data": {"button": "left", "down": True}},
            {"type": "abs", "data": {"axis": "x", "value": axis_value(to_x, screen[0])}},
            {"type": "abs", "data": {"axis": "y", "value": axis_value(to_y, screen[1])}},
            {"type": "btn", "data": {"button": "left", "down": False}},
        ))
        if problem is not None:
            return problem
    return None


def screen_dump(socket_path: Path, device: str, filename: str) -> str | None:
    """One console's screen, as a PPM QEMU writes: the acceptance check's eyes.
    None when the dump happened, QMP's error text when it did not.

    A cue fires on a console line, which the guest may print *before* QEMU has
    processed the flush that carries the pixels a service just drew (the
    virtio-gpu scanout is the host-side copy a RESOURCE_FLUSH updates, and that
    runs on the device, not the vCPU). A short settle lets the flush land, so a
    dump taken at a cue sees the frame the cue announced rather than the one
    before it. It only helps that race: a frame that is genuinely wrong stays
    wrong and still fails the check."""
    time.sleep(0.15)
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


def ensure_disk(target: Target, build_dir: Path) -> None:
    """The machine's block device needs a disk to be a block device *of*. It is
    a GPT built by make_disk.py in the build directory, with the command set
    packed as Sys:C (specs/dos.md). Rebuilt on every run, because the commands
    it carries change with the build and the AEGIR partition is sized from
    them: a stale disk would serve a stale command. A writing run is kept off it
    by the overlay `with_overlay` puts beside it. It lives in the build output
    rather than the repository, where scratch belongs.

    A target that carries the development tree (specs/development.md) also gets
    `Sys:Development`, laid out from this build's own artifacts so the compiler
    and the sysroot are one build."""
    disk = build_dir / "disk.img"
    commands = build_dir / "sys-c"
    datatypes = build_dir / "sys-datatypes"
    command = [sys.executable, str(Path(__file__).parent / "make_disk.py"), str(disk)]
    if commands.is_dir():
        command += ["--commands", str(commands)]
    if datatypes.is_dir():
        command += ["--datatypes", str(datatypes)]
    if target.development:
        command += ["--development", str(development_tree(build_dir))]
    subprocess.run(command, check=True)


def development_tree(build_dir: Path) -> Path:
    """Lay out `Sys:Development` for make_disk (specs/development.md): the
    compiler and the scale acceptance's huge command under C, the sysroot under
    Include and Libs. The compiler is this build's `aegir-cc` app, deployed as
    `cc`; the sysroot is the next increment -- the first compile is freestanding
    (`specs/clang-on-aegir.md`), so Include and Libs are the tree's shape and not
    yet its content."""
    root = build_dir / "development"
    shutil.rmtree(root, ignore_errors=True)
    (root / "C").mkdir(parents=True)
    (root / "Include").mkdir()
    (root / "Libs").mkdir()
    compiler = build_dir / "apps/hosted/aegir-cc/aegir-cc"
    if compiler.is_file():
        shutil.copy2(compiler, root / "C" / "cc")
    # The scale acceptance's deliberately huge command (specs/memory.md) travels
    # beside the compiler: it runs at session start, and a spawn-path cap that
    # creeps back fails on it by name.
    big = build_dir / "apps/hosted/aegir-big/aegir-big"
    if big.is_file():
        shutil.copy2(big, root / "C" / "aegir-big")
    # The POSIX process surface's acceptance client (specs/posix.md): a plain
    # program that spawns a child with `posix_spawn` and waits for it with
    # `wait4`, proving the runtime's POSIX face.
    posix_test = build_dir / "apps/hosted/aegir-posix-test/aegir-posix-test"
    if posix_test.is_file():
        shutil.copy2(posix_test, root / "C" / "posix-test")
    # The POSIX acceptance's child (specs/posix.md): it lies beside the test
    # under a name of its own, so the launcher's `command started
    # aegir-posix-child` is a cue no acceptance step shares -- a child named
    # `date` would fire the acceptance's own `date` step (scripts/run_target.py).
    posix_child = build_dir / "apps/hosted/aegir-posix-child/aegir-posix-child"
    if posix_child.is_file():
        shutil.copy2(posix_child, root / "C" / "aegir-posix-child")
    # The path view's acceptance client (specs/posix.md's first sub-arc): a
    # plain program -- no Aegir call of its own -- that opens `/AEGIR/AEGIR.TXT`
    # and browses `/`. It is run from Sys:S/Shell-Startup, and its own exit
    # status (63) is the cue the acceptance's step waits on, so it needs no
    # marker of its own.
    posix_path = build_dir / "apps/hosted/aegir-posix-path-test/aegir-posix-path-test"
    if posix_path.is_file():
        shutil.copy2(posix_path, root / "C" / "posix-path-test")
    # The file sub-arc's acceptance client (specs/posix.md): the writing half of
    # the surface -- create, write, read back, truncate, rename, unlink, mkdir --
    # over the view's spelling of the scratchpad volume. The same shape as the
    # path view's client: no Aegir call of its own, and its own marker line
    # (AEGIR_POSIX_FILE_OK) is the cue the acceptance's step waits on.
    posix_file = build_dir / "apps/hosted/aegir-posix-file-test/aegir-posix-file-test"
    if posix_file.is_file():
        shutil.copy2(posix_file, root / "C" / "posix-file-test")
    return root


def ensure_tftp(build_dir: Path) -> None:
    """The directory QEMU's user-mode network serves over TFTP (specs/net.md):
    the acceptance's `tftp` fetch reads a file from it, so the bytes the wire
    carried are the run's own. QEMU resolves the `tftp=` path under its working
    directory, which is the build directory, so the directory is `tftp/` there.
    It lives in the build output, where scratch belongs, and is rewritten each
    run so its content is never a stale surprise."""
    directory = build_dir / "tftp"
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "aegir.txt").write_bytes(
        b"Aegir TFTP: this file crossed the wire.\n"
        b"The block came back through the socket port.\n"
    )


def ensure_host(build_dir: Path) -> None:
    """The directory QEMU's 9P transport shares into the machine (specs/9p.md):
    a host tree the guest reads and writes as a volume, so files move in and
    out of a run without rebuilding the image. QEMU resolves the `path=host`
    under its working directory, which is the build directory, so the share is
    `host/` there. It is rewritten each run with one known file the acceptance
    can read; AEGIR_9P_DIR points the share at a real host tree instead, by
    symlink, for interactive work."""
    share = build_dir / "host"
    source = os.environ.get("AEGIR_9P_DIR")
    if source:
        if share.is_symlink() or share.is_file():
            share.unlink()
        elif share.is_dir():
            shutil.rmtree(share)
        share.symlink_to(source)
        return
    if share.is_symlink():
        share.unlink()
    share.mkdir(parents=True, exist_ok=True)
    (share / "hello.txt").write_bytes(b"Aegir 9P: this file lives on the host.\n")
    # A name longer than the namespace's name field: the list answer carries it
    # because the wire's ceiling is the envelope, not kNameMax (specs/9p.md).
    (share / "a-filename-longer-than-twenty-four-bytes.txt").write_bytes(
        b"Aegir 9P: a long name crossed the wire.\n"
    )


def with_overlay(target: Target, build_dir: Path) -> tuple[list[str], Path]:
    """The target's QEMU arguments with the disk answered through an overlay this
    run owns, and the overlay itself for the caller to delete.

    QEMU's `-snapshot` used to do this and cannot here: it makes its overlay in a
    directory compiled into the binary (`/var/tmp`), so a host that mounts that
    one read-only -- or has filled it -- cannot start a guest at all, whatever
    the target or the disk. The same overlay made in the build directory, beside
    the disk it backs, needs nothing of the host but a writable build directory,
    which every other part of a run already needs: the guest's writes are real to
    the guest and gone after it, and `disk.img` itself never changes -- the
    property the disk's created-once rule (specs/development.md) wants.

    The overlay is named by a relative path for the same reason the disk is: the
    arguments travel to QEMU as one string through simulate's --extra-qemu-args,
    and QEMU resolves that against the build directory it is started in.
    """
    disk = build_dir / "disk.img"
    overlay = build_dir / "overlay.qcow2"
    overlay.unlink(missing_ok=True)
    subprocess.run(
        [
            "qemu-img",
            "create",
            "-q",
            "-f",
            "qcow2",
            "-b",
            str(disk),
            "-F",
            "raw",
            str(overlay),
        ],
        check=True,
    )
    arguments: list[str] = []
    tokens = shlex.split(" ".join(target.qemu_args))
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-drive" and index + 1 < len(tokens):
            arguments += [
                "-drive",
                ",".join(
                    f"file={overlay.name}"
                    if field.startswith("file=")
                    else "format=qcow2"
                    if field.startswith("format=")
                    else field
                    for field in tokens[index + 1].split(",")
                ),
            ]
            index += 2
            continue
        arguments.append(token)
        index += 1
    return arguments, overlay


def boot_interactive(target: Target, build_dir: Path) -> int:
    """Boot the image with QEMU's own window on the displays: the user is the
    runner. The keys the acceptance check's script would press are theirs to
    press (the console says when, and which), the heads are the window's tabs,
    and QEMU stops when its window closes, not at a marker -- so nothing here
    watches, and nothing here is timed out but the user."""
    ensure_disk(target, build_dir)
    ensure_tftp(build_dir)
    ensure_host(build_dir)
    arguments, overlay = with_overlay(target, build_dir)
    extra = " ".join(arguments)
    # -g/-s replace simulate's -nographic: a GTK window on the consoles, the
    # serial console on the terminal. Attached with `=`, for the same reason
    # --extra-qemu-args is: argparse reads a loose value starting with `-` as
    # an option of its own.
    command = (
        "./simulate --graphic='-display gtk' --serial='-serial stdio' --extra-qemu-args="
        + shlex.quote(extra)
    )
    try:
        return subprocess.call(
            ["bash", "-c", f"set -euo pipefail; . {ENV_SCRIPT}; exec {command}"],
            cwd=str(build_dir),
        )
    finally:
        # The window closing, or a signal to this script: either way the run is
        # over and its overlay is scratch.
        overlay.unlink(missing_ok=True)


def boot_and_watch(target: Target, build_dir: Path, timeout: int) -> tuple[bool, bool, str]:
    """Boot the image, streaming the console until the marker appears.

    Answers (marker seen, an acceptance check failed, test summary): a failed
    screen check is a failed run even when the marker arrived."""
    # The target's extra arguments belong to QEMU, not to the simulate script, so
    # they go through --extra-qemu-args as one string -- attached with `=` rather
    # than passed as a separate argument, because the value starts with `-bios`
    # and the script's argparse refuses a value that looks like an option (and
    # would read a loose `-bios` as its own `-b`).
    ensure_disk(target, build_dir)
    ensure_tftp(build_dir)
    ensure_host(build_dir)

    arguments, overlay = with_overlay(target, build_dir)
    extra = " ".join(arguments)
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
    # The guest's rectangles by name (RECT_CUE), and the screen in pixels from
    # the last screendump -- the size the pixel-to-axis map needs. A click that
    # names a rectangle follows the layout (specs/testing.md's rect cues).
    anchors: dict[str, tuple[int, int, int, int]] = {}
    screen = (1280, 800)
    summary = ""
    boot_faulted: int | None = None
    # The previous console line, without its newline. A cue can be split at a
    # line boundary by a writer that flushed mid-line; joining the two lines
    # recovers it. The logger is the serial's one writer now (specs/console.md),
    # so this is a guard against a logging regression, not the fix.
    previous = ""
    try:
        stream = process.stdout
        if stream is None:  # pragma: no cover - Popen above always pipes
            return False, True, ""
        # The console is read on its own thread so the loop below can time out
        # on a quiet guest: a step's cue that never prints must fail the run,
        # not wedge it until the outer budget expires (the rule the build's own
        # timeouts keep). `timeout` is that bound, and every line resets it.
        lines: queue.Queue[str | None] = queue.Queue()

        def pump() -> None:
            try:
                for text in stream:
                    lines.put(text)
            finally:
                lines.put(None)

        threading.Thread(target=pump, daemon=True).start()
        while True:
            try:
                line = lines.get(timeout=timeout)
            except queue.Empty:
                print(
                    f"    runner: FAIL no console line within {timeout}s -- "
                    "the guest stopped talking",
                    flush=True,
                )
                failed = True
                break
            if line is None:
                # The console ended: QEMU is gone and nothing more will be said,
                # so a marker that never came means a dead machine -- not a
                # timeout. Named here, with QEMU's own status, because the verdict
                # below used to say "never saw X within 900s", which reads like a
                # wait that never happened; that wording belongs to the quiet
                # timeout, printed only when a live guest really does go quiet.
                if not seen:
                    code = process.poll()
                    # Only a *non-zero* status says anything: 0 with no marker is
                    # the simulate wrapper swallowing a QEMU that never started,
                    # and QEMU's own error is already on the lines above.
                    status = (
                        f" (the console process exited with status {code})"
                        if code is not None and code != 0
                        else ""
                    )
                    print(
                        f"    runner: FAIL QEMU's console ended before "
                        f"{target.marker!r}{status}",
                        flush=True,
                    )
                break
            stripped = line.rstrip("\n")
            if stripped:
                print(f"    {stripped}", flush=True)
            cue = RECT_CUE.search(stripped)
            if cue:
                anchors[cue.group(1)] = (int(cue.group(2)), int(cue.group(3)),
                                         int(cue.group(4)), int(cue.group(5)))
            match = TEST_SUMMARY.search(stripped)
            if match:
                summary = f"{match.group(1)} tests passed, {match.group(2)} disabled"
            if GUEST_FAILURE.search(stripped):
                failed = True
            # A fault report is expected only for `hello`, and only before the
            # boot marker. `seen` is the marker's from a previous line, so a
            # report after it is a service that died when nobody asked it to.
            if seen and SUPERVISOR_FAULT.search(stripped):
                print(
                    f"    runner: FAIL a service faulted after the boot marker: {stripped}",
                    flush=True,
                )
                failed = True
            boot_summary = BOOT_SUMMARY.search(stripped)
            if boot_summary:
                boot_faulted = int(boot_summary.group(2))
            for index, step in enumerate(target.qmp_steps):
                if step.times != 0 and step_matches[index] >= step.times:
                    continue
                matched = re.search(step.trigger, stripped) is not None
                if not matched and previous:
                    # A cue split at the line boundary: neither half is the cue,
                    # but their join is. A trigger already matched whole in the
                    # previous line fired on that line and must not fire again.
                    matched = re.search(step.trigger, previous) is None and re.search(
                        step.trigger, previous + stripped
                    ) is not None
                if not matched:
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
                    screen = (width, height)
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
                    for region in step.dark:
                        named, x, y, w, h, least = region
                        if named != device:
                            continue
                        found = 0
                        for yy in range(y, min(y + h, height)):
                            for xx in range(x, min(x + w, width)):
                                at = (yy * width + xx) * 3
                                if (
                                    pixels[at] < 90
                                    and pixels[at + 1] < 90
                                    and pixels[at + 2] < 90
                                ):
                                    found += 1
                        if found < least:
                            print(
                                f"    runner: FAIL {device} region ({x},{y},{w},{h}) "
                                f"has {found} dark pixels, expected at least {least}",
                                flush=True,
                            )
                            failed = True
                    for pin in step.pins:
                        named, rect_name, rx, ry, pr, pg, pb = pin
                        if named != device:
                            continue
                        anchor = anchors.get(rect_name)
                        if anchor is None:
                            print(
                                f"    runner: FAIL {device} has no rect cue for "
                                f"{rect_name!r} to pin a pixel in",
                                flush=True,
                            )
                            failed = True
                            continue
                        ax, ay, aw, ah = anchor
                        px = ax + int(rx * aw)
                        py = ay + int(ry * ah)
                        if not (0 <= px < width and 0 <= py < height):
                            print(
                                f"    runner: FAIL the pin in {rect_name!r} lands "
                                f"({px},{py}), off {device}",
                                flush=True,
                            )
                            failed = True
                            continue
                        at = (py * width + px) * 3
                        shown = (pixels[at], pixels[at + 1], pixels[at + 2])
                        if shown != (pr, pg, pb):
                            print(
                                f"    runner: FAIL {device} {rect_name!r} at "
                                f"({px},{py}) shows {shown}, expected ({pr},{pg},{pb})",
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
                if step.clicks:
                    # A click at a rectangle the guest reported (RECT_CUE): the
                    # layout decides where it lands, so a font or metric change
                    # moves the widget and the click follows it.
                    problem = input_send_clicks(socket_path, step.clicks, anchors, screen)
                    if problem is not None:
                        print(f"    runner: FAIL input-send-clicks: {problem}", flush=True)
                        failed = True
                if step.drags:
                    # A drag between two reported rectangles: the thumb or knob
                    # is grabbed at the first and carried to the second, so a
                    # widget that only moves by dragging -- not by a click in
                    # its gutter -- is exercised.
                    problem = input_send_drags(socket_path, step.drags, anchors, screen)
                    if problem is not None:
                        print(f"    runner: FAIL input-send-drags: {problem}", flush=True)
                        failed = True
                if step.press is not None:
                    # The guest said it is waiting: type the keys. Events
                    # persist in the driver's posted buffers, so the presses
                    # are not a race.
                    if not send_key(socket_path, step.press, step.press_delay):
                        print(
                            f"    runner: FAIL a character of '{step.press}' has no qcode",
                            flush=True,
                        )
                        failed = True
            previous = stripped
            if target.marker in stripped:
                seen = True
            # The run is done when the marker has printed and the script is
            # played out -- the business may continue past the boot marker (a
            # login on the greeter, the bureau's backdrop), and a cue that
            # never comes is the timeout's and the gate's to report.
            if seen and all(played > 0 for played in step_matches):
                break
        # `hello` faults on purpose -- that is the supervision path's whole
        # test -- and the boot summary is where it is counted. Exactly one is
        # the design: zero would mean hello stopped walking the path it exists
        # to walk, more would mean another service died too. A target that
        # never prints the summary (sel4test) is not checked here.
        if boot_faulted is not None and boot_faulted != 1:
            print(
                f"    runner: FAIL the boot reported {boot_faulted} faulted, "
                "expected 1 (hello, on purpose)",
                flush=True,
            )
            failed = True
        # The write side of the 9P volume (specs/9p.md): the guest created a
        # file through the volume, and the runner finds it in the shared host
        # directory. The guest's read-back cue proves the path; this proves the
        # bytes left the machine and landed on the host. Only targets that
        # actually export a 9P directory are checked.
        if any("virtio-9p-device" in argument for argument in target.qemu_args):
            written = build_dir / "host" / "written.txt"
            expected = b"Aegir 9P: the machine wrote this through the volume.\n"
            if not written.is_file() or written.read_bytes() != expected:
                print(
                    f"    runner: FAIL the host file the guest wrote is missing or "
                    f"wrong: {written}",
                    flush=True,
                )
                failed = True
            else:
                print(
                    f"    runner: the guest's write is on the host: {written.name}",
                    flush=True,
                )
    finally:
        # Whatever ended the run -- the loop's own timeout, a manual Ctrl-C as
        # you watch it drift, or the all-steps-played exit -- a cue that never
        # printed is a check that never ran, and the run does not get to pass on
        # evidence that was never taken. Listed here, on every path out, because
        # the console's last lines only ever name the last cue it *did* see; the
        # cue that failed is the line that is not there, and pulling it out of a
        # long log by eye is exactly what this saves (specs/testing.md).
        never = [i for i, played in enumerate(step_matches) if played == 0]
        if never:
            print(
                f"    runner: FAIL {len(never)} cue(s) never printed -- the run "
                "stopped at the last cue it saw; these are the missing checks:",
                flush=True,
            )
            for index in never:
                print(f"      {target.qmp_steps[index].trigger}", flush=True)
            failed = True
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
        # QEMU is done with the overlay, and the overlay is scratch: the disk
        # image it backs is the thing that stays.
        overlay.unlink(missing_ok=True)
    return seen, failed, summary


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--target", required=True, choices=sorted(TARGETS))
    parser.add_argument(
        "--build-timeout",
        type=int,
        default=1800,
        help="seconds one build step (a runtime script, configure, ninja) may take",
    )
    parser.add_argument(
        "--quiet-timeout",
        type=int,
        default=300,
        # A green run talks all the way -- a whole development-target run is two
        # to three minutes end to end -- so minutes of silence are a wedge, not a
        # slow step. Kept well below the Makefile's RUN_TIMEOUT on purpose: the
        # runner's own report (the cues that never printed) has to land *before*
        # that outer belt kills the script, or a wedge is reported as a bare
        # SIGTERM. A QEMU that dies instead of going quiet never reaches this: its
        # console ends, and boot_and_watch says so at once.
        help="seconds a run may print nothing before it is treated as stopped",
    )
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

    # The outer `timeout` the Makefile wraps every invocation in SIGTERMs this
    # script, and Python's default handler exits without unwinding -- a build's
    # process group would be left behind. Becoming an exception lets bash() take
    # the tree down on the way out (stop_group); the QEMU path installs the same
    # handler for its own process group.
    #
    # These handlers are what stops a run, and a signal has to reach *this*
    # process to reach them. Measured, because the chain is longer than it looks
    # (`make` -> the Makefile's own `timeout` -> this script -> simulate -> QEMU):
    # SIGKILL of `make` left this script running, and QEMU with it, still deep in
    # the acceptance a minute and a half later -- the Makefile's `timeout`
    # survives its parent and keeps this script a child of the chain, so nothing
    # here hears about the kill at all. Kill this process, or signal the whole
    # process group (a terminal's Ctrl-C does), and the run takes itself down:
    # QEMU is reaped and the cues that never printed are listed as the diagnosis.
    def _stop(signum: int, _frame: object) -> None:
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

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
    extra_flags = (
        f"-DAEGIR_HOSTED_CXX={'ON' if hosted else 'OFF'} "
        f"-DAEGIR_TOOLKIT={'ON' if toolkit() else 'OFF'}"
    )
    wanted_flags = f"{wanted_flags} {extra_flags}".strip()
    try:
        build_builtins(target, arguments.build_timeout)
        if hosted:
            build_runtimes(target, arguments.build_timeout)
        stamp = configured_flags(build_dir)
        # A directory configured with seL4's GNU toolchain -- one made before
        # configs/ set TRIPLE -- is not reusable: CMake keeps its cached
        # compiler across a re-run, so it would build everything with
        # riscv64-unknown-elf-g++ and fail in our own sources (the whole build
        # is clang, specs/build.md). It is wiped and configured again, which is
        # what lets TRIPLE's clang take effect on an old tree at all.
        stale_toolchain = (build_dir / "build.ninja").is_file() and not configured_with_clang(
            build_dir
        )
        if (
            arguments.reconfigure
            or not (build_dir / "build.ninja").is_file()
            or stale_toolchain
            or (stamp != "" and stamp != wanted_flags)
        ):
            if stale_toolchain:
                print(
                    f"INFO  {target.name}'s build directory was configured with a "
                    f"non-clang toolchain; removing it and configuring again",
                    flush=True,
                )
                shutil.rmtree(build_dir)
            print(f"INFO  (re)configuring {target.name} as: {wanted_flags or 'defaults'}", flush=True)
            configure(target, build_dir, arguments.build_timeout, extra_flags)
            record_flags(build_dir, wanted_flags)
        elif stamp == "":
            # An existing build directory from before this record existed: adopt
            # it as configured with what the target now asks for, so a later
            # change is still noticed.
            record_flags(build_dir, wanted_flags)
        build(target, build_dir, arguments.build_timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        pins.report(False, f"{target.name} build failed", str(exc))
        return 1

    if arguments.build_only:
        pins.report(True, f"{target.name} built", target.description)
        return 0

    if arguments.interactive:
        return boot_interactive(target, build_dir)

    try:
        seen, failed, summary = boot_and_watch(target, build_dir, arguments.quiet_timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        pins.report(False, f"{target.name} boot failed", str(exc))
        return 1

    if not seen or failed:
        pins.report(
            False,
            f"{target.name} did not report success",
            # A guest that went quiet while alive is named by the runner's own
            # line (the quiet timeout); a console that *ended* is a dead machine,
            # which boot_and_watch reports with QEMU's status. Neither is a wait,
            # so neither verdict says "within Ns".
            "the guest stopped before the marker -- the runner's lines above say where"
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
