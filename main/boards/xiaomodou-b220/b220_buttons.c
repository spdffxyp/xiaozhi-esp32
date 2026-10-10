/*
 * b220_buttons.c - poller for the B220's three keys, which live behind the
 * ZL38063 audio front-end (see b220_buttons.h for the whole story).
 *
 *   every B220_BUTTON_POLL_MS:
 *       status = hbi_read(0x0000)          // 0 idle, 3 press, 2 release
 *       if (status & 0x0001) {             // a PRESS, not the release
 *           key = hbi_read(0x0002)         // 0x08 / 0x0A / 0x0B
 *           act on `key` once
 *       }
 *
 * There is NO long press: this interface reports one press event and never says
 * how long the key was down (measured - see the header).  An earlier revision
 * tried three different ways to time a hold and all three failed on hardware;
 * the driver therefore does not pretend to offer one.
 *
 * GPIO38 is the front-end's interrupt line.  It is used only to decide WHEN to
 * look; it does not carry the key state (it reads HIGH while a key is held).
 * Installing the ISR just makes a press visible a few ms sooner; the poll loop is
 * the state machine and works with or without it.
 *
 * Everything is READ ONLY on the DSP.  In particular this module never touches
 * HBI 0x0014 (writing it silently resets the running application into its boot
 * ROM - PART 2 §8.1), never resets the device and never re-uploads firmware.
 */

#include "b220_buttons.h"

#include "config.h"
#include "zl38063_hbi.h"

#include <string.h>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "b220-btn";

/*
 * HBI register addresses.  Page-0 registers, so hbi_send_cmd() uses the direct
 * command encoding - the same one the stock firmware uses.
 */
#define B220_BTN_HBI_REG_STATUS  0x0000u   /* idle / press / release */
#define B220_BTN_HBI_REG_KEY     0x0002u   /* the key code           */

#define B220_BTN_STATUS_IDLE     0x0000u
#define B220_BTN_STATUS_PRESS    0x0003u
#define B220_BTN_STATUS_RELEASE  0x0002u

/* A press is "status bit 0 set"; the release has bit 0 clear. */
#define B220_BTN_STATUS_IS_PRESS(s)   (((s) & 0x0001u) != 0u)

/* GPIO38: the front-end's interrupt output, used as a "look now" hint only. */
#define B220_BTN_INT_GPIO        GPIO_NUM_38

#define B220_BTN_TASK_STACK      3072
#define B220_BTN_TASK_PRIO       5
/* NOTE: there is deliberately no "tick" period here any more.  The poller sleeps
 * exactly until the next poll deadline (B220_BUTTON_POLL_MS).  A fixed short tick
 * that can end up smaller than the poll period is what used to make this task
 * spin and starve its core - see the comment inside button_task(). */

static b220_button_callback_t s_on_action = nullptr;
static TaskHandle_t s_task = nullptr;
static b220_buttons_stats_t s_stats;
static volatile uint32_t s_irq_count = 0;
static bool s_irq_installed = false;
static bool s_seen_first_press = false;   /* startup-noise filter */

static void IRAM_ATTR button_int_isr(void* arg)
{
    (void)arg;
    ++s_irq_count;
}

static bool hbi_read_reg(uint16_t addr, uint16_t* out)
{
    uint16_t v = 0xFFFF;
    if (zl38063_hbi_read(addr, 1, &v) != 0) {
        return false;
    }
    *out = v;
    return true;
}

static b220_button_action_t action_for_key(uint16_t key, bool* known)
{
    *known = true;
    switch (key) {
#if B220_BUTTON_KEY_1
    case B220_BUTTON_KEY_1: return B220_BTN_ACTION_RECORD;
#endif
#if B220_BUTTON_KEY_2
    case B220_BUTTON_KEY_2: return B220_BTN_ACTION_VOLUME_UP;
#endif
#if B220_BUTTON_KEY_3
    case B220_BUTTON_KEY_3: return B220_BTN_ACTION_VOLUME_DOWN;
#endif
    default:
        *known = false;
        return B220_BTN_ACTION_RECORD;
    }
}

static const char* action_name(b220_button_action_t action)
{
    switch (action) {
    case B220_BTN_ACTION_RECORD:      return "RECORD";
    case B220_BTN_ACTION_VOLUME_UP:   return "VOLUME_UP";
    case B220_BTN_ACTION_VOLUME_DOWN: return "VOLUME_DOWN";
    default:                          return "?";
    }
}

static void emit(b220_button_action_t action, uint16_t key, uint16_t status)
{
    ESP_LOGW(TAG, "key %s: status(0x0000)=0x%04X key(0x0002)=0x%04X",
             action_name(action), (unsigned)status, (unsigned)key);
    s_stats.last_key = key;
    s_stats.last_status = status;
    if (s_on_action != nullptr) {
        s_on_action(action);
    }
}

