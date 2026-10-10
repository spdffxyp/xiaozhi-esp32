/*
 * b220_led.c - the RGB indicator light on the AW9106B (see b220_led.h for what is
 * on which channel, and for why the earlier revisions of this file were wrong).
 *
 * This module owns exactly three registers: 0x20 (RED), 0x21 (GREEN) and 0x22
 * (BLUE) - the six parallel common-anode RGB LEDs on the AW9106B's OUT0/1/2.
 * Everything else on that chip belongs to somebody else:
 *
 *   0x02 / 0x04 / 0x12   the AW8737S SHDN path, owned by the board's PA enable
 *                        (one rising edge per boot; a second mode selection would
 *                        silently change the amplifier's output power)
 *   0x23 / 0x24          the LCD backlight (a low value switches the panel off)
 *
 * Two facts from the datasheet drive the implementation:
 *   - DIMx is 8-bit (00H..FFH), so brightness is a plain 0..255 duty value;
 *   - OUTx power up as GPIO and only sink current once the output driver has been
 *     configured, which the board's boot sequence now does (0x15 <- 0x1B,
 *     0x16 <- 0x08) together with GPMD_A = 0x0A.
 * Neither is set from here: this module only sets colour, so the boot-critical
 * register writes stay in one reviewable place (the board constructor) and this
 * file stays a light driver rather than a second init sequence.
 */

#include "b220_led.h"

#include "config.h"

#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "b220-led";

#define B220_LED_AW9106_ADDR   0x5B

/* The three RGB channels.  Named so a wrong index is visible at the call site. */
#define B220_LED_REG_RED       0x20
#define B220_LED_REG_GREEN     0x21
#define B220_LED_REG_BLUE      0x22

/* The backlight channels - never written here; named only for the log line that
 * states what this module deliberately does not touch. */
#define B220_LED_REG_BACKLIGHT_A 0x23
#define B220_LED_REG_BACKLIGHT_B 0x24

#define B220_LED_TASK_STACK    3072
#define B220_LED_TASK_PRIO     3
#define B220_LED_TASK_TICK_MS  10

/* Colours.  Plain constants so effects read as intent. */
const b220_led_rgb_t B220_LED_OFF     = {0x00, 0x00, 0x00};
const b220_led_rgb_t B220_LED_RED     = {0xFF, 0x00, 0x00};
const b220_led_rgb_t B220_LED_GREEN   = {0x00, 0xFF, 0x00};
const b220_led_rgb_t B220_LED_BLUE    = {0x00, 0x00, 0xFF};
const b220_led_rgb_t B220_LED_WHITE   = {0xFF, 0xFF, 0xFF};
const b220_led_rgb_t B220_LED_YELLOW  = {0xFF, 0xFF, 0x00};
const b220_led_rgb_t B220_LED_CYAN    = {0x00, 0xFF, 0xFF};
const b220_led_rgb_t B220_LED_MAGENTA = {0xFF, 0x00, 0xFF};

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_started = false;
static esp_err_t s_last_error = ESP_OK;

/* The colour on the LEDs now, and the colour to return to when an effect ends. */
static b220_led_rgb_t s_color = {0x00, 0x00, 0x00};
static b220_led_rgb_t s_idle_color = {0x00, 0x00, 0x00};
/* The last colour GIVEN to set_level, before it was scaled: an animation needs the
 * unscaled colour so successive frames do not compound the scaling. */
static b220_led_rgb_t s_base_color = {0x00, 0x00, 0x00};

/* Effect state, serviced by led_task().  s_fx_until_us == 0 means "no effect". */
static volatile int64_t s_fx_until_us = 0;
static b220_led_rgb_t s_fx_then;

static bool s_state_hook = false;

/* --------------------------------------------------------------------------
 * Low level
 * ------------------------------------------------------------------------*/

static esp_err_t led_write_reg(uint8_t reg, uint8_t value)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t b[2] = {reg, value};
    const esp_err_t err = i2c_master_transmit(s_dev, b, sizeof(b), 100);
    if (err != ESP_OK) {
        s_last_error = err;
    }
    return err;
}

/*
 * Attach this module's own device handle to the bus the board created.  Idempotent
 * so every entry point can call it.  The bus itself is never created or freed
 * here: it belongs to InitializeBacklight(), and tearing it down would take the
 * panel with it.
 */
static bool led_attach(void)
{
    if (s_dev != NULL) {
        return true;
    }

    i2c_master_bus_handle_t bus = b220_led_i2c_bus();
    if (bus == NULL) {
        s_last_error = ESP_ERR_INVALID_STATE;
        ESP_LOGW(TAG, "no I2C bus published by the board yet - the RGB light cannot be driven");
        return false;
    }

    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = B220_LED_AW9106_ADDR;
    cfg.scl_speed_hz = 100000;

    const esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        s_last_error = err;
        ESP_LOGW(TAG, "AW9106B 0x%02X did not attach (%s) - no indicator light",
                 (unsigned)B220_LED_AW9106_ADDR, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "AW9106B 0x%02X attached for the RGB indicator (OUT0/1/2 = reg "
                  "0x%02X/0x%02X/0x%02X = R/G/B); the LCD backlight (0x%02X/0x%02X) and the "
                  "PA registers 0x02/0x04/0x12 are never touched here",
             (unsigned)B220_LED_AW9106_ADDR, (unsigned)B220_LED_REG_RED,
             (unsigned)B220_LED_REG_GREEN, (unsigned)B220_LED_REG_BLUE,
             (unsigned)B220_LED_REG_BACKLIGHT_A, (unsigned)B220_LED_REG_BACKLIGHT_B);
    return true;
}

