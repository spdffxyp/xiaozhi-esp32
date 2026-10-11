/*
 * b220_led_policy.c - what the RGB indicator light SHOWS, as opposed to how it is
 * driven (b220_led.c owns the AW9106B registers).
 *
 * THE POLICY (agreed with the bench, 2026-10-10)
 * ---------------------------------------------
 *   starting / provisioning / connecting   RED breathing     "trying"
 *   connected but no cloud / reconnect     RED solid         "needs attention"
 *   firmware upgrade                       MAGENTA breathing "do not power off"
 *   LISTENING (the user is talking)        GREEN breathing   "I am listening"
 *   SPEAKING / notifying (the toy talks)   WHITE breathing   "I am answering"
 *   ready and idle                         BLUE, very dim    "I am on and waiting"
 *   fatal error                            RED fast flash    "something is wrong"
 *
 * One colour, one meaning: red always means "a problem with getting connected",
 * green the user's turn, white the toy's turn, magenta maintenance, blue standby.
 * Breathing means "in progress / waiting"; steady means "you need to do something";
 * fast flashing means "this is a fault".  The listening and speaking colours are
 * deliberately different because on a toy the difference between "it is listening
 * to me" and "it is answering" is the single most useful thing the light can say -
 * and a user who is talking cannot see the screen.
 *
 * Idle is a dim blue rather than off: "off" and "on but idle" would otherwise look
 * identical, and the user cannot tell whether the toy is alive without touching a
 * key.  It is kept at B220_POLICY_IDLE_LEVEL (about an eighth) so it works as an
 * ambient standby light instead of a lamp.
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

#include <wifi_manager.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "b220-policy";

#define B220_POLICY_TASK_STACK   3072
#define B220_POLICY_TASK_PRIO    2
#define B220_POLICY_TICK_MS      50

/* One full breath (up and back down) in milliseconds.  Slower for the "trying"
 * states (they can last many seconds and a fast pulse reads as panic), faster for
 * voice states (they are short and the motion should read as activity). */
#define B220_POLICY_BREATH_MS        1600   /* voice: green listening / white speaking */
#define B220_POLICY_BREATH_SLOW_MS   3000   /* red "trying", magenta upgrading        */
/* Breathing never reaches zero: a light that goes out looks like a fault rather
 * than a heartbeat. */
#define B220_POLICY_BREATH_FLOOR     0x20
/* Idle standby level: dim enough to be ambient, bright enough to prove "I am on". */
#define B220_POLICY_IDLE_LEVEL       0x1F   /* ~12% of the 8-bit range */
/* How often a fault light flashes. */
#define B220_POLICY_FAULT_PERIOD_MS  400

static TaskHandle_t s_task = nullptr;

/*
 * The REAL link state.
 *
 * The device state machine is not sufficient on its own: Application keeps its
 * state when the network drops (HandleNetworkDisconnectedEvent() does not move it),
 * so "connected, then lost the AP" would otherwise still show the idle light - and
 * that is exactly the case this policy exists for.
 *
 * WifiManager::IsConnected() is the authoritative, synchronous answer, so the policy
 * asks it directly instead of inferring the link from events.  Two earlier attempts
 * here are worth remembering:
 *   1. shadowing WifiBoard::OnNetworkEvent does NOT work - it is not virtual, and
 *      WifiBoard calls it on a WifiBoard* from its own WifiManager lambda, so the
 *      derived version is never reached (it compiles and silently does nothing);
 *   2. recording the state in Application's event bits would mean reaching into
 *      Application's internals for a board's indicator light.
 * A plain public query avoids both.  This does tie the policy to WiFi, which is
 * what this board is; a cellular variant would query its own manager instead.
 */
static bool s_link_ever_up = false;   /* for the log line only, see policy_apply */

/*
 * What the light should be doing.  Every device state maps to exactly one of
 * these; the switch in policy_mode_for() is the whole policy in one place.
 */
typedef enum {
    B220_LIGHT_OFF = 0,
    B220_LIGHT_IDLE,        /* dim blue: on, waiting                        */
    B220_LIGHT_TRYING,      /* red breathing: no cloud yet, working on it   */
    B220_LIGHT_NO_CLOUD,    /* red solid: gave up / needs the user          */
    B220_LIGHT_UPGRADING,   /* magenta breathing: do not power off          */
    B220_LIGHT_LISTENING,   /* green breathing: the user's turn             */
    B220_LIGHT_SPEAKING,    /* white breathing: the toy's turn              */
    B220_LIGHT_FAULT,       /* red fast flash: something is broken          */
} b220_light_t;

