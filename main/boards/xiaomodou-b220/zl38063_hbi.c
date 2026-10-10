/****************************************************************************
* zl38063_hbi.c - ZL38063 host bus interface (HBI) over SPI
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

#include "zl38063_hbi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

/* SPI2 is shared with the LCD; the LCD is the only code allowed to
 * spi_bus_initialize()/spi_bus_free() it. */
#define ZL38063_HBI_HOST SPI2_HOST

/* zl38063_hbi_rebind() may lose a race with a display transfer that is still in
 * flight (spi_bus_remove_device() then returns ESP_ERR_INVALID_STATE), so the
 * teardown is retried a few times before it gives up. */
#define ZL38063_HBI_REMOVE_RETRIES   20
#define ZL38063_HBI_REMOVE_RETRY_MS  1

#define TAG "ZL38063_HBI"

/*
 * The ZL38063 HBI port is big-endian on the wire: the most significant byte of
 * each 16-bit word is clocked out first.  ESP-IDF's SPI master sends the byte
 * at the lowest address first, so on a little-endian host the word has to be
 * swapped before transmission (and after reception).  This is exactly what
 * BIGENDIAN/spi_common.c did upstream with lwip's htons()/ntohs(); we use the
 * compiler builtin instead so that this file does not need lwip headers.
 *
 * Set to 0 only if a logic-analyser capture proves the device wants the low
 * byte first.
 *
 * This is now only the POWER-ON DEFAULT of the runtime switch below
 * (zl38063_hbi_set_swap()); the wire order can be flipped at run time without a
 * rebuild, which is what the B220_ZL_DIAG probe in zl38063_dsp.c uses to tell a
 * wrong byte order apart from a dead MISO line.
 */
#define ZL38063_HBI_BIG_ENDIAN 1

static spi_device_handle_t s_hbi_dev = NULL;

/*
 * ONE HBI COMMAND AT A TIME - and this is not an optimisation, it is a
 * correctness requirement.
 *
 * An HBI command is not atomic at the SPI level: reaching a register outside
 * page 0 is TWO transactions (SELECT_PAGE, then the paged read/write), and even a
 * page-0 register is a command transaction plus one transaction per data word.
 * Two tasks interleaving those pieces corrupt the protocol: the second task's
 * SELECT_PAGE lands between the first task's page select and its payload, so the
 * first task's data goes to the wrong page.
 *
 * On hardware this showed up as an abort in the SPI driver
 * (`assert failed: spi_device_transmit spi_master.c:1297 (ret_trans ==
 * trans_desc)`) followed by a reboot, triggered as soon as the key poller and the
 * codec's volume write were using the link from different tasks.  Before the
 * volume work existed, the boot task was the only HBI user, so no lock was needed
 * and none was there.
 *
 * A RECURSIVE mutex, because the composite operations (hbi_send_cmd() ->
 * zl38063_hbi_wr16_cmd()) take it more than once from the same task.  It is
 * created lazily, so the boot path may reach it before a mutex can be created.
 */
static SemaphoreHandle_t s_hbi_lock = NULL;

static bool hbi_lock_take(void)
{
    if (s_hbi_lock == NULL) {
        s_hbi_lock = xSemaphoreCreateRecursiveMutex();
        if (s_hbi_lock == NULL) {
            ESP_LOGE(TAG, "could not create the HBI mutex; HBI access is NOT serialised");
            return false;
        }
    }
    /* Short timeout: a stuck holder must not wedge the caller forever. */
    return xSemaphoreTakeRecursive(s_hbi_lock, pdMS_TO_TICKS(500)) == pdTRUE;
}

static void hbi_lock_give(void)
{
    if (s_hbi_lock != NULL) {
        (void)xSemaphoreGiveRecursive(s_hbi_lock);
    }
}

/* Runtime byte-order switch: true = swap every 16-bit word (big-endian on the
 * wire, the upstream behaviour and the default). */
