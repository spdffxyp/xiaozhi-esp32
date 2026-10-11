/*
 * b220_battery.cc - the B220's battery gauge.
 *
 * WHAT THIS IS
 *   The battery voltage arrives on the ESP32's own ADC (GPIO36 / ADC1_CH0, see
 *   config.h).  Nothing else on this board measures the battery: there is no
 *   fuel-gauge IC, and the baseboard MCU does not report one.  The stock firmware
 *   samples that channel once a second, and the independent open-source port of
 *   this toy (ref/Flower_modou/components/modou_drivers/modou_battery.c) does the
 *   same - and, unlike the stock binary, says in comments exactly what it does.
 *
 *   This file reproduces that chain and nothing else:
 *
 *     read 10 raw samples, 800 us apart
 *       -> sort, drop the highest and the lowest, average the middle 8
 *       -> raw -> mV      (the stock firmware's own constant)
 *       -> mV  -> percent (the stock firmware's own discharge table)
 *
 * WHY THE STOCK CONVERSIONS AND NOT adc_cali
 *   The framework already ships AdcBatteryMonitor, which asks
 *   espressif__adc_battery_estimation to turn a raw reading into a capacity.  It
 *   is not used here for one reason: it needs the divider's two resistor values,
 *   and they are not known for this board.  Guessing them would silently produce
 *   a plausible wrong number.
 *
 *   The stock conversion needs no such guess - it is an empirical table for
 *   exactly this battery - so it is what runs by default.  The calibrated
 *   millivolt reading is still taken whenever the eFuse calibration is available,
 *   and logged next to the table's value: that is the measurement needed to decide
 *   later whether the table (or the divider) has to be corrected, and it costs one
 *   extra line only when B220_BATTERY_LOG_SAMPLES is on.
 *
 *   The factory millivolt figure is deliberately not called "the battery voltage":
 *   it is the number the stock firmware FEEDS ITS TABLE, and the table is what was
 *   verified against real battery levels.  Treat the pair as one calibrated unit.
 *
 * CHARGING
 *   Always reported as "not charging".  The stock firmware's charge-insert and
 *   charge-done lines are not identified anywhere in this port's reversals, and a
 *   made-up charging flag would be worse than none: the UI would show a bolt while
 *   the battery drains.  See config.h for what to fill in once the line is known.
 *
 * THREADING
 *   A FreeRTOS task of its own (the ADC oneshot read is blocking).  The published
 *   state is a small struct written under portMUX_TYPE; readers get a torn-free
 *   copy via b220_battery_get_state().  Nothing here runs in the audio path.
 */

#include "b220_battery.h"

#include "config.h"

#include <string.h>

#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "b220-bat";

/*
 * The stock firmware's discharge curve: two parallel tables, voltage first.  Taken
 * from the same reversal the reference port documents (s_battery_mv_table /
 * s_battery_percent_table in modou_battery.c).
 *
 * Reading it: 4185 mV and above is 100%, every step down to 3400 mV is a 5% step,
 * and 3400 mV or below is 0%.  Between two points the percentage is interpolated,
 * so the displayed value moves smoothly instead of jumping 5% at a time.
 */
static const int kBatteryMvTable[] = {
    4185, 4113, 4066, 4022, 3983, 3949, 3917, 3889, 3864, 3835, 3805,
    3787, 3777, 3773, 3770, 3765, 3752, 3724, 3680, 3605, 3400,
};
static const int kBatteryPercentTable[] = {
    100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50, 45, 40, 35, 30, 25, 20, 15, 10, 5, 0,
};

#define B220_BATTERY_TABLE_LEN ((int)(sizeof(kBatteryMvTable) / sizeof(kBatteryMvTable[0])))

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static b220_battery_state_t s_state = {
    .ready = false,
    .charging = false,
    .raw_adc = 0,
    .voltage_mv = 0,
    .percent = -1,
    .sample_count = 0,
};

static adc_oneshot_unit_handle_t s_adc = nullptr;
static adc_cali_handle_t s_cali = nullptr;
static bool s_cali_ready = false;
static TaskHandle_t s_task = nullptr;
/* Resolved once in InitAdc() and then only read.  adc_oneshot_io_to_channel() is
 * cheap, but calling it once per sample would be pointless work on a timer. */
