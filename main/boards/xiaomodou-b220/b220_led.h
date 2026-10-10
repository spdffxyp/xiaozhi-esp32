/*
 * b220_led.h - the board's own indicator lights, which live on the AW9106B.
 *
 * WHAT IS ON THIS CHIP (and what is not)
 * --------------------------------------
 * The B220's status/indicator lights are not GPIOs: they are constant-current
 * channels of the AW9106B LED driver on I2C0 at 7-bit address 0x5B, the same
 * chip that drives the LCD backlight and the AW8737S amplifier's SHDN line.
 *
 *   channel register   this board
 *   ----------------   ------------------------------------------------------
 *   0x20 / 0x21 / 0x22 LCD BACKLIGHT - owned by InitializeBacklight(), which
 *                      writes 0x3F to all three (the stock firmware's own
 *                      values).  This module may DIM them but never turns them
 *                      into a colour light.
 *   0x23 / 0x24        the two indicator channels.  InitializeBacklight() leaves
 *                      them at 0x80; they are what this module animates.
 *   0x25               not used by this port.
 *
 * The head/tail lights are NOT here: they are on the baseboard and are driven
 * over the baseboard UART (see b220_base_uart.h).
 *
 * The node-0x19 device on the same I2C bus is an accelerometer (SC7A20), not a
 * light driver.
 *
 * WHY THIS MODULE EXISTS
 * ----------------------
 * An earlier state of this port had no way to read or drive these channels at
 * all: the boot sequence wrote 0x80 into 0x23/0x24 and nothing ever touched them
 * again, so the indicator lights were simply "whatever the factory init left".
 * The reference port of this same toy (ref/Flower_modou, components/modou_drivers/
 * modou_key_lights.c) implements a small effect layer on these channels - idle
 * dark, a short colour flash per key event - and that is what this module
 * reproduces, with one deliberate difference: it never rewrites the backlight
 * channels, because on this board those are the LCD backlight, not an RGB LED.
 *
 * SAFETY: THE ONE THING THAT MUST NOT MOVE
 * ----------------------------------------
 * The AW8737S amplifier's SHDN is bit 1 of register 0x02, and its MODE is chosen
 * by counting rising edges on that line - exactly one rising edge is allowed per
 * boot (PART 2 §9).  Register 0x02, 0x04 and 0x12 are therefore OWNED by the
 * board's PA enable and are never written by this module.  All it touches is
 * 0x20..0x25.
 */

#ifndef B220_LED_H
#define B220_LED_H

#include <stdbool.h>
#include <stdint.h>

#include <driver/i2c_master.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The board's I2C0 master bus, borrowed (never created or freed here).  Defined
 * in xiaomodou_b220.cc; returns NULL when InitializeBacklight() has not run or
 * failed to create the bus.
 */
i2c_master_bus_handle_t b220_led_i2c_bus(void);

/*
 * Start the module: it attaches its own read/write device for the AW9106B to the
 * I2C0 bus the board created (it does not own the bus and must not create or
 * free it) and starts the effect servicer.  Safe to call once per boot, after
 * InitializeBacklight().
 */
esp_err_t b220_led_start(void);

/*
 * Indicator channels, for callers that want to drive one directly.
 * (These are the AUTHORED names for the two channels the factory init leaves at
 * 0x80.  Confirming which physical light each one is - and whether there is a
 * second/third indicator - still needs a bench check; see B220-LED-AND-BASE-UART.md.)
 */
typedef enum {
    B220_LED_INDICATOR_A = 0,   /* AW9106 register 0x23 */
    B220_LED_INDICATOR_B = 1,   /* AW9106 register 0x24 */
    B220_LED_INDICATOR_COUNT
} b220_led_indicator_t;

#define B220_LED_BRIGHTNESS_MAX 0x3F

/* Set one indicator channel.  brightness 0 = off, B220_LED_BRIGHTNESS_MAX = max. */
esp_err_t b220_led_set_indicator(b220_led_indicator_t which, uint8_t brightness);

/* Set both indicator channels at once. */
esp_err_t b220_led_set_both(uint8_t brightness);

/*
 * LCD backlight level, 0..B220_LED_BRIGHTNESS_MAX.  This is the only writer of
 * 0x20/0x21/0x22 outside InitializeBacklight() (which sets them to max at boot),
 * and it is how a "screen dim/wake" style effect would be done.  It never
 * changes the panel's DISPLAY_ON/OFF state.
 */
esp_err_t b220_led_set_backlight(uint8_t brightness);
uint8_t   b220_led_get_backlight(void);

/*
 * A short two-pulse blink on the indicator channels, used to acknowledge a key
 * press or a state change.  Non-blocking: the effect is serviced by this
 * module's own task, so calling it from an audio callback is safe.
 */
esp_err_t b220_led_flash(uint8_t brightness, uint32_t on_ms, uint32_t off_ms, uint32_t pulses);

/* The "I am alive / something happened" one-shot used by the board. */
esp_err_t b220_led_blink_once(void);

/*
 * Set the LED state hook up to the device state: this board has no per-state
 * colour LED, so the hook only adds a short blink on the transitions worth
 * noticing (idle -> listening, i.e. the user is being listened to).
 */
void b220_led_set_state_hook(bool enabled);

typedef struct {
    bool present;          /* the AW9106B answered on I2C */
    bool started;
    uint8_t backlight;     /* last value written to 0x20/0x21/0x22 */
    uint8_t indicator[B220_LED_INDICATOR_COUNT];
    esp_err_t last_error;
} b220_led_state_t;
void b220_led_get_state(b220_led_state_t* out);

#ifdef __cplusplus
}
#endif

#endif /* B220_LED_H */