static b220_light_t policy_mode_for(DeviceState st)
{
    switch (st) {
    /* No cloud yet, and it is being worked on. */
    case kDeviceStateStarting:
    case kDeviceStateWifiConfiguring:
    case kDeviceStateConnecting:
        return B220_LIGHT_TRYING;

    /* Activating talks to the server for the first time: a cloud round trip, not a
     * fault, so it gets the maintenance colour rather than red. */
    case kDeviceStateActivating:
    case kDeviceStateUpgrading:
        return B220_LIGHT_UPGRADING;

    /* The two turns of a conversation, deliberately different colours. */
    case kDeviceStateListening:
        return B220_LIGHT_LISTENING;
    case kDeviceStateSpeaking:
    case kDeviceStateNotifying:
        return B220_LIGHT_SPEAKING;

    case kDeviceStateFatalError:
        return B220_LIGHT_FAULT;

    /* Audio testing is a bring-up mode; it has no user-facing meaning, so it stays
     * on the standby light rather than inventing a colour for it. */
    case kDeviceStateIdle:
    case kDeviceStateAudioTesting:
    default:
        return B220_LIGHT_IDLE;
    }
}

/* The colour and period of a breathing mode. */
static void policy_breath_params(b220_light_t mode, b220_led_rgb_t* color, int64_t* period_us)
{
    switch (mode) {
    case B220_LIGHT_TRYING:
        *color = B220_LED_RED;
        *period_us = (int64_t)B220_POLICY_BREATH_SLOW_MS * 1000;
        break;
    case B220_LIGHT_UPGRADING:
        *color = B220_LED_MAGENTA;
        *period_us = (int64_t)B220_POLICY_BREATH_SLOW_MS * 1000;
        break;
    case B220_LIGHT_LISTENING:
        *color = B220_LED_GREEN;
        *period_us = (int64_t)B220_POLICY_BREATH_MS * 1000;
        break;
    default:
        *color = B220_LED_WHITE;
        *period_us = (int64_t)B220_POLICY_BREATH_MS * 1000;
        break;
    }
}

/*
 * A triangle wave in B220_POLICY_BREATH_FLOOR..255: up over the first half of the
 * period, down over the second.
 */
static uint8_t policy_breath_level(int64_t now_us, int64_t period_us)
{
    const int64_t phase = now_us % period_us;
    const int64_t half = period_us / 2;
    const int64_t t = (phase < half) ? phase : (period_us - phase);
    const int64_t span = 255 - B220_POLICY_BREATH_FLOOR;
    return (uint8_t)(B220_POLICY_BREATH_FLOOR + (t * span) / half);
}

/* Steady colours, and the fault flash. */
static bool policy_fault_on(int64_t now_us)
{
    return ((now_us / 1000) % B220_POLICY_FAULT_PERIOD_MS) < (B220_POLICY_FAULT_PERIOD_MS / 2);
}

/* The name of a mode, for the log.  Keeping it next to the policy means a new mode
 * cannot be added without the log naming it. */
static const char* policy_mode_name(b220_light_t mode)
{
    switch (mode) {
    case B220_LIGHT_OFF:       return "OFF";
    case B220_LIGHT_IDLE:      return "BLUE dim (ready, waiting)";
    case B220_LIGHT_TRYING:    return "RED breathing (no cloud yet)";
    case B220_LIGHT_NO_CLOUD:  return "RED solid (no cloud)";
    case B220_LIGHT_UPGRADING: return "MAGENTA breathing (do not power off)";
    case B220_LIGHT_LISTENING: return "GREEN breathing (listening)";
    case B220_LIGHT_SPEAKING:  return "WHITE breathing (speaking)";
    case B220_LIGHT_FAULT:     return "RED flashing (fault)";
    default:                   return "?";
    }
}