/*
 * A press event: status bit 0 set.  Runs the action once - there is no duration
 * to classify, which is the whole point of the header comment.
 */
static void handle_press(uint16_t status)
{
    uint16_t key = 0;
    if (!hbi_read_reg(B220_BTN_HBI_REG_KEY, &key)) {
        ESP_LOGW(TAG, "press seen (status 0x%04X) but the key register did not answer",
                 (unsigned)status);
        return;
    }

    bool known = false;
    const b220_button_action_t action = action_for_key(key, &known);
    s_stats.presses++;

    /*
     * Startup noise: the front-end presents one stale event while it is still
     * coming up (seen on hardware as `key 0x0301 status 0x0001`, right after the
     * poller starts).  A code this board never produces is not a key press.
     */
    if (!s_seen_first_press && !known) {
        s_seen_first_press = true;
        ESP_LOGW(TAG, "ignoring one unconfigured key 0x%04X (status 0x%04X) at start-up; "
                      "the configured codes (0x%04X / 0x%04X / 0x%04X) are the only real keys",
                 (unsigned)key, (unsigned)status,
                 (unsigned)B220_BUTTON_KEY_1, (unsigned)B220_BUTTON_KEY_2,
                 (unsigned)B220_BUTTON_KEY_3);
        return;
    }
    s_seen_first_press = true;

    if (!known) {
        s_stats.unknown_keys++;
        ESP_LOGW(TAG, "press with UNCONFIGURED key 0x%04X (status 0x%04X) - not acted on; "
                      "add it to B220_BUTTON_KEY_1/2/3 in config.h",
                 (unsigned)key, (unsigned)status);
        return;
    }

    /* One physical press, one action. */
    emit(action, key, status);
}

static void button_task(void* arg)
{
    (void)arg;

    ESP_LOGI(TAG, "ZL38063 key poller up: status=HBI 0x%04X, key=HBI 0x%04X, FIXED %d ms poll "
                  "(GPIO%d is only an activity hint and is not used as a trigger - see the note "
                  "in b220_buttons.c); read-only, the running DSP application is not disturbed",
             (unsigned)B220_BTN_HBI_REG_STATUS, (unsigned)B220_BTN_HBI_REG_KEY,
             (int)B220_BUTTON_POLL_MS, (int)B220_BTN_INT_GPIO);
    ESP_LOGI(TAG, "one press event per press; NO long press - this interface never reports how "
                  "long a key was held (docs/B220-LED-AND-BASE-UART.md §2.6)");
#if !B220_BUTTON_KEY_1 || !B220_BUTTON_KEY_2 || !B220_BUTTON_KEY_3
    ESP_LOGW(TAG, "the key->function mapping is INCOMPLETE (B220_BUTTON_KEY_1/2/3 in config.h): "
                  "every press is logged with its raw key value but not acted on.  The hardware "
                  "codes are 0x08 = volume up, 0x0A = volume down, 0x0B = record.");
#endif

    uint32_t last_irq = s_irq_count;
    uint16_t prev_status = 0xFFFF;
    int64_t next_poll_us = esp_timer_get_time();

    while (true) {
        /*
         * Sleep to the NEXT poll deadline - never merely "delay a little and
         * look again".  The previous version delayed by B220_BTN_TICK_MS and
         * `continue`d when that was not yet the deadline; because
         * B220_BTN_TICK_MS (5 ms) was SMALLER than B220_BUTTON_POLL_MS (10 ms)
         * and CONFIG_FREERTOS_HZ=100 rounds 5 ms up to a 10 ms tick, every
         * wake-up was "too early" - so this task spun at priority 5 and starved
         * everything below it on its core, including the board constructor.  That
         * looked exactly like the boot hanging right after this task started, and
         * vTaskList showed this task in state 'X' (Running).
         */
        const int64_t now_us = esp_timer_get_time();
        const int64_t wait_us = next_poll_us - now_us;
        if (wait_us > 0) {
            uint32_t wait_ms = (uint32_t)((wait_us + 999) / 1000);
            if (wait_ms == 0) {
                wait_ms = 1;
            }
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }
        next_poll_us += (int64_t)B220_BUTTON_POLL_MS * 1000;
        if (next_poll_us <= now_us) {
            /* Fell behind (a long HBI stall, or the first iteration):
             * resynchronise instead of trying to catch up. */
            next_poll_us = now_us + (int64_t)B220_BUTTON_POLL_MS * 1000;
        }
        (void)last_irq;

#if B220_DIAG_TASK_STATS
        /*
         * TEMPORARY BRING-UP DIAGNOSTIC, deliberately placed in THIS task: it is
         * the one whose liveness is not in question (its banner and its press
         * lines appear in every log).  So if this dump does not show up, the
         * poller itself has stopped looping - which is itself the answer.
         *
         * vTaskList gives name / state / priority / stack high-water mark / core
         * for every task, which is what distinguishes "a task is really spinning"
         * from "the watchdog is just naming whoever was scheduled at the sample".
         */
        {
            static int64_t next_stats_us = 0;
            if (next_stats_us == 0) {
                next_stats_us = now_us + 5000000;   /* first dump 5 s after start */
            }
            if (now_us >= next_stats_us) {
                next_stats_us = now_us + 10000000;
                static char stats_buf[1800];
                vTaskList(stats_buf);
                ESP_LOGW(TAG, "---- task list (name / state / prio / stack free / core) ----");
                char* line = stats_buf;
                while (line != NULL && *line != '\0') {
                    char* nl = strchr(line, '\n');
                    if (nl != NULL) {
                        *nl = '\0';
                    }
                    ESP_LOGW(TAG, "  %s", line);
                    if (nl == NULL) {
                        break;
                    }
                    line = nl + 1;
                }
                ESP_LOGW(TAG, "---- end task list; free heap %u (min %u) ----",
                         (unsigned)esp_get_free_heap_size(),
                         (unsigned)esp_get_minimum_free_heap_size());
            }
        }
#endif

        uint16_t status = 0;
        if (!hbi_read_reg(B220_BTN_HBI_REG_STATUS, &status)) {
            /* The HBI link is not up (or the DSP is mid-reset).  Say it once in a
             * while instead of flooding. */
            static unsigned failures = 0;
            if (++failures % 200u == 1u) {
                ESP_LOGW(TAG, "HBI read failed (%u in a row) - the ZL38063 key path is not "
                              "readable right now", failures);
            }
            continue;
        }

        if (status != prev_status) {
            if (B220_BTN_STATUS_IS_PRESS(status)) {
                handle_press(status);
            } else if (status == B220_BTN_STATUS_RELEASE) {
                /* The interface's acknowledgement that the event was consumed.
                 * It is NOT a user action, and it carries no duration. */
                s_stats.releases++;
            } else if (status != B220_BTN_STATUS_IDLE) {
                ESP_LOGD(TAG, "status 0x%04X is neither idle, press nor release",
                         (unsigned)status);
            }
            prev_status = status;
        }
    }
}

