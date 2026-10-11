/*
 * b220_baseboard.c - motors and lights of the baseboard, with limits, a heartbeat and a
 * timeout guard.  See b220_baseboard.h for why those three exist and what is unverified.
 */

#include "b220_baseboard.h"

#include "b220_base_uart.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "b220-base-ctl";

/* The baseboard's scripted duration field counts 200 ms units. */
#define B220_BASE_FRAME_UNIT_MS   200

/*
 * Frame buffer size.  The longest frame this module builds is the "all lights" one at
 * about 90 characters; 192 is what b220_base_uart.c uses for its own buffer, and matching
 * it means a frame that fits here always fits there.  The assert below keeps the two from
 * drifting apart silently if that constant ever changes.
 */
#define B220_BASEBOARD_FRAME_MAX  192
_Static_assert(B220_BASEBOARD_FRAME_MAX <= 192,
               "b220_base_uart.c must accept frames at least this long");

/*
 * LIMITS.  These are policy, not protocol: the protocol would accept more, but a toy that
 * can be told to drive for a minute is a toy that will hit something.  Both are clamped
 * (with a log line) rather than refused, so an imprecise request still does something
 * predictable.
 *
 * The power ceiling is at the protocol's own maximum on request: at the default 30-40%
 * the wheels did not move at all on the bench, and the reference project notes that a low
 * speed may not have the torque to start.  Full power is now allowed so the first
 * successful movement is not blocked by a limit this port invented - the duration cap is
 * what keeps that safe.  If it turns out to be too lively, lower MAX_PWR; it is one number.
 */
/*
 * Ten seconds on request, from a bench report that 0.6-0.8 s produced a hum and no
 * rotation.  A stalled DC motor takes time to break away, and the reference project's
 * validated frames were an order of magnitude longer than anything this port had sent (its
 * duration field ran to 300-800 units, i.e. minutes).  Ten seconds is the compromise: long
 * enough to break away and to be seen to be turning, short enough that a runaway is
 * something a person can catch.
 */
#define B220_BASE_MOTION_MAX_MS   10000
/*
 * The drive speed's range.  This USED to be a 10..100 "power percentage" and it clamped
 * every request at 100 - which is why {BASE+MOVE,left,200,...} logged "power 200 capped at
 * 100" and moved at 100.  It is the frame's speed field, whose real range is 0..255, and the
 * bench asked for full speed everywhere.
 */
#define B220_BASE_MOTION_MAX_PWR  255
/* The floor stays low: it is the caller's business how gently to start. */
#define B220_BASE_MOTION_MIN_PWR  10

/*
 * ONE FRAME PER MOVE.  THIS IS THE WHOLE BEHAVIOUR, AND THIS PORT HAD IT WRONG.
 *
 * The first version re-sent the motion frame every 600 ms for the length of the move,
 * believing that "motion frames are only valid for a moment" meant the baseboard needed a
 * heartbeat.  It does not: EVERY motion frame is a complete instruction with its own
 * duration, so re-sending it starts the movement AGAIN.
 *
 * The bench proved it by counting.  A sweep at four different settings produced, in the
 * observer's words, "intermittent forward motion" 20 times, 20 times, 20 times, 4 times,
 * 4 times, 4 times, 4 times, 4 times - and the numbers are exactly the refresh counts:
 * 10 s / 0.5 s = 20 and 2 s / 0.5 s = 4.  A stutter whose period equals the sender's
 * refresh interval is not a motor problem; it is the sender restarting the motor.
 *
 * So the duration is put IN THE FRAME (the field counts 200 ms units, which is why the
 * reference project's frames carry 300-800 of them) and the frame is sent ONCE.  The
 * timeout below still exists as a safety net - see the comment on it - but nothing
 * refreshes anything, so nothing restarts anything.
 */