static void policy_apply(DeviceState st, bool force)
{
    /*
     * Only the MODE is cached: the device state is logged when the mode changes, so
     * a second copy of it here would be state nothing reads.
     */
    static b220_light_t last_mode = B220_LIGHT_OFF;
    static bool last_valid = false;

    b220_light_t mode = policy_mode_for(st);
    const int64_t now_us = esp_timer_get_time();

    /*
     * Link-aware refinement.  The state name alone says "idle/listening/speaking",
     * which is only the truth while the cloud link is up.
     *
     *   link down, never connected yet  -> keep "trying" (still provisioning)
     *   link down, but the device is in a state that assumes the cloud
     *                                   -> "no cloud": the user has to be told
     */
    const bool link_up = WifiManager::GetInstance().IsConnected();
    if (link_up) {
        s_link_ever_up = true;
    } else {
        if (!s_link_ever_up) {
            mode = B220_LIGHT_TRYING;      /* still provisioning: it is being worked on */
        } else if (mode == B220_LIGHT_IDLE || mode == B220_LIGHT_SPEAKING ||
                   mode == B220_LIGHT_LISTENING) {
            mode = B220_LIGHT_NO_CLOUD;    /* had a link, lost it: the user must know */
        }
    }

    /* A key-press acknowledgement outranks the policy: it is the user's own action
     * being confirmed, and it lasts a fraction of a second.  The policy resumes on
     * the next tick without re-logging, which is why this check comes before the
     * "mode unchanged" test. */
    if (b220_led_fx_active()) {
        return;
    }

    if (!force && last_valid && mode == last_mode) {
        /* Same mode: only the animated ones need continuous updates; the steady ones
         * have already been written. */
        switch (mode) {
        case B220_LIGHT_TRYING:
        case B220_LIGHT_UPGRADING:
        case B220_LIGHT_LISTENING:
        case B220_LIGHT_SPEAKING: {
            b220_led_rgb_t color;
            int64_t period_us;
            policy_breath_params(mode, &color, &period_us);
            (void)b220_led_set_level(color, policy_breath_level(now_us, period_us));
            break;
        }
        case B220_LIGHT_FAULT:
            (void)b220_led_set_level(B220_LED_RED,
                                     policy_fault_on(now_us) ? B220_LED_BRIGHTNESS_MAX : 0x00);
            break;
        default:
            break;
        }
        return;
    }

    last_mode = mode;
    last_valid = true;

    switch (mode) {
    case B220_LIGHT_TRYING:
    case B220_LIGHT_UPGRADING:
    case B220_LIGHT_LISTENING:
    case B220_LIGHT_SPEAKING: {
        b220_led_rgb_t color;
        int64_t period_us;
        policy_breath_params(mode, &color, &period_us);
        (void)b220_led_set_level(color, B220_POLICY_BREATH_FLOOR);
        break;
    }
    case B220_LIGHT_IDLE:
        /* Dim, not off: "on and waiting" must be distinguishable from "dead". */
        (void)b220_led_set_level(B220_LED_BLUE, B220_POLICY_IDLE_LEVEL);
        break;
    case B220_LIGHT_NO_CLOUD:
        (void)b220_led_set_color(B220_LED_RED);
        break;
    case B220_LIGHT_FAULT:
        (void)b220_led_set_level(B220_LED_RED, B220_LED_BRIGHTNESS_MAX);
        break;
    case B220_LIGHT_OFF:
    default:
        (void)b220_led_set_color(B220_LED_OFF);
        break;
    }

    /*
     * Logged at INFO, including the idle state.  The idle line was ESP_LOGD at first
     * and therefore never appeared - the runtime level filters DEBUG - which made
     * "the light went to its standby colour" invisible in a trace even though it was
     * working.  On a board whose only user feedback is this light, being able to see
     * every policy transition is worth one line per state change.
     */
    ESP_LOGI(TAG, "light: %s (device state %s)", policy_mode_name(mode),
             DeviceStateMachine::GetStateName(st));
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
    ESP_LOGI(TAG, "light policy up: GREEN breathing = listening, WHITE breathing = speaking,");
    ESP_LOGI(TAG, "                  RED breathing = connecting, RED solid = no cloud, MAGENTA = ");
    ESP_LOGI(TAG, "                  upgrading, BLUE dim = ready and idle, RED flash = fault");
    ESP_LOGI(TAG, "                  (key presses - white/green/red - always win for their moment)");
    return ESP_OK;
}