static bool s_swap = (ZL38063_HBI_BIG_ENDIAN != 0);

/*----------------------------------------------------------------------------
 * One 16-bit word, one SPI transaction (one CS assertion).
 *--------------------------------------------------------------------------*/

static esp_err_t hbi_transmit_word(const uint16_t *tx, uint16_t *rx)
{
    spi_transaction_t t;
    uint16_t tx_swapped = 0;
    uint16_t rx_raw = 0;

    memset(&t, 0, sizeof(t));
    t.length = 16;      /* bits */
    t.rxlength = 16;    /* only used when rx != NULL */

    if (tx != NULL) {
        if (s_swap) {
            tx_swapped = __builtin_bswap16(*tx);
            t.tx_buffer = &tx_swapped;
        } else {
            t.tx_buffer = tx;
        }
    } else {
        /* Read-only transaction: leave tx_buffer NULL, the driver then clocks
         * out zeroes while the slave drives MISO. */
        t.tx_buffer = NULL;
    }

    if (rx != NULL) {
        t.rx_buffer = &rx_raw;
    }

    esp_err_t err = spi_device_transmit(s_hbi_dev, &t);
    if (err != ESP_OK) {
        return err;
    }

    if (rx != NULL) {
        if (s_swap) {
            *rx = __builtin_bswap16(rx_raw);
        } else {
            *rx = rx_raw;
        }
    }
    return ESP_OK;
}

/*----------------------------------------------------------------------------
 * Runtime byte-order control
 *--------------------------------------------------------------------------*/

void zl38063_hbi_set_swap(bool enable)
{
    /* Only ever changed by the diagnostics, but the byte order is process-wide
     * state: flip it under the same lock that serialises the transactions. */
    const bool locked = hbi_lock_take();
    s_swap = enable;
    ESP_LOGI(TAG, "word byte-order swap %s", enable ? "ENABLED (big-endian on "
             "the wire, default)" : "DISABLED (raw little-endian wire order)");
    if (locked) {
        hbi_lock_give();
    }
}

bool zl38063_hbi_get_swap(void)
{
    return s_swap;
}

/*----------------------------------------------------------------------------
 * Public low-level primitives (VprocHALWrite / VprocHALRead back-ends).
 *--------------------------------------------------------------------------*/

int zl38063_hbi_wr16_cmd(uint16_t cmd)
{
    if (s_hbi_dev == NULL) {
        return -1;
    }
    if (!hbi_lock_take()) {
        return -1;
    }
    const int rc = (hbi_transmit_word(&cmd, NULL) == ESP_OK) ? 0 : -1;
    hbi_lock_give();
    return rc;
}

int zl38063_hbi_wr16_data(uint16_t data)
{
    if (s_hbi_dev == NULL) {
        return -1;
    }
    if (!hbi_lock_take()) {
        return -1;
    }
    const int rc = (hbi_transmit_word(&data, NULL) == ESP_OK) ? 0 : -1;
    hbi_lock_give();
    return rc;
}

int zl38063_hbi_rd16(uint16_t *data)
{
    if ((s_hbi_dev == NULL) || (data == NULL)) {
        return -1;
    }
    if (!hbi_lock_take()) {
        return -1;
    }
    const int rc = (hbi_transmit_word(NULL, data) == ESP_OK) ? 0 : -1;
    hbi_lock_give();
    return rc;
}

/*----------------------------------------------------------------------------
 * Life cycle
 *--------------------------------------------------------------------------*/

/*
 * Attach one device to the bus the LCD already initialised.  There is
 * deliberately no spi_bus_initialize() here: the LCD owns SCLK/MOSI/MISO and
 * the DMA channel, and freeing the bus would tear the display down.
 */
