"""The targets Aegir can build and boot.

Data, not logic: `scripts/run_target.py` does the work. Adding a target here is
how a new machine or configuration becomes runnable, and the architecture
specific values themselves live in configs/.

Two of the values a target carries are *not* run-time flags: the amount of RAM
and the number of CPU cores are baked in at configure time, because the device
tree is dumped from QEMU with them and everything downstream reads that
(kernel/src/plat/qemu-riscv-virt/config.cmake:83-132). A different pair of
numbers is therefore a different build directory, which is why they appear as
configure flags here and as `-smp` in the simulate arguments -- and why the two
have to agree (specs/build.md).
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class QmpStep:
    """One outside-in action on QEMU, cued by the console. A guest cannot see
    its own screen, so the screen is read from here: `trigger` is a regex
    matched against console lines, and the action runs on each match, up to
    `times` (0: every match -- a cue every session prints wants an answer
    every time, however many logins the test bed happens to do) --
    action runs -- screendump each QEMU device in `dumps` and check the PPMs
    (dimensions are the multiset `expect`, and the driver's band pattern must
    be at its posts), send the input `events` (QMP input-send-event dicts:
    pointer moves and clicks -- no device is named, because with no console
    bound the events fall through to the unbound input handlers, and ours
    are), then press the key `press`, if any."""

    trigger: str
    times: int = 1  # how often the action may run; 0 is every match
    press: str | None = None
    dumps: tuple[str, ...] = ()
    expect: tuple[tuple[int, int], ...] = ()
    events: tuple[dict, ...] = ()


@dataclass(frozen=True)
class Target:
    name: str
    description: str
    build_dir: str
    # Flags passed to init-build.sh. Aegir's own targets need none beyond the
    # machine they are for: our pins live in settings.cmake and configs/.
    # Upstream targets have to be told, which is a useful reminder that their
    # defaults are not our decisions.
    configure_flags: tuple[str, ...]
    marker: str
    # The project this target configures, relative to the repository root. Our
    # own targets configure the root project; an upstream project inside the tree
    # (projects/sel4test) has to be named, because the root's init-build.sh would
    # configure *our* root and the cached source directory would not match.
    source_dir: str = "."
    # Extra arguments handed to QEMU through the simulate script. The script has
    # no concept of cores, so `-smp` comes through here, and `-bios none` is its
    # own default repeated because passing anything replaces that default rather
    # than adding to it.
    qemu_args: tuple[str, ...] = field(default_factory=lambda: ("-bios none",))
    # The acceptance check's outside-in actions, cued by console lines and
    # played against QEMU's QMP socket (its name is relative to the build
    # directory, where QEMU runs). Empty for targets that never wait for
    # input and have no screen to read.
    qmp_steps: tuple[QmpStep, ...] = ()
    qmp_socket: str | None = None


def _aegir(memory_mib: int, cores: int, name: str) -> Target:
    """One point in the memory/cores envelope Aegir designs to (specs/aegir.md)."""
    return Target(
        name=name,
        description=f"Aegir's own root task -- {memory_mib} MiB, {cores} core(s)",
        build_dir=f"out/{name}",
        configure_flags=(f"-DQEMU_MEMORY={memory_mib}", f"-DKernelMaxNumNodes={cores}"),
        marker="AEGIR_BOOT_OK",
        # Five real virtio devices, so the transports the tree describes are
        # not all empty: an entropy source (which needs no backing file), a
        # block device (which does -- the path is relative because QEMU runs
        # with the build directory as its working directory, and the runner
        # puts a disk there), a keyboard, and two displays. The keyboard's
        # acceptance check needs a finger, and the displays' a screen to read
        # back: the QMP socket is both (scripts/run_target.py). The gpu ids
        # name the consoles, which is how a screendump says which head it
        # read. EDID is on by default on this QEMU, so each head answers how
        # big its glass is.
        qemu_args=(
            "-bios none",
            f"-smp {cores}",
            "-device virtio-rng-device",
            # -snapshot: the disk answers writes through a throwaway overlay
            # and the file on disk never changes -- a run's writes are real
            # to the run and gone after it, which keeps the image's
            # created-once invariant true now that filesystems write.
            "-snapshot",
            "-drive file=disk.img,if=none,format=raw,id=hd",
            "-device virtio-blk-device,drive=hd",
            "-device virtio-keyboard-device",
            # The pointers: an absolute tablet and a relative mouse. Both are
            # virtio id 18, like the keyboard -- the config space's EV_BITS
            # says which is which (the registry's evtype key). Headless
            # injection is QMP input-send-event with NO device argument: with
            # no console bound, events fall through to the unbound handlers
            # (ui/input.c's qemu_input_find_handler) -- abs lands on the
            # tablet and rel on the mouse by uniqueness, and btn lands on the
            # relative handler, because QEMU bundles buttons into its mask;
            # a tablet click cannot be injected without a bound console.
            "-device virtio-tablet-device",
            "-device virtio-mouse-device",
            "-device virtio-gpu-device,id=gpu0",
            "-device virtio-gpu-device,id=gpu1",
            "-qmp unix:qmp.sock,server,nowait",
        ),
        qmp_socket="qmp.sock",
        # The script the runner plays against the QMP socket, in order. The
        # keyboard's cue is the test bed saying it waits; the screens' cues
        # are the gpu drivers' marker lines, two heads naming themselves.
        # After each screen moment the runner dumps both heads (the dumps are
        # which-console-is-which agnostic: the *set* of dimensions is what is
        # checked) and presses the key that paces the guest's next step.
        qmp_steps=(
            QmpStep(r"test: kbd\.virtio0 opened -- a key, please", press="a"),
            # Every login's session opens the tablet and waits for a move of
            # its own (specs/auth.md's input path): the cue prints once per
            # session, and each gets the same move.
            QmpStep(
                r"session\.smoke: tablet\.virtio0 opened -- a pointer move, please",
                times=0,
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 10000}},
                    {"type": "abs", "data": {"axis": "y", "value": 20000}},
                ),
            ),
            # The tablet: absolute positions in the axis's own units (0..32767
            # both ways, as the driver announces), so the numbers sent here
            # are the numbers the guest must see -- no display size between.
            QmpStep(
                r"test: tablet\.virtio0 opened -- a pointer move, please",
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 10000}},
                    {"type": "abs", "data": {"axis": "y", "value": 20000}},
                ),
            ),
            # The mouse: the click first, then the nudge -- headless, buttons
            # route to the relative handler (QEMU bundles BTN into its mask),
            # and the deltas pass through exactly. The guest waits for them in
            # this order, which is the order they are queued in.
            QmpStep(
                r"test: mouse\.virtio0 opened -- a nudge, please",
                events=(
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                    {"type": "rel", "data": {"axis": "x", "value": 120}},
                    {"type": "rel", "data": {"axis": "y", "value": -60}},
                ),
            ),
            # Both heads up at the display's preferred mode. The cue is the
            # test bed's, not the drivers': their marker lines pass while the
            # boot is still spawning, before the test could be listening, and
            # a key pressed then would be consumed by the keyboard check's
            # own wait. The screens have been up since the markers; what the
            # cue paces is the reading of them.
            QmpStep(
                r"test: both heads answered -- the screens, please",
                dumps=("gpu0", "gpu1"),
                expect=((1280, 800), (1280, 800)),
                press="b",
            ),
            # One head shrank; the other did not move.
            QmpStep(
                r"gpu\.virtio\d: scanout 1024x768",
                dumps=("gpu0", "gpu1"),
                expect=((1024, 768), (1280, 800)),
                press="c",
            ),
            # One head is 4K now: the window's whole reason for being 32 MiB.
            # The other head never moved from the preferred mode.
            QmpStep(
                r"gpu\.virtio\d: scanout 3840x2160",
                dumps=("gpu0", "gpu1"),
                expect=((3840, 2160), (1280, 800)),
            ),
        ),
    )


TARGETS: dict[str, Target] = {
    # The floor of the envelope: the smallest machine Aegir supports, and where
    # capacity problems are meant to show up first.
    "aegir": _aegir(2048, 1, "aegir"),
    # The rest of the QEMU matrix we care about, at the floor's memory and at the
    # upper end of the expected range.
    "aegir-2g-smp2": _aegir(2048, 2, "aegir-2g-smp2"),
    "aegir-2g-smp4": _aegir(2048, 4, "aegir-2g-smp4"),
    "aegir-8g-smp4": _aegir(8192, 4, "aegir-8g-smp4"),
    "sel4test": Target(
        name="sel4test",
        description="upstream seL4 test suite (acceptance test for the vendored kernel)",
        build_dir="out/sel4test",
        configure_flags=(
            "-DPLATFORM=qemu-riscv-virt",
            "-DCROSS_COMPILER_PREFIX=riscv64-unknown-elf-",
            "-DKernelRiscvExtD=ON",
            "-DSIMULATION=ON",
        ),
        marker="All is well in the universe",
        source_dir="projects/sel4test",
    ),
}

# The envelope, in the order it is worth trying: the floor first, then the same
# machine with more cores, then the upper end of the expected memory range.
ENVELOPE: tuple[str, ...] = ("aegir", "aegir-2g-smp2", "aegir-2g-smp4", "aegir-8g-smp4")
