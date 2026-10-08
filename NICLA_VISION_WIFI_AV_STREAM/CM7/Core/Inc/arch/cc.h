/*
 * lwIP architecture definitions: Cortex-M7, GCC, bare metal
 */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stdlib.h>

#define LWIP_NO_STDINT_H        0
#define LWIP_NO_INTTYPES_H      0

/* Little endian; GCC provides the byte-swap builtins */
#ifndef BYTE_ORDER
#define BYTE_ORDER              LITTLE_ENDIAN
#endif
#define lwip_htons(x)           __builtin_bswap16(x)
#define lwip_htonl(x)           __builtin_bswap32(x)

/* No console: diagnostics are dropped, a failed assertion stops in a loop */
#define LWIP_PLATFORM_DIAG(x)   do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { lwip_port_assert(x, __FILE__, __LINE__); } while (0)
void lwip_port_assert(const char *msg, const char *file, int line);

/* Pseudo-random numbers for DHCP transaction IDs, TCP ports and ISNs */
uint32_t lwip_port_rand(void);
#define LWIP_RAND()             lwip_port_rand()

#endif /* LWIP_ARCH_CC_H */
