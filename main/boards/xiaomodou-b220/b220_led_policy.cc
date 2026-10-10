/*
 * b220_led_policy.c - what the RGB indicator light SHOWS, as opposed to how it is
 * driven (b220_led.c owns the AW9106B registers).
 *
 * THE POLICY (asked for from the bench, 2026-10-10)
 * ------------------------------------------------
 *   no network / still coming up   RED, solid
 *   listening or speaking          WHITE, breathing (fade in and out)
 *   anything else (ready and idle) dark
 *
 * Why red rather than a pattern: "there is no cloud connection" is a condition the
 * user needs to notice, and it is the one state the toy cannot express any other
 * way - there is no text on screen for it, and the device cannot talk about it
 * without a connection.
 *
 * WHY THIS IS A POLLER AND NOT A HOOK
 * -----------------------------------
 * The device state machine lives in Application, which this board must not reach
 * into for callbacks (Board -> Application is the wrong direction, and the audio
 * and protocol tasks must not be blocked by an I2C write).  A small task that
 * samples Application::GetDeviceState() is the narrow way to do it: it reads one
 * atomic, and a 50 ms sampling period is far finer than a human notices.
 *
 * INTERACTION WITH THE KEY FEEDBACK
 * ---------------------------------
 * b220_led.c's pulses (key acknowledgements) win: this policy stays quiet while
 * one is running, and takes over again when it ends.  Otherwise a key press during
 * "no network" would be swallowed by the red light.
 */

#include "b220_led.h"
#include "b220_led_policy.h"

#include "application.h"
#include "config.h"
#include "device_state.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "b220-policy";

#define B220_POLICY_TASK_STACK   3072
#define B220_POLICY_TASK_PRIO    2
#define B220_POLICY_TICK_MS      50

/* One full breath (up and back down) in milliseconds. */
#define B220_POLICY_BREATH_MS    1600
/* The breathing never goes fully dark: a light that reaches zero looks like a
 * fault rather than a heartbeat. */
#define B220_POLICY_BREATH_FLOOR 0x20

static TaskHandle_t s_task = nullptr;

/*
 * Is the cloud connection usable?  A device that is starting, reconnecting or
 * sitting in the provisioning AP has no cloud, which is exactly what the red light
 * means.  Idle/listening/speaking/notifying all imply a live connection; the
 * remaining states (upgrading, activating, audio testing) are "busy, not normal
 * operation", which is also better said with the same red than with darkness.
 */
static bool policy_no_network(DeviceState st)
{
    switch (st) {
    case kDeviceStateStarting:
    case kDeviceStateWifiConfiguring:
    case kDeviceStateConnecting:
    case kDeviceStateActivating:
    case kDeviceStateUpgrading:
    case kDeviceStateFatalError:
        return true;
    default:
        return false;
    }
}

static bool policy_voice_active(DeviceState st)
{
    return st == kDeviceStateListening || st == kDeviceStateSpeaking ||
           st == kDeviceStateNotifying;
}

/*
 * A triangle wave in 0..255 for the breathing level, scaled so it never falls
 * below B220_POLICY_BREATH_FLOOR.
 */
static uint8_t policy_breath_level(int64_t now_us)
{
    const int64_t phase = now_us % ((int64_t)B220_POLICY_BREATH_MS * 1000);
    const int64_t half = (int64_t)B220_POLICY_BREATH_MS * 1000 / 2;
    /* 0..half counts up, half..end counts down. */
    const int64_t t = (phase < half) ? phase : (((int64_t)B220_POLICY_BREATH_MS * 1000) - phase);
    const int64_t span = 255 - B220_POLICY_BREATH_FLOOR;
    return (uint8_t)(B220_POLICY_BREATH_FLOOR + (t * span) / half);
}

static void policy_apply(DeviceState st, bool force)
{
    /*
     * Only the MODE is cached.  The state itself is logged when the mode changes,
     * so a second copy of it here would be state that nothing reads (the compiler
     * says so, and it is right).
     */
    static int last_mode = -1;   /* 0 = dark, 1 = red, 2 = white breathing */

    const bool no_net = policy_no_network(st);
    const bool voice = policy_voice_active(st);
    const int mode = no_net ? 1 : (voice ? 2 : 0);

    if (!force && mode == last_mode) {
        /* Same mode: only the breathing needs continuous updates. */
        if (mode == 2 && !b220_led_fx_active()) {
            const uint8_t level = policy_breath_level(esp_timer_get_time());
            (void)b220_led_set_level(B220_LED_WHITE, level);
        }
        return;
    }

    if (b220_led_fx_active()) {
        /* A key acknowledgement is in flight; let it finish before changing the
         * policy (the mode is re-evaluated on the next tick). */
        return;
    }

    last_mode = mode;

    switch (mode) {
    case 1:
        (void)b220_led_set_color(B220_LED_RED);
        ESP_LOGI(TAG, "light: RED (no cloud connection; device state %s)",
                 DeviceStateMachine::GetStateName(st));
        break;
    case 2:
        (void)b220_led_set_level(B220_LED_WHITE, B220_POLICY_BREATH_FLOOR);
        ESP_LOGI(TAG, "light: WHITE breathing (device state %s)",
                 DeviceStateMachine::GetStateName(st));
        break;
    default:
        (void)b220_led_set_color(B220_LED_OFF);
        ESP_LOGD(TAG, "light: dark (ready, device state %s)",
                 DeviceStateMachine::GetStateName(st));
        break;
    }
}

static void policy_task(void* arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(B220_POLICY_TICK_MS));
        const DeviceState st = Application::GetInstance().GetDeviceState();
        policy_apply(st, false);
    }
}

esp_err_t b220_led_policy_start(void)
{
    if (s_task != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(policy_task, "b220_led_policy", B220_POLICY_TASK_STACK, nullptr,
                    B220_POLICY_TASK_PRIO, &s_task) != pdPASS) {
        s_task = nullptr;
        ESP_LOGW(TAG, "could not create the light-policy task; the light stays on the idle colour");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "light policy up: RED = no cloud connection, WHITE breathing = listening/"
                  "speaking, dark = ready and idle (key presses still flash on top)");
    return ESP_OK;
}
