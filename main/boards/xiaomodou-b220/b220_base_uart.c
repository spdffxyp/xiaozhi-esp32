/*
 * b220_base_uart.c - baseboard (motors / head-tail lights) link.
 *
 * See b220_base_uart.h for the bus, the sharing caveats and the validated
 * command set.  Everything here is a thin, non-blocking wrapper around UART0.
 *
 * The frame is the stock one: "{" + command + "}" + CRLF.  ref/Flower_modou
 * (components/modou_drivers/modou_base_uart.c) sends exactly that and its
 * base-board test page works on hardware, which is the evidence for the shape.
 */

#include "b220_base_uart.h"

#include "config.h"

#include <stdio.h>
#include <string.h>

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_rom_serial_output.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char* TAG = "b220-base";

#define B220_BASE_UART_PORT      UART_NUM_0
#define B220_BASE_UART_RX_BUF    512
#define B220_BASE_UART_TX_BUF    256
#define B220_BASE_FRAME_MAX      192
#define B220_BASE_LAST_MAX       160

/*
 * HOW THE FRAME IS PUT ON THE WIRE, AND WHY NOT THROUGH THE UART DRIVER
 * --------------------------------------------------------------------
 * The baseboard shares UART0 with the console, and on ESP-IDF 6.1 the console
 * claims that port in a way that leaves no room for a second user:
 *
 *     esp_driver_uart/src/uart_vfs.c:1209
 *     uart_driver_install(channel, 256, 0, 0, NULL, 0);
 *                                      ^^^ RX ring   ^ TX ring = ZERO
 *
 * The console needs a driver to do its line editing, but allocates no TX ring
 * because it writes through the ROM.  Consequences, both observed on this board:
 *
 *   - uart_write_bytes() fails with "uart driver error": the driver exists, but
 *     there is no TX ring for it to queue into;
 *   - uart_driver_install() here cannot help either: the port is taken, so it
 *     returns ESP_ERR_INVALID_STATE.
 *
 * The reference port of this toy installs its own driver and both sends and reads;
 * it can, because the ESP-IDF version it uses does NOT install a console driver
 * (its console writes through the ROM).  That line cannot be copied here.
 *
 * So this port writes through the SAME path the console uses - the ROM's
 * single-character transmitter, which waits for FIFO space and hands the byte to
 * the hardware - and does not install a driver at all.  Two consequences worth
 * stating plainly:
 *
 *   1. a frame can interleave with log text on the wire.  That was already true
 *      (both share the pin); what is new is that a frame may appear in the middle
 *      of a log line rather than between two of them.  The baseboard parses
 *      "{AT+...}\r\n", so a frame that is intact as a substring still works.
 *   2. RX stays with the console: the console's driver owns the RX ring, so base
 *      replies are not readable from here.  Transmit-only is the honest
 *      description, and b220_base_uart_read_pending() reports that rather than
 *      pretending.
 *
 * esp_rom_output_tx_one_char() blocks only while the TX FIFO is full, and the FIFO
 * is 128 bytes, so a whole frame is handed over in microseconds of CPU time in the
 * normal case.  send() takes a semaphore around the frame so two tasks cannot
 * interleave bytes of different frames.
 */
static bool s_ready = false;
static bool s_rx_enabled = false;
static SemaphoreHandle_t s_lock = NULL;
static char s_last_tx[B220_BASE_LAST_MAX];
static char s_last_rx[B220_BASE_LAST_MAX];

/* Send one frame through the ROM transmitter.  Returns the number of bytes the
 * hardware accepted (the frame length) or -1 when the FIFO never drained. */
static int base_write_raw(const char* data, size_t len, uint32_t timeout_ms)
{
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms == 0 ? 1 : timeout_ms);
    const TickType_t start = xTaskGetTickCount();

    for (size_t i = 0; i < len; i++) {
        /* esp_rom_output_tx_one_char returns 0 on success.  It should not fail with
         * the console UART configured, but if it does there is nothing useful to do
         * except stop and say so, which the caller's short-write warning covers. */
        if (esp_rom_output_tx_one_char((uint8_t)data[i]) != 0) {
            return (int)i;
        }
        if (timeout_ms != 0 && (xTaskGetTickCount() - start) > timeout_ticks) {
            /* Only reachable if the FIFO jammed: the call above waits for space, so
             * normal frames never spend a tick here. */
            return (int)i;
        }
    }
    return (int)len;
}

/* The stop frames, byte-for-byte from ref/Flower_modou
 * (components/modou_ui_pages/src/base_board_test_page.cc, BASE_STOP_CMDS). */
static const char* const s_stop_frames[] = {
    "AT+CMD=(1,2,3,2),(255,0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)",
    "AT+CMD=(1,3,3,3),(255,0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)",
};

