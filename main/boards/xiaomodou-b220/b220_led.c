/*
 * b220_led.c - indicator-channel driver for the AW9106B (I2C 0x5B).
 *
 * See b220_led.h for what is on which channel and for the hard rule about
 * register 0x02 (the AW8737S SHDN line, exactly one rising edge per boot).
 *
 * Design choices worth stating:
 *   - this module attaches its OWN i2c_master_dev_t for 0x5B to the bus the
 *     board created.  It does not create or free the bus, and it does not share
 *     the board's device handle, so a failure here cannot disturb the backlight
 *     or the PA path;
 *   - the only registers it writes are 0x20..0x25;
 *   - every write is idempotent (a value equal to the cached one is not
 *     written), so a blink does not generate pointless I2C traffic.
 */

#include "b220_led.h"

#include "config.h"

#include <string.h>

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "b220-led";

/* AW9106B registers used here (see the AW9106 datasheet; names match it). */
#define B220_LED_REG_DIM0   0x20u   /* LCD backlight R/B channel, max at boot */
#define B220_LED_REG_DIM1   0x21u   /* LCD backlight                             */
#define B220_LED_REG_DIM2   0x22u   /* LCD backlight                             */
#define B220_LED_REG_DIM3   0x23u   /* indicator A (factory init leaves 0x80)    */
#define B220_LED_REG_DIM4   0x24u   /* indicator B (factory init leaves 0x80)    */
#define B220_LED_REG_DIM5   0x25u   /* unused by this port                       */

#define B220_LED_TASK_STACK 2560
#define B220_LED_TASK_PRIO  3

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_started = false;
static bool s_present = false;
static esp_err_t s_last_error = ESP_OK;
static uint8_t s_backlight = B220_LED_BRIGHTNESS_MAX;
/*
 * The PROVEN-SAFE value for 0x23/0x24: it is what the stock initialization
 * replay leaves in them on a board whose screen works, so writing it back can
 * never change anything.  Everything this module does with those two channels
 * stays at or above it.
 *
 * This is not a stylistic choice.  Writing 0x00 to 0x23/0x24 turned the LCD off
 * (symptom: press a key, the screen flickers once, then goes black), so at least
 * one of the two is wired to the panel's power/backlight rather than to a plain
 * indicator LED.  See B220-LED-AND-BASE-UART.md §1.4.
 */
#define B220_LED_INDICATOR_SAFE_LEVEL 0x80
static uint8_t s_indicator[B220_LED_INDICATOR_COUNT] = {
    B220_LED_INDICATOR_SAFE_LEVEL,
    B220_LED_INDICATOR_SAFE_LEVEL,
};
static bool s_state_hook = true;

/* Effect state.  An effect is a sequence of (level, duration) steps; `pulses`
 * counts "on" phases.  Serviced by led_service_task. */
static uint8_t s_fx_level = 0x80;
static uint32_t s_fx_on_ms = 120;
static uint32_t s_fx_off_ms = 120;
static uint32_t s_fx_pulses_left = 0;
static bool s_fx_on_phase = false;
static int64_t s_fx_deadline_us = 0;
static bool s_fx_active = false;
/* The level to restore when an effect ends (the idle indicator state).  Seeded
 * with the proven-safe value, never 0: see B220_LED_INDICATOR_SAFE_LEVEL. */
static uint8_t s_indicator_idle = B220_LED_INDICATOR_SAFE_LEVEL;

static const uint8_t s_indicator_reg[B220_LED_INDICATOR_COUNT] = {
    B220_LED_REG_DIM3,
    B220_LED_REG_DIM4,
};

/* ------------------------------------------------------------------ I2C ---- */

static esp_err_t led_write_reg(uint8_t reg, uint8_t value)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[2] = {reg, value};
    const esp_err_t err = i2c_master_transmit(s_dev, b, sizeof(b), 100);
    s_last_error = err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AW9106B reg 0x%02X <- 0x%02X failed (%s)",
                 (unsigned)reg, (unsigned)value, esp_err_to_name(err));
    }
    return err;
}

