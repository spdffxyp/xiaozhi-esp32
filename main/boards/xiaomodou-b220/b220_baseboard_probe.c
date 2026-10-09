/*
 * b220_baseboard_probe.c - find the UART link to the B220 baseboard MCU.
 *
 * See b220_baseboard_probe.h for the rationale and for the exact log format.
 * Short version: the stock firmware talks to a motorised baseboard over some
 * two-wire link at some unknown baud on some unknown pin, and we want to know
 * which one before we try to replay that handshake to bring the audio section
 * up.
 *
 * This file is self-contained: it borrows nothing but config.h (for
 * B220_ZL_DIAG and the DSP reset pin) and the ESP-IDF UART/GPIO drivers.  It is
 * read-only on the hardware - every pin it configures is put back, and no byte
 * is ever transmitted.
 */

#include "b220_baseboard_probe.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h" /* esp_timer_get_time() - the sampling clock */

#if B220_ZL_DIAG

#define TAG "B220BB"

/*----------------------------------------------------------------------------
 * Tunables and the time budget
 *
 * The listen window is sized so that even the slowest baud in the list sees a
 * whole "{AT+...}" frame: the stock frames are ~8-20 bytes, which is 12.5-21 ms
 * at 9600 baud, so 120 ms catches one with room to spare.
 *
 * Budget: 13 pins * (25 ms prefilter + 2 * 120 ms mandatory) = 3.45 s, plus
 * 3 * 120 ms for every pin that shows edge activity, plus ~0.4 s of UART
 * driver/pin setup.  In the common case (one live link) that lands under 4 s;
 * the sweep also stops early once the wall-clock budget is used up, so a
 * pathological board (every pin noisy) degrades to fewer bauds tried, never to
 * a longer probe.
 *--------------------------------------------------------------------------*/

#define B220_BB_UART UART_NUM_2
#define B220_BB_UART_RX_BUF 256 /* > UART_HW_FIFO_LEN(128) as the driver requires */
#define B220_BB_UART_TX_BUF 0
#define B220_BB_UART_QUEUE 0 /* no event queue: we poll, we do not block */

#define B220_BB_TIME_BUDGET_MS 4000
#define B220_BB_EDGE_MS 25
#define B220_BB_LISTEN_MS 120

/* How much of what we heard is worth putting in the log. */
#define B220_BB_SNAPSHOT_BYTES 48

/* Poll every N iterations of the edge loop: 1 us of ADC-free stub cost per poll
 * is far below the bit time of the slowest baud we care about (9600 -> 104 us),
 * so no run of transitions can hide between two samples. */
#define B220_BB_EDGE_POLL_MASK 0x3F

/*----------------------------------------------------------------------------
 * Candidate pins
 *
 * Every GPIO we are NOT sure about, minus everything this board already uses:
 *   0              ZL38063 HBI chip-select
 *   1, 3           console UART0
 *   5, 25, 26, 35  I2S to the DSP (bck/ws/dout/din)
 *   18, 23         AW9106B I2C (backlight/indicator LED driver)
 *   21, 22, 27     LCD DC / RST / CS
 *   32, 33, 34     shared SPI2 (SCK/MOSI/MISO) - also the HBI bus
 *   6..11, 16, 17  flash and PSRAM
 *
 * What is left is the list below.  GPIO19 is in it on purpose: it is the DSP
 * reset and therefore "in use", but it is a plausible spare pad on the stock
 * hardware, so it is tested and immediately put back to output-HIGH.
 *
 * GPIO38 and GPIO39 are NOT in the list.  On this chip SOC_GPIO_VALID_GPIO_MASK
 * clears GPIO24 and GPIO28-31 (not bonded out) and SOC_GPIO_VALID_OUTPUT_GPIO_MASK
 * additionally clears 34-39, so no pin above 33 can be used here: the ESP32-D0WD
 * has no pad 38/39 that a QFN48 part exposes, and even where 34-39 exist they
 * are input-only.  The earlier RE saw gpio_set_intr_type(38, ...) in the stock
 * image; 38 is out of range for that API here (SOC_GPIO_OUT_RANGE_MAX is 33) and
 * is reported below as the artefact it is.
 *--------------------------------------------------------------------------*/

static const int kB220BbPins[] = {
    2,
    4,
    12,
    13,
    14,
    15,
    19,
    20,
#if !CONFIG_IDF_TARGET_ESP32
    /* Not an ESP32-D0WD board - only then do 38/39 mean anything.  Kept
     * compiled-out rather than deleted so the intent survives. */
    38,
    39,
#endif
};

