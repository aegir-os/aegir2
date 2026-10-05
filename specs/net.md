# The network stack

Status: decided (2026-10). This is the "TCP/IP and a socket layer" of
`specs/direction.md`. It supersedes that file's "a network stack loaded per
interface": **one stack owns every interface** (a netif each), because a host's
routing is a property of the stack, not of one NIC.

## What we are building

Three pieces and two protocols:

    aegir-virtio-net  --aegir/ethernet.h-->  aegir-net  --aegir/net.h-->  clients
      owns virtio id 1    raw Ethernet frames  runs lwIP   BSD-shaped sockets

- **`aegir-virtio-net`** owns the network device and serves raw Ethernet
  frames. It is a driver like any other: a registry row, spawned by the
  device manager (`specs/services.md`). **One driver per device** -- a machine
  with two NICs runs two of them and neither multiplexes.
- **`aegir-net`** runs lwIP over **every** link, a netif per interface, and
  serves a socket port. One stack means one routing table and one socket
  namespace: a packet's route is a decision across all the interfaces,
  not a decision two unaware stacks would each take alone. It knows nothing
  about virtio, so on a board only the link drivers change.
- **`aegir/ethernet.h`** and **`aegir/net.h`** are the two protocol headers,
  the `aegir/block.h` shape: the owner includes one to serve, the client to
  call, and there is no code in either.

## The constraint that shapes everything

**Nothing in the stack may name the machine.** No QEMU, no virtio, no
`10.0.2.x`, no slirp. The reasons are concrete and each one is a seam:

- the **MAC** comes from the device's config space (`VIRTIO_NET_F_MAC`), not
  from a constant;
- the **MTU and link state** come from the device when it offers them, and the
  protocol's own 1500 / up when it does not;
- the **address, netmask, gateway and DNS server** come from DHCP, not from the
  build;
- **checksums are lwIP's**, because the driver negotiates no offload feature --
  QEMU's transport may not offer one and a board's MAC may, so the portable
  path is the guest's.

The only machine-specific things in the whole arc are a registry row and the
runner's QEMU arguments, both outside the stack.

## The link: `aegir-virtio-net`

Device id 1 (`specs/reference/virtio-mmio.md`). It follows the shape the block,
input and display drivers already share (`libs/aegir-virtio`): the register
window, the status handshake, and two queues on the shared `Queue` object.

- **receiveq (index 0)** is primed the way virtio-input's eventq is: one
  device-writable buffer per descriptor, re-primed as each is consumed, so the
  device always has somewhere to put a frame. A completed buffer yields one
  frame after the device header.
- **transmitq (index 1)** carries a chain: the device header, then the frame.
- The **device header** is the 10-byte `virtio_net_hdr`. We do not negotiate
  `MRG_RXBUF`, so there is no `num_buffers` field, and we do not negotiate the
  GSO/csum features, so the header is ignored on receive and zeroed on send.