static bool led_attach(void)
{
    if (s_dev != NULL) {
        return true;
    }
    i2c_master_bus_handle_t bus = b220_led_i2c_bus();
    if (bus == NULL) {
        ESP_LOGW(TAG, "the board's I2C0 bus is not available; the indicator lights stay at "
                      "whatever the factory init left them");
        return false;
    }
    if (i2c_master_probe(bus, B220_AW9106_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "AW9106B 0x%02X does not answer on I2C0", (unsigned)B220_AW9106_ADDR);
        return false;
    }
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = B220_AW9106_ADDR;
    cfg.scl_speed_hz = 100000;
    const esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AW9106B 0x%02X: could not attach (%s)",
                 (unsigned)B220_AW9106_ADDR, esp_err_to_name(err));
        s_dev = NULL;
        s_last_error = err;
        return false;
    }
    s_present = true;
    ESP_LOGI(TAG, "AW9106B 0x%02X attached for the indicator channels "
                  "(backlight 0x20/0x21/0x22 = 0x%02X, indicators 0x%02X/0x%02X)",
             (unsigned)B220_AW9106_ADDR, (unsigned)s_backlight,
             (unsigned)B220_LED_REG_DIM3, (unsigned)B220_LED_REG_DIM4);
    return true;
}

/* -------------------------------------------------------------- channels --- */

esp_err_t b220_led_set_indicator(b220_led_indicator_t which, uint8_t brightness)
{
    if (which < 0 || which >= B220_LED_INDICATOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (brightness > B220_LED_BRIGHTNESS_MAX) {
        brightness = B220_LED_BRIGHTNESS_MAX;
    }
    /*
     * Never below the proven-safe level.  If a caller asks for "off", the
     * channel still stays at whatever the stock replay left, because on this
     * board a lower value has been observed to switch the LCD off.
     */
    if (brightness < B220_LED_INDICATOR_SAFE_LEVEL) {
        brightness = B220_LED_INDICATOR_SAFE_LEVEL;
    }
    if (!led_attach()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_indicator[which] == brightness) {
        return ESP_OK;   /* already there: no I2C traffic */
    }
    const esp_err_t err = led_write_reg(s_indicator_reg[which], brightness);
    if (err == ESP_OK) {
        s_indicator[which] = brightness;
    }
    return err;
}

esp_err_t b220_led_set_both(uint8_t brightness)
{
    esp_err_t first = ESP_OK;
    for (int i = 0; i < B220_LED_INDICATOR_COUNT; i++) {
        const esp_err_t err = b220_led_set_indicator((b220_led_indicator_t)i, brightness);
        if (first == ESP_OK && err != ESP_OK) {
            first = err;
        }
    }
    return first;
}

esp_err_t b220_led_set_backlight(uint8_t brightness)
{
    if (brightness > B220_LED_BRIGHTNESS_MAX) {
        brightness = B220_LED_BRIGHTNESS_MAX;
    }
    if (!led_attach()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_backlight == brightness) {
        return ESP_OK;
    }
    esp_err_t first = ESP_OK;
    const uint8_t regs[3] = {B220_LED_REG_DIM0, B220_LED_REG_DIM1, B220_LED_REG_DIM2};
    for (int i = 0; i < 3; i++) {
        const esp_err_t err = led_write_reg(regs[i], brightness);
        if (first == ESP_OK && err != ESP_OK) {
            first = err;
        }
    }
    if (first == ESP_OK) {
        s_backlight = brightness;
    }
    return first;
}

uint8_t b220_led_get_backlight(void)
{
    return s_backlight;
}

/* ---------------------------------------------------------------- effects -- */

esp_err_t b220_led_flash(uint8_t brightness, uint32_t on_ms, uint32_t off_ms, uint32_t pulses)
{
    if (!led_attach()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness > B220_LED_BRIGHTNESS_MAX) {
        brightness = B220_LED_BRIGHTNESS_MAX;
    }
    s_fx_level = brightness;
    s_fx_on_ms = (on_ms == 0) ? 1u : on_ms;
    s_fx_off_ms = (off_ms == 0) ? 1u : off_ms;
    s_fx_pulses_left = (pulses == 0) ? 1u : pulses;
    s_fx_on_phase = true;
    s_fx_active = true;
    s_fx_deadline_us = esp_timer_get_time() + (int64_t)s_fx_on_ms * 1000;
    (void)b220_led_set_both(s_fx_level);
    return ESP_OK;
}

esp_err_t b220_led_blink_once(void)
{
    /* Two short pulses: unmistakable, and short enough not to be annoying. */
    return b220_led_flash(0x30, 60, 70, 2);
}

static void led_service(void)
{
    if (!s_fx_active) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now < s_fx_deadline_us) {
        return;
    }

    if (s_fx_on_phase) {
        /* End of an "on" phase: back to the safe baseline (NOT to 0 - a level
         * below the baseline is what switched the LCD off). */
        s_fx_on_phase = false;
        s_fx_pulses_left--;
        (void)b220_led_set_both(s_indicator_idle);
        if (s_fx_pulses_left == 0) {
            /* Done: restore the idle level. */
            s_fx_active = false;
            (void)b220_led_set_both(s_indicator_idle);
            return;
        }
        s_fx_deadline_us = now + (int64_t)s_fx_off_ms * 1000;
        return;
    }

    /* End of an "off" phase: on again. */
    s_fx_on_phase = true;
    (void)b220_led_set_both(s_fx_level);
    s_fx_deadline_us = now + (int64_t)s_fx_on_ms * 1000;
}