/* Baud rates, in the order they are tried.  The first two are the mandatory
 * pair; the rest are the "if it is cheap" tail used only for pins that look
 * alive. */
static const int kB220BbBauds[] = {115200, 9600, 57600, 38400, 4800};
#define B220_BB_BAUDS_FULL (sizeof(kB220BbBauds) / sizeof(kB220BbBauds[0]))
#define B220_BB_BAUDS_MIN 2

/*----------------------------------------------------------------------------
 * Small helpers
 *--------------------------------------------------------------------------*/

static int64_t bb_now_ms(void) { return esp_timer_get_time() / 1000; }

/* Millisecond sleep that lets the idle task run (the UART driver's ISR needs
 * no help, but a 120 ms busy-wait on a single-core part would trip the task
 * watchdog). */
static void bb_sleep_ms(int ms) {
    if (ms > 0) {
        vTaskDelay((TickType_t)((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS));
    }
}

/* One line such as "2 4 12 19".  Used for the summary, so the same pins are
 * easy to copy out of a serial capture into the next probe.  Truncation is
 * explicit rather than silent. */
static void bb_pin_list(char* out, size_t len, const int* pins, int count) {
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < count; i++) {
        int n = snprintf(out + used, len - used, "%s%d", (used == 0) ? "" : " ", pins[i]);
        if ((n <= 0) || ((size_t)n >= (len - used))) {
            if (used >= 2) {
                out[used - 1] = '\0'; /* drop the trailing space */
            }
            strncat(out, " ...", len - strlen(out) - 1);
            return;
        }
        used += (size_t)n;
    }
    if (used == 0) {
        snprintf(out, len, "-");
    }
}

/*----------------------------------------------------------------------------
 * Step 1: edge-activity prefilter
 *
 * Configure the pin as a pulled-up input and poll it for B220_BB_EDGE_MS.  The
 * stock baseboard link runs continuously, so a pin that never moves is very
 * unlikely to be the link - and skipping the baud sweep for those pins is what
 * keeps the whole probe inside the budget.  A pin with edges still gets the
 * full treatment, and even a silent pin gets the two mandatory bauds as a
 * safety net, so this filter can only cost time, not evidence.
 *
 * The pull-up matches an idle UART line; a bidirectional half-duplex link (one
 * wire, open drain) idles high for exactly that reason.  A push-pull line
 * driven low by the other MCU just reads 0, which is not an edge and not data
 * either - there is nothing to receive in that state, so no evidence is lost.
 *--------------------------------------------------------------------------*/

static int bb_edge_activity(int pin, int ms) {
    gpio_config_t cfg = {0};
    int transitions = 0;
    int last;
    int64_t deadline;

    if (!GPIO_IS_VALID_GPIO(pin)) {
        return -1; /* not a pin on this chip - report, do not touch */
    }

    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) {
        return -1;
    }

    last = gpio_get_level((gpio_num_t)pin);
    deadline = bb_now_ms() + ms;
    while (bb_now_ms() < deadline) {
        uint32_t spin = 0;
        do {
            int level = gpio_get_level((gpio_num_t)pin);
            if (level != last) {
                transitions++;
                last = level;
            }
        } while (++spin <= B220_BB_EDGE_POLL_MASK);
    }

    /* Back to a safe, floating state - the pin is re-configured as UART RX (or
     * finally restored) by the caller. */
    gpio_reset_pin((gpio_num_t)pin);
    return transitions;
}

/*----------------------------------------------------------------------------
 * Step 2/3: one bounded listen + the log line
 *--------------------------------------------------------------------------*/

/* Render @snap as a C-ish string.  Carriage returns and newlines are shown
 * escaped (the stock frames end in \r\n), backslashes and quotes are escaped,
 * and anything outside printable ASCII is written as \xHH so the capture stays
 * faithful even for garbage.  Returns false as soon as a byte needs escaping
 * for being unprintable - that is the caller's "is this text?" answer. */
