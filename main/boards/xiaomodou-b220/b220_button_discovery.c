/*
 * b220_button_discovery.c - locate the B220's three buttons empirically.
 *
 * See b220_button_discovery.h for the "why".  This file is the "how".
 *
 * Everything in here is a READ.  The module never writes to the ZL38063, the
 * AW9106B, the SC7A20 or the panel.  Two consequences worth stating, because
 * they are the reason this can run on a working unit:
 *
 *   * the ZL38063 booted application keeps running: no reg 0x0014 write (the
 *     documented "kill the running application silently" register, PART 2 §8),
 *     no reset pulse, no page-255 window-base write (reg 0x000C), no config
 *     record.  A button probe that reset the DSP would destroy the very state
 *     it is trying to observe;
 *   * the AW8737S keeps its one allowed SHDN rising edge: reg 0x02 is never
 *     written, only read back for the log.
 *
 * Log tags: `btn-disc` for findings, `btn-disc-cfg` for the GPIO part of setup.
 */

#include "b220_button_discovery.h"

#include "config.h"

#include "zl38063_hbi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "btn-disc";

/* ------------------------------------------------------------------ tuning -- */

/* ESP32 pins to sample.
 *
 * SOC_GPIO_VALID_GPIO_MASK is 0xFFFFFFFFFF & ~(BIT24|BIT28|BIT29|BIT30|BIT31),
 * i.e. 24 and 28..31 are not bonded out, and SOC_GPIO_VALID_OUTPUT_GPIO_MASK
 * additionally excludes 34..39.  Subtracting the pins this port already owns
 * (flash/PSRAM 6..11,16,17; LCD 21,22,27,32,33,34; I2S 5,25,26,35; ZL38063
 * reset 19 and HBI CS 0; I2C 18,23) leaves exactly 20 and 36..39 - of which 20
 * is the only one that could also be an output.
 *
 * 37 and 38 are included because a reversed generation of the ESP32 pin map in
 * this product treats pin 38 as an "interrupt/HINT-style input line" (see
 * zl38063/BRINGUP-ABC-REVERSE.md), and the ZL38063 has a HINT output that could
 * have been wired to the keys instead of its own GPIOs.  They are input-only, so
 * they cannot be driven by anything this firmware does.
 *
 * GPIO 0 is appended when B220_BUTTON_PROBE_GPIO0 is 1 - it is the DSP chip
 * select and the boot strapping pin, so it is off by default. */
static const int s_gpio_pins[] = {
    20, 36, 38, 39,
#if B220_BUTTON_PROBE_GPIO0
    0,
#endif
};
#define B220_BTN_GPIO_COUNT (sizeof(s_gpio_pins) / sizeof(s_gpio_pins[0]))

/* AW9106B registers worth reading: the two input-data ports plus the mode and
 * output registers, so the log shows whether the pin is even an input. */
static const uint8_t s_aw9106_regs[] = {
    0x00,   /* GPIO input/data A (the input port, if the pins are inputs)      */
    0x01,   /* GPIO input/data B                                               */
    0x02,   /* OUT_A: bit1 = AW8737S SHDN - read-only here, never written      */
    0x03,   /* OUT_B                                                           */
    0x04,   /* GPIO_CFG_A                                                      */
    0x05,   /* GPIO_CFG_B                                                      */
    0x11,   /* CTL                                                             */
    0x12,   /* GPMD_A                                                          */
    0x13,   /* GPMD_B                                                          */
};
#define B220_BTN_AW9106_COUNT (sizeof(s_aw9106_regs) / sizeof(s_aw9106_regs[0]))

/* SC7A20 (accelerometer, I2C 0x19): CLICK_CFG / CLICK_SRC / CLICK_THS / INT1_SRC
 * / STATUS, plus REFERENCE for context.  A key wired to this part would most
 * likely appear in CLICK_SRC (0x39) or INT1_SRC (0x31). */
static const uint8_t s_sc7a20_regs[] = {
    0x39,   /* CLICK_SRC      */
    0x38,   /* CLICK_CFG      */
    0x3A,   /* CLICK_THS      */
    0x31,   /* INT1_SRC       */
    0x27,   /* STATUS         */
};
#define B220_BTN_SC7A20_COUNT (sizeof(s_sc7a20_regs) / sizeof(s_sc7a20_regs[0]))

/* Register-space sweep.  The ZL38063 HBI register address is (page << 8) | word
 * offset, 7 bits of offset per page, so one page is 128 words.  Pages 0..3 are
 * the documented range (the config record itself only uses 0x02..0x0A); the deep
 * sweep widens that to page B220_BUTTON_DSP_PAGES-1 to catch a GPIO block living
 * further out.
 *
 * Which sweep runs is chosen by B220_BUTTON_DSP_LIVE_PAGES:
 *   fast sweep (every B220_BUTTON_FAST_PERIOD_MS) : pages 0 .. LIVE-1
 *   deep sweep (at boot, then every 30 s)         : pages 0 .. PAGES-1
 */
