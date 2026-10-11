/*
 * b220_tcp_log.h - the debug log channel that works when the serial port cannot.
 *
 * WHY THIS EXISTS
 * ---------------
 * The baseboard MCU hangs off UART0 - the same pins as the console and the flashing
 * port.  Once the baseboard is plugged in it physically covers that connector, so the
 * serial log is no longer reachable exactly when debugging the baseboard matters
 * most, and the firmware cannot read baseboard replies while the console owns the
 * port either.
 *
 * So the log goes out over the network as well: install a vprintf hook, keep the
 * recent log in a ring buffer, and let a PC connect over TCP and read it - including
 * the boot log it missed, because the buffer is replayed to every new client.
 *
 * USE
 * ---
 *   nc <device-ip> 3333          (or: telnet <device-ip> 3333, PuTTY raw, ...)
 *
 * The serial log keeps working exactly as before; this only adds a second consumer.
 * The hook is installed before any of the board's own logging happens, so the TCP
 * stream contains the boot bring-up from the board constructor onwards.
 *
 * FALLBACK
 * --------
 * If the network never comes up, the log would be invisible.  b220_tcp_log_dump()
 * prints the recent log to the SERIAL port on demand, and the board calls it from a
 * key press, so there is always a way out that does not depend on WiFi.
 */

#ifndef B220_TCP_LOG_H
#define B220_TCP_LOG_H

#include <stdbool.h>
#include <stddef.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the log hook, start the ring buffer and the TCP listener.  Safe to call
 * more than once; the second call is a no-op.  Called automatically by a static
 * initialiser, so normally nothing needs to call it explicitly. */
esp_err_t b220_tcp_log_start(void);

/* Print the recent log to the serial port (the fallback above). */
void b220_tcp_log_dump(void);

/* How many clients are currently attached (0 when nobody is listening). */
int b220_tcp_log_clients(void);

#ifdef __cplusplus
}
#endif

#endif /* B220_TCP_LOG_H */
