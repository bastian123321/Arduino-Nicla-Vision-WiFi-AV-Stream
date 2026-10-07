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
#define MEMP_NUM_UDP_PCB                5             /* DHCP + mDNS + spare */
/* lwIP's own timers plus mDNS probing/announcing */
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)

/* ---- Protocols ------------------------------------------------------------ */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1             /* answers ping */
#define LWIP_RAW                        0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_IGMP                       1             /* multicast, needed by mDNS */
#define LWIP_DNS                        0

/* ---- mDNS: answers "nicla-vision.local" ------------------------------------- */
#define LWIP_MDNS_RESPONDER             1
#define LWIP_NUM_NETIF_CLIENT_DATA      1             /* mDNS state per netif */
#define LWIP_NETIF_EXT_STATUS_CALLBACK  1             /* re-announce when the IP changes */
#define MDNS_MAX_SERVICES               1             /* advertises the web page (_http._tcp) */

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