#define B220_BTN_DSP_WORDS_PER_PAGE 128

/* Ignore changes smaller than this when reporting a DSP register: an audio
 * application's register space does drift by a few LSBs on its own. */
#define B220_BTN_DSP_DELTA 64

/* With no button pressed the DSP sits still, so a huge number of simultaneous
 * changes means the sweep itself is being disturbed rather than a key moving -
 * clamp the log either way. */
#define B220_BTN_MAX_CHANGES_PER_SWEEP 24
#define B220_BTN_MAX_TOP_ENTRIES       16

/* ------------------------------------------------------------------- state -- */

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_aw9106 = NULL;
static i2c_master_dev_handle_t s_sc7a20 = NULL;
static bool s_baseline_ready = false;
static bool s_started = false;

/* DSP mirror: the previous value of every address of the live sweep window.
 * 0xFFFF is "never read yet" so the first sweep only fills it in. */
static uint16_t* s_dsp_last = NULL;
static uint16_t* s_dsp_first = NULL;        /* first reading after the baseline */
static uint16_t* s_dsp_max_abs_delta = NULL; /* |cur - first|, for the TOP report */
/* "this index has already been placed in the ranking for this period" - a
 * separate flag, because the delta itself can legitimately be 0xFFFF and using
 * that as the sentinel would make such an entry un-consumable. */
static uint8_t* s_dsp_ranked = NULL;
static int s_dsp_live_words = 0;
static int s_dsp_deep_words = 0;

/* Number of addresses that answered with something other than 0x0000/0xFFFF in
 * the last sweep - the "is this page mapped at all" counter. */
static int s_dsp_live_answer_count = 0;
static int s_dsp_ffff_count = 0;

/* GPIO/AW9106/SC7A20 previous values, one flat array per source. */
static int s_gpio_last[B220_BTN_GPIO_COUNT];
static int s_gpio_pullup_last[B220_BTN_GPIO_COUNT];
static int s_aw_last[B220_BTN_AW9106_COUNT];
static int s_sc_last[B220_BTN_SC7A20_COUNT];

typedef struct {
    char name[40];
    int32_t delta;          /* |current - first| magnitude, for the ranking */
    uint16_t first;
    uint16_t now;
    uint16_t addr;
} btn_top_t;

static btn_top_t s_top[B220_BTN_MAX_TOP_ENTRIES];
static int s_top_count = 0;

/* --------------------------------------------------------------- utilities -- */

static int dsp_index_to_addr(int index)
{
    const int page = index / B220_BTN_DSP_WORDS_PER_PAGE;
    const int word = index % B220_BTN_DSP_WORDS_PER_PAGE;
    return (page << 8) | (word * 2);
}

/* One HBI register read.  zl38063_hbi_read() picks the page itself (a direct
 * command for page 0, SELECT_PAGE + a paged command otherwise), so this is
 * exactly the access the production HBI client performs. */
static bool dsp_read(int addr, uint16_t* out)
{
    uint16_t v = 0xFFFF;
    if (zl38063_hbi_read((uint16_t)addr, 1, &v) != 0) {
        return false;
    }
    *out = v;
    return true;
}

static esp_err_t i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* out)
{
    if (dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, out, 1, 100);
}

static esp_err_t i2c_add(i2c_master_bus_handle_t bus, uint8_t addr,
                         i2c_master_dev_handle_t* out)
{
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr;
    cfg.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(bus, &cfg, out);
}

/* ------------------------------------------------------------------ report -- */

static void top_reset(void)
{
    s_top_count = 0;
    memset(s_top, 0, sizeof(s_top));
}

static void top_offer(const char* name, uint16_t first, uint16_t now, uint16_t addr)
{
    const int32_t delta = (int32_t)now - (int32_t)first;
    const int32_t magnitude = (delta < 0) ? -delta : delta;
    if (magnitude == 0) {
        return;
    }

    /* Insertion sort into a small fixed table; s_top[0] is the largest. */
    int slot = -1;
    for (int i = 0; i < B220_BTN_MAX_TOP_ENTRIES; i++) {
        if (i >= s_top_count || magnitude > s_top[i].delta) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return;
    }
    if (s_top_count < B220_BTN_MAX_TOP_ENTRIES) {
        s_top_count++;
    }
    for (int i = s_top_count - 1; i > slot; i--) {
        s_top[i] = s_top[i - 1];
    }
    snprintf(s_top[slot].name, sizeof(s_top[slot].name), "%s", name);
    s_top[slot].delta = magnitude;
    s_top[slot].first = first;
    s_top[slot].now = now;
    s_top[slot].addr = addr;
}