static esp_err_t hbi_attach(int cs_gpio, int mode_hz, int clock_hz)
{
    spi_device_interface_config_t devcfg = {0};
    devcfg.clock_speed_hz = clock_hz;
    devcfg.mode = (uint8_t)mode_hz;
    devcfg.spics_io_num = cs_gpio;
    devcfg.queue_size = ZL38063_HBI_QUEUE_SZ;

    esp_err_t err = spi_bus_add_device(ZL38063_HBI_HOST, &devcfg, &s_hbi_dev);
    if (err != ESP_OK) {
        s_hbi_dev = NULL;
        ESP_LOGE(TAG, "spi_bus_add_device(SPI2, cs=%d, %d Hz, mode %d) failed: %s",
                 cs_gpio, clock_hz, mode_hz, esp_err_to_name(err));
        return err;
    }

    /*
     * GPIO0 is the ESP32 boot strapping pin; the upstream driver leaves it
     * floating so that it is never driven while the ZL38063 is not selected.
     * Failure here is not fatal (the SPI driver already routed the pin).
     */
    err = gpio_set_pull_mode((gpio_num_t)cs_gpio, GPIO_FLOATING);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_set_pull_mode(GPIO%d, floating) failed: %s",
                 cs_gpio, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "HBI attached to SPI2 (cs=%d, %d Hz, mode %d)",
             cs_gpio, clock_hz, mode_hz);
    return ESP_OK;
}

esp_err_t zl38063_hbi_init(void)
{
    if (s_hbi_dev != NULL) {
        return ESP_OK;      /* idempotent */
    }
    return hbi_attach((int)ZL38063_HBI_CS_GPIO, ZL38063_HBI_MODE,
                      (int)ZL38063_HBI_CLOCK_HZ);
}

esp_err_t zl38063_hbi_rebind(int cs_gpio, int mode_hz, int clock_hz)
{
    if (s_hbi_dev != NULL) {
        /*
         * Remove the old device FIRST: the ESP32 SPI master supports at most
         * three devices per bus and the LCD already owns one of them.  The bus
         * itself (and therefore the display) is never touched - no
         * spi_bus_free(), no spi_bus_initialize().
         *
         * spi_bus_remove_device() refuses with ESP_ERR_INVALID_STATE while any
         * transaction is still in flight, and the LCD shares this bus, so a
         * single attempt can lose a race with a display transfer.  Retry
         * briefly instead of abandoning the sweep; on this board no other task
         * is talking to SPI2 at boot time, so the first attempt normally wins.
         */
        esp_err_t err = ESP_FAIL;
        for (int attempt = 0; attempt < ZL38063_HBI_REMOVE_RETRIES; attempt++) {
            err = spi_bus_remove_device(s_hbi_dev);
            if (err == ESP_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(ZL38063_HBI_REMOVE_RETRY_MS));
        }
        s_hbi_dev = NULL;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "spi_bus_remove_device failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGD(TAG, "rebind -> cs=%d, %d Hz, mode %d", cs_gpio, clock_hz, mode_hz);
    return hbi_attach(cs_gpio, mode_hz, clock_hz);
}

void zl38063_hbi_deinit(void)
{
    if (s_hbi_dev == NULL) {
        return;
    }
    /* Only the device is removed - never spi_bus_free(), the LCD still uses it. */
    esp_err_t err = spi_bus_remove_device(s_hbi_dev);
    s_hbi_dev = NULL;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spi_bus_remove_device failed: %s", esp_err_to_name(err));
    }
}

/*----------------------------------------------------------------------------
 * Word-oriented transfer, including the page-select framing.
 *--------------------------------------------------------------------------*/

/*
 * Build and send the HBI command word for @addr / @numwords.
 * Ported from spis_tw_hbi_wr16_cmd() / spis_tw_hbi_rd16_cmd() in
 * api_lib/vprocTwolf_access.c, which is where the framing actually lives
 * upstream (tw_spi_access.c only orchestrates firmware loading and the CLI).
 *
 *   page   = addr >> 8
 *   offset = (addr & 0xFF) / 2        <- register address is a WORD offset
 *
 * Page 0 is reached with a direct command; any other page requires a page
 * select command first, and page 0xFF (255) keeps its own number while pages
 * 1..254 are re-based to 0..253.
 */