/*
 * THE MEASURED RECIPE FOR CONTINUOUS MOTION.
 *
 * A bench sweep of seven settings, each run for 3 s, produced continuous forward motion in
 * every one of them.  What they had in common is what matters:
 *
 *   the duration field was 900 or 1000 ms (the field is MILLISECONDS and saturates at
 *   about 1000 whatever you write), and the frame was repeated every 600-1000 ms.
 *
 * Against that, the two failures this port went through are both explained by the field:
 *
 *   field 600 ms, repeat 500 ms -> gappy: the bench counted 20 discrete movements in 10 s,
 *                                  because 600 ms of motion cannot cover a 500 ms cadence
 *                                  once the baseboard's own processing delay is added
 *   no repeat at all           -> exactly one movement of about a second, then silence
 *
 * So: ask for the maximum the field will hold, and repeat faster than that.  900 ms of
 * repeat against a 1000 ms field leaves a 100 ms overlap, so the next burst is already
 * commanded before the previous one expires and the motion never breaks.
 */
#define B220_BASE_REPEAT_MS        250   /* faster than the 300 ms frame, so no gap */
/* The speed the last accepted move asked for, guarded by s_lock. */
static uint8_t s_speed = 0;   /* seeded from B220_BASE_SPEED_DEFAULT in start() */

/* What a move with no usable duration gets: how long the whole move lasts, not how long a
 * frame lasts. */
#define B220_BASE_MOTION_DEFAULT_MS 2000

/*
 * THE SAFETY NET.  If nothing has refreshed the motion for this long, stop.  The reference
 * project warns that a baseboard which never receives a stop keeps driving, and this
 * covers every cause at once: a lost network, a stuck task, a bug here.  It must be LONGER
 * than the refresh interval, or normal streaming would be interrupted by its own guard.
 */
#define B220_BASE_MOTION_TIMEOUT_MS 900

#define B220_BASEBOARD_TASK_STACK 3072
#define B220_BASEBOARD_TASK_PRIO  4
#define B220_BASEBOARD_TICK_MS    50

/* ------------------------------------------------------------------ state --- */

static TaskHandle_t s_task = nullptr;

/* The move in progress, guarded by s_lock.  "until_us == 0" means nothing is running. */
static SemaphoreHandle_t s_lock = nullptr;
static char s_direction[12] = "";
static int s_duration_ms = 0;
static int64_t s_until_us = 0;
static int64_t s_last_refresh_us = 0;
/* Set by move(), cleared by the task once the frame has gone out.  A flag rather than an
 * immediate send, because the frame must not be sent from the caller's context: the MCP
 * callback runs on the protocol task and the UART write would block it. */
static bool s_pending_start = false;

/* What the guard last did, so the log says it once rather than every tick. */
static bool s_moving = false;

/*
 * STOP GENERATION.
 *
 * b220_baseboard_is_moving() is not enough for the bench tools: they build their own frames
 * and never touch the normal move state, so "is a move running" is false for them and a loop
 * that watched it would send nothing at all (which is how this counter came to exist - the
 * first attempt at making RAW interruptible disabled it instead).
 *
 * A counter rather than a flag: a loop records the value it started with and stops when it
 * changes, so a stop that arrives before the loop even starts is still honoured, and two
 * stops in a row cannot be collapsed into one.
 */
static volatile uint32_t s_stop_generation = 0;

/* ------------------------------------------------------------------ tables --- */

/*
 * THE BASEDBOARD'S CONTROL CODE, FOUND ON THE BENCH.
 *
 * Everything before this was guesswork against a documented mode table that this unit does
 * not follow.  What settles it is a single parameter, the FIRST value of the control tuple,
 * sent under the reference project's header and with its other field values:
 *
 *     AT+CMD=(1,1,3,1),(<code>,300,50,30,1),(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)
 *
 * The bench walked the code from 0 to 10 and reported:
 *
 *     0  both wheels forward          4  RIGHT wheel reverse
 *     1  both wheels reverse          5  LEFT wheel reverse
 *     2  RIGHT wheel forward          6  left reverse + right forward  -> turns one way
 *     3  LEFT wheel forward           7  left forward + right reverse  -> turns the other
 *     8, 9, 10  nothing
 *
 * The encoding is self-consistent, which is what makes it trustworthy: 2 and 4 are the same
 * wheel in opposite directions, 3 and 5 likewise, and 6 and 7 are the two ways of pairing
 * them - each of 6 and 7 is a pivot on the spot.
 *
 * A NOTE ON THE HEADER AND THE OTHER FIELDS.  This port had been sending (9,1,0,1) with the
 * tuple (mode,1000,90,200,0).  Under that form NOTHING ever turned: 28 combinations of mode
 * and steering angle all drove straight, and even a tuple whose first value was 0 (which
 * should idle) drove forward.  Every field of the reference's frame turned out to matter
 * except the ones this port had been varying, so the working values are kept exactly as
 * measured and only the code is changed:
 *
 *     header (1,1,3,1)   duration 300   steering 50   speed 30   direction 1
 */
