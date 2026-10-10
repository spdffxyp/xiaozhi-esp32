/*
 * b220_button_discovery.h - find out where the B220's three buttons are wired.
 *
 * WHY THIS EXISTS
 * ---------------
 * The toy has three physical buttons - a record/talk key, volume-up and
 * volume-down - but nothing in the stock firmware image, in the board's
 * schematics-as-reversed (docs/GPIO-FINDINGS.md) or in the xiaozhi port so far
 * says which chip samples them:
 *
 *   * the stock firmware's own button code (buttonTask / gpiopress /
 *     DEVICE_NOTIFY_KEY_REC|VOL_UP|VOL_DOWN) reads its pins from a runtime
 *     configuration, so the pin numbers are not immediates in the image;
 *   * the stock firmware's 1042-entry ZL38063 configuration record leaves the
 *     whole DSP GPIO register block (0x0700..0x0A22) at ZERO, i.e. the stock
 *     never configures the audio DSP's GPIOs either;
 *   * the ESP32 side has very few pins left: of the ones this port already
 *     owns (LCD, I2S, HBI, AW9106B I2C, ZL38063 reset) only GPIO 20, 36 and 39
 *     are realistically free, and 36/39 cannot be outputs.
 *
 * So instead of guessing, this module turns the device into a probe: it samples
 * EVERY input source the board could plausibly use, and logs a line the moment
 * one of them changes.  Press one button at a time and the log names the source.
 *
 * WHAT IT SAMPLES
 * ---------------
 *   1. the free ESP32 GPIOs (20, 36, 37, 38, 39 - plus 0 only if asked for),
 *      each read twice: with the internal pull-up enabled and with every pull
 *      disabled;
 *   2. the AW9106B GPIO/LED driver on I2C 0x5B: its two input-data registers
 *      (0x00, 0x01) and, for context, the group-A/group-B mode and output
 *      registers;
 *   3. the ZL38063 audio DSP over HBI: the whole HBI register address space of
 *      pages 0..B220_BUTTON_DSP_PAGES-1, i.e. pages 0..3 (the documented range,
 *      which contains every known system/mailbox register AND is re-read every
 *      B220_BUTTON_FAST_PERIOD_MS) in the fast sweep, and up to page 0x3F in
 *      the deep sweep;
 *   4. the SC7A20 accelerometer on I2C 0x19, whose CLICK/INT1 engine could also
 *      have been wired to the keys: its status/interrupt registers.
 *
 * WHAT IS ALREADY KNOWN ABOUT THE DSP PATH (checked against public sources, see
 * ZL38063-GPIO-HOST-ACCESS.md)
 * -------------------------------------------------------------------------
 *   * the ZL38063 has 11..14 GPIOs; after power-on or reset they are INPUTS with
 *     an internal PULL-DOWN (datasheet Table 15 and §5.5 / §8.2) - so a key that
 *     shorts a DSP GPIO to GND reads 0 both pressed and released and is
 *     therefore INVISIBLE.  A DSP-wired key has to switch the pin to 3V3.  This
 *     is the first thing to check with a multimeter;
 *   * the datasheet names a "GPIO Read" register and a "GPIO Configuration
 *     Register" but numbers NEITHER - the definitions live in the unpublished
 *     ZLS38063 Firmware Manual.  The addresses are NOT publicly documented, and
 *     the official Microsemi host driver has no GPIO API at all.  That is why
 *     this module has to find the address empirically rather than being told it;
 *   * the stock firmware's own 1042-entry configuration record writes ZERO to
 *     the whole suspected GPIO block (0x0700..0x0A22), so the stock never
 *     configured DSP GPIOs - the keys are more likely on the ESP32 side or on
 *     the AW9106B.  The DSP sweep is here to make that conclusion testable
 *     rather than assumed.
 *
 * WHAT IT DOES NOT DO
 * -------------------
 * Nothing here writes to any device.  Every read is a plain read, so the module
 * is safe to enable on a working unit: the ZL38063 keeps its booted application
 * (no reg 0x0014 write, no reset, no window-base write - see PART 2 §8), the
 * AW8737S keeps its single SHDN rising edge (nothing touches reg 0x02) and the
 * panel keeps its transform.  That is deliberate: a probe that changes the thing
 * it measures is worthless.
 *
 * HOW TO USE IT
 * -------------
 *   1. set B220_BUTTON_DISCOVERY to 1 in config.h and build/flash;
 *   2. leave the unit alone for ~30 s (the banner says when the DSP baseline is
 *      captured) and watch the log - that is the "no button pressed" state;
 *   3. press and hold the RECORD key for ~2 s, release, wait ~5 s;
 *   4. do the same for VOLUME UP, then VOLUME DOWN;
 *   5. read the `btn-disc: CHANGE ...` lines: the source that changed three
 *      times, once per press, is the button.  The `btn-disc: TOP` lines
 *      (every 30 s) rank the addresses by how much they moved.
 *
 * A period dump (`btn-disc: sweep`) also reports how many addresses answered at
 * all and how long the sweep took, which is how you tell "the DSP answered 0"
 * from "the DSP did not answer", and whether the DSP is really running
 * (`btn-disc: DSP health`, read-only, printed once after the baseline).
 */

#ifndef B220_BUTTON_DISCOVERY_H
#define B220_BUTTON_DISCOVERY_H

#include <stdbool.h>
#include <stdint.h>

#include <driver/i2c_master.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The board's I2C0 master bus handle (see InitializeBacklight() in
 * xiaomodou_b220.cc).  Returns NULL when that initialisation has not run or
 * failed; the discovery module only ever borrows it to add READ-ONLY devices.
 * The type is the real ESP-IDF one on purpose - the board and this module then
 * cannot drift apart in a way the compiler would miss.
 */
i2c_master_bus_handle_t b220_button_discovery_i2c_bus(void);

/*
 * Start the discovery task.  Safe to call once per boot from the board
 * constructor; it only creates the task (returns immediately).  Requires the
 * HBI device to exist (i.e. zl38063_dsp_begin() has run) for the DSP part of
 * the sweep - without it that third of the probe logs a warning and skips.
 *
 * @return ESP_OK when the task was created, ESP_ERR_NO_MEM / ESP_FAIL otherwise.
 */
esp_err_t b220_button_discovery_start(void);

/*
 * True once the DSP baseline sweep has completed.  The board uses it to put one
 * line in the boot log so the operator knows when to start pressing buttons.
 */
bool b220_button_discovery_baseline_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* B220_BUTTON_DISCOVERY_H */
