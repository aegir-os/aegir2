# The network stack

Status: decided (2026-10). This is the "TCP/IP and a socket layer" of
`specs/direction.md`, and it takes the fourth item there -- composition chosen
at runtime -- one step, because a stack is loaded per interface.

## What we are building

Three pieces and two protocols:

    aegir-virtio-net  --aegir/ethernet.h-->  aegir-net  --aegir/net.h-->  clients
      owns virtio id 1    raw Ethernet frames  runs lwIP   BSD-shaped sockets

- **`aegir-virtio-net`** owns the network device and serves raw Ethernet
  frames. It is a driver like any other: a registry row, spawned by the
  device manager (`specs/services.md`).
- **`aegir-net`** runs lwIP over one link, and serves a socket port. It knows
  nothing about virtio, so on a board only the link driver changes.
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
  message registers carry 952, so frames cross through a **shared window** --
  the same discipline as `aegir/block.h`, one window per client mapped by the
  spawner, its physical base carried so nothing else has to guess.
- a `mac`/`mtu`/`link` method answers what the stack needs to configure its
  netif; a `send` transmits the frame in the caller's window; a `receive` is a
  **held reply** that lands a frame in the window and answers its length
  (`specs/services.md`'s held-reply shape, `specs/signal.md`).
- the driver's interrupt is bound and acked the way every driver's is
  (`specs/services.md`); a driver without a pair polls.
- registry row: `compatible=virtio,mmio id=1 prefix=eth bus=virtio
  binary=aegir-virtio-net memory=<n> window=<the frame window>`.

## Vendoring lwIP

**lwIP 2.2.1**, BSD-3-Clause, pinned by commit SHA (the
`STABLE_2_2_1_RELEASE` tag on Savannah's git) in `manifests/aegir.xml`, and
extracted to **`third_party/lwip`**.

That path is deliberate: lwIP 2.2.1 ships a root `CMakeLists.txt`, and the seL4
build system does

    file(GLOB result RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" projects/*/CMakeLists.txt)
    foreach(file ${result}) add_subdirectory("${file}") endforeach()

(`tools/seL4/cmake-tool/projects.cmake`) -- so a checkout under `projects/`
would be swallowed into the kernel build. `third_party/` is outside that glob
and already gitignored. The source tree is read-only, as every vendored tree is
(`specs/third_party.md`); any change of ours is a patch under
`third_party/patches/third_party/lwip/`.

`scripts/build_lwip.sh [TARGET]` builds it out of tree and static, the way
`scripts/build_zlib.sh` builds zlib, linking only the modules we enable and
against the freestanding musllibc.

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

## Acceptance

The runner grows `-device virtio-net-device,netdev=net0 -netdev user,id=net0`
and the test is deliberately **offline and deterministic**:

1. **the link** -- the driver reports id 1, the device's MAC, and link up; a
   small landing, the shape of the block driver's first sector read.
2. **the stack** -- DHCP completes, and the address, netmask and gateway are
   printed as cues: the numbers are the network's, obtained not embedded. Then
   ARP, then an **ICMP echo to the DHCP-supplied gateway**, which slirp answers.
3. DNS is left to interactive use, not the gate: it forwards to the host
   resolver, and reverse lookup is a slirp limitation.

A later test fetches a file from QEMU's built-in TFTP server: still fully
offline, and it exercises the client socket path end to end.

## Phases

0. **Vendor lwIP** -- the pin, `scripts/build_lwip.sh`, the `THIRD-PARTY.md`
   row.
1. **`aegir-virtio-net`** and `aegir/ethernet.h`; the registry row; acceptance
   is the handshake, the MAC and link state.
2. **`timer.main`'s periodic subscription**, on `aegir::signal`.
3. **`aegir-lwip` and `aegir-net`** -- `sys_arch`, the tcpip thread, the netif,
   DHCP/ARP/ICMP, held replies; acceptance is the address and the echo.
4. **`aegir/net.h` and the socket shim** -- a `ping` client first, then the
   musl BSD-socket rerouting, then TCP and DNS.

## What this is not

- **Not lwIP in the driver.** The stack is its own service so a board's NIC is
  a new registry row and nothing else (`specs/direction.md`).
- **Not a thread per client.** The held reply is the blocking read
  (`specs/signal.md`); the kernel's non-owning reply cap is what makes N of them
  safe.
- **Not the raw API at the edge.** The raw API is how the service learns a
  connection moved; clients speak BSD sockets through the port.
- **Not a fixed-size stack.** Its memory is the region the manifest grants.