static adc_unit_t s_unit = ADC_UNIT_1;
static adc_channel_t s_channel = ADC_CHANNEL_0;

/*
 * The stock firmware's raw -> mV constant, replayed exactly:
 *
 *   mv = ((raw * 0x1c3) * 0x043b3d5b) >> 33
 *
 * (the second term of the original expression, -((raw * 0x1c3) >> 31), is zero for
 * every positive raw reading).  0x1c3 = 451 and the shift divides by 2^33, i.e.
 * about raw * 451 / 121 - roughly 3.7 mV per count, which is what an 11 dB
 * attenuated 12-bit ESP32 ADC channel reads.
 */
int b220_battery_raw_to_mv(int raw_adc) {
    if (raw_adc <= 0) {
        return 0;
    }
    const int64_t scaled = (int64_t)raw_adc * 0x1c3;
    const int mv = (int)((scaled * 0x043b3d5bLL) >> 33);
    return mv < 0 ? 0 : mv;
}

int b220_battery_voltage_to_percent(int voltage_mv) {
    if (voltage_mv >= kBatteryMvTable[0]) {
        return kBatteryPercentTable[0];
    }
    if (voltage_mv <= kBatteryMvTable[B220_BATTERY_TABLE_LEN - 1]) {
        return kBatteryPercentTable[B220_BATTERY_TABLE_LEN - 1];
    }

    /* The tables are ordered high voltage -> low voltage, so the first entry whose
     * voltage is BELOW the reading brackets it from underneath. */
    for (int i = 1; i < B220_BATTERY_TABLE_LEN; ++i) {
        if (kBatteryMvTable[i] < voltage_mv) {
            const int mv_hi = kBatteryMvTable[i - 1];
            const int mv_lo = kBatteryMvTable[i];
            const int pct_hi = kBatteryPercentTable[i - 1];
            const int pct_lo = kBatteryPercentTable[i];
            /* Round to nearest rather than truncate: the gap between table points
             * is only 5%, so truncation would bias every reading downwards. */
            const int numerator = (pct_hi - pct_lo) * (voltage_mv - mv_lo);
            const int rounded = ((numerator * 2) / (mv_hi - mv_lo) + 1) / 2;
            int percent = pct_lo + rounded;
            if (percent < 0) {
                percent = 0;
            }
            if (percent > 100) {
                percent = 100;
            }
            return percent;
        }
    }
    return 0;
}

static void SortAscending(int* values, int count) {
    /* Insertion sort: ten entries, called once a second, so clarity wins over
     * cleverness here. */
    for (int i = 1; i < count; ++i) {
        const int key = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = key;
    }
}

/* Ten raw readings, minus the extremes, averaged.  Also reports what the eFuse
 * calibration makes of the same raw value, purely for the log. */