esp_err_t b220_buttons_start(b220_button_callback_t on_action)
{
    if (s_task != nullptr) {
        ESP_LOGW(TAG, "already started - the poller is created once per boot");
        return ESP_ERR_INVALID_STATE;
    }
    s_on_action = on_action;
    memset(&s_stats, 0, sizeof(s_stats));

    /* GPIO38: input, falling edge.  It is an input-only pad (34..39 have no
     * internal pull-up) and on this board it is not a reliable key signal at all
     * (it reads LOW at boot and HIGH while a key is held).  The handler only
     * counts edges for diagnostics; the poll loop neither waits for it nor is
     * speed up by it, so a noisy line cannot affect anything. */
    gpio_config_t int_cfg = {};
    int_cfg.pin_bit_mask = 1ULL << B220_BTN_INT_GPIO;
    int_cfg.mode = GPIO_MODE_INPUT;
    int_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    int_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    int_cfg.intr_type = GPIO_INTR_NEGEDGE;
    const esp_err_t gpio_err = gpio_config(&int_cfg);
    if (gpio_err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d input config failed (%s); the poll loop still works",
                 (int)B220_BTN_INT_GPIO, esp_err_to_name(gpio_err));
    }

    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err == ESP_OK || isr_err == ESP_ERR_INVALID_STATE) {
        (void)gpio_isr_handler_remove(B220_BTN_INT_GPIO);
        isr_err = gpio_isr_handler_add(B220_BTN_INT_GPIO, button_int_isr, nullptr);
        if (isr_err == ESP_OK) {
            s_irq_installed = true;
            ESP_LOGI(TAG, "GPIO%d falling-edge IRQ armed (a 'look now' hint only; the line reads "
                          "level %d and is HIGH while a key is held)",
                     (int)B220_BTN_INT_GPIO, gpio_get_level(B220_BTN_INT_GPIO));
        } else {
            ESP_LOGW(TAG, "GPIO%d ISR add failed (%s); falling back to polling only",
                     (int)B220_BTN_INT_GPIO, esp_err_to_name(isr_err));
        }
    } else {
        ESP_LOGW(TAG, "GPIO ISR service install failed (%s); falling back to polling only",
                 esp_err_to_name(isr_err));
    }

    if (xTaskCreate(button_task, "b220_buttons", B220_BTN_TASK_STACK, nullptr,
                    B220_BTN_TASK_PRIO, &s_task) != pdPASS) {
        s_task = nullptr;
        ESP_LOGE(TAG, "could not create the key poller task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void b220_buttons_get_stats(b220_buttons_stats_t* out)
{
    if (out != nullptr) {
        *out = s_stats;
    }
}
