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
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_TCP                    1
#define LWIP_UDP                    1

/* No lwIP socket/netconn API: the service's clients speak Aegir's socket port,
 * not lwIP's, and the service drives the raw API (specs/net.md). */
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_NETIF_API              0

/* No IP reassembly, no IGMP, no autoip for now: the first acceptance is DHCP,
 * ARP and ICMP echo. */
#define LWIP_IP_FRAG                0
#define LWIP_IP_REASSEMBLY          0
#define LWIP_IGMP                   0
#define LWIP_AUTOIP                 0

/* Memory comes from the service's region, not from fixed arrays (specs/net.md,
 * the project's no-hardcoded-capacity rule): MEM_LIBC_MALLOC routes lwIP's heap
 * and MEMP_MEM_MALLOC routes the pbuf/pcb pools through it, so the port's
 * malloc/free over the manifest region is the only bound. */
#define MEM_LIBC_MALLOC             1
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
