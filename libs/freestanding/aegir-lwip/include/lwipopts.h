/*
 * lwIP's configuration for Aegir's network service (specs/net.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * lwIP is vendored (third_party/lwip, BSD-3-Clause) and compiled here for the
 * freestanding target; this file is the *port's* configuration, and the two
 * arch headers beside it (arch/cc.h, arch/sys_arch.h) are the rest of it.
 *
 * NO_SYS=0: lwIP runs its own tcpip thread (aegir-thread) and the service talks
 * to the protocol code through it. The service itself serves its own
 * BSD-shaped socket protocol over the raw API, so lwIP's netconn and socket
 * layers are off -- nothing here needs them, and they would drag the host's
 * <sys/socket.h> into a freestanding build.
 */

#ifndef AEGIR_LWIP_LWIPOPTS_H
#define AEGIR_LWIP_LWIPOPTS_H

/* Threaded lwIP: a tcpip thread, and the sys_arch the port provides
 * (arch/sys_arch.h declares the handles; the implementation is the port's). */
#define NO_SYS                      0
#define SYS_LIGHTWEIGHT_PROT        1

/* IPv4 first. IPv6 is a later, deliberate step: a board's network may want it,
 * and it is a config change here plus the addresses the service exposes
 * (specs/net.md). */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0

/* Protocols. DHCP is how the interface learns its address, netmask, gateway
 * and DNS server -- nothing about 10.0.2.x is compiled in (specs/net.md). */
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
/* Raw IP: the socket layer's first slice is a raw ICMP socket, which is what
 * ping is (specs/net.md). */
#define LWIP_RAW                    1
#define LWIP_DHCP                   1
/* No address-conflict probing after the DHCP ACK (lwIP 2.2's ACD): it costs
 * seconds of boot delay for a lease a DHCP server already made unique, and this
 * network's server is trusted (QEMU's slirp). A deployment that shares a
 * segment with hand-configured hosts would turn it back on. */
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_DNS                    1
#define LWIP_TCP                    1
#define LWIP_UDP                    1

/* No lwIP socket/netconn API: the service's clients speak Aegir's socket port,
 * not lwIP's, and the service drives the raw API (specs/net.md). */
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_NETIF_API              0

/* The loopback interface, always: a host has one before any NIC is bound, and
 * the stack must be useful with nothing but it -- 127.0.0.1 is up from the
 * first moment (lwIP's netif_init adds it when LWIP_HAVE_LOOPIF is set). The
 * two options are a pair: loopback needs the loopback path, and the default
 * LWIP_HAVE_LOOPIF is derived from it. */
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_HAVE_LOOPIF            1

/* No IP reassembly, no IGMP, no autoip for now: the first acceptance is DHCP,
 * ARP and ICMP echo. */
#define LWIP_IP_FRAG                0
#define LWIP_IP_REASSEMBLY          0
#define LWIP_IGMP                   0
#define LWIP_AUTOIP                 0

/* Memory comes from the service's region, not from a fixed heap (specs/net.md):
 * MEM_CUSTOM_ALLOCATOR routes lwIP's mem_malloc/mem_free/mem_calloc to the
 * port's allocator (mem.cc) over the region the service holds, and
 * MEMP_MEM_MALLOC routes the pbuf/pcb pools through it too -- so there is no
 * MEM_SIZE anywhere, and the heap is as big as what the service was given. */
#define MEM_LIBC_MALLOC             0
#define MEM_CUSTOM_ALLOCATOR        1
#define MEM_CUSTOM_MALLOC           aegir_lwip_malloc
#define MEM_CUSTOM_FREE             aegir_lwip_free
#define MEM_CUSTOM_CALLOC           aegir_lwip_calloc
#define MEMP_MEM_MALLOC             1
#define MEM_ALIGNMENT               4

/* The TCP window: the interface's MTU less the IPv4 and TCP headers. The
 * window and send buffer are multiples of it; a larger window is a memory
 * decision the region sizes, not a constant here. */
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)

/* lwIP prints through the port's diag hook, which the port wires to Aegir's
 * line writer; assertions land there too and stop the thread. Debug is off in
 * the shipped build. */
#define LWIP_DEBUG                  0
#define LWIP_STATS                  0

#endif /* AEGIR_LWIP_LWIPOPTS_H */
