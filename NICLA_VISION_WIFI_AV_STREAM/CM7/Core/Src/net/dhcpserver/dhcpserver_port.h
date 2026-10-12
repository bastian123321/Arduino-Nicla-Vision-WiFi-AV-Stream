/*
 * Port shim for MicroPython's shared/netutils/dhcpserver.c (MIT, see its
 * header): the only MicroPython pieces it needs.
 * Source: https://github.com/micropython/micropython/tree/master/shared/netutils
 */
#ifndef DHCPSERVER_PORT_H
#define DHCPSERVER_PORT_H

#include "main.h"

#define MP_ENOMEM           (12)
#define mp_hal_ticks_ms()   HAL_GetTick()

/* No console: drop the "client connected" message */
#define printf(...)         ((void)0)

#endif /* DHCPSERVER_PORT_H */