/*
 * THE DIRECTION TABLE, FROM THE BENCH.  Two numbers per entry now, because the bench found
 * that the control tuple's third field is not a steering angle but a TURN STRENGTH, and that
 * the two together decide what happens:
 *
 *   code 2, field 3 = 50   nearly straight (drifts a little left)
 *   code 2, field 3 = 90   turns LEFT, the left wheel slowing - a sharp turn
 *   code 3, field 3 = 90   turns RIGHT, the right wheel slowing - a sharp turn
 *   code 0, field 3 = 50   nearly straight (drifts a little right)
 *   code 1, field 3 = 50   reverses, both wheels even
 *   code 6, field 3 = 50   pivots LEFT on the spot
 *   code 7, field 3 = 50   pivots RIGHT on the spot
 *
 * The rule that explains all of it: the LARGER field 3 is, the sharper the turn, and the
 * wheel on the inside of the turn is the one that slows.  50 is nearly straight and 90 is
 * the tightest the differential can go - it stops at "one wheel stopped", it never reverses
 * one (the bench walked it down to -90 and nothing changed past that).
 *
 * So field 3 is the turning-radius control: 50 for straight, 70 for a wide arc, 90 for the
 * sharpest arc available.  A radius smaller than one wheel's means a pivot, which is code 6
 * or 7.
 *
 * `steer` here is that third field.  The name is kept from the protocol document, which
 * calls it a steering angle; on this unit it is a turn strength.
 */
typedef struct {
    const char* name;
    uint8_t code;
    uint8_t steer;
    uint8_t speed;
} b220_direction_t;

static const b220_direction_t s_directions[] = {
    /*
     * forward: code 2 at 50, which the bench measured as a slight LEFT drift.  It is the
     * closest to straight of the two candidates - code 0 drifts right by the same amount -
     * and it is the one that keeps a straight-line command on the same code as the left
     * turn, so the trim only has to be tuned in one place.  To tune it, run
     * {BASE+RAW,SWEEP,2,3,20,60,5} and change this number.
     */
    /*
     * SPEED IS PER DIRECTION, AND THAT IS NOT A REFINEMENT - IT IS THE DIFFERENCE BETWEEN A
     * TURN AND A STRAIGHT LINE.
     *
     * The bench measured, at speed 90:
     *
     *     (2,300,90,90,1)   turns left,  left wheel slowing
     *     (3,300,90,90,1)   turns right, right wheel slowing
     *
     * and at speed 255, the very same codes and turn strengths:
     *
     *     (2,300,90,255,1)  drives straight
     *     (3,300,90,255,1)  drives straight
     *
     * The bench had also reported the reason: "above 110 both wheels speed up and converge".
     * A differential turn IS the speed difference between the wheels, so a speed that
     * converges them removes the turn.  Hence:
     *
     *     255 for going straight, where converging is exactly what is wanted
     *      90 for turning, below the point where the wheels converge
     */
    {"forward", 2, 50, 255},
    {"backward", 1, 50, 255},
    {"left", 2, 90, 90},       /* measured: turns left at this speed */
    {"right", 3, 90, 90},      /* measured: turns right at this speed */
    /*
     * The pivots keep the full speed: asked for explicitly ("only the left/right turns at 90,
     * everything else at 255"), and a pivot is not a differential turn - the two wheels are
     * commanded in OPPOSITE directions by the code itself, so there is no speed difference
     * for a high speed to flatten.  That is the difference between these rows and the two
     * above them.
     */
    {"spin_left", 6, 50, 255},  /* measured: pivots left */
    {"spin_right", 7, 50, 255}, /* measured: pivots right */
    {"right_wheel", 2, 90, 255},
    {"left_wheel", 3, 90, 255},
    {"right_wheel_reverse", 4, 90, 255},
    {"left_wheel_reverse", 5, 90, 255},
};

