/*
 * b220_baseboard_probe.h - hunt for the unidentified UART link to the B220
 * baseboard MCU.
 *
 * The 小魔豆 B220 is an ESP32 mainboard plus a motorised baseboard with its own
 * MCU.  The stock firmware logs a continuous stream of ASCII "{AT+Pon}"-style
 * frames (and the baseboard answers in the same dialect), i.e. there IS a serial
 * link between the two boards - but our firmware never touches it, so we do not
 * know which ESP32 GPIOs carry it.  The ZL38063 DSP on our firmware is powered
 * but never answers HBI and never drives I2S, and the current hypothesis is that
 * the stock firmware's baseboard handshake is part of what brings the audio
 * section up.  Finding the link is therefore the prerequisite for trying it.
 *
 * The probe is a bounded, read-only listener: it never transmits, never writes a
 * device register, and restores every pin it touches before it returns.
 *
 * Compiled only when B220_ZL_DIAG is 1 (config.h).
 */

#ifndef B220_BASEBOARD_PROBE_H
#define B220_BASEBOARD_PROBE_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if B220_ZL_DIAG

/**
 * @brief Sweep the candidate GPIOs as UART RX and report which ones carry data.
 *
 * For every candidate pin it does, in this order:
 *   1. prefilter - configure the pin as a pulled-up input and look for voltage
 *      transitions with a polling edge detector (~25 ms).  The stock link talks
 *      continuously, so a dead pin is very unlikely to show edges and the
 *      prefilter is what keeps the whole probe inside the time budget;
 *   2. UART listen - configure the pin as RX of UART2 (tx/rts/cts untouched,
 *      the driver's default TX is released first) and listen at each baud rate.
 *      Pins with edge activity get the full list ({115200, 9600, 57600, 38400,
 *      4800}); pins with no activity get a short retry of 115200 + 9600 so the
 *      prefilter cannot hide the link.  Each listen is a bounded window
 *      (~120 ms); nothing here can block forever;
 *   3. log one line per (pin, baud) that received anything at all, with the
 *      first 48 bytes in hex and, when they are all printable, the text form;
 *   4. restore the pin, and at the very end log one summary line listing the
 *      pins that produced data.
 *
 * Tags/format (ESP_LOGW under tag "B220BB"):
 *   bb uart start: N candidate pins, UART2, <=4000 ms budget
 *   bb edge pin=2  transitions=0
 *   bb uart pin=38 baud=115200 rx=27 bytes "7B 41 54 2B ..." text="{AT+Pon}"
 *   bb uart pin=38 baud=9600   rx=0 (silent)
 *   bb uart done: UART2 driver uninstalled, N pins restored
 *   bb uart summary: candidates=... detected-data=[...] edge-activity=[...]
 *
 * Pins that are NOT candidates because this board already uses them: 0 (ZL38063
 * HBI CS), 1/3 (console UART), 5/25/26/35 (I2S to the DSP), 18/23 (AW9106B
 * I2C), 21/22/27/32/33/34 (LCD + its DC/RST/CS and the shared SPI2 bus), 16/17
 * (PSRAM) and 6-11 (flash).  GPIO19 (the DSP reset) IS probed, because it is a
 * plausible spare pad, but it is put back to output-HIGH at once - that is how
 * our own driver left it (DSP out of reset).  GPIO38/39 are deliberately absent:
 * on the ESP32 SOC_GPIO_VALID_GPIO_MASK clears GPIO24 and GPIO28-31 (not bonded
 * out) while SOC_GPIO_VALID_OUTPUT_GPIO_MASK additionally clears GPIO34-39, so
 * 38/39 are not output-capable pads on this chip however the driver is called
 * (SOC_GPIO_OUT_RANGE_MAX is 33); the earlier RE hit gpio_set_intr_type(38, ...)
 * in the stock image, which the probe reports as the out-of-range artefact it is.
 *
 * @note Call it from the board constructor, after the ZL38063 diagnostics.
 */
void b220_baseboard_uart_sweep(void);

#endif /* B220_ZL_DIAG */

#ifdef __cplusplus
}
#endif

#endif /* B220_BASEBOARD_PROBE_H */
