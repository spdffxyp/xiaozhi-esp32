/*
 * b220_base_uart.h - talking to the baseboard (motors, head/tail lights).
 *
 * THE BUS
 * -------
 * The baseboard MCU hangs off the ESP32's UART0 at 115200 8N1 - the SAME pins
 * the console/log output and the flashing port use (TX GPIO1, RX GPIO3).  This
 * is confirmed twice over:
 *   - the porting guide reached the same conclusion from the stock firmware
 *     (the `{AT+...}` strings are the ESP32 talking to the baseboard), and
 *   - the independent port of this toy, ref/Flower_modou, drives exactly this
 *     (components/modou_drivers/modou_base_uart.c: UART_NUM_0, TX GPIO1,
 *     RX GPIO3, 115200).
 *
 * GPIO16/17 are PSRAM on this D0WD + PSRAM board and must never be used as an
 * alternative baseboard UART; neither may GPIO1/3 be moved.
 *
 * WHAT THAT SHARING MEANS (read this before debugging "no reply")
 * ---------------------------------------------------------------
 * The ESP32's own log output and the baseboard frames share one wire.  Both
 * directions are affected:
 *   - frames this firmware sends are also seen by the USB-TTL converter (they
 *     show up in the serial log as `{AT+...}`);
 *   - whatever this firmware logs is seen by the baseboard, which ignores what
 *     it does not recognise - that is how the stock firmware gets away with it;
 *   - the baseboard's replies land in the console input, and the ESP-IDF console
 *     may print "unknown command" style noise for them.
 * The baseboard is known to accept a frame of the form
 *
 *     {AT+...}\r\n
 *
 * (braces + CRLF).  This module only ever WRITES by default; reading is opt-in
 * because it takes bytes away from the console.
 *
 * COMMANDS THAT ARE KNOWN TO WORK (from ref/Flower_modou, which validated them
 * on hardware; the motion/light tuple semantics are documented in that project's
 * docs/MOTOR_REMOTE_PROTOCOL_2026-07-30.md)
 * ----------------------------------------------------------------------------
 *   AT+Ver                      version query (expects a reply)
 *   AT+Pon                      wake/handshake the baseboard; unlocks the motors
 *   AT+Onbase / AT+Run          presence / run state
 *   AT+CMD=(...),(...),...      parameterised motion + light frame
 *
 * A motion frame looks like
 *   AT+CMD=(9,1,0,1),(mode,ms,steer,power,0),(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)
 * with steer 0..180 (90 = straight), power 1..100, and ms in units of 200 ms for
 * the scripted form.  The head/tail light fields are the last four tuples.
 */

#ifndef B220_BASE_UART_H
#define B220_BASE_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define B220_BASE_UART_BAUD     115200
#define B220_BASE_UART_TX_GPIO  1
#define B220_BASE_UART_RX_GPIO  3

/*
 * Initialise the baseboard link.  UART0's pins and parameters are already the
 * right ones (that is the console), so this only installs the driver and flushes
 * the input.  Safe to call more than once.
 *
 * @param consume_console_input  true = install an RX buffer so replies can be
 *        read back (this takes bytes away from the console's own input); false =
 *        transmit-only, which leaves the console untouched.
 */
esp_err_t b220_base_uart_init(bool consume_console_input);

/*
 * Send one baseboard frame.  `cmd` is the bare command text WITHOUT braces, e.g.
 * "AT+Pon"; the braces and CRLF are added here.  Non-blocking apart from waiting
 * for the transmit FIFO to drain (bounded by tx_timeout_ms).
 */
esp_err_t b220_base_uart_send(const char* cmd, uint32_t tx_timeout_ms);

/*
 * Send a frame and collect the reply.  Only usable when the link was opened with
 * consume_console_input = true.  Reads until a LF, rx_size-1 bytes, or the
 * timeout; `rx` is always NUL-terminated and is left empty on timeout.
 */
esp_err_t b220_base_uart_send_and_receive(const char* cmd, char* rx, size_t rx_size,
                                          uint32_t timeout_ms);

/* Collect whatever is already pending without sending anything. */
esp_err_t b220_base_uart_read_pending(char* rx, size_t rx_size, uint32_t timeout_ms);

/*
 * Boot handshake: after a short delay (so the ROM banner and the first log lines
 * are out of the way) send "AT+Pon" once - that is what unlocks the baseboard's
 * motors.  Spawns a one-shot task; returns immediately.  A no-op when
 * B220_BASE_UART_HANDSHAKE is 0 in config.h.
 */
esp_err_t b220_base_uart_start_handshake(void);

/*
 * Ask the baseboard to run one motion frame.  steer and power are clamped to the
 * documented ranges; duration_ms is rounded UP to the scripted 200 ms unit.
 * `power` 0 sends the two stop frames instead.
 */
esp_err_t b220_base_motion(uint8_t mode, uint16_t steer, uint8_t power,
                           uint32_t duration_ms);

/* The two frames that stop both wheels (validated in ref/Flower_modou). */
esp_err_t b220_base_stop(void);

/* Head / tail light convenience frames (the last four tuples of the CMD form). */
esp_err_t b220_base_lights(uint8_t front_white, uint8_t rear_red);

/* Last frame this module sent / last reply it captured, for the MCP status blob. */
const char* b220_base_uart_last_tx(void);
const char* b220_base_uart_last_rx(void);

#ifdef __cplusplus
}
#endif

#endif /* B220_BASE_UART_H */
