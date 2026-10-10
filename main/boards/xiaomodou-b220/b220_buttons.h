/*
 * b220_buttons.h - the B220's three physical keys (record / volume+ / volume-).
 *
 * HOW THEY ARE WIRED (reverse engineered, then confirmed by hardware AND by an
 * independent open-source port of the same toy, ref/Flower_modou)
 * -------------------------------------------------------------------------
 * The keys are NOT on an ESP32 GPIO and NOT on the AW9106B.  An audio
 * front-end (ZL38063) reports them over the HBI link this port already drives
 * the DSP with, and pulls GPIO38 DOWN when it has something for the host:
 *
 *   GPIO38              idle HIGH; a falling edge announces a key event
 *                       (the front-end's interrupt line - input only, no
 *                       internal pull-up, so it is driven, not floating)
 *   HBI 0x0000 "status" 0x0000 idle; 0x0003 press; 0x0002 release
 *                       -> bit0 set == a real PRESS, bit0 clear == a RELEASE
 *   HBI 0x0002 "key"    the key code, valid only while status != 0:
 *                       0x0008 = volume up, 0x000A = volume down,
 *                       0x000B = record / MIC (the talk key)
 *
 * This mapping is confirmed twice over: it is what this port measured on
 * hardware (`key(0x0002)=0x0008/0x000A/0x000B`) and it is exactly what
 * Flower_modou's modou_dsp_keys.c implements
 * (`case 0x08: return "VOL+"; case 0x0a: return "VOL-"; case 0x0b: return "MIC";`).
 *
 * WHY status BIT0 MATTERS
 * -----------------------
 * One physical press produces TWO events: a press (status bit0 = 1, key code
 * present) and a release (status = 0x0002, bit0 = 0).  Acting on both is what
 * made an early revision of this driver fire every function twice.  The driver
 * therefore acts only on the PRESS event.
 *
 * WHAT THE EARLIER REVISIONS GOT WRONG (kept as a warning)
 * -------------------------------------------------------
 *   1. looked the key up in the register that actually carries the press/release
 *      flag, so every press was reported as an unconfigured code;
 *   2. treated "key value != idle" as "button held", but this is an EVENT
 *      interface - the value is presented once;
 *   3. acted on the release event as well, so one press ran its action twice;
 *   4. tried to time a hold from a signal that does not carry the hold.
 *
 * THERE IS NO LONG PRESS ON THIS HARDWARE (measured)
 * --------------------------------------------------
 * A deliberate 4-second hold and a 0.3-second tap produce the SAME sequence:
 * one 0x0003 carrying the key code, then nothing but 0x0000 until the interface
 * reports 0x0002 about 140..200 ms later - independent of how long the key was
 * actually held.  GPIO38 reads HIGH throughout the hold: it is a "something
 * happened" hint, not a "still held" line.
 *
 * The hold DURATION is therefore not observable, and a long press cannot be
 * implemented.  The three failed attempts (including three wrong theories) are
 * recorded in docs/B220-LED-AND-BASE-UART.md §2.6; the driver now offers exactly
 * one action per press.
 *
 * Threading: the callbacks run on the poller task, never on the main event loop,
 * so a callback must not block.  The board's callbacks use Application's
 * thread-safe entry points only.
 */

#ifndef B220_BUTTONS_H
#define B220_BUTTONS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One call per confirmed key action, from the poller task. */
typedef enum {
    B220_BTN_ACTION_RECORD = 0,   /* the record / talk key (front-end code 0x0B) */
    B220_BTN_ACTION_VOLUME_UP,    /* 0x08                                        */
    B220_BTN_ACTION_VOLUME_DOWN,  /* 0x0A                                        */
} b220_button_action_t;

/*
 * There is deliberately no press-duration parameter: the hardware does not report
 * one (see the header comment), and a "long" flag that can never be true would
 * only mislead the next reader.
 */
typedef void (*b220_button_callback_t)(b220_button_action_t action);

/*
 * Start the poller.  Creates one task; returns immediately.  Safe to call once
 * per boot, from the board constructor: it only touches the HBI link, which
 * zl38063_dsp_begin() has already attached by then.
 *
 * @param on_action  called once per confirmed key action (may be NULL, in which
 *                   case actions are only logged).
 */
esp_err_t b220_buttons_start(b220_button_callback_t on_action);

/*
 * Diagnostics: how many presses this driver has seen, and the last key/status
 * it decoded.  Read-only, safe from any task.
 */
typedef struct {
    uint32_t presses;         /* press events accepted                          */
    uint32_t releases;        /* release events ignored (they are not actions)  */
    uint32_t unknown_keys;    /* presses whose code matches no configured key   */
    uint16_t last_key;        /* HBI 0x0002 of the last accepted press          */
    uint16_t last_status;     /* HBI 0x0000 of the last accepted press          */
} b220_buttons_stats_t;
void b220_buttons_get_stats(b220_buttons_stats_t* out);

/*
 * The measured properties of this interface, kept as constants so the facts live
 * in one place instead of being scattered through the driver:
 *   - exactly one press event per physical press (never two);
 *   - 0x0002 arrives 140..200 ms after the press event, whatever the real hold
 *     duration was;
 *   - GPIO38 is HIGH while a key is held;
 *   - the hold duration is NOT readable through this interface.
 */
#define B220_BUTTON_MEASURED_RELEASE_DELAY_MS_MIN    140
#define B220_BUTTON_MEASURED_RELEASE_DELAY_MS_MAX    200
#define B220_BUTTON_MEASURED_HOLD_DURATION_READABLE  0

#ifdef __cplusplus
}
#endif

#endif /* B220_BUTTONS_H */