/* --------------------------------------------------------------------------
 * Public colour API
 * ------------------------------------------------------------------------*/

/*
 * Write a colour WITHOUT touching the effect state.  Everything that drives the
 * light goes through here; the public setter below is this plus "cancel any
 * running effect".
 */
static esp_err_t led_write_color(b220_led_rgb_t color)
{
    if (!led_attach()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t first = ESP_OK;
    const uint8_t regs[3] = {B220_LED_REG_RED, B220_LED_REG_GREEN, B220_LED_REG_BLUE};
    const uint8_t vals[3] = {color.r, color.g, color.b};
    for (int i = 0; i < 3; i++) {
        const esp_err_t err = led_write_reg(regs[i], vals[i]);
        if (first == ESP_OK && err != ESP_OK) {
            first = err;
        }
    }
    if (first == ESP_OK) {
        s_color = color;
    } else {
        ESP_LOGW(TAG, "colour %u/%u/%u partially failed (%s)", (unsigned)color.r,
                 (unsigned)color.g, (unsigned)color.b, esp_err_to_name(first));
    }
    return first;
}

esp_err_t b220_led_set_color(b220_led_rgb_t color)
{
    /* A direct write is a newer instruction than an effect, so it cancels one. */
    s_fx_until_us = 0;
    return led_write_color(color);
}

b220_led_rgb_t b220_led_get_color(void)
{
    return s_color;
}

b220_led_rgb_t b220_led_get_base_color(void)
{
    return s_base_color;
}

esp_err_t b220_led_set_level(b220_led_rgb_t color, uint8_t level)
{
    /*
     * Write `color` scaled by `level` (0 = off, 255 = the colour as given).  This
     * exists so an animation can be built out of plain writes instead of a second
     * effect engine inside this module: the breathing policy calls it from its own
     * task.  The BASE colour is remembered separately so successive calls scale the
     * same colour rather than compounding.
     */
    s_base_color = color;
    const b220_led_rgb_t scaled = {
        (uint8_t)(((uint16_t)color.r * level + 127u) / 255u),
        (uint8_t)(((uint16_t)color.g * level + 127u) / 255u),
        (uint8_t)(((uint16_t)color.b * level + 127u) / 255u),
    };
    return led_write_color(scaled);
}

bool b220_led_fx_active(void)
{
    return s_fx_until_us != 0;
}

void b220_led_set_idle_color(b220_led_rgb_t color)
{
    s_idle_color = color;
    ESP_LOGI(TAG, "idle colour set to %u/%u/%u", (unsigned)color.r, (unsigned)color.g,
             (unsigned)color.b);
}

esp_err_t b220_led_pulse(b220_led_rgb_t color, uint32_t on_ms, b220_led_rgb_t then_color)
{
    if (!led_attach()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Arm the follow-up BEFORE driving, then drive through the helper - NOT
     * through b220_led_set_color(), which cancels the effect that was just armed
     * (that was the "steady light instead of a flash" bug). */
    s_fx_then = then_color;
    s_base_color = color;
    s_fx_until_us = esp_timer_get_time() + (int64_t)(on_ms == 0 ? 1u : on_ms) * 1000;
    const esp_err_t err = led_write_color(color);
    if (err != ESP_OK) {
        s_fx_until_us = 0;
    }
    return err;
}

esp_err_t b220_led_show_temporary(b220_led_rgb_t color, uint32_t hold_ms)
{
    return b220_led_pulse(color, hold_ms, s_idle_color);
}

esp_err_t b220_led_blink_once(void)
{
    /* The acknowledgement every key path wants: a short white flash, then back to
     * whatever the idle colour is. */
    return b220_led_pulse(B220_LED_WHITE, 120, s_idle_color);
}

void b220_led_set_state_hook(bool enabled)
{
    s_state_hook = enabled;
    ESP_LOGI(TAG, "device-state colour hook %s", enabled ? "ENABLED" : "disabled");
}

bool b220_led_state_hook_enabled(void)
{
    return s_state_hook;
}

void b220_led_get_state(b220_led_state_t* out)
{
    if (out == nullptr) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->present = (s_dev != NULL);
    out->started = s_started;
    out->rgb[0] = s_color.r;
    out->rgb[1] = s_color.g;
    out->rgb[2] = s_color.b;
    /* The backlight is not owned here, so no value is reported for it. */
    out->backlight[0] = 0;
    out->backlight[1] = 0;
    out->last_error = s_last_error;
}

/* --------------------------------------------------------------------------
 * Effect servicer
 * ------------------------------------------------------------------------*/

static void led_task(void* arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(B220_LED_TASK_TICK_MS));
        const int64_t until = s_fx_until_us;
        if (until != 0 && esp_timer_get_time() >= until) {
            /* End of the pulse: hand the light to the follow-up colour (which is
             * usually the idle colour, i.e. back to rest). */
            const b220_led_rgb_t then = s_fx_then;
            s_fx_until_us = 0;              /* this effect is finished */
            (void)led_write_color(then);
        }
    }
}