static void led_task(void* arg)
{
    (void)arg;
#if B220_DIAG_TASK_STATS
    /*
     * TEMPORARY BRING-UP DIAGNOSTIC.  The task watchdog named b220_buttons as the
     * task running on CPU 0 while IDLE0 was starved, but the decoded backtrace
     * showed that task sleeping inside vTaskDelay - i.e. the watchdog's "currently
     * running" line is about whichever task was scheduled at the sample, not about
     * a CPU hog.  So instead of guessing, print the real per-task CPU share and
     * stack high-water marks, which is what says whether anything is actually
     * spinning (and which task it is).
     */
    int64_t next_report_us = esp_timer_get_time() + 10000000;
#endif
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        led_service();
#if B220_DIAG_TASK_STATS
        {
            const int64_t now_us = esp_timer_get_time();
            if (now_us >= next_report_us) {
                next_report_us = now_us + 10000000;
                static char buf[1600];
                ESP_LOGW(TAG, "---- task stats (10 s window) ----");
                vTaskList(buf);
                /* vTaskList output: name state prio stack Num */
                ESP_LOGW(TAG, "%-16s %-4s %-4s %-6s %s", "name", "st", "pri", "stack", "core");
                char* line = buf;
                while (line != NULL && *line != '\0') {
                    char* nl = strchr(line, '\n');
                    if (nl != NULL) {
                        *nl = '\0';
                    }
                    ESP_LOGW(TAG, "%s", line);
                    if (nl == NULL) {
                        break;
                    }
                    line = nl + 1;
                }
                ESP_LOGW(TAG, "free heap %u, min %u", (unsigned)esp_get_free_heap_size(),
                         (unsigned)esp_get_minimum_free_heap_size());
            }
        }
#endif
    }
}

void b220_led_set_state_hook(bool enabled)
{
    s_state_hook = enabled;
}

/* ------------------------------------------------------------------ start -- */

esp_err_t b220_led_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_started = true;

    if (!led_attach()) {
        /* Not fatal: the panel backlight was set up by InitializeBacklight() and
         * keeps working; only the indicator channels stay as the factory left
         * them.  Say so once. */
        ESP_LOGW(TAG, "indicator channels unavailable; the LCD backlight and the audio path "
                      "are unaffected");
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Idle state = the SAFE baseline, and deliberately NOT a boot blink.
     *
     * An earlier revision set these channels to 0 and blinked on every key press;
     * that switched the LCD off.  Until the two channels have been identified on
     * the bench, this module's job is to be able to DRIVE them without ever
     * dropping below the value a working board already has.
     */
    s_indicator_idle = B220_LED_INDICATOR_SAFE_LEVEL;
    (void)b220_led_set_both(s_indicator_idle);

    if (xTaskCreate(led_task, "b220_led", B220_LED_TASK_STACK, nullptr,
                    B220_LED_TASK_PRIO, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "could not create the LED effect task; flashes will not animate");
        return ESP_FAIL;
    }

    /* Leave the backlight exactly where InitializeBacklight() put it: reading it
     * back is not possible on this chip, so the cached value is the truth. */
    s_backlight = B220_LED_BRIGHTNESS_MAX;

    ESP_LOGI(TAG, "indicator channels ready: 0x%02X/0x%02X at the proven-safe 0x%02X "
                  "(never driven below it - a lower level switched the LCD off), backlight "
                  "left at 0x%02X",
             (unsigned)B220_LED_REG_DIM3, (unsigned)B220_LED_REG_DIM4,
             (unsigned)B220_LED_INDICATOR_SAFE_LEVEL, (unsigned)s_backlight);
    return ESP_OK;
}

void b220_led_get_state(b220_led_state_t* out)
{
    if (out == nullptr) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->present = s_present;
    out->started = s_started;
    out->backlight = s_backlight;
    for (int i = 0; i < B220_LED_INDICATOR_COUNT; i++) {
        out->indicator[i] = s_indicator[i];
    }
    out->last_error = s_last_error;
}