static void top_report(void)
{
    if (s_top_count == 0) {
        ESP_LOGW(TAG, "TOP: nothing moved at all since the baseline (no input source changed)");
        return;
    }
    ESP_LOGW(TAG, "TOP: %d input source(s) moved since the baseline, largest first:", s_top_count);
    for (int i = 0; i < s_top_count; i++) {
        ESP_LOGW(TAG, "TOP %2d/%d: %-34s 0x%04X -> 0x%04X (delta %ld)",
                 i + 1, s_top_count, s_top[i].name,
                 (unsigned)s_top[i].first, (unsigned)s_top[i].now, (long)s_top[i].delta);
    }
}

/* --------------------------------------------------------------- ESP32 GPIO -- */

/*
 * Read every candidate pin twice: once with the internal pull-up on and once
 * with every pull off.  A key that shorts the pin to GND reads 0 with the
 * pull-up (and stays 0 with no pull); a key that shorts to 3V3 reads 1 with no
 * pull and 1 with the pull-up.  Reading both tells the two apart and also
 * reveals a pin that is being driven by something else (both readings equal and
 * unrelated to the key).
 */
static void probe_gpio_pins(void)
{
    for (size_t i = 0; i < B220_BTN_GPIO_COUNT; i++) {
        const int pin = s_gpio_pins[i];
        /* GPIO 34..39 are input-only and have NO internal pull-up: asking for one
         * fails with `gpio_pullup_en(): GPIO number error`.  Only the
         * output-capable pins get the two-state (pull-up / no-pull) treatment. */
        const bool can_pull_up = (pin <= 33);

        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << pin;
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = can_pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            if (s_gpio_last[i] != -2) {
                ESP_LOGW(TAG, "GPIO%d: gpio_config() failed (%s) - not probed",
                         pin, esp_err_to_name(err));
                s_gpio_last[i] = -2;
            }
            continue;
        }
        const int with_pullup = gpio_get_level((gpio_num_t)pin);

        int no_pull = with_pullup;
        if (can_pull_up) {
            cfg.pull_up_en = GPIO_PULLUP_DISABLE;
            if (gpio_config(&cfg) == ESP_OK) {
                no_pull = gpio_get_level((gpio_num_t)pin);
            }
        }

        if (s_gpio_last[i] == -1) {
            s_gpio_last[i] = with_pullup;
            s_gpio_pullup_last[i] = no_pull;
            ESP_LOGI(TAG, "GPIO%d: idle baseline %s=%d nopull=%d (GND-key reads 0/0, 3V3-key reads "
                          "1/1, floating on an output-capable pin reads 1 then leaks)",
                     pin, can_pull_up ? "pullup" : "pullup(n/a,input-only)", with_pullup, no_pull);
            continue;
        }
        if (s_gpio_last[i] != with_pullup || s_gpio_pullup_last[i] != no_pull) {
            ESP_LOGW(TAG, "CHANGE esp32-gpio: GPIO%d %s %d -> %d, nopull %d -> %d "
                          "[NOTE: the HBI traffic this probe generates also rings onto these "
                          "input-only pins, so treat a pin change with suspicion unless it lines "
                          "up with a BTN pulse]",
                     pin, can_pull_up ? "pullup" : "pullup(n/a)",
                     s_gpio_last[i], with_pullup, s_gpio_pullup_last[i], no_pull);
            char name[40];
            snprintf(name, sizeof(name), "esp32-gpio-%d%s", pin, can_pull_up ? "(pullup)" : "");
            top_offer(name, (uint16_t)s_gpio_last[i], (uint16_t)with_pullup, (uint16_t)pin);
            s_gpio_last[i] = with_pullup;
            s_gpio_pullup_last[i] = no_pull;
        }
    }
}

/* ----------------------------------------------------------------- AW9106B -- */

static bool aw9106_setup(void)
{
    if (s_aw9106 != NULL) {
        return true;
    }
    if (s_bus == NULL) {
        return false;
    }
    if (i2c_master_probe(s_bus, B220_AW9106_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "AW9106B 0x%02X does not answer on this bus - skipping that source",
                 (unsigned)B220_AW9106_ADDR);
        return false;
    }
    const esp_err_t err = i2c_add(s_bus, B220_AW9106_ADDR, &s_aw9106);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AW9106B 0x%02X: could not add the device (%s)",
                 (unsigned)B220_AW9106_ADDR, esp_err_to_name(err));
        s_aw9106 = NULL;
        return false;
    }
    ESP_LOGI(TAG, "AW9106B 0x%02X attached read-only", (unsigned)B220_AW9106_ADDR);
    return true;
}