/*
 * ONE-SHOT BENCH PASS (B220_LED_WALK): does the LCD BACKLIGHT use the full 8-bit
 * DIM range too?
 *
 * The RGB channels demonstrably do (0x0F -> 0xFF stepped brighter every time) and
 * DIMx is documented as 8-bit for every channel, so the backlight almost certainly
 * does as well - but "almost certainly" is exactly what this port has been wrong
 * about, twice.  It is worth measuring rather than assuming because this is also
 * the channel where a wrong value shows up as a black screen: the port parks it at
 * 0x80 (what the factory init wrote), and if the panel can go brighter that is
 * worth knowing before any "max brightness" constant is set.
 *
 * Registers 0x23/0x24 are the backlight on this unit.  The RGB channels are
 * switched off first so the only thing changing during the ramp is the panel.
 */
#if B220_LED_WALK
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void led_walk_task(void* arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(6000));

    ESP_LOGW(TAG, "=========== LCD BACKLIGHT RANGE MEASUREMENT ===========");
    ESP_LOGW(TAG, "RGB is off; only reg 0x23/0x24 (the LCD backlight) changes.");
    ESP_LOGW(TAG, "Each step is held 3 s.  Report which step is BRIGHTEST, and whether any");
    ESP_LOGW(TAG, "step looks WORSE than the previous one (that would mean the value is out of");
    ESP_LOGW(TAG, "range for this register).");
    ESP_LOGW(TAG, "======================================================");

    (void)led_write_reg(B220_LED_REG_RED, 0x00);
    (void)led_write_reg(B220_LED_REG_GREEN, 0x00);
    (void)led_write_reg(B220_LED_REG_BLUE, 0x00);

    static const uint8_t ramp[] = {0x3F, 0x80, 0xBF, 0xE0, 0xFF};
    for (size_t i = 0; i < sizeof(ramp); i++) {
        ESP_LOGW(TAG, "  reg 0x23/0x24 <- 0x%02X (%u/255) ... WATCH THE SCREEN",
                 (unsigned)ramp[i], (unsigned)ramp[i]);
        (void)led_write_reg(B220_LED_REG_BACKLIGHT_A, ramp[i]);
        (void)led_write_reg(B220_LED_REG_BACKLIGHT_B, ramp[i]);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ESP_LOGW(TAG, "  reg 0x23/0x24 <- 0x80 (the port's normal value)");
    (void)led_write_reg(B220_LED_REG_BACKLIGHT_A, 0x80);
    (void)led_write_reg(B220_LED_REG_BACKLIGHT_B, 0x80);

    ESP_LOGW(TAG, "=========== BACKLIGHT MEASUREMENT DONE ===========");
    vTaskDelete(NULL);
}

static esp_err_t led_walk_start(void)
{
    if (xTaskCreate(led_walk_task, "b220_led_walk", 3072, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "could not create the backlight measurement task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
#else
static esp_err_t led_walk_start(void) { return ESP_OK; }
#endif

esp_err_t b220_led_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!led_attach()) {
        /* Not fatal: the panel and the audio path do not depend on this module. */
        ESP_LOGW(TAG, "RGB indicator unavailable; the display and the audio path are unaffected");
        return ESP_ERR_NOT_FOUND;
    }

    s_started = true;
    s_idle_color = B220_LED_OFF;
    s_color = B220_LED_OFF;

    /* Idle is DARK, and there is no boot blink: the panel and the audio bring-up
     * already show the device is alive. */
    (void)b220_led_set_color(s_idle_color);

    if (xTaskCreate(led_task, "b220_led", B220_LED_TASK_STACK, nullptr, B220_LED_TASK_PRIO,
                    nullptr) != pdPASS) {
        ESP_LOGW(TAG, "could not create the effect task; direct colour writes still work");
    }

    ESP_LOGI(TAG, "RGB indicator ready: idle %s, full brightness 0x%02X (DIMx is 8-bit, so the "
                  "whole 0x00..0xFF range is usable)",
             (s_idle_color.r | s_idle_color.g | s_idle_color.b) ? "COLOURED" : "DARK",
             (unsigned)B220_LED_BRIGHTNESS_MAX);

    /*
     * One-shot bench pass (compiles to nothing unless B220_LED_WALK is 1): measures
     * whether the LCD backlight also uses the full 8-bit DIM range.  Called here, at
     * the end of start-up, so it runs after the panel and the policy are up.
     */
    (void)led_walk_start();
    return ESP_OK;
}
