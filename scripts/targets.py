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
    (dimensions are the multiset `expect`; each device named in `bands` must
    show the driver's band pattern at its posts, and each `pixels` entry --
    device, x, y, r, g, b -- names one pixel one dumped device must show),
    send the input `events` (QMP input-send-event dicts:
    pointer moves and clicks -- no device is named, because with no console
    bound the events fall through to the unbound input handlers, and ours
    are), then type the keys `press`, one send-key per character, if any."""

    trigger: str
    times: int = 1  # how often the action may run; 0 is every match
    press: str | None = None
    dumps: tuple[str, ...] = ()
    expect: tuple[tuple[int, int], ...] = ()
    bands: tuple[str, ...] = ()
    pixels: tuple[tuple[str, int, int, int, int, int], ...] = ()
    # A region that must have drawn ink: device, x, y, width, height, and the
    # least dark pixels it must hold. A grid's text is dark on a light
    # background, so a region with none is a grid that drew nothing -- which the
    # solid-background `pixels` samples cannot tell.
    dark: tuple[tuple[str, int, int, int, int, int], ...] = ()
    events: tuple[dict, ...] = ()


# The terminal window's own click (its content, in the tablet's coordinates):
# focus it before typing. The bureau's bar and the demo's gadgets take the
# focus, and a key typed while another window has it is dropped, so every
# terminal step that types clicks first.
TERMINAL_CLICK = (
    {"type": "abs", "data": {"axis": "x", "value": 8192}},
    {"type": "abs", "data": {"axis": "y", "value": 12699}},
    {"type": "btn", "data": {"button": "left", "down": True}},
    {"type": "btn", "data": {"button": "left", "down": False}},
)


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
            "-drive file=disk.img,if=none,format=raw,id=hd,discard=unmap",
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
        # test bed's lines are the cues it waits on; the screens' other cues
        # are the gpu drivers' marker lines, two heads naming themselves.
        # After each screen moment the runner dumps both heads (the dumps are
        # which-console-is-which agnostic: the *set* of dimensions is what is
        # checked) and presses the key that paces the guest's next step.
        qmp_steps=(
            # The process environment's acceptance client (specs/environment.md):
            # a spawned boot service that checks the argv, the environment and
            # the current directory its spawner gave it. Its cue is the boot's
            # first, so it is checked before anything else.
            QmpStep(r"ENV_SMOKE_OK"),
            # The hosted C++ runtime's acceptance client (specs/cxx.md): its
            # malloc and libc++ container checks pass, and then -- returning
            # from main rather than halting -- its run-time exit handler runs
            # through the __funcs_on_exit bridge and prints the second marker.
            QmpStep(r"CXX_SMOKE_OK"),
            QmpStep(r"CXX_ATEXIT_OK"),
            # The greeter first (specs/console.md's login arc): auth starts
            # it before the test bed runs, so its cue is the boot's first
            # input cue. The dump reads the Workbench look up
            # (specs/amiga-fidelity.md): the Workbench-blue backdrop, the
            # window's raised frame and its #6688bb title bar (the greeter
            # asks for the focus at startup, so the gadgets are filled), the
            # black left-justified "Aegir" title, the box-in-box zoom and the
            # cascaded depth gadgets, the window's light grey, the white name
            # field with its focused blue border, the black label text, and
            # the button's grey. Then the form
            # STANDS through the test bed: the login is the run's last
            # business, played after the boot marker.
            QmpStep(
                r"greeter: a name and a secret, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 10, 10, 0, 85, 170),
                    ("gpu0", 410, 205, 102, 136, 187),
                    ("gpu0", 408, 206, 0, 0, 0),
                    ("gpu0", 842, 206, 255, 255, 255),
                    ("gpu0", 862, 203, 170, 170, 170),
                    ("gpu0", 865, 207, 255, 255, 255),
                    ("gpu0", 410, 230, 204, 204, 204),
                    ("gpu0", 500, 290, 255, 255, 255),
                    ("gpu0", 424, 288, 0, 120, 215),
                    ("gpu0", 424, 265, 0, 0, 0),
                    ("gpu0", 434, 398, 224, 224, 224),
                ),
            ),
            # The console owns the input devices (specs/console.md), so the
            # checks are paced through its channel: a click focuses the test
            # bed's window, and the keys land in its ring. The pointer starts
            # at the screen's centre; the window's centre is (-376,-186) of
            # relative motion away, and buttons land on the mouse headless --
            # QEMU bundles BTN into the relative handler's mask.
            QmpStep(
                r"test: the console's channel -- a click, please",
                events=(
                    {"type": "rel", "data": {"axis": "x", "value": -376}},
                    {"type": "rel", "data": {"axis": "y", "value": -186}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # Both heads up at the display's preferred mode. The cue is the
            # test bed's, not the drivers': their marker lines pass while the
            # boot is still spawning, before the test could be listening, and
            # a key pressed then would be consumed by the keyboard check's
            # own wait. The screens have been up since the markers; what the
            # cue paces is the reading of them. gpu0 is the console's screen
            # (specs/console.md) -- the Workbench-blue backdrop, painted over
            # the driver's bands at boot; gpu1 is the parked head the driver
            # protocol is exercised on, bands and all.
            QmpStep(
                r"test: both heads answered -- the screens, please",
                dumps=("gpu0", "gpu1"),
                expect=((1280, 800), (1280, 800)),
                bands=("gpu1",),
                pixels=(
                    ("gpu0", 10, 10, 0, 85, 170),
                    ("gpu0", 1279, 799, 0, 85, 170),
                ),
                press="b",
            ),
            # The parked head shrinks; the console's screen does not move.
            QmpStep(
                r"gpu\.virtio\d: scanout 1024x768",
                dumps=("gpu0", "gpu1"),
                expect=((1024, 768), (1280, 800)),
                bands=("gpu1",),
                pixels=(("gpu0", 10, 10, 0, 85, 170),),
                press="c",
            ),
            # The parked head is 4K now: the window's whole reason for being
            # 32 MiB. The console's screen never moved from the preferred
            # mode, and its backdrop stands.
            QmpStep(
                r"gpu\.virtio\d: scanout 3840x2160",
                dumps=("gpu0", "gpu1"),
                expect=((3840, 2160), (1280, 800)),
                bands=("gpu1",),
                pixels=(("gpu0", 10, 10, 0, 85, 170),),
            ),
            # The window protocol, from the test bed: a white window over
            # the blue backdrop, then a red one overlapping on top, then the
            # white destroyed and the backdrop redrawn beneath it. The
            # window's rectangles: first (64,64)-(463,363), second
            # (300,200)-(699,499).
            QmpStep(
                r"test: a window of one's own -- the screen, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 10, 10, 0, 85, 170),
                    ("gpu0", 100, 100, 255, 255, 255),
                    ("gpu0", 463, 363, 255, 255, 255),
                ),
                press="d",
            ),
            QmpStep(
                r"test: two windows, the newer on top -- the screen, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 100, 100, 255, 255, 255),
                    ("gpu0", 350, 250, 255, 0, 0),
                    ("gpu0", 500, 400, 255, 0, 0),
                    ("gpu0", 10, 10, 0, 85, 170),
                ),
                press="e",
            ),
            # The depth (specs/window-manager.md): the white raised over the
            # red, then lowered beneath it. The overlap (300..463 x,
            # 200..363 y) reads white while it is up and red once it is down;
            # the region only one window covers does not change.
            QmpStep(
                r"test: the white one raised -- the screen, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 100, 100, 255, 255, 255),
                    ("gpu0", 350, 250, 255, 255, 255),
                    ("gpu0", 500, 400, 255, 0, 0),
                    ("gpu0", 10, 10, 0, 85, 170),
                ),
                press="f",
            ),
            QmpStep(
                r"test: the white one lowered -- the screen, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 100, 100, 255, 255, 255),
                    ("gpu0", 350, 250, 255, 0, 0),
                    ("gpu0", 500, 400, 255, 0, 0),
                    ("gpu0", 10, 10, 0, 85, 170),
                ),
                press="h",
            ),
            QmpStep(
                r"test: the first window left -- the screen, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 100, 100, 0, 85, 170),
                    ("gpu0", 350, 250, 255, 0, 0),
                    ("gpu0", 10, 10, 0, 85, 170),
                ),
                # No key: focus went with the destroyed window, so a key now
                # would go nowhere. The next click refocuses.
            ),
            # The click refocuses on the red window -- its centre is
            # (+236,+136) from where the pointer stands -- and then the key
            # and the pointer arrive through the ring: the keymap's 'g', and
            # the tablet's (10000,20000) in the axis's own units (0..32767),
            # which is the screen's (390,488) and the window's (90,288).
            QmpStep(
                r"test: the red one takes the focus, please",
                events=(
                    {"type": "rel", "data": {"axis": "x", "value": 236}},
                    {"type": "rel", "data": {"axis": "y", "value": 136}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            QmpStep(r"test: a key through the keymap, please", press="g"),
            QmpStep(
                r"test: the pointer, window-local, please",
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 10000}},
                    {"type": "abs", "data": {"axis": "y", "value": 20000}},
                ),
            ),
            # The move (specs/window-manager.md): the red window to the
            # top-left, clear of the greeter's window and of the login's
            # click. The console repainted the vacated and the new
            # rectangle, so the red reads at its new place and the backdrop
            # where it stood.
            QmpStep(
                r"test: the red one moved -- the screen, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 10, 10, 0, 85, 170),
                    ("gpu0", 100, 100, 255, 0, 0),
                    ("gpu0", 350, 450, 0, 85, 170),
                ),
            ),
            # The resize (specs/window-manager.md): the red one shrinks to
            # 200x150 at (64,64), and the strips it leaves -- past its right
            # and bottom edges -- read as backdrop.
            QmpStep(
                r"test: the red one resized -- the screen, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 100, 100, 255, 0, 0),
                    ("gpu0", 300, 100, 0, 85, 170),
                    ("gpu0", 100, 300, 0, 85, 170),
                    ("gpu0", 10, 10, 0, 85, 170),
                ),
            ),
            # The pre-bureau drag (specs/workbench.md): the demo is on the
            # screen before login, and the bureau is a session that does not
            # exist until the login starts it. A drag focuses the demo; if it
            # called menu_register then, the ownerless port would block it and
            # it would never move. The drag takes it down 150 px, and the dump
            # after the login reads it there. It comes after the boot marker
            # because the test bed's click is placed for the pointer starting
            # at the screen's centre, and the demo is left where it lands --
            # the later demo clicks are placed for (900,450).
            QmpStep(
                r"AEGIR_BOOT_OK",
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 25600}},
                    {"type": "abs", "data": {"axis": "y", "value": 11796}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "abs", "data": {"axis": "y", "value": 17938}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # The boot marker ends the test bed's part, not the script's:
            # the runner holds until every step has played, and the login is
            # what remains. The click focuses the greeter's window; it lands
            # on the window's right strip (the axis's 0..32767 maps to the
            # screen's 1280x800, so (22033,21325) is the screen's (860,520))
            # -- inside the window, clear of its widgets, and clear of the
            # test bed's surviving red window, which ends at x=699.
            QmpStep(
                r"AEGIR_BOOT_OK",
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 22033}},
                    {"type": "abs", "data": {"axis": "y", "value": 21325}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # The demo moved before the bureau: its titlebar reads #6688bb at
            # (1050,438) -- clear of the cursor left at (1000,438) -- and the
            # console's backdrop stands where the titlebar was.
            QmpStep(
                r"greeter: the window has the focus",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 1050, 438, 102, 136, 187),
                    ("gpu0", 1000, 288, 0, 85, 170),
                ),
            ),
            # The typing answers the focus, not the click: the click rides
            # the mouse's queue and the keys the keyboard's, and which queue
            # the console drains first is the boot's timing, not the
            # script's -- keys drained before the click land while nothing
            # is focused and are dropped. The greeter's line says the focus
            # event is in its ring, so the routing is already its window's.
            QmpStep(
                r"greeter: the window has the focus",
                press="rroland\taegir\n",
            ),
            # Auth's half of the arc: the greeter's windows and slice go back
            # before the session starts. Evidence only -- the bureau's dump
            # below reads what the screen shows once they are gone.
            QmpStep(r"auth: the greeter's windows are reaped"),
            # The terminal (specs/terminal.md, specs/shell.md): auth starts it
            # beside the bureau at login, focused, with its window clear of the
            # test bed's red one, the demo's and the screen bar's samples. The
            # shell's startup read loads the persistent environment
            # (specs/environment.md): the system archive's `exitcode` is 11, so
            # the first command -- run before any Set -- reports it, and the
            # 11 can only have come from the union. The next step Sets 9 (in
            # memory and to the user's archive, the create target), reads it
            # back through the union, and runs a command that inherits it. Each
            # step waits for the demo to close and clicks the terminal to focus
            # it again (the runner fires steps by cue, so the demo's zoom would
            # otherwise take the focus).
            QmpStep(
                r"terminal: ready",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 60, 200, 204, 204, 204),
                    ("gpu0", 500, 300, 204, 204, 204),
                ),
            ),
            # The DOS toolset (specs/dos.md). Every command here is a program
            # read from Sys:C and started by the terminal on the shell's
            # request, and each step is cued by the *name* of the command that
            # just started -- not by its exit status, which two commands can
            # share. The sequence uses the session's Home:, which it may write,
            # and exercises the whole set: date, wait, makedir, copy, list, dir,
            # type, search, sort, join, more, rename, protect, info, which,
            # assign, version, delete, filenote. filenote is on the public FAT16
            # volume, which has no attributes: it names the filesystem and
            # fails with 10 -- the error path, and a FAT volume in the
            # acceptance. version reads VER.TXT through the alias assign
            # bound.
            QmpStep(
                r"demo: closed",
                events=TERMINAL_CLICK,
                # The shell's own words first, read by the shell itself rather
                # than spawned: SetEnv sets, GetEnv reads it back, UnSet removes
                # it, and the second GetEnv says so; an alias (hi stands for
                # echo) expands, Prompt changes the prompt, Eval runs a line,
                # and Why explains a return code. date is the first program they
                # queue behind.
                press="setenv PROBE value\ngetenv PROBE\nunset PROBE\n"
                      "getenv PROBE\nalias hi echo\nhi alias-expanded\nprompt AEGIR\n"
                      "eval echo eval-line\nwhy 10\ndate\n",
            ),
            QmpStep(
                r"terminal: command started date",
                events=TERMINAL_CLICK,
                # wait is a program too (C:WAIT), and with no period it waits a
                # second (specs/dos.md).
                press="wait\n",
            ),
            QmpStep(
                r"terminal: command started wait",
                events=TERMINAL_CLICK,
                press="makedir Home:DosTest Home:DosTest2\n",
            ),
            QmpStep(
                r"terminal: command started makedir",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 50, 145, 500, 60, 40),),
                events=TERMINAL_CLICK,
                press="copy Sys:AEGIR.TXT Sys:DOCS/NESTED.TXT Home:DosTest\n",
            ),
            QmpStep(
                r"terminal: command started copy",
                events=TERMINAL_CLICK,
                press="list Home:DosTest Home:DosTest2\n",
            ),
            QmpStep(
                r"terminal: command started list",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 50, 145, 500, 60, 40),),
                events=TERMINAL_CLICK,
                # dir is List's names-only sibling (the Amiga's Dir): the same
                # directory, without the sizes.
                press="dir Home:DosTest\n",
            ),
            QmpStep(
                r"terminal: command started dir",
                events=TERMINAL_CLICK,
                press="type Home:DosTest/AEGIR.TXT Home:DosTest/NESTED.TXT\n",
            ),
            QmpStep(
                r"terminal: command started type",
                events=TERMINAL_CLICK,
                press="search Sys:AEGIR.TXT Sys:DOCS/NESTED.TXT disk\n",
            ),
            QmpStep(
                r"terminal: command started search",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 50, 145, 500, 60, 40),),
                events=TERMINAL_CLICK,
                press="sort Sys:AEGIR.TXT Home:DosTest/SORTED.TXT\n",
            ),
            QmpStep(
                r"terminal: command started sort",
                events=TERMINAL_CLICK,
                press="join Sys:AEGIR.TXT Sys:AEGIR.TXT AS Home:DosTest/JOINED.TXT\n",
            ),
            QmpStep(
                r"terminal: command started join",
                events=TERMINAL_CLICK,
                # LONG.TXT is longer than a window, so more pages it and waits.
                press="more Sys:LONG.TXT\n",
            ),
            QmpStep(
                r"terminal: command started more",
                events=TERMINAL_CLICK,
                # q is the key more waits for; the rename line queued behind it
                # runs once more exits -- the terminal hands it to the shell.
                # The rename is same-directory: the volume protocol has no
                # cross-directory rename yet (specs/vfs.md).
                press="qrename Home:DosTest/AEGIR.TXT Home:DosTest/MOVED.TXT\n",
            ),
            QmpStep(
                r"terminal: command started rename",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # more's first page reached the grid.
                dark=(("gpu0", 50, 145, 500, 60, 40),),
                events=TERMINAL_CLICK,
                # Protect sets the file's mode; the user owns it, so it is
                # allowed (specs/bfs.md decision 7).
                press="protect Home:DosTest/MOVED.TXT rwe\n",
            ),
            QmpStep(
                r"terminal: command started protect",
                events=TERMINAL_CLICK,
                # info lists the volumes the session may resolve.
                press="info\n",
            ),
            QmpStep(
                r"terminal: command started info",
                events=TERMINAL_CLICK,
                # which resolves a command name through the C: assignment.
                press="which copy\n",
            ),
            QmpStep(
                r"terminal: command started which",
                events=TERMINAL_CLICK,
                # assign binds an alias in the session's namespace; the version
                # line reads a file through it, so the binding is exercised by
                # a later command and not only by its own exit.
                press="assign FOOVOL Sys:\n",
            ),
            QmpStep(
                r"terminal: command started assign",
                events=TERMINAL_CLICK,
                press="version FOOVOL:VER.TXT\n",
            ),
            QmpStep(
                r"terminal: command started version",
                events=TERMINAL_CLICK,
                press="delete Home:DosTest Home:DosTest2 ALL\n",
            ),
            QmpStep(
                r"terminal: command started delete",
                events=TERMINAL_CLICK,
                # filenote on FAT: the interchange volume is public, and FAT has
                # no attributes, so the command names the filesystem and fails
                # with 10 -- the error path, naming FAT rather than the file.
                press="filenote FAT16:NOTE.TXT amiga\n",
            ),
            QmpStep(
                r"terminal: command exited 10",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 50, 145, 500, 60, 40),),
            ),
            # The greeter's login starts the bureau (specs/workbench.md): the
            # trinket full-screen backdrop with the screen title bar across
            # its top -- #6688bb, the Workbench menus in it -- over the
            # theme's Workbench grey. The samples keep clear of the red window
            # (64..263 x, 64..213 y, where the move and resize left it), which
            # still stands above the backdrop, and of the cursor at (860,520);
            # the bar's pixels are the top corners, the backdrop's the rest.
            QmpStep(
                r"bureau: the screen is yours",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 2, 10, 102, 136, 187),
                    ("gpu0", 1279, 0, 102, 136, 187),
                    ("gpu0", 0, 799, 170, 170, 170),
                    ("gpu0", 1279, 799, 170, 170, 170),
                    ("gpu0", 860, 240, 170, 170, 170),
                    ("gpu0", 640, 700, 170, 170, 170),
                ),
            ),
            # The window manager's demo client (specs/window-manager.md), last
            # so its clicks do not race the login: its decorated window was
            # dragged to (900,450) before the login, clear of the samples
            # above. The runner clicks its zoom gadget (the right-hand pair,
            # specs/amiga-fidelity.md); the window fills the screen with its
            # #6688bb bars; clicks zoom again and it is back; then clicks
            # close, at the titlebar's far left, and the backdrop stands where
            # it was.
            QmpStep(
                r"bureau: the screen is yours",
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 28773}},
                    {"type": "abs", "data": {"axis": "y", "value": 17938}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            QmpStep(
                r"demo: zoomed",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 1000, 10, 102, 136, 187),
                    ("gpu0", 1000, 400, 204, 204, 204),
                    ("gpu0", 1000, 795, 102, 136, 187),
                ),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 31845}},
                    {"type": "abs", "data": {"axis": "y", "value": 327}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            QmpStep(
                r"demo: restored",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 910, 460, 204, 204, 204),),
                # The demo's terminal grid has text on it: a grid that wrapped
                # every character into one column, or drew nothing, has far less
                # ink than this.
                dark=(("gpu0", 902, 470, 240, 90, 100),),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 768}},
                    {"type": "abs", "data": {"axis": "y", "value": 450}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # The bureau.menu server (specs/workbench.md), while the demo is
            # still up: it registered its tree when it gained the focus, so the
            # bar's first title is the demo's, and clicking it drops the demo's
            # menu -- clicking the bar must not take the demo's focus
            # (kBackdropTakesFocus). Clicking the first item prints the demo's
            # cue: proof the tree crossed to the bureau and the action back.
            QmpStep(
                r"bureau: client menu",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 2, 10, 102, 136, 187),
                    ("gpu0", 100, 33, 240, 240, 240),
                ),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 768}},
                    {"type": "abs", "data": {"axis": "y", "value": 1352}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            QmpStep(
                r"demo: about",
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 23448}},
                    {"type": "abs", "data": {"axis": "y", "value": 17938}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # The demo closed, so the bureau's own menus stand again: the runner
            # clicks the bar's first title, the bureau drops its menu, and reads
            # the Workbench action's cue.
            QmpStep(
                r"demo: closed",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 1000, 400, 170, 170, 170),),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 768}},
                    {"type": "abs", "data": {"axis": "y", "value": 450}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            QmpStep(
                r"bureau: menu",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 2, 10, 102, 136, 187),
                    ("gpu0", 100, 33, 240, 240, 240),
                ),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 768}},
                    {"type": "abs", "data": {"axis": "y", "value": 1352}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            QmpStep(r"bureau: Aegir, the Workbench"),
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