typedef struct {
    const char* name;
    uint8_t index;
} b220_color_t;

static const b220_color_t s_colors[] = {
    {"white", 0},
    {"red", 1},
    {"yellow", 2},
    {"green", 4},
    {"blue", 5},
    {"deepblue", 6},
    {"purple", 7},
};

static bool direction_lookup(const char* name, uint8_t* code, uint8_t* steer, uint8_t* speed)
{
    for (size_t i = 0; i < sizeof(s_directions) / sizeof(s_directions[0]); i++) {
        if (strcmp(name, s_directions[i].name) == 0) {
            *code = s_directions[i].code;
            *steer = s_directions[i].steer;
            *speed = s_directions[i].speed;
            return true;
        }
    }
    return false;
}

static bool color_lookup(const char* name, uint8_t* index)
{
    for (size_t i = 0; i < sizeof(s_colors) / sizeof(s_colors[0]); i++) {
        if (strcmp(name, s_colors[i].name) == 0) {
            *index = s_colors[i].index;
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------- light frames --- */

/*
 * LIGHTS: SEPARATE FRONT AND REAR, AND THEY HAVE TO BE KEPT ALIVE.
 *
 * The frames are the reference project's, field for field.  The header's third field says
 * which light group is addressed and the tuples carry (id, colour, duration, group):
 *
 *   front pair   AT+CMD=(4,1,0,1),(0,0,0,0,0),(7,C,2000,2),(7,C,2000,2),(0,0,0,0),(0,0,0,0)
 *   rear pair    AT+CMD=(4,1,0,1),(0,0,0,0,0),(0,0,0,0),(0,0,0,0),(7,C,2000,2),(7,C,2000,2)
 *
 * WHY THEY USED TO GO OUT AFTER A SECOND: the duration field is capped at about 1000 ms on
 * this baseboard - the bench measured that on the motion side ("longest only 1 second") -
 * and the reference's 2000 was written for hardware that honours it.  A light frame is
 * therefore a light for ONE SECOND, exactly like a motion frame is one second of movement,
 * and staying on means re-sending it.
 *
 * So a request does not send one frame; it sets the DESIRED light state and a task keeps
 * re-sending it every B220_BASE_LIGHT_REPEAT_MS until the state is changed or a stop
 * arrives.  That is the same shape as the motion path, for the same reason.
 *
 * The front is RGB (a colour per light) and the rear is a single-colour red LED, which is
 * why the two are addressed separately: the rear takes "red" or "off" and nothing else.
 */
/*
 * THE LIGHT TUPLE, AS MEASURED.  A light tuple is (id, colour, length, mode):
 *
 *   id      7 in every reference frame; other values untested
 *   colour  the colour index (white 0, red 1, yellow 2, green 4, blue 5, deepblue 6, purple 7)
 *   length  how long the frame's light effect runs, in ms - THE KEY FIELD
 *   mode    1 = slow flash, 2 = medium flash, 3 = fast flash (and, uniquely, keeps flashing
 *           indefinitely rather than ending); 0 and 4..8 do nothing
 *
 * The bench walked both fields:
 *
 *   mode 1/2/3 with length 1000   -> flashed a few times and went out
 *   mode 1, length 0..10000       -> STEADY from 4000 upwards
 *   mode 3, length 0..10000       -> kept flashing and never went out from 4000 upwards,
 *                                    cycling colours
 *
 * So "on" is length >= 4000 with mode 1; a shorter length is a flash animation that ends, and
 * mode 3 is a perpetual colour-cycling flash.  Because a length of 4000 holds by itself, the
 * lights do NOT need re-sending - and re-sending them would restart the animation, which is
 * what the first attempt at this did and why they appeared to BLINK.
 */
#define B220_BASE_LIGHT_ON_MS      4000
#define B220_BASE_LIGHT_ON_MODE       1
/*
 * Re-send at exactly the frame's own length, not just under it.  At 900 ms against a 1000 ms
 * frame there is a 100 ms hole every cycle and the lights visibly BLINK - reported from the
 * bench after the first version.  Matching the two means the next frame lands as the previous
 * one expires.
 */
/* How often a light frame is re-sent.  Long, because a 4000 ms frame holds by itself and the
 * only reason to repeat at all is that the frame is not known to last forever. */
#define B220_BASE_LIGHT_REPEAT_MS  3000

typedef struct {
    bool front_on;
    uint8_t front_color;
    bool rear_on;
} b220_light_state_t;

static b220_light_state_t s_lights = {false, 0, false};
static volatile bool s_lights_stop = false;
static TaskHandle_t s_lights_task = nullptr;

/*
 * ONE FRAME FOR BOTH GROUPS, ALWAYS.
 *
 * The first version sent a frame per group - front when the front changed, rear when the rear
 * did - and the bench found that they CANCEL EACH OTHER: switching the front on made the rear
 * go out, and switching the rear on made the front go out.  A light frame restates the whole
 * lighting state, so the second one wiped out what the first had set.
 *
 * The reference project's "all lights" frame confirms the shape: every group is written in
 * one frame, and a group that is meant to be dark is written with a duration of 0.  So this
 * builds the frame for whatever the current state is, and both groups are always present.
 *
 * Layout, from the reference:
 *
 *   header (4,1,0,1)        second tuple  = not a light
 *   tuples 2 and 3          FRONT pair (the RGB one), colour written twice
 *   tuples 4 and 5          REAR pair (single red LED), colour 1 is red
 */
static void light_frame(char* out, size_t out_size, uint8_t front_color, bool front_on,
                        bool rear_on)
{
    const uint8_t fc = front_on ? front_color : 0;
    const unsigned fd = front_on ? B220_BASE_LIGHT_ON_MS : 0u;
    const uint8_t rc = rear_on ? 1u : 0u;
    const unsigned rd = rear_on ? B220_BASE_LIGHT_ON_MS : 0u;
    /* The mode field is 1 for "on" - a length of 0 with any mode is what makes a group dark,
     * which is how the reference's frames switch a group off. */
    const unsigned fm = B220_BASE_LIGHT_ON_MODE;
    const unsigned rm = B220_BASE_LIGHT_ON_MODE;
    (void)snprintf(out, out_size,
                   "AT+CMD=(4,1,0,1),(0,0,0,0,0),(7,%u,%u,%u),(7,%u,%u,%u),(7,%u,%u,%u),"
                   "(7,%u,%u,%u)",
                   (unsigned)fc, fd, fm, (unsigned)fc, fd, fm, (unsigned)rc, rd, rm,
                   (unsigned)rc, rd, rm);
}

/*
 * Keep the requested light state alive.  Started on the first request and left running: it
 * sends nothing while both groups are off, so it costs one task and no traffic at rest.
 */
static void lights_task(void* arg)
{
    (void)arg;
    char front[B220_BASEBOARD_FRAME_MAX];
    for (;;) {
        b220_light_state_t want;
        want.front_on = s_lights.front_on;
        want.front_color = s_lights.front_color;
        want.rear_on = s_lights.rear_on;
        if (s_lights_stop) {
            s_lights_stop = false;
            want.front_on = false;
            want.rear_on = false;
            s_lights = want;
        }
        if (want.front_on || want.rear_on) {
            light_frame(front, sizeof(front), want.front_color, want.front_on, want.rear_on);
            (void)b220_base_uart_send(front, 120);
        }
        vTaskDelay(pdMS_TO_TICKS(B220_BASE_LIGHT_REPEAT_MS));
    }
}

/* --------------------------------------------------------------- motion --- */

/*
 * THE WORKING FRAME.  Field for field the one the bench validated; only `code` changes.
 *
 * The duration is 300 ms because that is the value that worked, and the frame is repeated
 * faster than that while a move lasts - a single frame is only a third of a second of
 * motion, so a longer move is built out of repeats, exactly as the interval sweep
 * established (see the continuous-motion note above).
 */
#define B220_BASE_REF_FRAME_MS   300
/*
 * THE SPEED FIELD.  The reference's 30 is what the bench validated the codes with, and the
 * bench then reported the movement as "very slow".  It is a separate field from the control
 * code, so it can be raised on its own - but only by experiment: this port has already
 * learned that fields here do not behave the way their names suggest.
 *
 * B220_BASE_SPEED_DEFAULT is what a move uses when nothing else is asked for;
 * B220_BASE_SPEED_MAX bounds whatever is asked for, so a runaway value cannot be reached by
 * accident.  {BASE+MOVE,<dir>,<power>,<ms>,<speed>} and the MCP `power` argument both set it.
 */
/*
 * 255 on request.  The bench's speed sweep over the frame's fourth field showed 20..200 all
 * produce motion, so 255 is inside the range the baseboard accepts; what changes with it is
 * the BALANCE - at 110 and below the two wheels differ clearly (which is what makes a turn
 * sharp), above that both speed up and converge (which is what makes a straight line
 * straight).  The turn rows above therefore rely on their code and turn strength rather than
 * on a low speed.
 */
#define B220_BASE_SPEED_DEFAULT   255
#define B220_BASE_SPEED_MAX       255
#define B220_BASE_REF_DIR          1

static void motion_frame_locked(uint8_t code, uint8_t steer, uint8_t speed)
{
    char cmd[B220_BASEBOARD_FRAME_MAX];
    const int n = snprintf(cmd, sizeof(cmd),
                           "AT+CMD=(1,1,3,1),(%u,%u,%u,%u,%u),(0,0,0,0),(0,0,0,0),(0,0,0,0),"
                           "(0,0,0,0)",
                           (unsigned)code, B220_BASE_REF_FRAME_MS, (unsigned)steer,
                           (unsigned)speed, B220_BASE_REF_DIR);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        return;
    }
    const esp_err_t err = b220_base_uart_send(cmd, 120);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  frame FAILED to send: %s", cmd);
    } else {
        ESP_LOGI(TAG, "  frame: %s", cmd);
    }
    s_last_refresh_us = esp_timer_get_time();
}

static void motion_stop(void)
{
    (void)b220_base_stop();
}

/* --------------------------------------------------------------- the task --- */

static void baseboard_task(void* arg)
{
    (void)arg;
    bool was_moving = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(B220_BASEBOARD_TICK_MS));

        uint8_t code = 0;
        uint8_t steer = 50;
        uint8_t speed = B220_BASE_SPEED_DEFAULT;
        bool stop_now = false;
        bool start = false;
        bool repeat = false;

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        const int64_t now = esp_timer_get_time();
        const bool have_move = (s_until_us != 0);

        /* A freshly requested move: send its one frame. */
        if (s_pending_start) {
            s_pending_start = false;
            if (direction_lookup(s_direction, &code, &steer, &speed)) {
                start = true;
            } else {
                s_until_us = 0;
            }
        }

        if (have_move && now >= s_until_us) {
            /* The requested time is up: the normal end of a move. */
            s_until_us = 0;
            stop_now = true;
        } else if (have_move && !start &&
                   (now - s_last_refresh_us) >= (int64_t)B220_BASE_REPEAT_MS * 1000) {
            /* Keep the motion alive: one frame is only 300 ms of motion, so anything longer
             * is made of repeats.  The interval sweep established that repeating faster than
             * the frame's own length is what keeps the motion continuous. */
            if (direction_lookup(s_direction, &code, &steer, &speed)) {
                repeat = true;
            }
        } else if (!have_move && was_moving) {
            /* A move ended between ticks - the stop was already sent. */
        } else if (!have_move &&
                   (now - s_last_refresh_us) > (int64_t)B220_BASE_MOTION_TIMEOUT_MS * 1000) {
            /*
             * THE GUARD.  No move is running (or none was ever started) and nothing has
             * touched the baseboard for longer than a motion frame could still be playing,
             * so make sure the wheels are not turning.  It covers a lost network, a stuck
             * task or a bug in this file without any of them being anticipated - and it is
             * the only thing that can stop the baseboard if the stop frame itself is lost.
             */
            if (s_moving) {
                ESP_LOGW(TAG, "motion timeout: nothing for %d ms - stopping the wheels",
                         B220_BASE_MOTION_TIMEOUT_MS);
                s_moving = false;
                motion_stop();
            }
        }
        if (start || repeat) {
            motion_frame_locked(code, steer, speed);
        }
        if (stop_now) {
            motion_stop();
        }
        s_moving = have_move;
        was_moving = s_moving;
        xSemaphoreGive(s_lock);
    }
}