static void probe_aw9106(void)
{
    if (!aw9106_setup()) {
        return;
    }
    for (size_t i = 0; i < B220_BTN_AW9106_COUNT; i++) {
        uint8_t v = 0;
        if (i2c_read_reg(s_aw9106, s_aw9106_regs[i], &v) != ESP_OK) {
            if (s_aw_last[i] != -2) {
                ESP_LOGW(TAG, "AW9106B reg 0x%02X: read failed", (unsigned)s_aw9106_regs[i]);
                s_aw_last[i] = -2;
            }
            continue;
        }
        if (s_aw_last[i] == -1) {
            s_aw_last[i] = v;
            continue;
        }
        if (s_aw_last[i] != v) {
            ESP_LOGW(TAG, "CHANGE aw9106: reg 0x%02X (0x5B) 0x%02X -> 0x%02X",
                     (unsigned)s_aw9106_regs[i], (unsigned)s_aw_last[i], (unsigned)v);
            char name[40];
            snprintf(name, sizeof(name), "aw9106-0x5B-reg0x%02X", (unsigned)s_aw9106_regs[i]);
            top_offer(name, (uint16_t)s_aw_last[i], (uint16_t)v,
                      (uint16_t)s_aw9106_regs[i]);
            s_aw_last[i] = v;
        }
    }
}

/*                             ZL38063 button/event registers (see below) -- */

/*
 * THE MOST LIKELY ANSWER, AND WHY IT IS POLLED SEPARATELY.
 *
 * Reverse engineering of the stock image found that this board's three keys are
 * NOT on an ESP32 GPIO and NOT on the AW9106B: the stock `buttonTask`
 * (0x40124d4c) gets its only input from two ZL38063 HBI register reads, and the
 * first hardware run of this probe nailed down which is which:
 *   HBI reg 0x0002 -> "status": 0x8400 at rest, 0x0002 / 0x0003 during a press
 *   HBI reg 0x0000 -> event / key code: 0x0008 / 0x000A / 0x000B on a press,
 *                     which is exactly the stock's 5..12 dispatch range
 * through the same 16-bit read primitive this port already uses
 * (zl38063_hbi.c), with the stock reporting status values 10 and 11 and
 * dispatching codes 5..12 (`GPIO_BTN` log tag, timer cadence 10 / 50 / 2000
 * ticks = 10 ms poll, 50 ms debounce, 2 s long press).
 *
 * So these two registers are the single most likely place for the keys to show
 * up, and they deserve a much faster look than the 300 ms full-register sweep:
 * a key event on the DSP may well be a pulse that the coarse sweep misses
 * entirely.  This routine polls them at the stock firmware's own 10 ms cadence
 * and logs EVERY value change, with a timestamp, so the mapping
 * (key -> value) can simply be read off the log.
 *
 * Read-only, exactly like everything else in this file. */
static uint16_t s_btn_status_last = 0;
static uint16_t s_btn_code_last = 0;
static bool s_btn_seen = false;
static int64_t s_btn_start_us = 0;

/*
 * The codes the stock firmware's jump table (0x3f43140c) dispatches, with the
 * meaning reverse engineering could attribute to each.  Printed next to the raw
 * value purely to make the log readable: WHICH PHYSICAL KEY produces which code
 * is exactly what this probe is here to establish, so these are hints, never
 * conclusions.
 */
static const char* btn_code_hint(uint16_t code)
{
    switch (code) {
    case 5:  return "stock branch: charge-complete";
    case 6:  return "stock branch: charge-insert";
    case 7:  return "stock branch: other-line";
    case 8:  return "stock branch: GPIO_BTN";
    case 9:  return "stock branch: other-line";
    case 10: return "stock branch: GPIO_BTN";
    case 11: return "stock branch: GPIO_BTN";
    case 12: return "stock branch: GPIO_BTN";
    default: return nullptr;
    }
}

