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

from dataclasses import dataclass, field, replace


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
    # The typist's pace between keys: the terminal's grid keeps up at the
    # default 0.05 s, but a widget that repaints its window and waits on the GPU
    # per keystroke does not, and a dropped key mangles the line
    # (run_target.send_key).
    press_delay: float = 0.05
    dumps: tuple[str, ...] = ()
    expect: tuple[tuple[int, int], ...] = ()
    bands: tuple[str, ...] = ()
    pixels: tuple[tuple[str, int, int, int, int, int], ...] = ()
    # A region that must have drawn ink: device, x, y, width, height, and the
    # least dark pixels it must hold. A grid's text is dark on a light
    # background, so a region with none is a grid that drew nothing -- which the
    # solid-background `pixels` samples cannot tell.
    dark: tuple[tuple[str, int, int, int, int, int], ...] = ()
    # A pixel pinned to a named rectangle rather than to a screen coordinate:
    # (device, rect name, fraction x, fraction y, r, g, b). The rectangle is
    # the guest's own (`rect <name> ...`, RECT_CUE), so a layout change moves
    # the pixel with the widget and the acceptance does not pin a coordinate
    # the layout no longer holds -- the same idea as `clicks`, for a pixel.
    pins: tuple[tuple[str, str, float, float, int, int, int], ...] = ()
    events: tuple[dict, ...] = ()
    # Clicks at a named screen rectangle the guest reports, rather than at a
    # pinned pixel: each is `(name, rx, ry)`, the fraction of the rectangle's
    # width and height to click, and the name is set by a console line
    # `rect <name> <x> <y> <w> <h>` (screen pixels). The rectangle is the
    # widget's own, so a font or metric change moves the click with it and the
    # acceptance does not pin a coordinate the layout no longer holds
    # (the runner's `input_send_clicks`).
    clicks: tuple[tuple[str, float, float], ...] = ()
    # Drags between two named rectangles: each is `(from_name, frx, fry,
    # to_name, trx, try)` -- press at the fraction of the first, move to the
    # fraction of the second, release. A widget's *drag* (a scrollbar's thumb, a
    # slider's knob) is a press, a motion and a release, which `clicks` (a press
    # and a release in place) cannot express (the runner's `input_send_drags`).
    drags: tuple[tuple[str, float, float, str, float, float], ...] = ()


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
        # The machine's virtio devices, so the transports the tree describes
        # are not all empty: an entropy source (which needs no backing file), a
        # block device (which does -- the path is relative because QEMU runs
        # with the build directory as its working directory, and the runner
        # puts a disk there), a keyboard, two pointers, one display and a NIC.
        # The keyboard's acceptance check needs a finger, and the display's a
        # screen to read back: the QMP socket is both (scripts/run_target.py).
        # The gpu id names the console, which is how a screendump says which
        # head it read. EDID is on by default on this QEMU, so the head answers
        # how big its glass is. The RISC-V virt machine has exactly eight
        # virtio-mmio slots (VIRTIO_COUNT in QEMU's hw/riscv/virt.h) and they
        # are all here: a second display stood in one until the 9P transport
        # wanted it (specs/9p.md), so one head remains.
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
            # One display: the console's screen. The machine's eighth
            # virtio-mmio slot, where a second head once stood, is the 9P
            # transport's now (specs/9p.md); the driver-protocol mode test that
            # poked the parked head went with it.
            "-device virtio-gpu-device,id=gpu0",
            # The network device (specs/net.md). QEMU's user-mode networking
            # hands the guest a DHCP lease, a gateway to reach and a DNS server,
            # and a default MAC; none of it is compiled into the stack, which
            # learns every bit of it over the wire. `tftp=tftp` is the
            # acceptance's server: the virtual host serves the build directory's
            # `tftp/` over TFTP, so the `tftp` command fetches a file whose bytes
            # the run chose -- still offline, nothing external.
            "-netdev user,id=net0,tftp=tftp",
            "-device virtio-net-device,netdev=net0",
            # The 9P transport (specs/9p.md): a host directory shared into the
            # machine, so files move in and out without rebuilding the image.
            # QEMU resolves `path=host` under its working directory, which is
            # the build directory, where the runner creates it -- and
            # AEGIR_9P_DIR points it at a real tree for interactive work
            # (scripts/run_target.py). security_model=none hands the guest the
            # host user's own files.
            "-fsdev local,id=fsdev0,path=host,security_model=none",
            "-device virtio-9p-device,fsdev=fsdev0,mount_tag=host",
            "-qmp unix:qmp.sock,server,nowait",
        ),
        qmp_socket="qmp.sock",
        # The script the runner plays against the QMP socket, in order. The
        # test bed's lines are the cues it waits on; the screen's other cue is
        # the gpu driver's marker line, the head naming itself. After each
        # screen moment the runner dumps the head (a screendump names the gpu
        # id it read, and the dimensions are what is checked) and presses the
        # key that paces the guest's next step.
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
            # `hello` faults on purpose -- the one boot service whose job is to
            # fail, so the supervision path is walked every run (specs/
            # director.md). This is its last line before the fault, and the run
            # requires it; the boot must report exactly that one fault, and any
            # *other* service faulting after the boot marker is a failure.
            QmpStep(r"AEGIR_CLIENT_FAULTING_ON_PURPOSE"),
            # The network driver (specs/net.md): the pinned lwIP tree is
            # compiled, the transport answers device id 1, and the MAC, MTU and
            # link state came from the device's config space -- read, not
            # compiled in. The cue is the driver's own line; a run where the
            # device is not bound, or the config space reads wrong, fails this
            # rather than passing quietly.
            QmpStep(r"eth\.virtio0: mac [0-9a-f]{2}(:[0-9a-f]{2}){5}, mtu \d+, link up"),
            # The driver's transmit self-test (specs/net.md): a frame built in
            # the queue page and sent, and the device read the chain and posted
            # a used entry. It is what proves the second queue works, not just
            # the first.
            QmpStep(r"transmit: the device took the \d+-byte frame"),
            # The 9P transport (specs/9p.md): the driver laid out its queue,
            # read the export's name from the device's config space, and
            # answered a version handshake -- the whole transport (queue, kick,
            # completion, reply) proved before any filesystem stands on it. The
            # line names the dialect and the size the host offered, so a
            # transport that came up but could not carry a message fails here.
            QmpStep(r"p9\.virtio0: 9P2000\.L, msize \d+, tag host"),
            # The 9P filesystem (specs/9p.md): a service opened the bound
            # transport through the registry, spoke 9P2000.L over it, and read
            # the file the runner put in the shared host directory -- walk,
            # open, read and clunk proved against the host's own bytes.
            QmpStep(r"9p: read \d+ bytes: Aegir 9P: this file lives on the host\."),
            # ...and once through a read handle, so the read-open path (walk and
            # open kept, read by offset) is proved alongside the write handle.
            QmpStep(r"9p: read-handle -> \d+ bytes"),
            # A directory entry whose name is longer than the namespace's name
            # field: the list answer carries it, because the entry name's
            # ceiling is the answer's room and not kNameMax (specs/9p.md).
            QmpStep(r"9p: list carries a \d+-byte name"),
            # ...and it registered the export's own tag as a volume, so the host
            # directory is reachable by the ordinary file protocol.
            QmpStep(r"9p: host: registered, serving"),
            # The write side (specs/9p.md): the machine created a file through
            # the volume and read it back, so the write handlers and the
            # transport's write path are proved. The runner checks the host
            # directory afterwards, which is where the bytes had to land.
            QmpStep(r"9p: wrote \d+ bytes, read back: Aegir 9P: the machine wrote this "
                    r"through the volume\."),
            # The network stack (specs/net.md): director started it as a system
            # service, and it brought lwIP up -- lwip_init, the tcpip thread
            # (where the sys_arch, the thread builder and the page-backed heap
            # are all exercised) and the timer tick that drives lwIP's timers.
            # Loopback is up from this first moment, before any link is bound:
            # a host has an interface even with no NIC. The link and socket
            # halves build on this.
            QmpStep(r"net: loopback lo0 127\.0\.0\.1/8 up"),
            # ...and the virtio link, which the stack learned through the device
            # manager's registry: it opened the bound eth.* row, mapped the
            # link's window, read the MAC/MTU/link state from the device, and
            # added a netif (down, until a configuration brings it up). This is
            # the seam that keeps the stack free of any machine: the numbers are
            # the device's own.
            QmpStep(r"net: NE0 eth\.virtio0 mac [0-9a-f]{2}(:[0-9a-f]{2}){5}, "
                    r"mtu \d+, link up, down"),
            QmpStep(r"net: ready: the stack is up"),
            # NetConfig (specs/net.md): the boot session's Startup-Sequence runs
            # `netconfig`, and it read Sys:S/network.manifest through the VFS and
            # applied it through the control port -- the stack never reads the
            # file. The hostname is set first, so the stack prints it, and DHCP
            # option 12 carries it; that it reached the netif before dhcp_start
            # is why the name is the network's and not only ours.
            QmpStep(r"launcher: command started netconfig"),
            QmpStep(r"net: NE0 hostname aegir"),
            QmpStep(r"netconfig: \d+ of \d+ adapters configured, hostname on \d+"),
            # And the numbers DHCP gave: the address, netmask and gateway are
            # the network's, obtained over the wire, never embedded in the
            # build. slirp's lease is what a run sees.
            QmpStep(r"net: NE0 ipv4 \d+\.\d+\.\d+\.\d+ netmask \d+\.\d+\.\d+\.\d+ "
                    r"gateway \d+\.\d+\.\d+\.\d+"),
            # The socket port's first client (specs/net.md): a raw ICMP echo to
            # the DHCP-supplied gateway, which slirp answers. The command, run
            # from Startup-Sequence after netconfig, asks the stack for the
            # gateway rather than naming it. It is the plan's small landing --
            # the whole path (socket, send, held recv) end to end before TCP.
            QmpStep(r"launcher: command started ping"),
            QmpStep(r"ping: reply from \d+\.\d+\.\d+\.\d+, type 0, \d+ bytes"),
            # The names path (specs/net.md): `ping localhost` resolves through
            # Sys:S/hosts -- the resolver reads it in the client, before DNS --
            # and 127.0.0.1 is lwIP's own loopback, so no packet touches the
            # wire. Both commands are Startup-Sequence lines, run one at a time
            # with the console idle; a name typed into the interactive shell
            # would risk the keyboard queue instead, for no more evidence.
            QmpStep(r"ping: reply from 127\.0\.0\.1, type 0, \d+ bytes"),
            # The datagram slice (specs/net.md): `tftp` fetches a file from the
            # virtual host's TFTP server over UDP, so the socket port carries
            # real bytes both ways -- an RRQ out, DATA blocks back, an ACK for
            # each. The first line is the run's own, so the cue proves the
            # content and not only the count; nothing external was reached.
            QmpStep(r"tftp: \d+ bytes in \d+ blocks? from \d+\.\d+\.\d+\.\d+"),
            QmpStep(r"tftp: first line: Aegir TFTP: this file crossed the wire\."),
            # The stream slice (specs/net.md): `tcpecho` binds a listener on
            # 127.0.0.1, connects to it, accepts the connection, writes a string,
            # reads the echo back and compares it -- bind, listen, connect,
            # accept, write, recv, the whole lifecycle, all on the loopback with
            # nothing on the wire.
            QmpStep(r"tcpecho: \d+ bytes echoed over loopback: Aegir TCP echo"),
            # The bulk path (specs/net.md): `tcpbulk` carries a payload past the
            # message registers through a window it carves and hands the stack by
            # capability -- write-window out, recv-window back, compared.
            QmpStep(r"tcpbulk: \d+ bytes crossed the window"),
            # The live half (specs/net.md): the Net: filesystem view registers
            # itself with the VFS as a synthetic volume -- one directory per
            # adapter, one file per parameter -- that reads and reprograms the
            # running stack through its control port.
            QmpStep(r"netvol: Net: registered, serving"),
            # `net NE0/ipv4_address` reads the address back through that volume:
            # resolved by path through the ordinary namespace, read with the
            # ordinary file protocol, so the cue is the DHCP number coming back
            # out of Net: the way it went into the stack.
            QmpStep(r"net: NE0/ipv4_address = \d+\.\d+\.\d+\.\d+"),
            # The write half: `net NE0/hostname aegir-live` opens the parameter
            # for writing, writes it, and closes -- which applies it to the
            # stack through the control port -- and the read that follows shows
            # the stack's own new value, so the cue is a write that landed.
            QmpStep(r"net: NE0/hostname = aegir-live"),
            # The musl shim (specs/net.md): `net-smoke` is a hosted program
            # that reaches the stack only through libc's socket calls, does a
            # loopback echo, and prints what came back -- the rerouting the shim
            # exists for, proved end to end. Its peer's address comes from
            # libc's getaddrinfo over Sys:S/hosts, which musl's own resolver
            # (reading /etc/hosts and a nameserver) cannot answer.
            QmpStep(r"net-smoke: getaddrinfo localhost:4242 -> 127\.0\.0\.1"),
            QmpStep(r"net-smoke: \d+ bytes echoed through libc: Aegir libc sockets"),
            # The shim's bulk window (specs/net.md): a payload past the envelope
            # crosses in a frame the hosted runtime carves and hands the stack.
            QmpStep(r"net-smoke: \d+ bytes crossed the window through libc"),
            # The socket reap (specs/net.md): net-smoke exits with one socket
            # still open, so the launcher reaps its badge and the stack drops
            # what it left -- the cleanup a client that dies gets.
            QmpStep(r"net: reaped \d+ socket\(s\) a client left open"),
            # The boot session's Startup-Sequence runs a command (specs/boot.md):
            # the boot session has a launcher now, so a sequence line starts a
            # program like any shell's line does. `filenote` is the marker -- no
            # other step cues on it -- and the boot's proceeding to the greeter
            # is what proves the launch, since a line that could not start a
            # program fails the sequence and leaves the failure view instead.
            QmpStep(r"launcher: command started filenote"),
            # The toolkit's font comes off the system volume (specs/fonts.md): a
            # BDF face is scanned and read directly, and an OpenType one -- the
            # theme's default Noto Sans -- is served by font.main, which read it
            # from the same volume. Either cue proves the toolkit did not fall
            # back to the byte array compiled into the library.
            QmpStep(r"trinket: font (Terminus 12 from Sys:Fonts/Terminus/ter-u12n\.bdf"
                    r"|Noto Sans 11 from font\.main)"),
            # The font service (specs/fonts.md): it read each face's own name
            # from the volume and proved its own loading by opening Noto Sans
            # from it, checking the metrics it got back and rasterizing a glyph.
            # The count is a regex: what matters is that the scan found faces at
            # all.
            QmpStep(r"font: \d+ faces from Sys:Fonts"),
            QmpStep(r"font: open Noto Sans 16 is sane"),
            QmpStep(r"font: glyph A \d+x\d+ at -?\d+,-?\d+, advance \d+"),
            QmpStep(r"font: ready, serving font.main"),
            # The toolkit's ServerFont (specs/fonts.md): the demo's label draws
            # a Noto Sans face through the service, and prints the box of the
            # first glyph it got back -- proof a glyph crossed the client's own
            # transfer page and into the client's atlas, not only the service's.
            # The demo is a command now (specs/window-manager.md), started by
            # the session's Shell-Startup, so this cue comes after the login,
            # not before it.
            QmpStep(r"demo: outline A \d+x\d+ advance \d+"),
            # The session's datatypes broker (specs/datatypes.md): the demo
            # asked it to open each file, and it started the class through the
            # session launcher rather than the client starting one directly. The
            # cue is the broker's, and it names the path the class was resolved
            # from, so a session that fell back to the direct serve-launch would
            # miss it.
            QmpStep(r"datatypes: open DataTypes:ilbm\.datatype"),
            # The program-directory half of the search (specs/datatypes.md): the
            # demo's own directory is `Sys:C`, and `png.datatype` ships there
            # beside the demo's binary, so the class resolves to `C:png.datatype`
            # before `DataTypes:`. The cue names that path -- proof the program
            # directory was searched first.
            QmpStep(r"datatypes: open C:png\.datatype"),
            # The content-first walk and the same-name override
            # (specs/datatypes.md): the demo opens a PNG named `.ilbm`, so the
            # extension's hint is ilbm.datatype -- which declines -- and the
            # broker walks the candidates until a class claims it by content.
            # `aaa.datatype` is a **shared name**: the system ships it as the
            # ilbm class and the user's `Home:DataTypes` ships it as the png
            # class, so the union resolves the name to the user's member and the
            # user's class claims the PNG. A system-first union would reach the
            # ilbm class, which declines a PNG, and the cue would name
            # `png.datatype` instead -- so this step is the override's proof. The
            # RGBA the class states proves the class, not the name, decided.
            QmpStep(r"datatypes: DataTypes:ilbm\.datatype declines"),
            QmpStep(r"datatypes: open DataTypes:aaa\.datatype"),
            QmpStep(r"demo: mystery 64x48 format 2 palette 0"),
            # The datatypes client's first call (specs/datatypes.md): the demo
            # opened Sys:TestImage.ilbm through the ilbm class, which decoded
            # the file and served the frame a page at a time. The cue names what
            # the class stated -- 64x48, INDEXED (0) and a two-colour palette --
            # so the class, the client and the transfer page all proved out.
            QmpStep(r"demo: image 64x48 format 0 palette 2"),
            # The second class (specs/datatypes.md): the demo opened
            # Sys:TestImage.png through png.datatype, which decoded it (through
            # the vendored libpng) and served it as RGBA -- format 2, no
            # palette. The two files are different pictures, so the pins below
            # name colours only the right class produces.
            QmpStep(r"demo: png 64x48 format 2 palette 0"),
            # The third class (specs/datatypes.md): the demo opened
            # Sys:TestImage.jpg through jpeg.datatype, which decoded it (through
            # the vendored libjpeg-turbo) and served it as RGB -- format 1, no
            # palette. Its colours are its own once more, so the JPEG pin below
            # names a colour only the jpeg class produces; the class resolved
            # from DataTypes:, where it is staged, not from the program
            # directory.
            QmpStep(r"datatypes: open DataTypes:jpeg\.datatype"),
            QmpStep(r"demo: jpeg 64x48 format 1 palette 0"),
            # The class goes back when the caller is done with it
            # (specs/datatypes.md): the demo reads each frame and lets the
            # object go, so the client closes through the broker and the broker
            # releases the class. A class held to the session's end is what let
            # a viewer's classes fill the launcher's CSpace.
            QmpStep(r"datatypes: closed"),
            # The greeter first (specs/console.md's login arc): auth starts
            # it before the test bed runs, so its cue is the boot's first
            # input cue. The dump reads the Workbench look up
            # (specs/amiga-fidelity.md, specs/trinket/theme-xen.md): the
            # Workbench-blue backdrop, the window's raised frame and its
            # #6688bb title bar (the greeter asks for the focus at startup, so
            # the gadgets are filled), the black left-justified "Aegir" title,
            # the box-in-box zoom and the cascaded depth gadgets, the window's
            # grey body, the name field's sunken #bfbfbf well with the black
            # outline focus gives it, the black label text, and the button's
            # #bfbfbf face. Then the form
            # STANDS through the test bed: the login is the run's last
            # business, played after the boot marker.
            QmpStep(
                r"greeter: a name and a secret, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 10, 10, 0, 85, 170),
                    ("gpu0", 405, 205, 102, 136, 187),
                    ("gpu0", 842, 206, 255, 255, 255),
                    ("gpu0", 862, 203, 170, 170, 170),
                    ("gpu0", 865, 207, 255, 255, 255),
                    ("gpu0", 410, 230, 170, 170, 170),
                    ("gpu0", 500, 290, 191, 191, 191),
                    ("gpu0", 424, 276, 0, 0, 0),
                    ("gpu0", 424, 288, 0, 0, 0),
                    ("gpu0", 434, 398, 191, 191, 191),
                ),
                # The title's ink as a region, not an exact black pixel: an
                # antialiased face has no full-black stem at this size, so a
                # pinned pixel would move and change with the face
                # (specs/fonts.md's AA; the region counts ink instead).
                dark=(("gpu0", 406, 200, 30, 18, 20),),
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
            # The head up at the display's preferred mode. The cue is the test
            # bed's, not the driver's: its marker line passes while the boot is
            # still spawning, before the test could be listening, and a key
            # pressed then would be consumed by the keyboard check's own wait.
            # The screen has been up since the marker; what the cue paces is
            # the reading of it. gpu0 is the console's screen
            # (specs/console.md) -- the Workbench-blue backdrop, painted over
            # the driver's bands at boot.
            QmpStep(
                r"test: the head answered -- the screens, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 10, 10, 0, 85, 170),
                    ("gpu0", 1279, 799, 0, 85, 170),
                ),
                press="b",
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
            # A move off the screen is refused (specs/window-manager.md), and
            # the refusal must see through a wrapped coordinate: a client's
            # negative origin arrives as a huge unsigned value, and
            # `origin + size` would wrap small and pass. The test asks for
            # minus one and minus a hundred; the red one must stay where the
            # resize left it -- these pixels, not the console's crash.
            QmpStep(
                r"test: wrapped moves off the screen were refused -- the screen, please",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 100, 100, 255, 0, 0),
                    ("gpu0", 300, 100, 0, 85, 170),
                    ("gpu0", 100, 300, 0, 85, 170),
                    ("gpu0", 10, 10, 0, 85, 170),
                ),
            ),
            # The demo is no longer on the screen before login: it is a
            # command the session's Shell-Startup starts (specs/window-
            # manager.md), so there is nothing to drag here. It starts at the
            # place the drag used to leave it (aegir-gui-demo's kWindowY), and
            # the demo's own `demo: rects` cue is where its gestures begin.
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
            # No demo stands west of the greeter any more -- it is a session
            # command now (specs/window-manager.md) -- so the backdrop reads
            # where its titlebar used to sit, and the console's backdrop
            # stands where the drag left the cursor.
            QmpStep(
                r"greeter: the window has the focus",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 1050, 438, 0, 85, 170),
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
                    ("gpu0", 60, 200, 170, 170, 170),
                    ("gpu0", 500, 300, 170, 170, 170),
                ),
            ),
            # The session's additions (specs/session.md): the session's own
            # shell runs `Home:S/User-Startup` once, before Shell-Startup. The
            # cue is the shell's, printed only when the marked session shell
            # finds and starts the file -- a nested shell does not run it, so a
            # session that ran it per shell would print it more than once.
            QmpStep(r"shell: User-Startup"),
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
            # The NIL: handler (specs/boot.md): a read is EOF and a write
            # disappears. Its registration is the proof the service started and
            # the VFS holds the name redirection reaches it by.
            QmpStep(r"nil: NIL: registered, serving"),
            # The boot session (specs/boot.md): auth spawns a system terminal
            # and shell, whose shell runs Sys:S/Startup-Sequence and signals
            # when it is done -- so auth's line after the wait is the proof both
            # the spawn and the handshake worked.
            QmpStep(r"auth: the boot session ran"),
            QmpStep(
                r"demo: closed",
                events=TERMINAL_CLICK,
                # The shell's own words first, read by the shell itself rather
                # than spawned: SetEnv sets, GetEnv reads it back, UnSet removes
                # it, and the second GetEnv says so; an alias (hi stands for
                # echo) expands, Prompt changes the prompt, Eval runs a line,
                # and Why explains a return code. The redirection (`echo ...
                # >file`) and the first command file come last. The command
                # files are typed one per step, cued by the command the one
                # before it starts, because a press sent while the guest is
                # starting an image fills its eight-deep input queue and drops
                # keys -- and a dropped key mangles the line.
                press="setenv PROBE value\necho \"quoted $PROBE\"\ngetenv PROBE\nunset PROBE\n"
                      "getenv PROBE\nalias hi echo\nhi alias-expanded\nprompt AEGIR\n"
                      "eval echo eval-line\nwhy 10\necho shell-redirect >Home:ShellOut.TXT\n"
                      "set prog aegir-print\nset value env-substituted\n"
                      "execute Sys:S/Subst-Test\n",
            ),
            # Substitution (specs/shell.md): Subst-Test's command word is the
            # variable $prog, so aegir-print starting proves the environment
            # expanded before the lookup; Params-Test's is aegir-echo, whose
            # argument is both the .KEY name {text} and the positional $1, so
            # the command starting proves the two bound to Execute's argument.
            QmpStep(
                r"launcher: command started aegir-print",
                press="execute Sys:S/Params-Test named-arg\n",
            ),
            QmpStep(
                r"launcher: command started aegir-echo",
                # Interpreter-Test last: its file's built-in Alias makes x stand
                # for date, and date starting is the proof the interpreter ran
                # the file in order.
                press="execute Sys:S/Interpreter-Test\n",
            ),
            # Control flow (specs/shell.md): Control-Test's only reachable last
            # line is aegir-echo 42, so this is a cue no other command gives.
            # Every branch a correct run must not take exits at or above the
            # fail level, which drops the file there rather than going on, so a
            # `command exited 42` means If/Else/EndIf, the condition words and
            # the Skip all chose the path they should.
            QmpStep(r"terminal: command exited 42"),
            QmpStep(
                r"launcher: command started date",
                events=TERMINAL_CLICK,
                # wait is a program too (C:WAIT), and with no period it waits a
                # second (specs/dos.md).
                press="wait\n",
            ),
            QmpStep(
                r"launcher: command started wait",
                events=TERMINAL_CLICK,
                press="makedir Home:DosTest Home:DosTest2\n",
            ),
            QmpStep(
                r"launcher: command started makedir",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 50, 145, 500, 60, 40),),
                events=TERMINAL_CLICK,
                # Sys:BIG.TXT is bigger than one envelope, so its copy goes a
                # frame at a time and, into a Home: directory, through the
                # union's write-frame -- the acceptance's proof of both.
                press="copy Sys:AEGIR.TXT Sys:DOCS/NESTED.TXT Sys:BIG.TXT Home:DosTest\n",
            ),
            QmpStep(
                r"launcher: command started copy",
                events=TERMINAL_CLICK,
                # `l` is an alias the system's Shell-Startup set (specs/shell.md,
                # Sys:S/Shell-Startup = `alias l list`), so list starting proves
                # the shell ran its startup file before it read the console.
                press="l Home:DosTest Home:DosTest2\n",
            ),
            QmpStep(
                r"launcher: command started list",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 50, 145, 500, 60, 40),),
                events=TERMINAL_CLICK,
                # dir is List's names-only sibling (the Amiga's Dir): the same
                # directory, without the sizes. S: is the session's script
                # directory (specs/shell.md): auth makes Home:S and binds it,
                # and it is empty at login. The output is redirected to a file
                # (`>`), so dir's stdout is proven to reach a path, not the grid.
                press="dir Home:DosTest S: >Home:DosTest/DIR.TXT\n",
            ),
            QmpStep(
                r"launcher: command started dir",
                events=TERMINAL_CLICK,
                # Sys:S holds the startup scripts (specs/boot.md); reading one
                # proves the system's script directory resolves and reads. The
                # other two are what the two redirections wrote: the shell's own
                # `echo >file` and the command's `dir >file`.
                press="type Sys:S/Shell-Startup Home:ShellOut.TXT Home:DosTest/AEGIR.TXT "
                      "Home:DosTest/DIR.TXT\n",
            ),
            QmpStep(
                r"launcher: command started type",
                events=TERMINAL_CLICK,
                # `>NIL:` is the Amiga's quiet output (specs/boot.md): search
                # writes its matches to NIL: and they disappear.
                press="search Sys:AEGIR.TXT Sys:DOCS/NESTED.TXT disk >NIL:\n",
            ),
            QmpStep(
                r"launcher: command started search",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 50, 145, 500, 60, 40),),
                events=TERMINAL_CLICK,
                press="sort Sys:AEGIR.TXT Home:DosTest/SORTED.TXT\n",
            ),
            QmpStep(
                r"launcher: command started sort",
                events=TERMINAL_CLICK,
                press="join Sys:AEGIR.TXT Sys:AEGIR.TXT AS Home:DosTest/JOINED.TXT\n",
            ),
            QmpStep(
                r"launcher: command started join",
                events=TERMINAL_CLICK,
                # LONG.TXT is longer than a window, so more pages it and waits.
                press="more Sys:LONG.TXT\n",
            ),
            QmpStep(
                r"launcher: command started more",
                events=TERMINAL_CLICK,
                # q is the key more waits for; the rename line queued behind it
                # runs once more exits -- the terminal hands it to the shell.
                # The rename is same-directory: the volume protocol has no
                # cross-directory rename yet (specs/vfs.md).
                press="qrename Home:DosTest/AEGIR.TXT Home:DosTest/MOVED.TXT\n",
            ),
            QmpStep(
                r"launcher: command started rename",
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
                r"launcher: command started protect",
                events=TERMINAL_CLICK,
                # info lists the volumes the session may resolve.
                press="info\n",
            ),
            QmpStep(
                r"launcher: command started info",
                events=TERMINAL_CLICK,
                # which resolves a command name through the C: assignment.
                press="which copy\n",
            ),
            QmpStep(
                r"launcher: command started which",
                events=TERMINAL_CLICK,
                # assign binds an alias in the session's namespace; the version
                # line reads a file through it, so the binding is exercised by
                # a later command and not only by its own exit.
                press="assign FOOVOL Sys:\n",
            ),
            QmpStep(
                r"launcher: command started assign",
                events=TERMINAL_CLICK,
                press="version FOOVOL:VER.TXT\n",
            ),
            QmpStep(
                r"launcher: command started version",
                events=TERMINAL_CLICK,
                # `Run` starts a background command (specs/shell.md): the shell
                # does not wait, so the next step cues on the background
                # command's own exit, which the terminal reaps without a
                # `return code` line (specs/memory.md Phase 5). The background
                # command is one more process under its own mem.main badge.
                press="Run aegir-echo background\n",
            ),
            QmpStep(
                r"terminal: background command exited 0",
                events=TERMINAL_CLICK,
                # A pipeline (specs/pipe.md): `type` reads a file and writes it
                # to its standard output, `aegir-read` drains its standard input
                # to the grid. The terminal starts both at once and names the
                # pipe between them, so `aegir-read` starting proves the
                # concurrent spawn and the connection, and the pipeline's exit
                # (its last stage's) proves the consumer ran to its end.
                press="type Sys:S/Shell-Startup | aegir-read\n",
            ),
            QmpStep(
                r"terminal: pipeline exited",
                events=TERMINAL_CLICK,
                press="delete Home:DosTest Home:DosTest2 ALL\n",
            ),
            QmpStep(
                r"launcher: command started delete",
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
                # Control flow (specs/shell.md): the last command has failed and
                # the console is idle, so the runner now runs Control-Test, whose
                # If/Else/EndIf and Skip are the next cue. It is typed here, not
                # in the big opening press, because a press longer than the
                # keyboard's queue drops keys and mangles a line.
                press="execute Sys:S/Control-Test\n",
            ),
            QmpStep(
                r"terminal: command exited 42",
                events=TERMINAL_CLICK,
                # Newshell (specs/launch.md): the shell asks the launcher for a
                # nested terminal -- a kind-3 launch. The launcher mints it its
                # own console.gui (so it has its own window and slice), hands it
                # the session's namespace and the unbadged kit it needs, carves
                # its runtime and shell pool from the memory service under the
                # terminal's badge, and gives it a reserved badge range, so it
                # stands up as a peer with its own shell. The arguments exercise
                # the Amiga words: WINDOW= is the new window's own specification,
                # and FROM names the startup file its shell runs in place of
                # Shell-Startup. It is the last thing typed here, because the
                # new window takes the focus.
                press="newshell WINDOW=CON:32/32/560/360/Nested FROM Sys:S/Nested-Startup\n",
            ),
            QmpStep(r"terminal: nested window"),
            QmpStep(r"launcher: nested terminal started"),
            QmpStep(r"terminal: nested ready"),
            # The FROM startup ran: the nested shell executed aegir-echo 77,
            # whose exit code is unique to this run (specs/launch.md).
            QmpStep(
                r"terminal: command exited 77",
                # The click is in the session terminal's lower half, below the
                # nested terminal's window (its content ends at y=392), so the
                # command is typed at a terminal with the launcher kit -- the
                # nested terminal is depth one (specs/launch.md).
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 7680}},
                    {"type": "abs", "data": {"axis": "y", "value": 19251}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
                # The editor (specs/trinket/editor.md): the second windowed
                # customer, launched by its bare name. The launcher hands it its
                # own console.gui; it resolves C:, reads the file through the
                # session's namespace, and opens it in a tab. It comes before the
                # viewer and closes before it, so the two never share the screen.
                press="edit Sys:AEGIR.TXT\n",
            ),
            QmpStep(r"launcher: command started edit"),
            # The editor is up: the block cursor of insert mode at the head of
            # the file's first line, its cell the theme's blue, with the glyph
            # under it drawn in the cursor's own colour.
            QmpStep(
                r"editor: ready",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 426, 227, 102, 136, 187),),
                dark=(("gpu0", 432, 226, 220, 12, 20),),
                # Type at the head and toggle to overwrite in the same press, so
                # the cue that paces the next dump is the mode, not a keystroke.
                press="Hi<insert>",
            ),
            # Overwrite mode: the cursor is the simple underline now, along the
            # foot of the cell the two keys moved the caret to.
            QmpStep(
                r"editor: overwrite",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 442, 241, 102, 136, 187),),
                press="<insert>",
            ),
            # Insert mode again, the block back at the caret. From here the File
            # menu drives the rest: the click on the bar drops it.
            QmpStep(
                r"editor: insert",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 442, 234, 102, 136, 187),),
                # The editor is the active window, so the screen bar's first
                # title -- "Ed" -- is its menu. The click is at the title's
                # left, x 10, not the demo's x 30: "Ed" is two cells wide and
                # its slot ends before 30 (desktop.cc title_slots), so the
                # demo's coordinate would land past it and drop nothing.
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 256}},
                    {"type": "abs", "data": {"axis": "y", "value": 450}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # The menu dropped. Its rows are 22 pixels at y 33, 55, 77, 99 and
            # 121 from the bar: New, Open..., Save, Save As..., Quit. Save As...
            # is the fourth, and raises the requester in save mode.
            QmpStep(
                r"bureau: client menu Ed",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # The bar still, and the menu below it: its first row's face,
                # and the five items' ink in the block the rows occupy.
                pixels=(
                    ("gpu0", 100, 5, 102, 136, 187),
                    ("gpu0", 100, 33, 240, 240, 240),
                ),
                dark=(("gpu0", 0, 22, 120, 110, 20),),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 256}},
                    {"type": "abs", "data": {"axis": "y", "value": 4055}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # The requester is up over the namespace, its drawer at Sys:, its
            # Pattern box focused. Type a name into its File box and Return: the
            # box's on_submit is the default OK, so the editor writes the buffer
            # under the new name (specs/trinket/file_requester.md).
            QmpStep(
                r"editor: save requester",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # The requester's titlebar, and its list of Home:'s entries.
                pixels=(("gpu0", 700, 200, 102, 136, 187),),
                dark=(("gpu0", 424, 261, 404, 160, 150),),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 17278}},
                    {"type": "abs", "data": {"axis": "y", "value": 21340}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
                press="EDITED.TXT\n",
                press_delay=0.2,
            ),
            # Save As... wrote Home:EDITED.TXT and renamed the tab. The
            # requester closed, and the console clears the focus with the window
            # it had -- so the editor is clicked back first (its content, not its
            # titlebar: a titlebar click can begin a drag if the pointer's next
            # move is drained before the button-up, and the window would slide
            # off its rectangle), then the bar's title and Save: the active tab
            # now has a name of its own, so this is the other write path.
            QmpStep(
                r"editor: saved as Home:EDITED\.TXT",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # The editor's titlebar and the tab Save As... renamed to the
                # file it wrote -- present whether the requester has closed yet.
                pixels=(("gpu0", 700, 190, 102, 136, 187),),
                dark=(("gpu0", 424, 202, 110, 18, 6),),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 17920}},
                    {"type": "abs", "data": {"axis": "y", "value": 12288}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                    {"type": "abs", "data": {"axis": "x", "value": 256}},
                    {"type": "abs", "data": {"axis": "y", "value": 450}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                    {"type": "abs", "data": {"axis": "x", "value": 256}},
                    {"type": "abs", "data": {"axis": "y", "value": 3154}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # Save wrote the tab's own name; New then makes a fresh Untitled tab.
            QmpStep(
                r"editor: saved Home:EDITED\.TXT",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # Save left the editor alone on the screen: its titlebar, the
                # named tab, and the buffer's first line under it.
                pixels=(("gpu0", 700, 190, 102, 136, 187),),
                dark=(("gpu0", 424, 202, 110, 18, 6),
                      ("gpu0", 432, 226, 220, 12, 20)),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 17920}},
                    {"type": "abs", "data": {"axis": "y", "value": 12288}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                    {"type": "abs", "data": {"axis": "x", "value": 256}},
                    {"type": "abs", "data": {"axis": "y", "value": 450}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                    {"type": "abs", "data": {"axis": "x", "value": 256}},
                    {"type": "abs", "data": {"axis": "y", "value": 1352}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # New made a third tab; Open... reads a system file, the readable
            # side, where Save As... proved the writable one.
            QmpStep(
                r"editor: new",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # New added a tab: two labels now cross the strip, where Save
                # left one.
                pixels=(("gpu0", 700, 190, 102, 136, 187),),
                dark=(("gpu0", 424, 202, 150, 18, 8),),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 17920}},
                    {"type": "abs", "data": {"axis": "y", "value": 12288}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                    {"type": "abs", "data": {"axis": "x", "value": 256}},
                    {"type": "abs", "data": {"axis": "y", "value": 450}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                    {"type": "abs", "data": {"axis": "x", "value": 256}},
                    {"type": "abs", "data": {"axis": "y", "value": 2253}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # The open-mode requester, its drawer at Sys:: the version file is a
            # fixture the read can name (scripts/make_disk.py).
            QmpStep(
                r"editor: open requester",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # The requester again, this time over Sys:'s entries.
                pixels=(("gpu0", 700, 200, 102, 136, 187),),
                dark=(("gpu0", 424, 261, 404, 160, 150),),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 17278}},
                    {"type": "abs", "data": {"axis": "y", "value": 21340}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
                press="VER.TXT\n",
                press_delay=0.2,
            ),
            # The read crossed: the tab holds the file Open... named. Quit
            # returns the shell its prompt.
            QmpStep(
                r"editor: opened Sys:VER\.TXT",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # Open added a third tab: three labels cross the strip.
                pixels=(("gpu0", 700, 190, 102, 136, 187),),
                dark=(("gpu0", 424, 202, 220, 18, 8),),
                events=(
                    {"type": "abs", "data": {"axis": "x", "value": 17920}},
                    {"type": "abs", "data": {"axis": "y", "value": 12288}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                    {"type": "abs", "data": {"axis": "x", "value": 256}},
                    {"type": "abs", "data": {"axis": "y", "value": 450}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                    {"type": "abs", "data": {"axis": "x", "value": 256}},
                    {"type": "abs", "data": {"axis": "y", "value": 4955}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # Quit returns the shell its prompt: the viewer is launched where
            # the later steps expect it.
            QmpStep(r"editor: quit", events=TERMINAL_CLICK,
                    press="view Sys:AEGIR.TXT\n"),
            QmpStep(r"launcher: command started view"),
            # The viewer is up: its window is its own default (no launcher
            # AEGIR_WINDOW was set), its content is the Workbench grey, and the
            # file it read through the namespace is drawn as text in its grid.
            QmpStep(
                r"view: ready",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 600, 450, 170, 170, 170),),
                dark=(("gpu0", 424, 204, 300, 24, 20),),
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
            # The window manager's demo client (specs/window-manager.md): a
            # session command now, started by Shell-Startup at login, so its
            # rectangles exist only after `demo: rects`. The runner clicks its
            # Image tab first -- the datatypes acceptance (specs/datatypes.md)
            # -- then returns to the Lists page the rest of the demo reads.
            QmpStep(
                r"demo: rects",
                clicks=(("demo.tabs.tab.5", 0.5, 0.5),),
            ),
            # The Image tab: all three decoded frames (specs/datatypes.md). The
            # ILBM is palette red left / green right, the PNG blue left / white
            # right, the JPEG (lossy) orange left / purple right; the pins read
            # a pixel in each half of each -- the JPEG's are the exact values
            # the pinned libjpeg-turbo decodes that fixture to -- so the class,
            # the client, the transfer page and the blit are one chain, and the
            # colours differ, so a rect naming the wrong image or a decode that
            # mixed the channels shows. Then back to the Lists page.
            QmpStep(
                r"demo: tab 5",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pins=(
                    ("gpu0", "demo.image", 0.25, 0.5, 255, 0, 0),
                    ("gpu0", "demo.image", 0.75, 0.5, 0, 255, 0),
                    ("gpu0", "demo.png", 0.25, 0.5, 0, 0, 255),
                    ("gpu0", "demo.png", 0.75, 0.5, 255, 255, 255),
                    ("gpu0", "demo.jpeg", 0.25, 0.5, 255, 127, 0),
                    ("gpu0", "demo.jpeg", 0.75, 0.5, 127, 0, 255),
                ),
                clicks=(("demo.tabs.tab.3", 0.5, 0.5),),
            ),
            QmpStep(
                r"demo: tab 3",
                clicks=(("demo.zoom", 0.5, 0.5),),
            ),
            QmpStep(
                r"demo: zoomed",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 1000, 10, 102, 136, 187),
                    ("gpu0", 1000, 400, 191, 191, 191),
                    ("gpu0", 1000, 795, 102, 136, 187),
                ),
                clicks=(("demo.zoom", 0.5, 0.5),),
            ),
            QmpStep(
                r"demo: restored",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # Lists opens first (specs/trinket/tabs.md), so the body holds
                # the list and its scrollbar over the horizontal bar. The list's
                # first row is chosen, the rows below it are ink, and the gadget
                # face stands where the window's old body was.
                pixels=(
                    ("gpu0", 950, 482, 102, 136, 187),
                    ("gpu0", 1000, 600, 191, 191, 191),
                ),
                dark=(("gpu0", 906, 507, 60, 100, 120),),
                clicks=(
                    # The list's third row (specs/trinket/listview.md) and the
                    # horizontal bar's increment: each is a later step's trigger.
                    ("demo.list.row.3", 0.5, 0.5),
                    ("demo.hbar.increment", 0.5, 0.5),
                ),
            ),
            # The list's click took: its third row is chosen and the first is
            # plain again (specs/trinket/listview.md). The rows are sixteen
            # pixels at 475, 491, 507 and 523.
            QmpStep(
                r"demo: list 3",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 950, 525, 102, 136, 187),
                    ("gpu0", 950, 482, 191, 191, 191),
                ),
                clicks=(
                    # Then its scrollbar's foot arrow (specs/trinket/scrollbar.md):
                    # a scroll moves the rows up one, and the cue that prints is a
                    # later step's trigger.
                    ("demo.list_scrollbar.increment", 0.5, 0.5),
                ),
            ),
            # The horizontal bar's increment took: its left and right arrows are
            # the MUI ArrowLeft/ArrowRight art (their white marks at 1115 and
            # 1136 on row 730), and the value stepped 40 -> 41. The bar is on
            # this page, so its check rides this cue's dump.
            QmpStep(
                r"demo: bar 41",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 1115, 730, 240, 240, 240),
                    ("gpu0", 1136, 730, 240, 240, 240),
                ),
                dark=(("gpu0", 1113, 722, 44, 24, 30),),
                # Drag the bar's thumb to the far end of its trough. A scrollbar
                # thumb is grabbed and carried, like the slider's knob; the
                # value must reach the maximum (100 - 25 = 75) the far end names
                # (specs/trinket/scrollbar.md).
                drags=(("demo.hbar.thumb", 0.5, 0.5,
                        "demo.hbar.trough", 1.0, 0.5),),
            ),
            # The thumb drag took: the bar's value reached 75, its maximum. A
            # click in the trough pages by a whole page, not to the end, so only
            # the thumb carried there explains this.
            QmpStep(r"demo: bar 75"),
            # The list's scrollbar arrow took: the rows moved up one, so the
            # chosen row now sits a row higher than it did. Then the Toggles tab
            # (specs/trinket/tabs.md), the last cue of the Lists page.
            QmpStep(
                r"demo: listed 1",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 950, 498, 102, 136, 187),),
                clicks=(("demo.tabs.tab.1", 0.5, 0.5),),
            ),
            # The Toggles tab is up: the checkbox and the radio group
            # (specs/trinket/checkbox.md, radio_group.md). The group's first
            # member is checked, its ring filled; the second's is hollow.
            QmpStep(
                r"demo: tab 1",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 1003, 620, 61, 101, 162),
                    ("gpu0", 1078, 620, 174, 174, 174),
                ),
                clicks=(
                    # The radio group's second member (specs/trinket/radio_group.md):
                    # it clears the first.
                    ("demo.radios.radio.2", 0.5, 0.5),
                ),
            ),
            # The radio's click took: the second member is checked and the first
            # cleared (specs/trinket/radio_group.md). Then the Text tab.
            QmpStep(
                r"demo: radio 2",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 1078, 620, 61, 101, 162),
                    ("gpu0", 1003, 620, 174, 174, 174),
                ),
                clicks=(("demo.tabs.tab.4", 0.5, 0.5),),
            ),
            # The Text tab is up: the terminal (specs/terminal.md) over the
            # outline label, with the terminal's scrollbar. The grid has text on
            # it -- a grid that wrapped every character into one column, or drew
            # nothing, has far less ink -- the label's band is ink too, and the
            # scrollbar's two arrow buttons stack at its foot, each drawing a
            # hollow mark whose dark trailing edges are the ink.
            QmpStep(
                r"demo: tab 4",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(
                    ("gpu0", 906, 478, 220, 120, 100),
                    ("gpu0", 906, 723, 220, 20, 20),
                    ("gpu0", 1138, 676, 16, 46, 8),
                ),
                clicks=(
                    # The scrollbar's decrement (up) button: a click scrolls the
                    # terminal up a line (specs/trinket/scrollbar.md).
                    ("demo.scrollbar.decrement", 0.5, 0.5),
                ),
            ),
            # The scrollbar's click scrolled the terminal, and the cue says so
            # (specs/trinket/scrollbar.md). Then the Values tab: a tab change is
            # the cue, so the page's own widgets are read on it.
            QmpStep(
                r"demo: scrolled",
                clicks=(("demo.tabs.tab.2", 0.5, 0.5),),
            ),
            # The Values tab is up: the cycle and popup button over the slider
            # (specs/trinket/cycle.md, popup_button.md, slider.md). The cycle
            # shows its short first entry -- the text band's ink is what says so
            # -- and the slider's knob sits at the middle, the trough's dither
            # left where the knob will land.
            QmpStep(
                r"demo: tab 2",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 1035, 519, 170, 170, 170),),
                dark=(("gpu0", 924, 478, 178, 14, 60),),
                clicks=(
                    # The slider's trough, right of its knob (specs/trinket/slider.md):
                    # a trough click steps the value one step toward the click
                    # (50 -> 60). Then the cycle's button cell: a click there
                    # advances to the next entry (specs/trinket/cycle.md).
                    ("demo.slider.trough", 0.7, 0.5),
                    ("demo.cycle.cell", 0.5, 0.5),
                ),
            ),
            # The slider's trough click took: the value stepped to 60 and the
            # knob moved right (specs/trinket/slider.md). The raised face stands
            # where the trough's dither was -- the click, the widget and the
            # repaint, end to end.
            QmpStep(
                r"demo: slider 60",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 1035, 519, 191, 191, 191),),
                # Then drag the knob itself to the trough's far end. A knob
                # moves only this way -- press, carry, release -- where a click
                # in the trough steps by one; the value must reach the maximum
                # the far end names (specs/trinket/slider.md). The knob's own
                # rectangle is re-reported every poll, so the press lands on it
                # where the 60 left it.
                drags=(("demo.slider.knob", 0.5, 0.5,
                        "demo.slider.trough", 1.0, 0.5),),
            ),
            # The knob drag took: the value reached 100, the far end. A click in
            # the trough could not have done this -- it steps by `step_` -- so
            # only the drag carrying the knob explains the cue.
            QmpStep(r"demo: slider 100"),
            # The cycle's button-cell click took: the active entry advanced from
            # the short one to the long one, so the text band's ink roughly
            # doubles (specs/trinket/cycle.md). Then the cycle's text, which opens
            # the entries' menu (specs/trinket/popup.md): a popup-opening gesture
            # goes after every cue the burst produced, so nothing intercepts it.
            QmpStep(
                r"demo: cycle 2",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 924, 478, 178, 14, 100),),
                clicks=(("demo.cycle.text", 0.5, 0.5),),
            ),
            # The menu the text opened: a framed list under the cycle, its rows
            # sixteen pixels at 498, 514 and 530. The active entry -- the long one
            # the cell just landed on -- carries the solid bar on the second row.
            # The pick is the third row's middle: A4000, an entry the active one
            # is not, so the pick is a change and reports.
            QmpStep(
                r"demo: menu 1",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 950, 521, 102, 136, 187),
                    ("gpu0", 950, 505, 191, 191, 191),
                ),
                clicks=(("demo.popup.row.3", 0.5, 0.5),),
            ),
            # The pick closed the cycle's menu. The popup button's own object is
            # in the demo but unasserted: its step comes once the cycle's menu
            # is settled, one uncertain thing per run.
            QmpStep(r"demo: menu 0"),
            # The pick took: the cycle's active entry is the picked one and the
            # menu is gone (specs/trinket/cycle.md). Then the popup button
            # (specs/trinket/popup_button.md), which opens its object: a
            # popup-opening gesture goes after every cue the burst produced, so
            # nothing is left to intercept it.
            QmpStep(
                r"demo: cycle 3",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 924, 478, 178, 14, 60),),
                clicks=(("demo.popup_button", 0.5, 0.5),),
            ),
            # The popup button's click took, and opened its object: a list
            # anchored under the button, with the magnifier's own colour still at
            # the pixel it held in the imported art.
            QmpStep(
                r"demo: popup",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 1140, 481, 96, 128, 176),),
            ),
            # The object itself: a list of three with its entries centred, its
            # rows sixteen pixels at 498, 514 and 530. The pick is the second
            # row's middle -- Save -- and the cue it prints is the next step's.
            # Its cue is `demo: object`, not the cycle's `demo: menu`: the two
            # popups are the window's one layer, and a cue the runner answers
            # with two steps would fire the object's pick on the cycle's menu.
            QmpStep(
                r"demo: object 1",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 1050, 500, 100, 46, 120),),
                clicks=(("demo.popup.row.2", 0.5, 0.5),),
            ),
            QmpStep(r"demo: object 0"),
            # The object was picked at its second row, Save
            # (specs/trinket/popup_button.md). Then the screen bar's first title
            # -- the demo's menu (specs/workbench.md) -- which drops the bureau's
            # menu over the screen, and so goes last of the demo's gestures, with
            # no demo popup left for it to be mistaken for. mistaken for.
            QmpStep(
                r"demo: picked 2",
                clicks=(("bureau.title.1", 0.5, 0.5),),
            ),
            # The bureau.menu server (specs/workbench.md), while the demo is
            # still up: it registered its tree when it gained the focus, so the
            # bar's first title is the demo's, and clicking it drops the demo's
            # menu -- clicking the bar must not take the demo's focus
            # (kBackdropTakesFocus). Clicking the first item prints the demo's
            # cue: proof the tree crossed to the bureau and the action back.
            QmpStep(
                r"bureau: client menu Demo",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 2, 10, 102, 136, 187),
                    ("gpu0", 100, 33, 240, 240, 240),
                ),
                clicks=(("bureau.item.1", 0.5, 0.5),),
            ),
            QmpStep(
                r"demo: about",
                # The About pick closed the demo's menu; reopen it from the
                # bar's first title and click Open... -- the third item, the
                # menu's rows 22 pixels at y 33, 55 and 77.
                clicks=(
                    ("bureau.title.1", 0.5, 0.5),
                    ("bureau.item.3", 0.5, 0.5),
                ),
            ),
            # The file requester (specs/trinket/file_requester.md): a window
            # centered on the screen -- its geometry is measured from the text,
            # so the font decides it and the rect cues are what the clicks
            # follow -- with a titles row and the entries under it, three
            # labelled control rows (Pattern, Drawer with its toggle, File) and
            # a row of four buttons, OK first. The list's text is ink.
            QmpStep(
                r"demo: requester up",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # The requester's titlebar, so the check fails if the cue fired
                # before the window was up.
                pixels=(("gpu0", 700, 200, 102, 136, 187),),
                dark=(
                    ("gpu0", 424, 261, 404, 160, 150),
                ),
                # Type a wildcard into the Pattern box. The requester opens it
                # focused (FileRequester::show), so no click is needed: a click
                # races the first key and loses it. The box opens holding #?
                # with the cursor at its head, so the keys land before it and
                # AEGIR#? is the pattern the cue then names.
                press="AEGIR",
            ),
            # The pattern kept one row, AEGIR.TXT: the cue says so and the
            # first row under the titles holds its ink.
            QmpStep(
                r"demo: filtered AEGIR\#\? 1",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 424, 253, 400, 12, 8),),
                # Volumes: the browsable volumes replace the list. The button is
                # the second of the four.
                clicks=(("demo.requester.button.2", 0.5, 0.5),),
            ),
            # The volumes list: the namespace's volumes less NIL: and PIPE:,
            # which carry kFlagNoDir -- seven rows, each with its backing device
            # and capacity beside the label. The cue names the count, so their
            # absence is checked rather than merely unread; the rows hold ink,
            # and the capacity columns (x 640 up, where no label reaches) ink
            # only when `space` answered.
            QmpStep(
                r"demo: volumes ",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 424, 276, 400, 140, 40),
                      ("gpu0", 640, 276, 190, 64, 30)),
                # Type a name into the File box and press Return: the default OK
                # takes it, and the demo reports the name it chose.
                clicks=(("demo.requester.file_box", 0.5, 0.5),),
                press="AEGIR.TXT\n",
                press_delay=0.2,
            ),
            # OK resolved the name and the requester closed; the demo's own
            # window is clear again, so close it from its titlebar.
            QmpStep(
                r"demo: opened AEGIR.TXT",
                clicks=(("demo.close", 0.5, 0.5),),
            ),
            # The demo closed, so the bureau's own menus stand again: the runner
            # clicks the bar's first title, the bureau drops its menu, and reads
            # the Workbench action's cue.
            QmpStep(
                r"demo: closed",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(("gpu0", 1000, 400, 170, 170, 170),),
                clicks=(("bureau.title.1", 0.5, 0.5),),
            ),
            QmpStep(
                r"bureau: menu",
                dumps=("gpu0",),
                expect=((1280, 800),),
                pixels=(
                    ("gpu0", 2, 10, 102, 136, 187),
                    ("gpu0", 100, 33, 240, 240, 240),
                ),
                # The Execute row's accelerator is drawn: `[Win] Space`, in the
                # row's right half, past the label. A named key that drew
                # nothing (a bare space) leaves this zone blank and fails.
                dark=(("gpu0", 95, 66, 75, 22, 8),),
                clicks=(("bureau.item.1", 0.5, 0.5),),
            ),
            QmpStep(r"bureau: Aegir, the Workbench"),
            # The Bureau's Execute (specs/launch.md): the Bureau has no console
            # stream of its own, so a command it starts goes to the launcher's
            # read-only output view. The runner reopens the Bureau menu and
            # clicks Execute...; the requester takes focus asynchronously, so the
            # typing waits for the Bureau's own "execute ready" cue instead of
            # racing it -- keys sent before the requester is focused land on the
            # window focused before it, or nowhere, and the word loses its head
            # (the "info" -> "nfo" flake). Then it reads the command start and
            # the view's own start; the view cues when the command's exit has
            # been seen, and the screendump there proves the text landed.
            # The Bureau's Execute: reopen the Bureau menu and click its third
            # item. The open menu is the console's screen layer, so a click into
            # it is the menu's, not a window overlapping it
            # (specs/workbench.md's pointer routing).
            QmpStep(
                r"view: ready",
                clicks=(
                    ("bureau.title.1", 0.5, 0.5),
                    ("bureau.item.3", 0.5, 0.5),
                ),
            ),
            # The requester is up and focused: the Bureau cued it after the
            # blocking focus call, so the keys cannot land elsewhere.
            QmpStep(
                r"bureau: execute ready",
                press="info\n",
            ),
            QmpStep(r"bureau: execute info"),
            QmpStep(r"launcher: command started info"),
            QmpStep(r"output: ready"),
            QmpStep(
                r"output: command done",
                dumps=("gpu0",),
                expect=((1280, 800),),
                dark=(("gpu0", 46, 446, 300, 60, 20),),
            ),
            # The screen shortcut (specs/workbench.md): Super+Space runs the
            # Bureau's Execute wherever the focus is, so the screen bar's menu
            # need not be walked with the pointer. Pressed on the menu
            # command's own end, and the bureau's cue proves the key crossed.
            QmpStep(
                r"output: command done",
                press="<win-space>",
            ),
            QmpStep(r"bureau: screen shortcut"),
        ),
    )