static esp_err_t ReadFiltered(int* out_raw, int* out_cali_mv) {
    if (s_adc == nullptr || out_raw == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    int samples[B220_BATTERY_SAMPLES] = {0};

    for (int i = 0; i < B220_BATTERY_SAMPLES; ++i) {
        const esp_err_t err = adc_oneshot_read(s_adc, s_channel, &samples[i]);
        if (err != ESP_OK) {
            return err;
        }
        esp_rom_delay_us(B220_BATTERY_SAMPLE_GAP_US);
    }

    SortAscending(samples, B220_BATTERY_SAMPLES);
    int sum = 0;
    for (int i = 1; i <= B220_BATTERY_TRIMMED; ++i) {
        sum += samples[i];
    }
    const int filtered = sum / B220_BATTERY_TRIMMED;
    *out_raw = filtered;

    if (out_cali_mv != nullptr) {
        int mv = 0;
        *out_cali_mv = (s_cali_ready && s_cali != nullptr &&
                        adc_cali_raw_to_voltage(s_cali, filtered, &mv) == ESP_OK)
                           ? mv
                           : -1;
    }
    return ESP_OK;
}

static esp_err_t InitAdc(void) {
    if (s_adc != nullptr) {
        return ESP_OK;
    }

    esp_err_t err = adc_oneshot_io_to_channel(B220_BATTERY_ADC_GPIO, &s_unit, &s_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC pin (%s); no battery gauge", (int)B220_BATTERY_ADC_GPIO,
                 esp_err_to_name(err));
        return err;
    }

    /* The pin comes from the reversals, the channel from the driver.  Naming both
     * makes a driver mapping that disagrees with the documentation visible in the
     * log rather than silent. */
    ESP_LOGI(TAG, "GPIO%d maps to ADC unit %d channel %d", (int)B220_BATTERY_ADC_GPIO, (int)s_unit,
             (int)s_channel);
    if (s_unit != ADC_UNIT_1 || s_channel != ADC_CHANNEL_0) {
        ESP_LOGW(TAG,
                 "expected ADC1_CH0 for GPIO%d (config.h and the stock firmware's "
                 "power task); the driver says unit %d channel %d - the divider and "
                 "the calibrated curve below assume the documented pin, so treat the "
                 "percentage as suspect until this is explained",
                 (int)B220_BATTERY_ADC_GPIO, (int)s_unit, (int)s_channel);
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = s_unit;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed (%s); no battery gauge", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = B220_BATTERY_ADC_ATTEN;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    err = adc_oneshot_config_channel(s_adc, s_channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel %d config failed (%s); no battery gauge", (int)s_channel,
                 esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        s_adc = nullptr;
        return err;
    }

    /* Optional: the eFuse characterization gives a calibrated millivolt reading.
     * The displayed percentage never uses it (see the file header), so a target
     * without it - or without a burnt-in calibration - is not an error. */
    adc_cali_line_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = s_unit;
    cali_cfg.atten = B220_BATTERY_ADC_ATTEN;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
#if CONFIG_IDF_TARGET_ESP32
    cali_cfg.default_vref = 1100; /* the stock firmware's 0x44c */
#endif
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_ready = true;
    }

    ESP_LOGI(TAG,
             "battery gauge ready on GPIO%d (ADC unit %d channel %d), stock curve "
             "%d..%d mV; eFuse calibration %s",
             (int)B220_BATTERY_ADC_GPIO, (int)s_unit, (int)s_channel, kBatteryMvTable[0],
             kBatteryMvTable[B220_BATTERY_TABLE_LEN - 1],
             s_cali_ready ? "available (logged only)" : "unavailable");
    return ESP_OK;
}

static void BatteryTask(void*) {
    b220_battery_state_t state = {};
    for (;;) {
        int raw = 0;
        int cali_mv = -1;
        const esp_err_t err = ReadFiltered(&raw, &cali_mv);
        if (err == ESP_OK) {
            const int mv = b220_battery_raw_to_mv(raw);
            state.ready = true;
            state.charging = false;
            state.raw_adc = raw;
            state.voltage_mv = mv;
            state.percent = b220_battery_voltage_to_percent(mv);
            ++state.sample_count;

            taskENTER_CRITICAL(&s_state_lock);
            s_state = state;
            taskEXIT_CRITICAL(&s_state_lock);

#if B220_BATTERY_LOG_SAMPLES
            ESP_LOGI(TAG, "raw %d -> %d mV -> %d%% (eFuse-calibrated ADC: %d mV)", raw, mv,
                     state.percent, cali_mv);
#endif
        } else {
            /* Keep the last good sample; only the very first reading has nothing to
             * fall back on, and that is what `ready` reports. */
            ESP_LOGW(TAG, "ADC read failed (%s)", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(B220_BATTERY_PERIOD_MS));
    }
}

esp_err_t b220_battery_start(void) {
    if (s_task != nullptr) {
        return ESP_OK;
    }
    const esp_err_t err = InitAdc();
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(BatteryTask, "b220_battery", B220_BATTERY_TASK_STACK, nullptr, 3, &s_task) !=
        pdPASS) {
        s_task = nullptr;
        ESP_LOGE(TAG, "cannot start the battery task; no battery gauge");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void b220_battery_get_state(b220_battery_state_t* state) {
    if (state == nullptr) {
        return;
    }
    taskENTER_CRITICAL(&s_state_lock);
    *state = s_state;
    taskEXIT_CRITICAL(&s_state_lock);
}
