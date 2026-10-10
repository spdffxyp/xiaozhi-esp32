/*
 * b220_led.h - the board's RGB indicator light, which lives on the AW9106B.
 *
 * WHAT IS ACTUALLY ON THIS CHIP (measured on the bench, 2026-10-10)
 * ----------------------------------------------------------------
 * The indicator is not a GPIO.  It is six RGB LEDs wired IN PARALLEL, COMMON
 * ANODE, with R, G and B on the AW9106B's OUT0, OUT1 and OUT2.  Those outputs are
 * constant-current channels controlled by the DIM registers:
 *
 *   channel register   this board
 *   ----------------   ------------------------------------------------------
 *   0x20 = DIM0/OUT0   RED    of the six parallel RGB LEDs
 *   0x21 = DIM1/OUT1   GREEN  of the six parallel RGB LEDs
 *   0x22 = DIM2/OUT2   BLUE   of the six parallel RGB LEDs
 *   0x23 = DIM3        LCD backlight (measured: writing it changes the panel
 *                      brightness, and a low value switches the panel off)
 *   0x24 = DIM4        LCD backlight-related (same, with pulse-engine behaviour)
 *   0x25 = DIM5        not connected on this unit
 *
 * Three earlier versions of this file got that mapping wrong - first reading
 * 0x20..0x22 as the backlight, then concluding the LEDs were "not connected".
 * Both mistakes came from driving DIMx with the value the other port of this toy
 * uses (0x3F) without the output-driver configuration that makes the constant
 * current flow at all.  See docs/B220-LED-AND-BASE-UART.md for the full record.
 *
 * WHY THE LEDS STAY DARK WITHOUT MORE THAN A DIM WRITE
 * ---------------------------------------------------
 * DIMx is EIGHT bits (AW9106B datasheet 9.2: "8-bits DIMx can be configured to 256
 * levels, from 00H to FFH"), so 0x3F is only 25% brightness - but that is not why
 * they were dark.  OUTx power up as GPIO and only become current sinks once the
 * output driver is configured, which needs two registers this port had never
 * written (0x15 <- 0x1B, 0x16 <- 0x08), and which the board's boot sequence now
 * writes.  With them, 0x20/0x21/0x22 give red, green, blue and white.
 *
 * It also needs the group-A drive mode to include the LED-driver bit: GPMD_A
 * (register 0x12) is 0x0A, not 0x02 - see xiaomodou_b220.cc, where the same value
 * also keeps the AW8737S SHDN line working (measured: LEDs AND sound).
 *
 * WHAT THIS MODULE MUST NEVER TOUCH
 * ---------------------------------
 * The AW8737S amplifier's SHDN is bit 1 of register 0x02, and its MODE is chosen
 * by counting rising edges on that line - exactly one is allowed per boot
 * (PART 2 §9).  Registers 0x02, 0x04 and 0x12 are owned by the board's PA enable;
 * 0x23/0x24 are the LCD backlight.  All this module writes is 0x20/0x21/0x22.
 *
 * The head/tail lights are NOT here: they are on the baseboard, driven over the
 * baseboard UART (see b220_base_uart.h).
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
 * I2C0 bus the board created (it does not own the bus and must not create or free
 * it), drives the RGB channels to the idle state and starts the effect servicer.
 * Safe to call once per boot, after InitializeBacklight().
 */
esp_err_t b220_led_start(void);

/*
 * Full-brightness ceiling.  DIMx is 8-bit, so this is the whole range; the
 * independent port of this toy caps itself at 0x3F (a quarter of it), which is a
 * dimming preference rather than a hardware limit.  ISEL[1:0] separately selects
 * the 0..IMAX current range (37 mA at the default).
 */
#define B220_LED_BRIGHTNESS_MAX 0xFF

/* An RGB colour: one 0..B220_LED_BRIGHTNESS_MAX value per channel. */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} b220_led_rgb_t;

/* Ready-made colours, defined so effects read as intent rather than as numbers. */
extern const b220_led_rgb_t B220_LED_OFF;
extern const b220_led_rgb_t B220_LED_RED;
extern const b220_led_rgb_t B220_LED_GREEN;
extern const b220_led_rgb_t B220_LED_BLUE;
extern const b220_led_rgb_t B220_LED_WHITE;
extern const b220_led_rgb_t B220_LED_YELLOW;
extern const b220_led_rgb_t B220_LED_CYAN;
extern const b220_led_rgb_t B220_LED_MAGENTA;

/*
 * Drive the RGB light to a colour immediately (each channel is clamped to
 * B220_LED_BRIGHTNESS_MAX and written to 0x20/0x21/0x22).
 */
esp_err_t b220_led_set_color(b220_led_rgb_t color);

/* The colour currently being shown (or the last one written). */
b220_led_rgb_t b220_led_get_color(void);

/* The last colour passed to b220_led_set_level(), before it was scaled. */
b220_led_rgb_t b220_led_get_base_color(void);

/*
 * Write `color` scaled by `level` (0..255).  This is the primitive an animation
 * is built from - the breathing policy calls it with a rising and falling level.
 * Calling it does NOT cancel a running pulse; use b220_led_set_color() when a
 * direct, unscaled colour is meant.
 */
esp_err_t b220_led_set_level(b220_led_rgb_t color, uint8_t level);

/*
 * A short colour pulse: the light goes to `color` for on_ms, then to `then_color`
 * (pass B220_LED_OFF to end dark).  Non-blocking - the effect is serviced by this
 * module's own task, so calling it from an audio or key callback is safe.  A new
 * call replaces the one in progress.
 */
esp_err_t b220_led_pulse(b220_led_rgb_t color, uint32_t on_ms, b220_led_rgb_t then_color);

/*
 * The "something happened" one-shot the board uses on a key press: a short white
 * flash back to the idle colour.  Kept as a named helper because every key path
 * wants exactly this.
 */
esp_err_t b220_led_blink_once(void);

/*
 * Hold a colour for `hold_ms` and then return to the idle colour.  Used for
 * states that last a while (listening, connecting).
 */
esp_err_t b220_led_show_temporary(b220_led_rgb_t color, uint32_t hold_ms);

/* True while a pulse is still running (used by the light policy to keep its hands
 * off a key-press acknowledgement that is mid-flight). */
bool b220_led_fx_active(void);

/*
 * Set the idle colour: what the light returns to when nothing else is happening.
 * Default is B220_LED_OFF (dark), which is what a toy should do when idle.
 */
void b220_led_set_idle_color(b220_led_rgb_t color);

/*
 * Set the LED state hook up to the device state machine: a short colour pulse on
 * the transitions worth noticing (idle -> listening).  Disabled by default.
 */
void b220_led_set_state_hook(bool enabled);

typedef struct {
    bool present;             /* the AW9106B answered on I2C */
    bool started;
    uint8_t rgb[3];           /* the last values written to 0x20/0x21/0x22 */
    uint8_t backlight[2];     /* the last values written to 0x23/0x24, for diagnosis */
    esp_err_t last_error;
} b220_led_state_t;
void b220_led_get_state(b220_led_state_t* out);

#ifdef __cplusplus
}
#endif

#endif /* B220_LED_H */
