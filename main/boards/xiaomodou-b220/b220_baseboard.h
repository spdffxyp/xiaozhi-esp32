/*
 * b220_baseboard.h - drive the baseboard's motors and lights, safely.
 *
 * WHAT WAS MISSING
 * ----------------
 * b220_base_uart.c could put a frame on the wire, but NOTHING ever called it: the motors
 * and lights had no entry point at all.  This module is that entry point, and it exists
 * as a separate file because everything between "a frame" and "a toy that moves" is
 * safety policy rather than protocol:
 *
 *   LIMITS   power and duration are capped, and a value outside the documented range is
 *            CLAMPED with a log line rather than rejected, so a vague request still does
 *            something sensible instead of failing.
 *   HEARTBEAT the baseboard's motion frames are only valid for a moment (the reference
 *            project measured 800 ms) and it expects a steady stream while moving.  A
 *            long move is therefore refreshed every B220_BASE_MOTION_REFRESH_MS by a task
 *            of its own, so a two-second move is smooth instead of a series of jerks.
 *   TIMEOUT  the safety net.  If nothing has refreshed the motion for
 *            B220_BASE_MOTION_TIMEOUT_MS, the wheels are stopped.  That covers every way
 *            this can go wrong at once - a lost network, a stuck task, a bug in the move
 *            task - without any of them needing to be anticipated.  The reference project
 *            warns that a baseboard which never receives a stop keeps driving.
 *
 * WHY A TASK AND NOT A BLOCKING CALL
 * ----------------------------------
 * The MCP callbacks run on the protocol task and must return promptly, so they only set
 * the request; the actual streaming happens in this module's task, which is also what
 * makes STOP able to interrupt a move that is already running.
 *
 * WHAT IS NOT VERIFIED
 * --------------------
 * The frames themselves are copied byte for byte from the reference project's validated
 * test page (see b220_base_uart.c), but NONE of this has been tried against a real
 * baseboard by this port: the motor directions, the steering range and the light colour
 * mapping are the reference's numbers, not measurements of this unit.  The first bench
 * session should treat "which way does 90 turn" as an open question.
 */

#ifndef B220_BASEBOARD_H
#define B220_BASEBOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the motion task and the timeout guard.  Call once, after the baseboard link is up
 * (the task waits for the link itself, so ordering is not critical).
 */
esp_err_t b220_baseboard_start(void);

/*
 * Drive for `duration_ms`, refreshing the frame until the time is up, then stop.
 *
 * `direction` is one of the strings the MCP tool accepts ("forward", "backward", "left",
 * "right"); `power` is 1..100 and `duration_ms` is capped.  Returns immediately: the
 * motion runs on this module's task.  A new request replaces the one in progress.
 */
esp_err_t b220_baseboard_move(const char* direction, int power, int duration_ms);

/* Stop now, whatever is running, and cancel any pending move. */
esp_err_t b220_baseboard_stop(void);

/* Set every light to one colour ("off", "white", "red", "yellow", "green", "blue",
 * "deepblue", "purple").  Returns immediately. */
esp_err_t b220_baseboard_lights(const char* color);

/*
 * Send exactly ONE light frame, with no repeat.  A diagnostic: the persistent path re-sends,
 * so it cannot show whether a light frame expires by itself or latches.  Time how long the
 * light stays on after this.
 */
esp_err_t b220_baseboard_lights_once(const char* color);

/*
 * True while the normal move path is driving.
 *
 * The bench tools built on this module ({BASE+RAW} and friends) loop for a fixed window, and
 * that window turned out to be UNINTERRUPTIBLE: {BASE+STOP} went to the normal path, the loop
 * carried on sending motion frames, and the bench had to repeat the stop until the window
 * expired by itself.  A toy that cannot be stopped is a fault rather than an inconvenience,
 * so those loops now poll this and bail out as soon as the normal path says "not moving".
 */
bool b220_baseboard_is_moving(void);

/*
 * A counter that increments on every stop.
 *
 * Bench tools that build their own frames run OUTSIDE the normal move state, so
 * b220_baseboard_is_moving() says nothing about them.  Such a loop should record this value
 * when it starts and stop as soon as it changes: that makes {BASE+STOP} able to cancel it,
 * including a stop that arrives before the loop's first frame.
 */
uint32_t b220_baseboard_stop_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* B220_BASEBOARD_H */