static bool bb_escape_text(const uint8_t* snap, int n, char* out, size_t len) {
    size_t used = 0;
    bool all_printable = true;

    out[0] = '\0';
    for (int i = 0; i < n && used + 6 < len; i++) {
        uint8_t b = snap[i];
        if ((b >= 0x20) && (b < 0x7F) && (b != '\\') && (b != '"')) {
            out[used++] = (char)b;
        } else if (b == '\r' || b == '\n' || b == '\t') {
            out[used++] = '\\';
            out[used++] = (b == '\r') ? 'r' : ((b == '\n') ? 'n' : 't');
        } else {
            all_printable = false;
            int w = snprintf(out + used, len - used, "\\x%02X", (unsigned)b);
            if (w <= 0) {
                break;
            }
            used += (size_t)w;
        }
    }
    out[used] = '\0';
    return all_printable;
}

/* Listen on the currently configured UART for B220_BB_LISTEN_MS and log the
 * result when anything arrived.  Returns the number of bytes received.
 *
 * `full_baud_line` selects the "nothing heard" line: silent pins only get it
 * for the mandatory bauds, so a 13-pin sweep with no link does not bury the
 * rest of the boot log.
 */
static int bb_listen(int pin, int baud, bool log_silence) {
    uint8_t snap[B220_BB_SNAPSHOT_BYTES];
    uint8_t chunk[64];
    char hex[B220_BB_SNAPSHOT_BYTES * 3 + 8];
    char text[B220_BB_SNAPSHOT_BYTES * 4 + 8];
    size_t snap_len = 0;
    size_t total = 0;
    int64_t deadline;

    uart_flush_input(B220_BB_UART);

    deadline = bb_now_ms() + B220_BB_LISTEN_MS;
    do {
        size_t pending = 0;
        if (uart_get_buffered_data_len(B220_BB_UART, &pending) != ESP_OK) {
            break;
        }
        while (pending > 0) {
            uint32_t want = (uint32_t)((pending < sizeof(chunk)) ? pending : sizeof(chunk));
            int got = uart_read_bytes(B220_BB_UART, chunk, want, 0);
            if (got <= 0) {
                break;
            }
            if (snap_len < sizeof(snap)) {
                size_t room = sizeof(snap) - snap_len;
                size_t take = ((size_t)got < room) ? (size_t)got : room;
                memcpy(snap + snap_len, chunk, take);
                snap_len += take;
            }
            total += (size_t)got;
            pending -= (size_t)got;
        }
        /* 5 ms granularity: short enough that a 120 ms window keeps its full
         * width, long enough that the idle task is fed. */
        if (bb_now_ms() < deadline) {
            bb_sleep_ms(5);
        }
    } while (bb_now_ms() < deadline);

    if (total == 0) {
        if (log_silence) {
            ESP_LOGW(TAG, "bb uart pin=%d baud=%d rx=0 (silent)", pin, baud);
        }
        return 0;
    }

    /* Hex of the first bytes, space separated, exactly as logged. */
    {
        size_t used = 0;
        for (size_t i = 0; i < snap_len && used + 4 < sizeof(hex); i++) {
            int w = snprintf(hex + used, sizeof(hex) - used, "%s%02X", (i == 0) ? "" : " ",
                             (unsigned)snap[i]);
            if (w <= 0) {
                break;
            }
            used += (size_t)w;
        }
        hex[used] = '\0';
    }

    {
        bool printable = bb_escape_text(snap, (int)snap_len, text, sizeof(text));
        if (printable) {
            ESP_LOGW(TAG, "bb uart pin=%d baud=%d rx=%u bytes \"%s\" text=\"%s\"", pin, baud,
                     (unsigned)total, hex, text);
        } else {
            /* Escaped, but not ASCII: still print the bytes, and say so. */
            ESP_LOGW(TAG,
                     "bb uart pin=%d baud=%d rx=%u bytes \"%s\" "
                     "text(non-ascii)=\"%s\"",
                     pin, baud, (unsigned)total, hex, text);
        }
    }

    return (int)total;
}

/*----------------------------------------------------------------------------
 * Pin restoration
 *--------------------------------------------------------------------------*/

/* Put a touched pin back the way the rest of the firmware expects it.
 *
 * The DSP reset is the one pin with a required state: zl38063_dsp_begin() left
 * it as an output driven HIGH (DSP out of reset), and the diagnostic that ran
 * just before this probe put it back to exactly that.  gpio_reset_pin() would
 * undefine the pad and float the DSP's reset line, so the reset pin is
 * explicitly re-driven HIGH instead.
 */
