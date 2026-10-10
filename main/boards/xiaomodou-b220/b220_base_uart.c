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
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char* TAG = "b220-base";

#define B220_BASE_UART_PORT      UART_NUM_0
#define B220_BASE_UART_RX_BUF    512
#define B220_BASE_UART_TX_BUF    256
#define B220_BASE_FRAME_MAX      192
#define B220_BASE_LAST_MAX       160

static bool s_ready = false;
static bool s_rx_enabled = false;
static SemaphoreHandle_t s_lock = NULL;
static char s_last_tx[B220_BASE_LAST_MAX];
static char s_last_rx[B220_BASE_LAST_MAX];

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

    if (consume_console_input && !uart_is_driver_installed(B220_BASE_UART_PORT)) {
        /*
         * The console normally owns UART0 through the ROM/log path, so the driver
         * is usually already installed; installing it here is only for the case
         * where it is not.  With CONFIG_ESP_CONSOLE_UART_DEFAULT the console
         * driver is installed by the startup code, in which case s_rx_enabled
         * simply becomes true and reads go to the same buffer the console uses.
         */
        err = uart_driver_install(B220_BASE_UART_PORT, B220_BASE_UART_RX_BUF,
                                  B220_BASE_UART_TX_BUF, 0, NULL, 0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "uart_driver_install failed (%s); staying transmit-only",
                     esp_err_to_name(err));
            consume_console_input = false;
        }
    }

    s_rx_enabled = consume_console_input && uart_is_driver_installed(B220_BASE_UART_PORT);
    if (s_rx_enabled) {
        uart_flush_input(B220_BASE_UART_PORT);
    }
    s_ready = true;

    ESP_LOGI(TAG, "baseboard link up: UART0 TX GPIO%d / RX GPIO%d @ %d 8N1, %s",
             B220_BASE_UART_TX_GPIO, B220_BASE_UART_RX_GPIO, B220_BASE_UART_BAUD,
             s_rx_enabled ? "replies readable (shares the console input)"
                          : "transmit-only (the console keeps its input)");
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
    const int written = uart_write_bytes(B220_BASE_UART_PORT, frame, strlen(frame));
    if (written == (int)strlen(frame)) {
        (void)uart_wait_tx_done(B220_BASE_UART_PORT, pdMS_TO_TICKS(tx_timeout_ms));
        snprintf(s_last_tx, sizeof(s_last_tx), "%s", cmd);
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "baseboard write short (%d of %u bytes)", written,
                 (unsigned)strlen(frame));
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
