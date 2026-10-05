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
- `sys_mbox` as a pointer ring with a semaphore;
- `sys_arch_protect`/`unprotect` as the critical section;
- `sys_now()` from a cached monotonic reading (`clock.main`/`timer.main`),
  advanced on the tick, never a port call per read;
- `sys_thread_new` starting the tcpip thread.

**The timed mailbox fetch is the lwIP timer.** `tcpip_thread` calls
`sys_arch_mbox_fetch(mbox, msg, timeout)` with the next lwIP timer's timeout so
`sys_check_timeouts()` runs while the link is idle (TCP retransmit, ARP, DHCP).
seL4 has no timed receive, so it waits on **two** notifications -- the mailbox's
and the timer tick -- and returns how long it waited. That is
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

## The client port: BSD sockets

**Clients see a BSD socket, not the raw API.** The port is shaped the way
`socket(2)` is, so an application -- including a hosted one -- uses the familiar
calls and never touches a `tcp_pcb`:

- `socket` (domain, type, protocol) answers an id; `bind`, `listen`, `connect`,
  `accept`, `send`, `recv`, `shutdown`, `close`, `resolve`;
- the calls that genuinely wait are **held replies**: `connect` until the
  connection is up or refused, `accept` until a connection arrives, `recv` until
  data or end-of-stream, `resolve` until DNS answers. `send` answers when the
  data is accepted, and a full send buffer makes it wait for `tcp_sent`;
- **data crosses in the client's shared window**, the way block data does; a
  `recv` answers the length (zero is end-of-stream) and the bytes are already in
  the window;
- unknown methods are refused, so the protocol is versioned like every other
  port.

A **hosted application gets a musl shim**: `socket`, `connect`, `recv`,
`send`, `getaddrinfo` and friends in `src/network/` are rerouted to the port,
over an fd-to-connection table, the way the hosted runtime answers the clock
through `clock.main` (`specs/cxx.md`). That shim is what makes "apps using our
TCP/IP stack" true, and it is its own phase.

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

A TOML file on the system volume, the `Sys:S/session.manifest` shape and
location (`specs/session.md`). It says what each adapter should be at boot:
DHCP or static, the address, netmask, gateway and DNS, the MTU, which link to
bind.

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

### The control port

A stack serves a **control port**, separate from the socket port: `list` the
adapters it serves, `get` a parameter, `set` a parameter. The `Net:` service is
one client and the boot-manifest reader is another. Keeping it apart from
`aegir/net.h` keeps "move bytes" and "reprogram the interface" as different
protocols, the way the block driver's `caps` is not its `read`
(`specs/services.md`).

The control port is the stack's own interface -- an interface that starts down
is useless without it -- so it lands with the stack (phase 3); the `Net:` volume
in front of it is the last step.

## Acceptance

The runner grows `-device virtio-net-device,netdev=net0 -netdev user,id=net0`
and the test is deliberately **offline and deterministic**:

1. **the link** -- the driver reports id 1, the device's MAC, and link up; a
   small landing, the shape of the block driver's first sector read.
2. **the stack** -- `NetConfig` brings the interface up from
   `Sys:S/network.manifest` (DHCP), and the address, netmask and gateway are
   printed as cues: the numbers are the network's, obtained not embedded. Then
   ARP, then an **ICMP echo to the DHCP-supplied gateway**, which slirp answers.
3. DNS is left to interactive use, not the gate: it forwards to the host
   resolver, and reverse lookup is a slirp limitation.

A later test fetches a file from QEMU's built-in TFTP server: still fully
offline, and it exercises the client socket path end to end.

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
4. **`aegir/net.h` and the socket shim** -- a `ping` client first, then the
   musl BSD-socket rerouting, then TCP and DNS.
5. **`Net:`, the filesystem view** -- the live half: the volume and its service,
   `Net:<adapter>/<parameter>`, so the running state can be read and set the way
   the boot manifest set it.

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