static esp_err_t build_frame(const char* cmd, char* out, size_t out_size)
{
    if (cmd == NULL || cmd[0] == '\0' || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int n = snprintf(out, out_size, "{%s}\r\n", cmd);
    if (n <= 0 || (size_t)n >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t b220_base_uart_init(bool consume_console_input)
{
    if (s_ready && (s_rx_enabled || !consume_console_input)) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /*
     * UART0 is already the console: its pins and baud rate are what the
     * baseboard expects, so re-configuring them is a no-op by construction.  It
     * is written out anyway so the dependency is explicit rather than implied.
     */
    const uart_config_t cfg = {
        .baud_rate = B220_BASE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(B220_BASE_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_param_config failed (%s)", esp_err_to_name(err));
        return err;
    }
    err = uart_set_pin(B220_BASE_UART_PORT, B220_BASE_UART_TX_GPIO, B220_BASE_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_set_pin failed (%s)", esp_err_to_name(err));
        return err;
    }

    /*
     * No uart_driver_install() here on purpose - see the block at the top of this
     * file.  The console already owns UART0 on ESP-IDF 6.1, so an install would
     * either be refused (ESP_ERR_INVALID_STATE) or, worse, replace the console's
     * driver.  Frames go through the ROM transmitter instead.
     *
     * What is still worth checking is whether the console's RX ring exists at all,
     * because that decides whether b220_base_uart_read_pending() can ever return
     * anything.  It is the console's ring either way, so a base reply and a typed
     * console command land in the same buffer.
     */
    const bool console_driver_present = uart_is_driver_installed(B220_BASE_UART_PORT);
    s_rx_enabled = consume_console_input && console_driver_present;
    if (s_rx_enabled) {
        /* Drop anything the console left behind, so a baseboard reply cannot be
         * confused with stale input from boot. */
        uart_flush_input(B220_BASE_UART_PORT);
    }
    s_ready = true;

    ESP_LOGI(TAG, "baseboard link up: UART0 TX GPIO%d / RX GPIO%d @ %d 8N1, frames sent "
                  "through the ROM transmitter (the console's driver has no TX ring, so the "
                  "UART driver cannot be used here), %s",
             B220_BASE_UART_TX_GPIO, B220_BASE_UART_RX_GPIO, B220_BASE_UART_BAUD,
             s_rx_enabled ? "replies readable from the console's input ring"
                          : "TRANSMIT-ONLY (the console owns the input ring)");
    return ESP_OK;
}

esp_err_t b220_base_uart_send(const char* cmd, uint32_t tx_timeout_ms)
{
    char frame[B220_BASE_FRAME_MAX];
    esp_err_t err = build_frame(cmd, frame, sizeof(frame));
    if (err != ESP_OK) {
        return err;
    }
    err = b220_base_uart_init(false);
    if (err != ESP_OK) {
        return err;
    }

    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(tx_timeout_ms + 100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /*
     * The frame goes out through the ROM transmitter, not uart_write_bytes(): see the
     * block at the top of this file.  On IDF 6.1 the console installs the UART0
     * driver with a ZERO-length TX ring, so uart_write_bytes() fails with
     * "uart driver error" and no install of our own can replace it.
     */
    const int len = (int)strlen(frame);
    const int written = base_write_raw(frame, (size_t)len, tx_timeout_ms);
    if (written == len) {
        snprintf(s_last_tx, sizeof(s_last_tx), "%s", cmd);
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "baseboard write short (%d of %d bytes) - the TX FIFO did not drain",
                 written, len);
        err = ESP_FAIL;
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return err;
}

static esp_err_t read_locked(char* rx, size_t rx_size, uint32_t timeout_ms)
{
    if (rx == NULL || rx_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    rx[0] = '\0';
    if (!s_rx_enabled) {
        return ESP_ERR_INVALID_STATE;   /* opened transmit-only on purpose */
    }

    size_t used = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (used + 1 < rx_size) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            break;
        }
        TickType_t wait = deadline - now;
        if (wait > pdMS_TO_TICKS(20)) {
            wait = pdMS_TO_TICKS(20);
        }
        uint8_t byte = 0;
        const int got = uart_read_bytes(B220_BASE_UART_PORT, &byte, 1, wait);
        if (got <= 0) {
            continue;
        }
        rx[used++] = (char)byte;
        rx[used] = '\0';
        if (byte == '\n') {
            break;
        }
    }

    if (used == 0) {
        return ESP_ERR_TIMEOUT;
    }
    snprintf(s_last_rx, sizeof(s_last_rx), "%s", rx);
    return ESP_OK;
}

esp_err_t b220_base_uart_send_and_receive(const char* cmd, char* rx, size_t rx_size,
                                          uint32_t timeout_ms)
{
    if (!s_rx_enabled) {
        /* Try to upgrade to a readable link once. */
        (void)b220_base_uart_init(true);
    }
    if (!s_rx_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = b220_base_uart_send(cmd, 120);
    if (err != ESP_OK) {
        return err;
    }
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms + 100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t rx_err = read_locked(rx, rx_size, timeout_ms);
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return rx_err;
}

esp_err_t b220_base_uart_read_pending(char* rx, size_t rx_size, uint32_t timeout_ms)
{
    if (!s_rx_enabled) {
        (void)b220_base_uart_init(true);
    }
    if (!s_rx_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms + 100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = read_locked(rx, rx_size, timeout_ms);
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return err;
}

/* ------------------------------------------------------------- handshake --- */

static void handshake_task(void* arg)
{
    (void)arg;
    /* Let the ROM banner and the first log burst finish: they share the wire. */
    vTaskDelay(pdMS_TO_TICKS(B220_BASE_UART_HANDSHAKE_DELAY_MS));

    ESP_LOGI(TAG, "baseboard handshake: sending AT+Pon (this is what unlocks the motors)");
    const esp_err_t err = b220_base_uart_send("AT+Pon", 200);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "baseboard handshake failed (%s); motion commands may be ignored",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "baseboard handshake sent");
    }
    vTaskDelete(NULL);
}

esp_err_t b220_base_uart_start_handshake(void)
{
#if B220_BASE_UART_HANDSHAKE
    const esp_err_t err = b220_base_uart_init(false);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(handshake_task, "b220_base_hs", 2560, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "could not create the handshake task");
        return ESP_FAIL;
    }
    return ESP_OK;
#else
    ESP_LOGI(TAG, "baseboard handshake disabled (B220_BASE_UART_HANDSHAKE=0)");
    return ESP_OK;
#endif
}

/* ---------------------------------------------------------------- motion --- */

esp_err_t b220_base_motion(uint8_t mode, uint16_t steer, uint8_t power,
                           uint32_t duration_ms)
{
    if (power == 0) {
        return b220_base_stop();
    }
    if (steer > 180) {
        steer = 180;
    }
    if (power > 100) {
        power = 100;
    }
    /* The scripted duration field counts 200 ms units; round up so a short
     * request still moves. */
    uint32_t units = (duration_ms + 199u) / 200u;
    if (units == 0) {
        units = 1;
    }
    if (units > 999) {
        units = 999;
    }

    /*
     * Pure-motion frame: head value 9 = coordinated motion with the four light
     * tuples cleared (ref/Flower_modou's documented "current pure motion frame").
     */
    char cmd[B220_BASE_FRAME_MAX];
    const int n = snprintf(cmd, sizeof(cmd),
                           "AT+CMD=(9,1,0,1),(%u,%u,%u,%u,0),(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)",
                           (unsigned)mode, (unsigned)units, (unsigned)steer, (unsigned)power);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return b220_base_uart_send(cmd, 120);
}

esp_err_t b220_base_stop(void)
{
    esp_err_t first = ESP_OK;
    for (size_t i = 0; i < sizeof(s_stop_frames) / sizeof(s_stop_frames[0]); i++) {
        const esp_err_t err = b220_base_uart_send(s_stop_frames[i], 120);
        if (first == ESP_OK && err != ESP_OK) {
            first = err;
        }
    }
    return first;
}

esp_err_t b220_base_lights(uint8_t front_white, uint8_t rear_red)
{
    /*
     * Light tuples are (id, colour, duration, target) in the reference's frames:
     * id 7 with duration 2000 and target 1 = front pair, target 2 = rear pair.
     * front_white / rear_red are "on" flags here (0 = off, non-zero = on) because
     * the colour index -> colour mapping is the baseboard's, and it is not ours
     * to guess: 0 is white and 1 is red in the reference's list.
     */
    char cmd[B220_BASE_FRAME_MAX];
    const unsigned front = (front_white != 0) ? 0u : 1u;   /* 0 = white, 1 = red */
    const unsigned rear = (rear_red != 0) ? 1u : 0u;
    const unsigned front_on = (front_white != 0) ? 1u : 0u;
    const unsigned rear_on = (rear_red != 0) ? 1u : 0u;

    const int n = snprintf(cmd, sizeof(cmd),
                           "AT+CMD=(4,1,0,1),(0,0,0,0,0),(7,%u,2000,%u),(7,%u,2000,%u),(0,0,0,0),(0,0,0,0)",
                           front_on ? front : 0u, front_on ? 1u : 0u,
                           rear_on ? rear : 0u, rear_on ? 2u : 0u);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return b220_base_uart_send(cmd, 120);
}

const char* b220_base_uart_last_tx(void)
{
    return s_last_tx;
}

const char* b220_base_uart_last_rx(void)
{
    return s_last_rx;
}
