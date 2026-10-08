/*
 * cyw43-driver port: functions called from the application side (wifi.c)
 */
#ifndef CYW43_PORT_H
#define CYW43_PORT_H

#include <stddef.h>

/* Start the microsecond time base. Call before cyw43_init(). */
void cyw43_port_init(void);

/* Run pending driver work. Call often from the main loop. */
void cyw43_port_poll(void);

/* Call from HAL_GPIO_EXTI_Callback() for WL_HOST_WAKE_Pin */
void cyw43_port_host_wake_irq(void);

/* Pop one buffered driver log line into `line`; returns 0 when none is ready */
size_t cyw43_port_take_log_line(char *line, size_t max);

#endif /* CYW43_PORT_H */