- **the frame does not fit the envelope.** A full frame is 1518 bytes and the
  message registers carry 952, so frames cross through the driver's **shared
  window** (the registry row's `window` bits, 64 KiB): `send` transmits the
  frame at the window's start, `receive` copies one to `kReceiveOffset` past it
  and answers its length, and the client maps the same frames. The two offsets
  are separate because the stack sends from its tcpip thread and receives on
  another: at one offset a send would overwrite a frame the receive thread had
  not read yet. One link serves one client, so one window per
  link suffices -- where block's per-client windows guard a preempted client's
  data against another's DMA, a second interface here is a second driver and a
  second window, not a shared one.
- a `mac`/`mtu`/`link` method answers what the stack needs to configure its
  netif; a `send` transmits the frame in the caller's window; a `receive` is a
  **held reply** that lands a frame in the window and answers its length
  (`specs/services.md`'s held-reply shape, `specs/signal.md`).
- the driver's interrupt is bound and acked the way every driver's is
  (`specs/services.md`); a driver without a pair polls.
- registry row: `compatible=virtio,mmio id=1 prefix=eth bus=virtio
  binary=aegir-virtio-net memory=<n> window=<the frame window>`.
- **one device, one driver, one stack.** Two Ethernet interfaces are two link
  drivers -- two bindings of the row, two instances (`eth.virtio0`,
  `eth.virtio1`), the instance model the two gpu heads already proved
  (`specs/services.md`); a driver never multiplexes two ports. But it is **one
  stack** that opens both and adds a netif to each: routing and the socket
  namespace belong to the stack, and a host with two NICs must not have two of
  each. (ARP is per-netif, but within the one stack, so the routes and the
  caches still see every interface together.)

## Vendoring lwIP

**lwIP 2.2.1**, BSD-3-Clause, pinned by commit SHA in `manifests/aegir.xml`
and extracted to **`third_party/lwip`**. Savannah's git server is not a reliable
fetch target here (the same reason musl is a signed tarball source), so the
project's official GitHub mirror is used, pinned to the
`STABLE-2_2_1_RELEASE` commit; git's object hashing is the anchor.

That path is deliberate: lwIP 2.2.1 ships a root `CMakeLists.txt`, and the seL4
build system does

    file(GLOB result RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" projects/*/CMakeLists.txt)
    foreach(file ${result}) add_subdirectory("${file}") endforeach()

(`tools/seL4/cmake-tool/projects.cmake`) -- so a checkout under `projects/`
would be swallowed into the kernel build. `third_party/` is outside that glob
and already gitignored. The source tree is read-only, as every vendored tree is
(`specs/third_party.md`); any change of ours is a patch under
`third_party/patches/third_party/lwip/`.

**It is compiled in-tree**, as the `lwip` static target in
`libs/freestanding/aegir-lwip` -- not by an out-of-tree script the way the
hosted vendored libraries (zlib, libpng, libjpeg) are. lwIP is a *freestanding*
library: it must see the freestanding flags and muslc's staged headers, and
those exist only inside the seL4 build. The source list is lwIP's own core,
IPv4, api and netif sets (`src/Filelists.cmake`); ppp, 6LoWPAN, bridge, slip and
the apps are the modules we did not take. `lwipopts.h` and `arch/cc.h` /
`arch/sys_arch.h` beside it are the port's configuration, and the sys_arch
implementation is the port's next slice.

## The stack: `aegir-net`

### lwIP runs threaded (`NO_SYS=0`)

The service starts lwIP's `tcpip_thread` with `aegir-thread`'s `Builder`
(`libs/aegir-thread`), and provides the `sys_arch` that `NO_SYS=0` requires:

- `sys_sem` and `sys_mutex` over **seL4 notifications** plus a protected
  counter, in-process;
- `sys_mbox` as a pointer ring whose post nudges an **endpoint** the fetch
  receives on -- an endpoint, not a notification, because the kernel delivers a
  bound notification only to a thread blocked in an endpoint receive
  (`ThreadState_BlockedOnReceive`, `kernel/src/object/notification.c:69`), and
  the timed fetch below needs the tick to wake it;
- `sys_arch_protect`/`unprotect` as the critical section;
- `sys_now()` from a cached monotonic reading (`clock.main`/`timer.main`),
  advanced on the tick, never a port call per read;
- `sys_thread_new` starting the tcpip thread.

**The timed mailbox fetch is the lwIP timer.** `tcpip_thread` calls
`sys_arch_mbox_fetch(mbox, msg, timeout)` with the next lwIP timer's timeout so
`sys_check_timeouts()` runs while the link is idle (TCP retransmit, ARP, DHCP).
seL4 has no timed receive, so the tcpip thread has the timer's tick bound to it
and waits on its mailbox's endpoint, receiving the tick's badge as well; a wake
with that badge returns `SYS_ARCH_TIMEOUT` and lwIP runs its timeouts. That is
`aegir::signal`'s `Receiver` shape (`specs/signal.md`).

### The tick comes from `timer.main`

`timer.main` gains a **periodic subscription**: a client registers a signal
`Context_capability` and a period, and the timer -- which owns the RTC's alarm
-- signals it each tick. This is where lwIP's timed fetch gets its timeout, and
it is also what `specs/timer.md` already promised ("a later timer with a queue
serves many"). The tick is the tcpip thread's alone; the service thread
completes held replies on readiness, not on the tick.

### Readiness: the raw API, held replies

The service owns `tcp_pcb`s and `udp_pcb`s, and registers `tcp_recv`,
`tcp_sent` and `tcp_err` on each. Those callbacks run **inside the tcpip
thread** and signal the service through a context (`aegir::signal`), so a
waiting client's answer crosses the instant lwIP finishes a segment -- no tick
of latency, no polling. This is the raw API *inside* the service only; the
socket API stays compiled and available.

### Serving many clients without trouble

A held reply keeps the service single-threaded (one blocked receive per thread,
`kernel/manual/parts/ipc.tex`), and the kernel makes it safe to hold many:
`seL4_CNode_SaveCaller` frees the receive, so N clients can each have a call
outstanding, and

> a reply capability is a **non-owning reference** ... if the caller thread is
> destroyed or modified in any way that would render a reply impossible ... the
> kernel would immediately destroy the reply capability.
> (`kernel/manual/parts/ipc.tex:267-273`)

So a client that dies mid-read leaves nothing: the cap is destroyed, its slot
empties, and the service's later answer to that slot is refused and dropped.
Capacity is **CSpace slots, grown on demand**, not threads -- which matters,
because a thread that ends is not rebuilt (`aegir/thread.h`), so a worker per
connection would leak a TCB, a stack and a TLS block for every short-lived
connection. There is no thread per client and none per call.

A client that dies leaves its **sockets**, though, and a socket is a `tcp_pcb`,
a `udp_pcb` or a `raw_pcb` in the stack's own heap -- the reply-cap rule does not
reach it. So the port carries a `reap`: a badge in, every socket that badge owns
closed, as though `close` had been called on each. It is the socket port's half
of the teardown the volumes and the console already have (their `reap`, called by
auth when a session ends), and the **launcher** is its caller: it already holds
the socket port and mints each command's caller half from it, so when it buries a
command -- exit or fault, the same path -- it names the command's badge and the
stack drops what it left. `netsmoke` proves it by exiting with a socket open.

## The client port: BSD sockets

**Clients see a BSD socket, not the raw API.** The port is shaped the way
`socket(2)` is, so an application -- including a hosted one -- uses the familiar
calls and never touches a `tcp_pcb`:

- `socket` (domain, type, protocol) answers an id; `bind`, `listen`, `connect`,
  `accept`, `send`, `recv`, `shutdown`, `close`, `resolve`, `reap`;
- the calls that genuinely wait are **held replies**: `connect` until the
  connection is up or refused, `accept` until a connection arrives, `recv` until
  data or end-of-stream, `resolve` until DNS answers. `send` answers when the
  data is accepted, and a full send buffer makes it wait for `tcp_sent`;
- **data crosses in the client's shared window**, the way block data does; a
  `recv` answers the length (zero is end-of-stream) and the bytes are already in
  the window;
- unknown methods are refused, so the protocol is versioned like every other
  port.

**The first slice is ping, and it is a raw ICMP socket.** `socket(AF_INET,
SOCK_RAW, IPPROTO_ICMP)` answers an id, `send` puts an echo request on the wire,
`recv` holds until an echo reply arrives, and `close` frees it; `resolve` turns a
name into an address.

**The datagram slice adds UDP.** `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)` is a
socket with no peer, so `sendto` carries the destination address and port and
`recvfrom` answers a datagram with its peer's port as well as its source, length
and bytes. `tftp` is its client and the acceptance: it reads a file from the
TFTP server QEMU's user-mode network serves (the DHCP-supplied gateway, asked of
the stack the way ping asks it), out and back with real bytes on the wire --
the "client socket path end to end" test -- with nothing external reached.

**The stream slice adds TCP, a lifecycle rather than a message.** A socket is a
`tcp_pcb`; `bind` and `listen` ready a listener; `accept` is a held reply that
answers **a new socket per connection**, so one listener serves many; `connect`
is a held reply that answers when the handshake completes or fails; `write`
sends a segment and `recv` (the same call, its answer a stream) returns data
with a zero length as the peer's close. `tcpecho` is its acceptance and needs
no concurrency: a TCP listener's backlog completes a connection whether or not
`accept` has been called, so one process binds 127.0.0.1, connects to it,
accepts, writes a string, reads the echo back and compares -- the whole
lifecycle with nothing on the wire. **The datagram rides the message envelope**
in these slices -- an ICMP message, a TFTP block, an echoed string are all small,
and the envelope's registers hold them -- while a TCP payload can outgrow the
registers. That is the **bulk window** (landed): the client carves a frame of its
own, mints a pristine copy, and hands the copy to the stack on a
`write-window`/`recv-window` call, the capability riding each call. The stack maps
the frame at one reserved address for the moment of the copy and unmaps it,
keeping nothing between calls -- so a client that dies leaves no window state, and
the frames come from the client's own memory, which its own teardown revokes.
`tcpbulk` is its acceptance: a payload past the envelope crosses the window out
and back over the loopback and is compared. (The window is a frame the client
carves, not the starter: only a client that does bulk pays for one.)

A **hosted application gets a musl shim**: `socket`, `connect`, `recv`,
`send`, `getaddrinfo` and friends are rerouted to the port, over an
fd-to-connection table, the way the hosted runtime answers the clock through
`clock.main` (`specs/cxx.md`). That shim is what makes "apps using our TCP/IP
stack" true, and it is its own phase.

Landed: the socket syscalls -- `socket`, `bind`, `listen`, `connect`, `accept`,
`sendto`/`send`, `recvfrom`/`recv`, `shutdown`, `getsockname`/`getpeername`,
`setsockopt`/`getsockopt` and `close` -- in `libs/hosted/aegir-network`, reached
through aegir-heap's dispatcher the way the filesystem calls are. A socket fd is
a row in the shim's own fd table, kept apart from the file fds by range, and a
process the session did not give `net.socket` has no sockets.
`aegir-net-smoke` (the command `netsmoke`) is the acceptance: a hosted program
that reaches the stack only through libc's calls, does a loopback echo, and
says what came back.

**Name lookup is landed too.** musl's own `getaddrinfo` reads `/etc/hosts` and
`/etc/resolv.conf` and asks a nameserver itself -- none of which this machine
has -- so `getaddrinfo`, `gethostbyname` and `freeaddrinfo` are replaced at link
time (`libs/hosted/aegir-network/src/resolver.cc`, this archive before
`musl_full`): Sys:S/hosts first, read through the runtime's own file layer, then
the stack's DNS through the port's `resolve`, matching names by
`aegir-resolver`'s `hosts_line` so a hosted and a freestanding client answer
alike. `netsmoke` proves it -- `localhost`, a name only Sys:S/hosts holds --
before it connects. And the **bulk window** is landed: `write-window` and
`recv-window` carry the bytes in a frame the client owns, its capability riding
the call, and `tcpbulk` proves a payload past the envelope crosses and returns.
The hosted shim using that window for its own large `send`/`recv` is the last
step.

## Memory: no fixed tables

lwIP is configured so its objects come from the **service's region**, sized by
the manifest's `memory_kib`, not from fixed arrays: `MEM_LIBC_MALLOC` with a
`malloc`/`free` over that region, and `MEMP_MEM_MALLOC` so the pbuf and pcb
pools allocate from the heap rather than from compile-time counts. There is no
`MEM_SIZE` holding a system-wide number, which is this project's rule against
hardcoded capacities (`AGENTS.md`). A region that is out of room refuses a
connection; it does not silently cap the machine.

## Configuration and introspection

Status: **shape decided.** An interface starts **down**: nothing brings it up
but a configuration. That is why boot bring-up -- the control port, `NetConfig`,
`Sys:S/network.manifest` and its `Startup-Sequence` line -- lands *with the
stack* (phase 3), and only the `Net:` filesystem view, the live half, is the
last step. The parameter list is the last phase's own; the manifest's TOML
schema lands with `NetConfig` (phase 3).

There are two paths to the same parameters -- boot and live -- and they must
agree, because they are the same state seen at two times rather than two
configurations. Both reach the stack's control port.

### At boot: `Sys:S/network.manifest`

A file on the system volume, the boot manifest's shape -- `key = value`,
`[section]`, `#` comments, a `format` version, and unknown keys are errors rather
than skips. It says what each adapter should be at boot -- DHCP or the static
addresses -- and the machine's `hostname`:

    format = 1
    hostname = aegir

    [NE0]
    dhcp = true

    [NE1]
    ipv4_address = 10.0.2.15
    ipv4_netmask = 255.255.255.0
    ipv4_gateway = 10.0.2.2

The section name is the adapter's **own** name -- `NE0`, the stack's name for
`eth.virtio0` (`describe` answers it, and the live `Net:` volume uses it) -- so
the manifest addresses interfaces without naming a machine. `hostname` is the
machine's name and rides **DHCP option 12**, so it is what the network sees and
not only a label kept here; it must come **before the first section**, because
the netif has to hold the name when `dhcp_start` runs. An adapter with no section
stays **down**. The MTU, `ipv4_dns`, and binding a link by name are the `Net:`
volume's own parameters and land with it; the schema grows with them.

The reader **streams**: a network manifest is read by a freestanding command,
which has no heap, so the file is fed to the parser in chunks and each completed
section is applied as it ends -- there is no table of adapters and so no capacity
to guess.

**It is applied by a command, not by the stack.** `NetConfig` (a command in
`C:`) is run from `Sys:S/Startup-Sequence` (`specs/boot.md`); it reads the
manifest and sets each parameter through the stack's control port -- the same
path a live write to `Net:` takes. Two things fall out of that:

- **there is no ordering wait.** Startup-Sequence *is* on `Sys:`, run by the
  boot session's shell with the boot alias already bound, so the volume is up by
  construction when the command runs. The stack -- the device manager's child,
  starting before the filesystem exists -- never has to wait for the storage
  stack, the VFS or auth.
- **boot config and live config are one act.** The command and a `write` to
  `Net:` reach the same control port, so the stack carries no config-file or TOML
  dependency at all: where boot config comes from is `C:`'s and `S:`'s business,
  not the driver's.

A malformed manifest is announced **loudly**, with the parser's line and reason
-- the loud-failure rule `specs/session.md` sets -- and the command leaves the
interface **down** rather than applying half a file. Down is the safe default:
nothing reaches the network until something asks it to.

**The interface is down until configured, and only a configuration moves it.**
`aegir-net` starts every interface down and does not configure itself; there is
no bring-up default and no transient -- a static setup is set down, configured,
then brought up, and a DHCP setup is brought up with `dhcp` set. This is why the
bring-up half lands with the stack rather than at the end: without it the stack
is unusable, so it is the stack's own interface, not an extra.

### Live: the `Net:` volume

`Net:<adapter>/<parameter>` is a synthetic, read/write filesystem:

- one directory per adapter, and **the adapter names itself the way a block
  device does**: a short prefix and the unit from its link's instance, so
  `eth.virtio0` is `NE0` and the parameter is `Net:NE0/ipv4_address`. Two
  Ethernet interfaces are `NE0` and `NE1` -- the same rule that makes
  `blk.virtio0` `BD0` (`specs/services.md`), and the volume lists one directory
  per bound link. (`NE` is network-ethernet; the prefix is a convention, not a
  mechanism.)
- files per parameter: `ipv4_address`, `ipv4_netmask`, `ipv4_gateway`,
  `ipv4_dns`, `mac`, `mtu`, `link`, `dhcp`, `state`, and `stats` -- the
  read-only ones answer the stack, the writable ones reprogram it.
- **a read answers the current value as text; a write applies it live**, the
  same action the manifest took at boot. This is the `Net:` volume the VFS
  resolves like any other (`specs/vfs.md`), so a client reads and writes it with
  the ordinary file protocol -- no new client API.
- it is served by a small service that registers the volume with
  `vfs.namespace` and calls each stack's control port, so the stack does not
  itself hold the namespace, and **every adapter appears in one volume**. The
  registration table grows on demand and a volume is registered, not compiled
  in, so a second interface is a second directory and nothing else.
- **reads are open; writes are owner/system.** Reprogramming a system interface
  is authority, not convenience -- the `specs/ownership.md` rule extended to a
  synthetic volume. A program lists and reads `Net:` freely; changing an
  address or turning DHCP off is a privileged write.

Landed: the service registers `Net:` with the VFS and serves `list`, `stat` and
`read`, plus the write side (`open`, `write`, `close`, and `reap` for a badge's
teardown) for the parameters the control port can answer today --
`ipv4_address`, `ipv4_netmask`, `ipv4_gateway`, `dhcp`, `link`, `state` and
`hostname`. A write is the ordinary file protocol, and the **close** applies
it: `open`, `write`, `close` is one act seen at two times with the manifest, both
through the same control port. The open handles live in the service's declared
memory grant, a free row one whose serial is zero, the way `fs-bfs`'s and
`fs-fat`'s handle pages are. The remaining parameters wait on the control port:
`mac`, `mtu`, `ipv4_dns` and `stats` are the stack's to expose before the volume
can answer them, and DNS is the one the manifest does not carry yet either.

### The control port

A stack serves a **control port**, separate from the socket port: `list` the
adapters it serves, `get` a parameter, `set` a parameter. Text-valued parameters
(the hostname) go through a `set-text`/`get-text` pair whose bytes ride the
message registers packed low byte first, the way a DNS name does on the socket
port; the word-valued parameters keep the three-word `set`. The `Net:` service is
one client and the boot-manifest reader is another. Keeping it apart from
`aegir/net.h` keeps "move bytes" and "reprogram the interface" as different
protocols, the way the block driver's `caps` is not its `read`
(`specs/services.md`).

The control port is the stack's own interface -- an interface that starts down
is useless without it -- so it lands with the stack (phase 3); the `Net:` volume
in front of it is the last step.

## Names and identity

Three things give the machine a name and let it use one, and none of them lives
in the stack:

- **the host name** is declared at boot in `Sys:S/network.manifest`
  (`hostname`), set through the control port by `NetConfig` like every other
  parameter, and read or set live as `Net:<adapter>/hostname`. The stack puts it
  in DHCP option 12, so it is what the network sees and not only a label the
  machine keeps to itself. Landed: the control port's text parameter, the
  stack's per-link storage, and `netconfig` reading the manifest.
- **`Sys:S/hosts`** is the resolver's first table, and it belongs to the client:
  a classic `address name [aliases]` file, `#` comments, consulted *before* DNS.
  A name it holds resolves with no network at all -- which an offline machine
  and the acceptance need -- and only names it lacks go to the DHCP-supplied DNS
  server. The resolver lives in the client, which has the VFS; the stack never
  reads the filesystem, the same rule that keeps the manifest out of it.
- **`ping`** is the first client of the socket port and takes either an address
  or a name: it resolves the name through `Sys:S/hosts` and then DNS, and sends
  an ICMP echo. It is also the acceptance's last step.

## Acceptance

The runner grows `-device virtio-net-device,netdev=net0 -netdev user,id=net0`
and the test is deliberately **offline and deterministic**:

1. **the link** -- the driver reports id 1, the device's MAC, and link up; a
   small landing, the shape of the block driver's first sector read.
2. **the stack** -- `NetConfig` brings the interface up from
   `Sys:S/network.manifest` (DHCP), and the address, netmask and gateway are
   printed as cues: the numbers are the network's, obtained not embedded. Then
   ARP, then an **ICMP echo to the DHCP-supplied gateway**, which slirp answers.
3. **shapes left to interactive use**, not the gate: DNS forwards to the host
   resolver, and reverse lookup is a slirp limitation.

The client socket path end to end is the TFTP fetch: `tftp` reads a file from
QEMU's built-in TFTP server, fully offline, out and back with real bytes on the
wire. The stream lifecycle is `tcpecho`, a loopback round trip with nothing on
the wire at all. Both are Startup-Sequence lines, run one at a time with the
console idle.

## Phases

0. **Vendor lwIP** -- the pin, the `lwip` CMake target in
   `libs/freestanding/aegir-lwip` that compiles it for the target, and the
   `THIRD-PARTY.md` row. Landed.
1. **`aegir-virtio-net`** and `aegir/ethernet.h`; the registry row; acceptance
   is the handshake, the MAC and link state.
2. **`timer.main`'s periodic subscription**, on `aegir::signal`.
3. **`aegir-lwip`, `aegir-net`, and bring-up** -- `sys_arch`, the tcpip thread,
   the netif, held replies, plus the control port, `NetConfig`,
   `Sys:S/network.manifest` and its `Startup-Sequence` line. An interface starts
   down and the command brings it up; acceptance is DHCP, ARP and the echo
   through that path.
4. **`aegir/net.h` and the socket shim** -- a `ping` client first (raw ICMP),
   then the datagram slice (UDP, and `tftp` as the client-path test), then the
   stream slice (TCP, and `tcpecho` as its lifecycle test), then the client's
   shared window for bulk data, then the musl BSD-socket rerouting. Landed: the
   shim's syscalls and its acceptance (`netsmoke`, a loopback echo through
   libc), the libc name lookup (`getaddrinfo`/`gethostbyname` over Sys:S/hosts
   and the port's `resolve`), and the bulk window (`write-window`/`recv-window`,
   `tcpbulk`). Remaining: the hosted shim sending and receiving through the
   window for payloads past the envelope.
5. **`Net:`, the filesystem view** -- the live half: the volume and its service,
   `Net:<adapter>/<parameter>`, so the running state can be read and set the way
   the boot manifest set it. Landed: the service registers with the VFS and
   serves reads (`list`, `stat`, `read`) and writes (`open`, `write`, `close`,
   `reap`) for the parameters the control port carries; the acceptance reads the
   DHCP address back and writes then reads back the hostname. The parameters the
   control port does not yet answer (`mac`, `mtu`, `ipv4_dns`, `stats`) land when
   it does.

## What this is not

- **Not lwIP in the driver.** The stack is its own service so a board's NIC is
  a new registry row and nothing else (`specs/direction.md`).
- **Not a thread per client.** The held reply is the blocking read
  (`specs/signal.md`); the kernel's non-owning reply cap is what makes N of them
  safe.
- **Not the raw API at the edge.** The raw API is how the service learns a
  connection moved; clients speak BSD sockets through the port.
- **Not a fixed-size stack.** Its memory is the region the manifest grants.
- **Not two configurations.** The boot manifest and the live `Net:` volume set
  the same state at two times; both reach the stack's control port, which is
  the one place the interface is actually changed.