static void probe_button_regs(void)
{
    if (s_btn_start_us == 0) {
        s_btn_start_us = esp_timer_get_time();
    }
    uint16_t code = 0;      /* HBI 0x0000 */
    uint16_t status = 0;    /* HBI 0x0002 */
    if (!dsp_read(0x0000, &code) || !dsp_read(0x0002, &status)) {
        return;   /* HBI not attached (or the read failed): stay quiet, the sweep reports it */
    }

    if (!s_btn_seen) {
        s_btn_seen = true;
        s_btn_status_last = status;
        s_btn_code_last = code;
        ESP_LOGW(TAG, "BTN idle value: status(0x0002) = 0x%04X, code(0x0000) = 0x%04X - this is the "
                      "'nothing pressed' pair.  A press is a SHORT PULSE here (tens of ms), so press "
                      "ONE key at a time and keep the log running.",
                 (unsigned)status, (unsigned)code);
        return;
    }

    if (status != s_btn_status_last || code != s_btn_code_last) {
        const long long ms = (long long)((esp_timer_get_time() - s_btn_start_us) / 1000);
        const char* hint = btn_code_hint(code);
        ESP_LOGW(TAG, "BTN t=+%lld ms: status(0x0002) 0x%04X -> 0x%04X | code(0x0000) 0x%04X -> 0x%04X%s%s%s",
                 ms, (unsigned)s_btn_status_last, (unsigned)status,
                 (unsigned)s_btn_code_last, (unsigned)code,
                 (hint != nullptr) ? "  [" : "", (hint != nullptr) ? hint : "",
                 (hint != nullptr) ? "]" : "");
        char name[40];
        snprintf(name, sizeof(name), "zl38063-status-reg0x0002");
        top_offer(name, s_btn_status_last, status, 0x0002);
        snprintf(name, sizeof(name), "zl38063-code-reg0x0000");
        top_offer(name, s_btn_code_last, code, 0x0000);
        s_btn_status_last = status;
        s_btn_code_last = code;
    }
}

/*                                              ZL38063 health (read-only) -- */
/*
 * Read-only proof that the ZL38063 is (still) running its application.  This
 * matters because a DSP that has fallen back into its boot ROM cannot report a
 * button state at all, and the failure would otherwise look exactly like "the
 * keys are not on the DSP".
 *
 * The registers and their meaning on this product (PART 2 §7):
 *   0x0030 app status      - NEVER updated by this product's application, so it
 *                            is informational only, never a verdict;
 *   0x0032 host command    - 0x0000 idle;
 *   0x0034 command result  - 0xD3D3 means "sitting in the boot ROM";
 *   0x0014 clock/route gate- 0x0200 is the real "application READY" indicator
 *                            (the stock volume/route routine gates on it);
 *   0x0006 host flags.
 * Nothing is written: reading 0x0014 is safe, WRITING it would silently kill the
 * running application (PART 2 §8.1). */
static void dsp_health_check(void)
{
    static const uint16_t regs[] = {0x0030, 0x0032, 0x0034, 0x0014, 0x0006};
    uint16_t v[sizeof(regs) / sizeof(regs[0])] = {0};
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        if (!dsp_read(regs[i], &v[i])) {
            ESP_LOGW(TAG, "DSP health: read of reg 0x%04X failed - the HBI link or the "
                          "device is not usable", (unsigned)regs[i]);
            return;
        }
    }
    ESP_LOGI(TAG, "DSP health (read-only): app-status 0x%04X (never meaningful here), "
                  "host-cmd 0x%04X (0 = idle), cmd-result 0x%04X (0xD3D3 = stuck in boot ROM), "
                  "route-gate 0x%04X (0x0200 = application READY), host-flags 0x%04X",
             (unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3], (unsigned)v[4]);
    if (v[2] == 0xD3D3) {
        ESP_LOGW(TAG, "DSP health: the DSP is in its BOOT ROM, not running an application - "
                      "any button state it might expose is not being sampled.  Check the "
                      "host boot before trusting the DSP half of this probe.");
    } else if (v[3] != 0x0200) {
        ESP_LOGW(TAG, "DSP health: route gate is 0x%04X, not 0x0200 - the application is not "
                      "proven ready (PART 2 §7.2).  The DSP half of this probe may be reading "
                      "a device that is not running.", (unsigned)v[3]);
    }
}

/* ------------------------------------------------------------------ SC7A20 -- */

static bool sc7a20_setup(void)
{
    if (s_sc7a20 != NULL) {
        return true;
    }
    if (s_bus == NULL) {
        return false;
    }
    if (i2c_master_probe(s_bus, 0x19, 100) != ESP_OK) {
        ESP_LOGW(TAG, "SC7A20 0x19 does not answer on this bus - skipping that source");
        return false;
    }
    const esp_err_t err = i2c_add(s_bus, 0x19, &s_sc7a20);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SC7A20 0x19: could not add the device (%s)", esp_err_to_name(err));
        s_sc7a20 = NULL;
        return false;
    }
    ESP_LOGI(TAG, "SC7A20 0x19 attached read-only");
    return true;
}