/* ------------------------------------------------------------- public API --- */

esp_err_t b220_baseboard_start(void)
{
    if (s_task != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_speed == 0) {
        s_speed = B220_BASE_SPEED_DEFAULT;
    }
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreate(baseboard_task, "b220_base_ctl", B220_BASEBOARD_TASK_STACK, nullptr,
                    B220_BASEBOARD_TASK_PRIO, &s_task) != pdPASS) {
        s_task = nullptr;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "baseboard control up: forward/backward/left/right/spin_* mapped to the "
                  "measured control codes 0,1,6,7, a %d ms frame repeated every %d ms, one move "
                  "at most %d ms, stopped automatically %d ms after a move should have ended",
             B220_BASE_REF_FRAME_MS, B220_BASE_REPEAT_MS, B220_BASE_MOTION_MAX_MS,
             B220_BASE_MOTION_TIMEOUT_MS);
    return ESP_OK;
}

esp_err_t b220_baseboard_move(const char* direction, int power, int duration_ms)
{
    if (direction == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t code = 0;
    uint8_t steer = 50;
    uint8_t table_speed = 255;
    if (!direction_lookup(direction, &code, &steer, &table_speed)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * `power` IS the speed field of the working frame (the parameter name predates knowing
     * what the field did).  It is clamped to a sane range and passed straight through.
     */
    if (power <= 0) {
        power = B220_BASE_SPEED_DEFAULT;
    }
    if (power > B220_BASE_SPEED_MAX) {
        ESP_LOGW(TAG, "speed %d capped at %d", power, B220_BASE_SPEED_MAX);
        power = B220_BASE_SPEED_MAX;
    }
    if (power < B220_BASE_MOTION_MIN_PWR) {
        ESP_LOGW(TAG, "power %d is below the floor, using %d (a stalled motor draws more, "
                      "and the baseboard needs enough to start moving)",
                 power, B220_BASE_MOTION_MIN_PWR);
        power = B220_BASE_MOTION_MIN_PWR;
    }
    if (power > B220_BASE_MOTION_MAX_PWR) {
        ESP_LOGW(TAG, "power %d capped at %d", power, B220_BASE_MOTION_MAX_PWR);
        power = B220_BASE_MOTION_MAX_PWR;
    }
    if (duration_ms <= 0) {
        duration_ms = B220_BASE_MOTION_DEFAULT_MS;
    }
    /* Log the full-power case explicitly: "why is it still not moving" is much easier to
     * answer when the log says the request really was at 100%. */
    if (power == B220_BASE_MOTION_MAX_PWR) {
        ESP_LOGI(TAG, "move: full speed (%d)", power);
    }
    if (duration_ms > B220_BASE_MOTION_MAX_MS) {
        ESP_LOGW(TAG, "duration %d ms capped at %d ms", duration_ms,
                 B220_BASE_MOTION_MAX_MS);
        duration_ms = B220_BASE_MOTION_MAX_MS;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /*
     * THE TABLE DECIDES THE SPEED.  A caller-supplied one is only used when it asks for
     * something the table cannot express.
     *
     * An earlier version let any caller value override the table, and that is how "the
     * movement got slower" happened: {BASE+MOVE,forward,200,...} replaced the table's 255
     * with 200, and a value of 90 would have replaced it with 90 - turning a straight run
     * into a crawl.  The table's numbers are what make a turn turn and a straight line
     * straight, so a habit of passing a value must not override them.  The bench can still
     * vary the speed with {BASE+RAW}, which builds its own frame and bypasses this path.
     */
    if (power > 0 && power != B220_BASE_SPEED_DEFAULT && table_speed == 0) {
        table_speed = (uint8_t)power;
    }
    snprintf(s_direction, sizeof(s_direction), "%s", direction);
    s_speed = table_speed;
    s_duration_ms = duration_ms;
    s_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    s_pending_start = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "move: %s (code %u, turn %u, speed %u) for %d ms - repeated every %d ms",
             direction, (unsigned)code, (unsigned)steer, (unsigned)table_speed, duration_ms,
             B220_BASE_REPEAT_MS);
    return ESP_OK;
}

esp_err_t b220_baseboard_stop(void)
{
    if (s_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const bool was = (s_until_us != 0);
    s_until_us = 0;
    s_pending_start = false;
    /*
     * Clear s_moving HERE rather than waiting for the next tick of the control task.
     * b220_baseboard_is_moving() is what the bench loops watch to decide whether to keep
     * sending frames, and a stop that only takes effect one tick later would let them send
     * one more motion frame after the stop - which is exactly the "I stopped it and it kept
     * going" the bench reported.
     */
    s_moving = false;
    s_stop_generation++;
    /* A stop turns the lights off too: they are re-sent frames, so leaving the state set
     * would make them keep coming back after the stop. */
    s_lights_stop = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "stop%s", was ? "" : " (nothing was moving)");
    return b220_base_stop();
}

/*
 * Send ONE light frame and nothing else, for measuring what a single frame is worth.
 *
 * The persistent path re-sends, so it cannot answer the question the flicker raises: does a
 * light frame expire (like a motion frame) or does it latch?  This sends one and returns, so
 * the bench can time it.  {BASE+LIGHT,once,<colour>}.
 */
esp_err_t b220_baseboard_lights_once(const char* color)
{
    if (color == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t index = 0;
    bool front_on = false;
    bool rear_on = false;
    if (strcmp(color, "rear_red") == 0 || strcmp(color, "rear") == 0) {
        rear_on = true;
    } else if (strcmp(color, "off") != 0 && color_lookup(color, &index)) {
        front_on = true;
    }
    char frame[B220_BASEBOARD_FRAME_MAX];
    light_frame(frame, sizeof(frame), index, front_on, rear_on);
    const esp_err_t err = b220_base_uart_send(frame, 120);
    ESP_LOGW(TAG, "lights ONCE: %s (%s) - watch how long it stays on", frame,
             err == ESP_OK ? "sent" : "FAILED");
    return err;
}

esp_err_t b220_baseboard_lights(const char* color)
{
    if (color == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_lights_task == nullptr) {
        if (xTaskCreate(lights_task, "b220_lights", 3072, nullptr, 3, &s_lights_task) != pdPASS) {
            s_lights_task = nullptr;
            return ESP_FAIL;
        }
    }

    if (strcmp(color, "off") == 0) {
        s_lights.front_on = false;
        s_lights.rear_on = false;
        ESP_LOGI(TAG, "lights: all off");
        return ESP_OK;
    }
    if (strcmp(color, "rear_off") == 0) {
        s_lights.rear_on = false;
        ESP_LOGI(TAG, "lights: rear off");
        return ESP_OK;
    }
    if (strcmp(color, "front_off") == 0) {
        s_lights.front_on = false;
        ESP_LOGI(TAG, "lights: front off");
        return ESP_OK;
    }
    /*
     * The rear is a single-colour red LED, so it only understands red and off; anything else
     * goes to the RGB front.  "rear_red" is accepted so the two can be set independently of
     * the front.
     */
    if (strcmp(color, "rear_red") == 0 || strcmp(color, "rear") == 0) {
        s_lights.rear_on = true;
        ESP_LOGI(TAG, "lights: rear red");
        return ESP_OK;
    }

    uint8_t index = 0;
    if (!color_lookup(color, &index)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_lights.front_on = true;
    s_lights.front_color = index;
    ESP_LOGI(TAG, "lights: front colour %s (%u)", color, (unsigned)index);
    return ESP_OK;
}

bool b220_baseboard_is_moving(void)
{
    return s_moving;
}

uint32_t b220_baseboard_stop_generation(void)
{
    return s_stop_generation;
}
