/****************************************************************************
* zl38063_hbi.h - ZL38063 host bus interface (HBI) over SPI
*
* Copyright Microsemi Inc, 2018. All rights reserved.
* Licensed under the MIT License. See LICENSE.txt in the project
* root for license information.
*
* Port of esp-adf release/v2.x components/audio_hal/driver/zl38063/
* (example_apps/tw_spi_access.c and api_lib/vproc_common.c) flattened into a
* single board-local module, with the SPI bus setup changed to attach to an
* already initialised bus instead of owning it.
*
* Licence: see ZL38063-LICENSE.txt next to this file.
***************************************************************************/

#ifndef ZL38063_HBI_H
#define ZL38063_HBI_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configuration of the ZL38063 HBI port on this board.
 *
 * The HBI shares SPI2 with the ILI9341 LCD (sclk 32 / mosi 33 / miso 34).  The
 * LCD code owns the bus - it calls spi_bus_initialize() - so nothing in this
 * module may call spi_bus_initialize() or spi_bus_free().  We only register a
 * second device on that bus, selected by CS = GPIO0.
 *
 * ZL38063_HBI_HOST is SPI2_HOST; it is defined in the .c because spi_host_device_t
 * lives in the SPI driver headers, which this public header does not pull in.
 *
 * ZL38063_HBI_CS_GPIO is already defined by the board's config.h (GPIO_NUM_0);
 * the fallback below keeps this header usable if config.h is not in scope.
 */
#ifndef ZL38063_HBI_CS_GPIO
#define ZL38063_HBI_CS_GPIO   0     /* GPIO0, matches config.h on the B220 */
#endif
#define ZL38063_HBI_CLOCK_HZ  10000000
#define ZL38063_HBI_MODE      0
#define ZL38063_HBI_QUEUE_SZ  6

/*
 * HBI command word encoding, taken verbatim from api_lib/vprocTwolf_access.c
 * (HBI_PAGED_READ / HBI_DIRECT_READ / ... macros).  The 16-bit command is
 *
 *   bit 15    : 0 = paged access, 1 = direct (page 0) access
 *   bit 14..8 : 7-bit register offset, in WORDS (byte offset / 2)
 *   bit 7     : 0 = read, 1 = write
 *   bit 6..0  : number of words minus one
 *
 * and the addressee page is chosen by sending HBI_SELECT_PAGE(page) first.
 */
#define ZL38063_HBI_PAGED_READ(offset, length) \
    ((uint16_t)(((uint16_t)(offset) << 8) | (length)))
#define ZL38063_HBI_DIRECT_READ(offset, length) \
    ((uint16_t)(0x8000 | ((uint16_t)(offset) << 8) | (length)))
#define ZL38063_HBI_PAGED_WRITE(offset, length) \
    ((uint16_t)(ZL38063_HBI_PAGED_READ(offset, length) | 0x0080))
#define ZL38063_HBI_DIRECT_WRITE(offset, length) \
    ((uint16_t)(ZL38063_HBI_DIRECT_READ(offset, length) | 0x0080))
#define ZL38063_HBI_SELECT_PAGE(page) \
    ((uint16_t)(0xFE00 | (page)))
#define ZL38063_HBI_CONFIGURE(pinConfig) \
    ((uint16_t)(0xFD00 | (pinConfig)))
#define ZL38063_HBI_NO_OP \
    ((uint16_t)0xFFFF)

/*
 * Page-255 window addressing: THE single place this port maps a DSP RAM target
 * address onto the page-255 window command word.
 *
 * The host-upload write path reaches a target address in two steps (see
 * HbiSrecBoot_alt() in zl38063_vproc.c):
 *
 *   1. the FULL 32-bit DSP address goes to HBI register 0x000C as two words,
 *      { hi16, lo16 };
 *   2. the payload is written through the window with the command word
 *      0xFF00 | lowbyte(target), i.e. VprocTwolfHbiWrite(0xFF00 | lowbyte).
 *
 * Step 2 is the trap.  zl38063_hbi_write() -> hbi_send_cmd() decodes ANY 16-bit
 * HBI address as
 *
 *     page   = addr >> 8             -> 0xFF, and SELECT_PAGE(0xFF) = 0xFEFF is
 *                                       sent first
 *     offset = (addr & 0x00FF) / 2   -> the 7-bit field in command bits 14..8
 *
 * so the number that actually reaches the device is lowbyte(target) / 2: the
 * target's WORD offset inside the window, not its byte offset.  A window READ
 * has to put the same number in bits 14..8, which is what the helper below
 * returns.  This mask/scale arithmetic lives here and nowhere else; the loader
 * (zl38063_vproc.c), the window read (VprocTwolfHbiPage255Read()) and the
 * empirical calibration (zl38063_dsp_page255_calibrate()) all call it.
 */

/* What goes into the window command word's offset field (bits 14..8). */
#define ZL38063_PAGE255_OFF_BYTE   0u  /* target & 0xFF        (byte offset) */
#define ZL38063_PAGE255_OFF_WORD   1u  /* (target & 0xFF) / 2  (word offset) */
/* ...which is exactly what the upload's own 0xFF00 | lowbyte write emits on the
 * wire, so this is the encoding a mirroring window read must use. */
#define ZL38063_PAGE255_OFF_UPLOAD ZL38063_PAGE255_OFF_WORD