static int hbi_send_cmd(uint16_t addr, uint8_t numwords, int is_write)
{
    uint8_t page = (uint8_t)(addr >> 8);
    uint8_t offset = (uint8_t)((addr & 0x00FFu) / 2u);
    uint16_t cmd;

    if (page == 0) {
        /* Direct page access. */
        cmd = is_write ? ZL38063_HBI_DIRECT_WRITE(offset, numwords - 1)
                       : ZL38063_HBI_DIRECT_READ(offset, numwords - 1);
    } else {
        /* Indirect page access: select the page first. */
        uint8_t sel = page;
        if (sel != 0xFF) {
            sel -= 1;
        }
        if (zl38063_hbi_wr16_cmd(ZL38063_HBI_SELECT_PAGE(sel)) != 0) {
            return -1;
        }
        cmd = is_write ? ZL38063_HBI_PAGED_WRITE(offset, numwords - 1)
                       : ZL38063_HBI_PAGED_READ(offset, numwords - 1);
    }

    return zl38063_hbi_wr16_cmd(cmd);
}

/*
 * Page-255 window offset - the one place this mask/scale lives (see the long
 * comment in zl38063_hbi.h).  hbi_send_cmd() above divides the low byte of a
 * 0xFF00 | lowbyte WRITE address by two, so:
 *
 *   ZL38063_PAGE255_OFF_WORD = (target & 0xFF) / 2  -> what the device sees for
 *       the upload's own window write, i.e. the word offset in the window;
 *   ZL38063_PAGE255_OFF_BYTE = (target & 0xFF)      -> the raw low byte, the
 *       value the loader hands to VprocTwolfHbiWrite() and (wrongly) the value
 *       the first read-back probe assumed the device received.
 *
 * The mask is part of the deal: the previous probe logged the whole low 16 bits
 * of the target address as if it were a "low byte" (0x00080200 printed 0x200).
 */
uint8_t zl38063_hbi_page255_window_offset(uint32_t target_addr,
                                         unsigned int encoding)
{
    const uint8_t low = (uint8_t)(target_addr & 0xFFu);

    return (encoding == ZL38063_PAGE255_OFF_WORD) ? (uint8_t)(low / 2u) : low;
}

int zl38063_hbi_write(uint16_t cmd, uint8_t numwords, const uint16_t *data)
{
    if ((data == NULL) || (numwords == 0)) {
        return -1;
    }
    if (s_hbi_dev == NULL) {
        return -1;
    }
    if (!hbi_lock_take()) {
        return -1;
    }

    /* SELECT_PAGE + command + every data word: one indivisible unit, or another
     * task's page select lands in the middle of it. */
    if (hbi_send_cmd(cmd, numwords, 1) != 0) {
        hbi_lock_give();
        return -1;
    }
    for (uint8_t i = 0; i < numwords; i++) {
        if (zl38063_hbi_wr16_data(data[i]) != 0) {
            hbi_lock_give();
            return -1;
        }
    }
    hbi_lock_give();
    return 0;
}

int zl38063_hbi_read(uint16_t cmd, uint8_t numwords, uint16_t *data)
{
    if ((data == NULL) || (numwords == 0)) {
        return -1;
    }
    if (s_hbi_dev == NULL) {
        return -1;
    }
    if (!hbi_lock_take()) {
        return -1;
    }

    /* SELECT_PAGE + command + every data word, indivisible - see the note on
     * s_hbi_lock above. */
    if (hbi_send_cmd(cmd, numwords, 0) != 0) {
        hbi_lock_give();
        return -1;
    }
    for (uint8_t i = 0; i < numwords; i++) {
        if (zl38063_hbi_rd16(&data[i]) != 0) {
            hbi_lock_give();
            return -1;
        }
    }
    hbi_lock_give();
    return 0;
}
