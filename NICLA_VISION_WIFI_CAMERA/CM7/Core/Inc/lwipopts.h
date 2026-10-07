/*
 * lwIP configuration: bare metal (NO_SYS), IPv4 over the CYW4343W WiFi netif
 *
 * Everything that touches lwIP runs from the main loop, never from an
 * interrupt, so no locking (SYS_LIGHTWEIGHT_PROT) is needed.
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ---- System ------------------------------------------------------------- */
#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_ALTCP                      0
#define LWIP_STATS                      0
#define LWIP_TIMERS                     1

/* ---- Memory --------------------------------------------------------------- */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (48 * 1024)   /* heap for TX segments */
#define MEMP_NUM_PBUF                   32
#define PBUF_POOL_SIZE                  24            /* RX buffers */
#define MEMP_NUM_TCP_PCB                6
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_UDP_PCB                4

/* ---- Protocols ------------------------------------------------------------ */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1             /* answers ping */
#define LWIP_RAW                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_IGMP                       0
#define LWIP_DNS                        0

/* ---- DHCP client ------------------------------------------------------------ */
#define LWIP_DHCP                       1
#define LWIP_DHCP_CHECK_LINK_UP         1
#define DHCP_DOES_ARP_CHECK             0             /* faster address assignment */

/* ---- TCP: tuned for streaming a few KB per frame out -------------------------- */
#define TCP_MSS                         1460
#define TCP_SND_BUF                     (12 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define MEMP_NUM_TCP_SEG                TCP_SND_QUEUELEN
#define TCP_WND                         (4 * TCP_MSS)
#define LWIP_TCP_KEEPALIVE              1
#define TCP_LISTEN_BACKLOG              1
#define SO_REUSE                        1

/* ---- Netif -------------------------------------------------------------------- */
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_CHKSUM_ALGORITHM           3

#endif /* LWIPOPTS_H */
