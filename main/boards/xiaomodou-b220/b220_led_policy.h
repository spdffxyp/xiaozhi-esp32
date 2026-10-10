/*
 * b220_led_policy.h - what the RGB indicator SHOWS (see b220_led_policy.c for the
 * policy itself: red = no cloud connection, white breathing = listening or
 * speaking, dark = ready and idle).
 *
 * Split from b220_led.h on purpose: b220_led.* is a hardware driver that knows
 * AW9106B registers, this is product behaviour that knows device states.  Keeping
 * them apart means the policy can be changed without going near the light driver,
 * and the driver can be tested without a state machine.
 */

#ifndef B220_LED_POLICY_H
#define B220_LED_POLICY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the policy task.  Call once per boot, after b220_led_start(): it samples
 * Application::GetDeviceState() and drives the light.  Returns ESP_ERR_INVALID_STATE
 * if it is already running.
 */
esp_err_t b220_led_policy_start(void);

#ifdef __cplusplus
}
#endif

#endif /* B220_LED_POLICY_H */