TARGETS: dict[str, Target] = {
    # The floor of the envelope: the smallest machine Aegir supports, and where
    # capacity problems are meant to show up first.
    # Two cores: the boot's own work is the run's long pole (the serial is
    # seconds of it, not minutes), so the baseline is the floor with the
    # parallelism the tree is meant to use (specs/aegir.md).
    "aegir": _aegir(2048, 2, "aegir"),
    # The boot failure view (specs/boot.md): the same machine, booted with
    # aegir.fail on the firmware's command line, so the boot session's sequence
    # is forced to fail and auth leaves the read-only view standing instead of
    # starting the greeter. The two cues are the proof; the success target is
    # what proves the other path.
    "aegir-fail": replace(
        _aegir(2048, 1, "aegir"),
        name="aegir-fail",
        description="Aegir's boot failure view, forced with -append aegir.fail",
        # The failure view is the marker, not AEGIR_BOOT_OK: on this boot the
        # director's boot wait is paced by the test bed, which the acceptance
        # does not drive through its whole sequence (no greeter runs), so the
        # marker that matters is the view coming up.
        marker="terminal: boot failed, the view is up",
        qemu_args=_aegir(2048, 1, "aegir").qemu_args + ("-append aegir.fail",),
        qmp_steps=(
            QmpStep(r"auth: the boot session failed -- the failure view stands"),
            QmpStep(r"terminal: boot failed, the view is up"),
            # The test bed paces itself through the console's channel and will
            # not report ready until its first click is answered, which the
            # director's boot waits on -- so the click is here too, though the
            # rest of the acceptance's steps are not (no greeter runs).
            QmpStep(
                r"test: the console's channel -- a click, please",
                events=(
                    {"type": "rel", "data": {"axis": "x", "value": -376}},
                    {"type": "rel", "data": {"axis": "y", "value": -186}},
                    {"type": "btn", "data": {"button": "left", "down": True}},
                    {"type": "btn", "data": {"button": "left", "down": False}},
                ),
            ),
            # The failure view's window is created at run time, so the console
            # has a frame or two of compositing to do; this cue is a later one,
            # so the screendump sees the window rather than the frame before it.
            QmpStep(
                r"test: the click focused the window",
                dumps=("gpu0",),
                expect=((1280, 800),),
                # The boot session's failure view: its window is up where the
                # session terminal's would be, its grid grey, with the console's
                # blue backdrop still around it.
                pixels=(
                    ("gpu0", 60, 200, 170, 170, 170),
                    ("gpu0", 500, 300, 170, 170, 170),
                    ("gpu0", 10, 10, 0, 85, 170),
                ),
            ),
        ),
    ),
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

# The envelope, in the order it is worth trying: the floor first (two cores
# now), then the same machine with more, then the upper end of the expected
# memory range. The floor's own core count is the baseline's, so there is no
# second entry for it.
ENVELOPE: tuple[str, ...] = ("aegir", "aegir-2g-smp4", "aegir-8g-smp4")