static void probe_sc7a20(void)
{
    if (!sc7a20_setup()) {
        return;
    }
    for (size_t i = 0; i < B220_BTN_SC7A20_COUNT; i++) {
        uint8_t v = 0;
        if (i2c_read_reg(s_sc7a20, s_sc7a20_regs[i], &v) != ESP_OK) {
            if (s_sc_last[i] != -2) {
                ESP_LOGW(TAG, "SC7A20 reg 0x%02X: read failed", (unsigned)s_sc7a20_regs[i]);
                s_sc_last[i] = -2;
            }
            continue;
        }
        if (s_sc_last[i] == -1) {
            s_sc_last[i] = v;
            continue;
        }
        if (s_sc_last[i] != v) {
            ESP_LOGW(TAG, "CHANGE sc7a20: reg 0x%02X (0x19) 0x%02X -> 0x%02X",
                     (unsigned)s_sc7a20_regs[i], (unsigned)s_sc_last[i], (unsigned)v);
            char name[40];
            snprintf(name, sizeof(name), "sc7a20-0x19-reg0x%02X", (unsigned)s_sc7a20_regs[i]);
            top_offer(name, (uint16_t)s_sc_last[i], (uint16_t)v,
                      (uint16_t)s_sc7a20_regs[i]);
            s_sc_last[i] = v;
        }
    }
}

/* --------------------------------------------------------------- ZL38063 -- */

/*
 * Sweep [0, words) HBI addresses.  `report` is false for the very first sweep
 * (which only fills s_dsp_last and s_dsp_first).
 */
static void probe_dsp(int words, bool report)
{
    s_dsp_live_answer_count = 0;
    s_dsp_ffff_count = 0;
    if (s_dsp_last == NULL || words <= 0) {
        return;
    }

    const int64_t started_us = esp_timer_get_time();
    int changes = 0;
    int answered = 0;
    int read_errors = 0;

    for (int index = 0; index < words; index++) {
        const int addr = dsp_index_to_addr(index);
        uint16_t v = 0xFFFF;
        if (!dsp_read(addr, &v)) {
            read_errors++;
            continue;
        }
        if (v == 0xFFFF) {
            s_dsp_ffff_count++;
        } else if (v != 0x0000) {
            answered++;
        }

        const uint16_t old = s_dsp_last[index];
        if (old == 0xFFFF) {
            /* First look at this address: baseline only. */
            s_dsp_last[index] = v;
            s_dsp_first[index] = v;
            continue;
        }
        if (report && v != old) {
            const int32_t delta = (int32_t)v - (int32_t)old;
            const int32_t magnitude = (delta < 0) ? -delta : delta;
            if (magnitude >= B220_BTN_DSP_DELTA) {
                if (changes < B220_BTN_MAX_CHANGES_PER_SWEEP) {
                    ESP_LOGW(TAG, "CHANGE zl38063-hbi: reg 0x%04X 0x%04X -> 0x%04X (delta %ld)",
                             (unsigned)addr, (unsigned)old, (unsigned)v, (long)delta);
                }
                changes++;
            }
            const int32_t from_first = (int32_t)v - (int32_t)s_dsp_first[index];
            const int32_t mag_first = (from_first < 0) ? -from_first : from_first;
            if (mag_first > (int32_t)s_dsp_max_abs_delta[index]) {
                s_dsp_max_abs_delta[index] = (uint16_t)mag_first;
            }
        }
        s_dsp_last[index] = v;
    }

    if (report) {
        s_dsp_live_answer_count = answered;
        ESP_LOGI(TAG, "sweep: %d address(es) read in %lld ms | %d live (non-0/0xFFFF), "
                      "%d read as 0xFFFF, %d read as 0x0000 | %d changed >= %d, %d read error(s)%s",
                 words, (long long)((esp_timer_get_time() - started_us) / 1000),
                 answered, s_dsp_ffff_count, words - answered - s_dsp_ffff_count - read_errors,
                 changes, (int)B220_BTN_DSP_DELTA, read_errors,
                 (changes > B220_BTN_MAX_CHANGES_PER_SWEEP) ? " (more than were logged)" : "");

        /* Rank the address space by its largest excursion away from the first
         * reading - that is what a held button looks like.
         *
         * This runs on EVERY reported sweep (every B220_BUTTON_FAST_PERIOD_MS),
         * and the per-pick flag is what stops one address from filling the whole
         * table: without it the same "best" register was picked in all sixteen
         * slots and the TOP report came out as sixteen copies of it. */
        if (s_dsp_max_abs_delta != NULL && s_dsp_ranked != NULL) {
            memset(s_dsp_ranked, 0, (size_t)s_dsp_deep_words);
            for (int i = 0; i < B220_BTN_MAX_TOP_ENTRIES; i++) {
                int best = -1;
                int32_t best_delta = 0;
                for (int index = 0; index < words; index++) {
                    if (s_dsp_ranked[index] == 0 &&
                        (int32_t)s_dsp_max_abs_delta[index] > best_delta) {
                        best_delta = (int32_t)s_dsp_max_abs_delta[index];
                        best = index;
                    }
                }
                if (best < 0) {
                    break;
                }
                char name[40];
                snprintf(name, sizeof(name), "zl38063-hbi-reg0x%04X", (unsigned)dsp_index_to_addr(best));
                top_offer(name, s_dsp_first[best], s_dsp_last[best],
                          (uint16_t)dsp_index_to_addr(best));
                s_dsp_ranked[best] = 1;   /* consume it for this period */
            }
        }
    }
}