/*
 * @param target_addr DSP RAM address as it appears in the image records (e.g.
 *                    0x00080200), NOT an HBI register address.
 * @param encoding    ZL38063_PAGE255_OFF_BYTE or ZL38063_PAGE255_OFF_WORD.
 *
 * @return the 7-bit value for the window command word's offset field.
 */
uint8_t zl38063_hbi_page255_window_offset(uint32_t target_addr,
                                         unsigned int encoding);

/*
 * Register a ZL38063 HBI device on the already initialised SPI2 bus.
 *
 * Safe to call twice: the second call returns ESP_OK without registering a
 * second device.  GPIO0 is left floating (high impedance) exactly like the
 * upstream driver does, so that the ESP32 boot strapping pin is not driven
 * when the ZL38063 is not being addressed.
 *
 * @return ESP_OK on success, otherwise the esp_err_t from the SPI driver.
 */
esp_err_t zl38063_hbi_init(void);

/*
 * Remove the device from the bus.  Does NOT free the bus (the LCD owns it).
 */
void zl38063_hbi_deinit(void);

/*
 * Re-point the HBI device at a different chip-select / SPI mode / clock.
 *
 * Tears the current device down with spi_bus_remove_device() and adds a fresh
 * one with @cs_gpio, @mode_hz and @clock_hz.  It NEVER calls spi_bus_free() or
 * spi_bus_initialize(): SPI2 belongs to the LCD, and the ESP32 SPI master allows
 * at most three devices per bus, so the old device is always removed before the
 * new one is added.
 *
 * Everything downstream of the transport is unchanged: the 16-bit command
 * framing and the runtime byte-order swap (zl38063_hbi_set_swap()) are exactly
 * the ones the default device uses, and the CS pin is left floating between
 * transactions like zl38063_hbi_init() does for GPIO0.
 *
 * @param cs_gpio  chip-select GPIO, or -1 for "no CS" (not useful here)
 * @param mode_hz  SPI mode 0..3
 * @param clock_hz SCLK frequency in Hz
 *
 * @return ESP_OK on success (the new device is live even if the DSP does not
 *         answer), otherwise the esp_err_t from spi_bus_add_device() - the HBI
 *         device is then detached and every transfer fails with -1.
 */
esp_err_t zl38063_hbi_rebind(int cs_gpio, int mode_hz, int clock_hz);

/*
 * Write @numwords 16-bit words to the ZL38063 register block selected by
 * @cmd.  @cmd is a plain 16-bit HBI address (0x0200, 0x0300, ...); this
 * function decomposes it into page and word offset and emits the page select
 * and command words itself, byte-for-byte as the reference driver does.
 *
 * Upstream called these three steps spis_tw_hbi_wr16_cmd() + one
 * spis_tw_hbi_wr16_data() per word.
 *
 * @param cmd      HBI register address, e.g. 0x0300
 * @param numwords 1..126 (upstream limit enforced by the VPROC layer)
 * @param data     source words, host byte order
 *
 * @return 0 on success, -1 on a transport error.
 */
int zl38063_hbi_write(uint16_t cmd, uint8_t numwords, const uint16_t *data);

/*
 * Read @numwords 16-bit words starting at HBI register @cmd.  Reverse of
 * zl38063_hbi_write(); upstream called this spis_tw_hbi_rd16_cmd() followed
 * by one spis_tw_hbi_rd16() per word.
 *
 * @return 0 on success, -1 on a transport error.
 */
int zl38063_hbi_read(uint16_t cmd, uint8_t numwords, uint16_t *data);

/*
 * Low-level primitives, needed by the VPROC port in zl38063_vproc.c to
 * implement the VprocHALWrite()/VprocHALRead() platform hooks.  Prefer the
 * two word-oriented functions above for anything else.
 *
 * Both send/receive exactly one 16-bit word in its own SPI transaction
 * (i.e. their own CS assertion) and perform the ZL38063 big-endian byte
 * order conversion for you.
 */
int zl38063_hbi_wr16_cmd(uint16_t cmd);
int zl38063_hbi_wr16_data(uint16_t data);
int zl38063_hbi_rd16(uint16_t *data);

/*
 * Runtime control of the 16-bit word byte order on the HBI wire.
 *
 * The ZL38063 HBI port is normally big-endian (the high byte of each word goes
 * out first), which on a little-endian host means every word must be swapped
 * before transmission and after reception.  That is the default and matches
 * what upstream (spi_common.c with htons()/ntohs()) did; the compile-time
 * ZL38063_HBI_BIG_ENDIAN macro in zl38063_hbi.c only sets the power-on default.
 *
 * These two functions exist so a bring-up probe can flip the order at run time
 * and compare the values read both ways (see zl38063_dsp_hbi_diag()): a real
 * register map read with the wrong byte order comes back as visibly swapped
 * words (0xD3D3 vs 0xD3D3-free garbage, 0x0100 vs 0x0001, ...), while a dead
 * MISO line gives 0x0000/0xFFFF in BOTH orders.
 *
 * The swap is applied to every subsequent 16-bit transfer, including inside
 * zl38063_hbi_write()/zl38063_hbi_read(); those keep their semantics unchanged.
 *
 * @param enable true  = swap (default, big-endian on the wire)
 *               false = do not swap (raw little-endian wire order)
 */
void zl38063_hbi_set_swap(bool enable);

/* @return the current runtime swap setting (true = swapping, the default). */
bool zl38063_hbi_get_swap(void);

#ifdef __cplusplus
}
#endif

#endif /* ZL38063_HBI_H */