static void bb_restore_pin(int pin) {
    if (pin == (int)ZL38063_RESET_GPIO) {
        gpio_config_t cfg = {0};
        cfg.pin_bit_mask = 1ULL << pin;
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&cfg) == ESP_OK) {
            gpio_set_level(ZL38063_RESET_GPIO, 1);
            ESP_LOGW(TAG,
                     "bb restore pin=%d -> output HIGH (DSP reset, as "
                     "zl38063_dsp_begin() left it)",
                     pin);
        } else {
            ESP_LOGE(TAG, "bb restore pin=%d FAILED - DSP reset state unknown", pin);
        }
        return;
    }

    /* Everything else was idle before we looked at it: input, no pulls. */
    gpio_reset_pin((gpio_num_t)pin);
}

/*----------------------------------------------------------------------------
 * The sweep
 *--------------------------------------------------------------------------*/

void b220_baseboard_uart_sweep(void) {
    const int pin_count = (int)(sizeof(kB220BbPins) / sizeof(kB220BbPins[0]));
    int detected[sizeof(kB220BbPins) / sizeof(kB220BbPins[0])];
    int active[sizeof(kB220BbPins) / sizeof(kB220BbPins[0])];
    int detected_count = 0;
    int active_count = 0;
    int pins_touched = 0;
    int skipped = 0;
    int64_t deadline = bb_now_ms() + B220_BB_TIME_BUDGET_MS;
    char line[192];

    ESP_LOGW(TAG, "===== B220 baseboard UART sweep (B220_ZL_DIAG) =====");
    ESP_LOGW(TAG,
             "bb uart start: %d candidate pins, UART%d, RX only (no byte is "
             "ever transmitted), <=%d ms budget",
             pin_count, (int)B220_BB_UART, B220_BB_TIME_BUDGET_MS);
    bb_pin_list(line, sizeof(line), kB220BbPins, pin_count);
    ESP_LOGW(TAG, "bb uart candidates=[%s]", line);
    ESP_LOGW(TAG,
             "bb uart not candidates: 0 (HBI CS) 1 3 (console) 5 25 26 35 "
             "(I2S) 18 23 (I2C) 21 22 27 32 33 34 (LCD/SPI2) 6-11 16 17 "
             "(flash/PSRAM); 38/39 are not valid ESP32 pads (see header)");
#if defined(CONFIG_IDF_TARGET_ESP32)
    ESP_LOGW(TAG,
             "bb note: the stock image's gpio_set_intr_type(38, ...) is "
             "out of range on ESP32 (valid: 0-23, 25-27, 32-33 for output, "
             "0-39 minus 24/28-31 for input); grep hits for GPIO38/39 in "
             "the RE are therefore an artefact of another chip's driver or "
             "a mis-decoded argument");
#endif

    /* A driver already installed on UART2 means somebody else owns the port
     * (an optional network modem, an echo demo, a future bridge).  Refusing is
     * the only safe answer: uart_driver_install() on a live port fails, and
     * deleting a driver we do not own would break that owner. */
    if (uart_is_driver_installed(B220_BB_UART)) {
        ESP_LOGE(TAG,
                 "bb uart abort: UART%d driver already installed - not "
                 "taking the port over",
                 (int)B220_BB_UART);
        return;
    }

    for (int i = 0; i < pin_count; i++) {
        int pin = kB220BbPins[i];

        if (!GPIO_IS_VALID_GPIO(pin)) {
            ESP_LOGW(TAG,
                     "bb pin=%d SKIPPED: not a valid GPIO on this chip "
                     "(SOC_GPIO_PIN_COUNT=%d)",
                     pin, (int)SOC_GPIO_PIN_COUNT);
            skipped++;
            continue;
        }

        /* ---- 1. cheap prefilter: does this pin move at all? ------------- */
        int edges = bb_edge_activity(pin, B220_BB_EDGE_MS);
        pins_touched++;
        if (edges < 0) {
            ESP_LOGW(TAG, "bb pin=%d SKIPPED: gpio_config failed", pin);
            skipped++;
            continue;
        }
        if (edges > 0) {
            active[active_count++] = pin;
        }

        /* ---- 2. UART listen -------------------------------------------- */
        if (bb_now_ms() >= deadline) {
            ESP_LOGW(TAG, "bb uart pin=%d SKIPPED: %d ms budget exhausted", pin,
                     B220_BB_TIME_BUDGET_MS);
            skipped++;
            bb_restore_pin(pin);
            continue;
        }

        esp_err_t err = uart_driver_install(B220_BB_UART, B220_BB_UART_RX_BUF, B220_BB_UART_TX_BUF,
                                            B220_BB_UART_QUEUE, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bb uart pin=%d: uart_driver_install failed (%s)", pin,
                     esp_err_to_name(err));
            skipped++;
            bb_restore_pin(pin);
            continue;
        }

        /* RX only: TX/RTS/CTS keep whatever they had.  The driver's default TX
         * (GPIO17 on this chip) is released by the same call, which matters
         * because 16/17 are the PSRAM pads - setup order, not use, is why this
         * must be passed UART_PIN_NO_CHANGE rather than left to the default. */
        err = uart_set_pin(B220_BB_UART, UART_PIN_NO_CHANGE, pin, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bb uart pin=%d SKIPPED: uart_set_pin failed (%s)", pin,
                     esp_err_to_name(err));
            uart_driver_delete(B220_BB_UART);
            skipped++;
            bb_restore_pin(pin);
            continue;
        }

        /* ---- 3. baud sweep --------------------------------------------- */
        int bauds = (edges > 0) ? (int)B220_BB_BAUDS_FULL : (int)B220_BB_BAUDS_MIN;
        for (int b = 0; b < bauds; b++) {
            uart_config_t uc = {0};
            int rx;

            /* Always run the two mandatory bauds; further ones only when a
             * whole listen window still fits inside the budget.  This is what
             * makes the worst case (every pin noisy) bounded by design rather
             * than by arithmetic on the mandatory pass alone. */
            if ((b >= (int)B220_BB_BAUDS_MIN) && (bb_now_ms() + B220_BB_LISTEN_MS > deadline)) {
                break;
            }

            uc.baud_rate = kB220BbBauds[b];
            uc.data_bits = UART_DATA_8_BITS;
            uc.parity = UART_PARITY_DISABLE;
            uc.stop_bits = UART_STOP_BITS_1;
            uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
            uc.source_clk = UART_SCLK_DEFAULT;
            if (uart_param_config(B220_BB_UART, &uc) != ESP_OK) {
                ESP_LOGW(TAG,
                         "bb uart pin=%d baud=%d: uart_param_config "
                         "failed",
                         pin, kB220BbBauds[b]);
                continue;
            }

            rx = bb_listen(pin, kB220BbBauds[b], b < (int)B220_BB_BAUDS_MIN);
            if (rx > 0) {
                bool seen = false;
                for (int d = 0; d < detected_count; d++) {
                    if (detected[d] == pin) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    detected[detected_count++] = pin;
                }
            }
        }

        /* ---- 4. give the pin back -------------------------------------- */
        uart_driver_delete(B220_BB_UART);
        bb_restore_pin(pin);

        /* The prefilter verdict is logged after the baud sweep so a capture
         * reads as one pin per group; a pin with edges but no data at any baud
         * is itself a useful finding (wrong framing/direction, or the MCU is
         * only listening when the stock handshake happens). */
        ESP_LOGW(TAG, "bb edge pin=%d transitions=%d%s", pin, edges,
                 (edges > 0) ? " (full baud list tried)" : " (mandatory bauds only)");
    }

    /* ---- 5. summary ----------------------------------------------------- */
    if (uart_is_driver_installed(B220_BB_UART)) {
        uart_driver_delete(B220_BB_UART);
    }
    ESP_LOGW(TAG,
             "bb uart done: UART%d driver uninstalled, %d pins touched, "
             "%d skipped",
             (int)B220_BB_UART, pins_touched, skipped);

    bb_pin_list(line, sizeof(line), detected, detected_count);
    ESP_LOGW(TAG, "bb uart summary: candidates=%d detected-data=%s", pin_count, line);
    bb_pin_list(line, sizeof(line), active, active_count);
    ESP_LOGW(TAG, "bb uart summary: edge-activity=%s", line);
    ESP_LOGW(TAG, "bb uart summary: bauds tried=115200 9600%s",
             (active_count > 0) ? " (+57600 38400 4800 on pins with edges)" : "");
    if (detected_count == 0) {
        ESP_LOGW(TAG,
                 "bb uart summary: no candidate pin produced a single byte "
                 "- the baseboard link is NOT on any free GPIO at these "
                 "bauds (or the baseboard is only woken by a stock-side "
                 "handshake we have not replayed yet)");
    }
    ESP_LOGW(TAG, "===== B220 baseboard UART sweep done =====");
}

#endif /* B220_ZL_DIAG */