/* ------------------------------------------------------------------- task --- */

static void button_discovery_task(void* arg)
{
    (void)arg;

    ESP_LOGW(TAG, "================= BUTTON DISCOVERY =================");
    ESP_LOGW(TAG, "Nothing is written to any device - every source is only READ.");
    ESP_LOGW(TAG, "Procedure:");
    ESP_LOGW(TAG, "  1. leave the unit untouched until the 'baseline captured' line below (~30 s)");
    ESP_LOGW(TAG, "  2. press and hold the RECORD key ~2 s, release, wait ~5 s");
    ESP_LOGW(TAG, "  3. do the same for VOLUME UP, then VOLUME DOWN");
    ESP_LOGW(TAG, "  4. read the 'CHANGE ...' lines: the source that changed once per press is the key");
    ESP_LOGW(TAG, "Sources sampled: ESP32 GPIOs 20/36/38/39, AW9106B 0x%02X, SC7A20 0x19, "
                  "ZL38063 HBI pages 0..%d (live) / 0..%d (deep, %s)",
             (unsigned)B220_AW9106_ADDR,
             (int)B220_BUTTON_DSP_LIVE_PAGES - 1, (int)B220_BUTTON_DSP_PAGES - 1,
#if B220_BUTTON_DEEP_SWEEP
             "enabled"
#else
             "disabled"
#endif
             );
    ESP_LOGW(TAG, "PLUS, at the stock firmware's own %d ms cadence, the two ZL38063 registers the "
                  "stock buttonTask reads: 0x0000 (status) and 0x0002 (event/key code).  "
                  "Lines tagged 'BTN' are those.", (int)B220_BUTTON_BTN_PERIOD_MS);
    ESP_LOGW(TAG, "NOTE: pressing a key is what moves a source; simply turning the robot "
                  "moves the SC7A20, and talking moves the DSP audio registers.");
    ESP_LOGW(TAG, "Reverse engineering says the keys reach the ESP32 through the ZL38063, NOT "
                  "through an ESP32 GPIO - so if only one source reacts, expect it to be 'BTN'.");
    ESP_LOGW(TAG, "===================================================");

    /* The I2C bus is owned by InitializeBacklight(); everything here borrows it. */
    s_bus = b220_button_discovery_i2c_bus();

    for (size_t i = 0; i < B220_BTN_GPIO_COUNT; i++) {
        s_gpio_last[i] = -1;
        s_gpio_pullup_last[i] = -1;
    }
    for (size_t i = 0; i < B220_BTN_AW9106_COUNT; i++) {
        s_aw_last[i] = -1;
    }
    for (size_t i = 0; i < B220_BTN_SC7A20_COUNT; i++) {
        s_sc_last[i] = -1;
    }

    s_dsp_deep_words = (int)B220_BUTTON_DSP_PAGES * B220_BTN_DSP_WORDS_PER_PAGE;
    s_dsp_live_words = (int)B220_BUTTON_DSP_LIVE_PAGES * B220_BTN_DSP_WORDS_PER_PAGE;
    if (s_dsp_live_words > s_dsp_deep_words) {
        s_dsp_live_words = s_dsp_deep_words;
    }

    s_dsp_last = (uint16_t*)calloc((size_t)s_dsp_deep_words, sizeof(uint16_t));
    s_dsp_first = (uint16_t*)calloc((size_t)s_dsp_deep_words, sizeof(uint16_t));
    s_dsp_max_abs_delta = (uint16_t*)calloc((size_t)s_dsp_deep_words, sizeof(uint16_t));
    s_dsp_ranked = (uint8_t*)calloc((size_t)s_dsp_deep_words, sizeof(uint8_t));
    if (s_dsp_last == NULL || s_dsp_first == NULL || s_dsp_max_abs_delta == NULL ||
        s_dsp_ranked == NULL) {
        ESP_LOGE(TAG, "no memory for the DSP mirror (%d words); the DSP source is disabled, "
                      "the GPIO/AW9106B/SC7A20 sources still run", s_dsp_deep_words);
        free(s_dsp_last);
        free(s_dsp_first);
        free(s_dsp_max_abs_delta);
        free(s_dsp_ranked);
        s_dsp_last = NULL;
        s_dsp_first = NULL;
        s_dsp_max_abs_delta = NULL;
        s_dsp_ranked = NULL;
    } else {
        memset(s_dsp_last, 0xFF, (size_t)s_dsp_deep_words * sizeof(uint16_t));
        memset(s_dsp_max_abs_delta, 0, (size_t)s_dsp_deep_words * sizeof(uint16_t));
        memset(s_dsp_ranked, 0, (size_t)s_dsp_deep_words * sizeof(uint8_t));
    }

    /* First pass: fill every baseline.  The DSP needs a moment: the board starts
     * this task right after the audio bring-up, and the DSP is mid-application. */
    vTaskDelay(pdMS_TO_TICKS(3000));
    probe_gpio_pins();
    probe_aw9106();
    probe_sc7a20();
#if B220_BUTTON_DEEP_SWEEP
    if (s_dsp_last != NULL) {
        ESP_LOGI(TAG, "deep baseline sweep: pages 0..%d (%d addresses) - this takes a moment",
                 (int)B220_BUTTON_DSP_PAGES - 1, s_dsp_deep_words);
        probe_dsp(s_dsp_deep_words, true);
    }
#else
    ESP_LOGI(TAG, "deep sweep disabled (B220_BUTTON_DEEP_SWEEP=0): only the live pages "
                  "(0..%d) and the dedicated key registers are read, so the probe stops "
                  "ringing onto the input-only GPIOs",
             (int)B220_BUTTON_DSP_LIVE_PAGES - 1);
#endif
    s_baseline_ready = true;
    ESP_LOGW(TAG, "baseline captured - you may start pressing the keys now");
    if (s_dsp_last != NULL) {
        dsp_health_check();
        ESP_LOGW(TAG, "baseline note: %d of %d ZL38063 addresses answered non-0/0xFFFF",
                 s_dsp_live_answer_count, s_dsp_deep_words);
    }

    const int64_t fast_us = (int64_t)B220_BUTTON_FAST_PERIOD_MS * 1000;
#if B220_BUTTON_DEEP_SWEEP
    const int64_t deep_us = (int64_t)B220_BUTTON_DEEP_PERIOD_MS * 1000;
#endif
    const int64_t top_us = (int64_t)B220_BUTTON_TOP_PERIOD_MS * 1000;
    int64_t last_fast = esp_timer_get_time();
#if B220_BUTTON_DEEP_SWEEP
    int64_t last_deep = last_fast;
#endif
    int64_t last_top = last_fast;

    /* The stock firmware polls the DSP's button/event registers every 10 ms.
     * Do the same here: a key event may be a short pulse, and the 300 ms sweep
     * below would walk straight past it. */
    int64_t last_btn = esp_timer_get_time();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5));
        const int64_t now = esp_timer_get_time();

        if (now - last_btn >= (int64_t)B220_BUTTON_BTN_PERIOD_MS * 1000) {
            last_btn = now;
            probe_button_regs();
        }

        if (now - last_fast >= fast_us) {
            last_fast = now;
            probe_gpio_pins();
            probe_aw9106();
            probe_sc7a20();
            if (s_dsp_last != NULL) {
                probe_dsp(s_dsp_live_words, true);
            }
        }
#if B220_BUTTON_DEEP_SWEEP
        if (s_dsp_last != NULL && now - last_deep >= deep_us) {
            last_deep = now;
            probe_dsp(s_dsp_deep_words, true);
        }
#endif
        if (now - last_top >= top_us) {
            last_top = now;
            ESP_LOGI(TAG, "----- input-source report (after %lld s) -----",
                     (long long)(now / 1000000));
            top_report();
            top_reset();
            /* Start a fresh excursion window: the ranking above consumed the
             * previous one, and a level (rather than edge) source stays visible
             * because the next sweep sees it again immediately. */
            if (s_dsp_max_abs_delta != NULL) {
                memset(s_dsp_max_abs_delta, 0, (size_t)s_dsp_deep_words * sizeof(uint16_t));
            }
        }
    }
}

/* ------------------------------------------------------------------- start -- */

esp_err_t b220_button_discovery_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;   /* already started */
    }
    s_started = true;                   /* set before the task can run: a second call is harmless */

    const BaseType_t ok = xTaskCreatePinnedToCore(button_discovery_task, "b220_btn_disc",
                                                 4096, NULL, 2, NULL, 0);
    if (ok != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "could not create the discovery task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool b220_button_discovery_baseline_ready(void)
{
    return s_baseline_ready;
}
