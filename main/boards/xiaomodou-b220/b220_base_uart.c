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

#include "b220_tcp_log.h"   /* B220_TCP_LOG_PORT_HINT, named in the link-up log */

#include "b220_baseboard.h"   /* the local test channel drives through the same guards */
#include "b220_gsensor.h"     /* {BASE+ACCEL}, alongside the other read-only probes */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>   /* atoi, for the test channel's numeric fields */
#include <string.h>

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_rom_serial_output.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char* TAG = "b220-base";

#define B220_BASE_UART_PORT      UART_NUM_0
#define B220_BASE_UART_RX_BUF    512
#define B220_BASE_UART_TX_BUF    256
#define B220_BASE_FRAME_MAX      224   /* long enough for a two-tuple frame plus CRLF */
#define B220_BASE_LAST_MAX       160

/*
 * HOW THE FRAME IS PUT ON THE WIRE, AND WHY NOT THROUGH THE UART DRIVER
 * --------------------------------------------------------------------
 * The baseboard shares UART0 with the console, and on ESP-IDF 6.1 the console
 * claims that port in a way that leaves no room for a second user:
 *
 *     esp_driver_uart/src/uart_vfs.c:1209
 *     uart_driver_install(channel, 256, 0, 0, NULL, 0);
 *                                      ^^^ RX ring   ^ TX ring = ZERO
 *
 * The console needs a driver to do its line editing, but allocates no TX ring
 * because it writes through the ROM.  Consequences, both observed on this board:
 *
 *   - uart_write_bytes() fails with "uart driver error": the driver exists, but
 *     there is no TX ring for it to queue into;
 *   - uart_driver_install() here cannot help either: the port is taken, so it
 *     returns ESP_ERR_INVALID_STATE.
 *
 * The reference port of this toy installs its own driver and both sends and reads;
 * it can, because the ESP-IDF version it uses does NOT install a console driver
 * (its console writes through the ROM).  That line cannot be copied here.
 *
 * So this port writes through the SAME path the console uses - the ROM's
 * single-character transmitter, which waits for FIFO space and hands the byte to
 * the hardware - and does not install a driver at all.  Two consequences worth
 * stating plainly:
 *
 *   1. a frame can interleave with log text on the wire.  That was already true
 *      (both share the pin); what is new is that a frame may appear in the middle
 *      of a log line rather than between two of them.  The baseboard parses
 *      "{AT+...}\r\n", so a frame that is intact as a substring still works.
 *   2. RX stays with the console: the console's driver owns the RX ring, so base
 *      replies are not readable from here.  Transmit-only is the honest
 *      description, and b220_base_uart_read_pending() reports that rather than
 *      pretending.
 *
 * esp_rom_output_tx_one_char() blocks only while the TX FIFO is full, and the FIFO
 * is 128 bytes, so a whole frame is handed over in microseconds of CPU time in the
 * normal case.  send() takes a semaphore around the frame so two tasks cannot
 * interleave bytes of different frames.
 */
static bool s_ready = false;
static bool s_rx_enabled = false;
/* True when THIS module installed the UART driver, i.e. the port was free because the
 * console was disabled at build time.  Then the driver owns both directions and frames
 * go out through it; otherwise they go through the ROM transmitter. */
static bool s_driver_ours = false;
static SemaphoreHandle_t s_lock = NULL;
static char s_last_tx[B220_BASE_LAST_MAX];
static char s_last_rx[B220_BASE_LAST_MAX];

/* Send one frame through the ROM transmitter.  Returns the number of bytes the
 * hardware accepted (the frame length) or -1 when the FIFO never drained. */
/*
 * Set while a {BASE+...} command is on the wire, so the RX task recognises the echo as its
 * own command rather than printing it as a baseboard reply.  Declared at file scope rather
 * than inside send(), which is where it started.
 */
bool b220_base_uart_echo_is_test_command(const char* frame);
void b220_base_uart_note_test_echo(void);

static int base_write_raw(const char* data, size_t len, uint32_t timeout_ms)
{
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms == 0 ? 1 : timeout_ms);
    const TickType_t start = xTaskGetTickCount();

    for (size_t i = 0; i < len; i++) {
        /* esp_rom_output_tx_one_char returns 0 on success.  It should not fail with
         * the console UART configured, but if it does there is nothing useful to do
         * except stop and say so, which the caller's short-write warning covers. */
        if (esp_rom_output_tx_one_char((uint8_t)data[i]) != 0) {
            return (int)i;
        }
        if (timeout_ms != 0 && (xTaskGetTickCount() - start) > timeout_ticks) {
            /* Only reachable if the FIFO jammed: the call above waits for space, so
             * normal frames never spend a tick here. */
            return (int)i;
        }
    }
    return (int)len;
}

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

    /*
     * WHO OWNS UART0?  Two situations, and the code handles both so the same firmware
     * source works with either Console setting:
     *
     *   console ENABLED (CONFIG_ESP_CONSOLE_UART_DEFAULT, the default build)
     *     The console has already installed a driver on this port with a ZERO-length TX
     *     ring (esp_driver_uart/src/uart_vfs.c:1209), so uart_write_bytes() fails with
     *     "uart driver error" and an install of our own is refused.  Frames go through
     *     the ROM transmitter, and the console's RX ring - shared with typed console
     *     input - is all there is to read.  TRANSMIT-ONLY in practice.
     *
     *   console DISABLED (CONFIG_ESP_CONSOLE_NONE, the baseboard build)
     *     Nothing has claimed the port, so install a proper driver with both rings.  RX
     *     becomes genuinely usable - which is the whole point of disabling the console,
     *     because AT+Ver and friends expect a reply - and TX goes through the driver.
     *
     * The log line below always says which one happened, and how to change it: a
     * transmit-only baseboard link is a legitimate configuration, not an error, but it
     * must not be a silent surprise.
     */
    bool driver_ready = uart_is_driver_installed(B220_BASE_UART_PORT);
    if (!driver_ready) {
        const esp_err_t derr = uart_driver_install(B220_BASE_UART_PORT, B220_BASE_UART_RX_BUF,
                                                   B220_BASE_UART_TX_BUF, 0, NULL, 0);
        if (derr == ESP_OK) {
            driver_ready = true;
            s_driver_ours = true;
        } else if (derr != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "uart_driver_install failed (%s); the link stays transmit-only",
                     esp_err_to_name(derr));
        }
    }

    s_rx_enabled = consume_console_input && driver_ready;
    if (s_rx_enabled) {
        /* Drop anything left over, so a baseboard reply cannot be confused with stale
         * input from boot. */
        uart_flush_input(B220_BASE_UART_PORT);
    }
    s_ready = true;

    if (s_driver_ours) {
        ESP_LOGI(TAG, "baseboard link up: UART0 TX GPIO%d / RX GPIO%d @ %d 8N1, THIS module "
                      "owns the driver (the console is off), so frames go out through it and "
                      "replies are readable",
                 B220_BASE_UART_TX_GPIO, B220_BASE_UART_RX_GPIO, B220_BASE_UART_BAUD);
    } else {
        ESP_LOGI(TAG, "baseboard link up: UART0 TX GPIO%d / RX GPIO%d @ %d 8N1, frames sent "
                      "through the ROM transmitter (the console's driver has no TX ring), %s. "
                      "For a readable baseboard, build with CONFIG_ESP_CONSOLE_NONE=y - the "
                      "log then moves to TCP port %d and the console stops owning this port",
                 B220_BASE_UART_TX_GPIO, B220_BASE_UART_RX_GPIO, B220_BASE_UART_BAUD,
                 s_rx_enabled ? "replies readable from the console's input ring"
                              : "TRANSMIT-ONLY (the console owns the input ring)",
                 B220_TCP_LOG_PORT_HINT);
    }
    return ESP_OK;
}

/*
 * SELF-TEST WITHOUT A BASEBOARD.
 *
 * With the baseboard unplugged there is otherwise no way to tell "the UART works and
 * nobody answered" from "the UART is broken", and the second is exactly what a wrong
 * CONFIG_ESP_CONSOLE_* setting or a wrong pin would produce.  uart_set_loop_back()
 * shorts TX to RX inside the chip, so a frame written here must come straight back:
 * that exercises the driver, the rings, the clock divider and the pin mux in one go.
 *
 * It is deliberately opt-in (B220_BASE_UART_SELFTEST): it must not run in normal
 * operation, where it would put bytes on the wire that the baseboard could mistake for
 * commands.
 */
esp_err_t b220_base_uart_selftest(void)
{
    const esp_err_t init_err = b220_base_uart_init(true);
    if (init_err != ESP_OK) {
        return init_err;
    }
    if (!s_rx_enabled) {
        ESP_LOGW(TAG, "selftest: no readable input on UART%d, so a loopback cannot be "
                      "observed - this is the console-owns-the-port build",
                 (int)B220_BASE_UART_PORT);
        return ESP_ERR_INVALID_STATE;
    }

    static const char probe[] = "{AT+Selftest}\r\n";
    (void)uart_flush_input(B220_BASE_UART_PORT);
    if (uart_set_loop_back(B220_BASE_UART_PORT, true) != ESP_OK) {
        ESP_LOGW(TAG, "selftest: uart_set_loop_back failed; the ROM path has no equivalent, "
                      "so the test needs a driver-owned port");
        return ESP_FAIL;
    }

    const esp_err_t send_err = b220_base_uart_send("AT+Selftest", 200);

    /* Read back what the loopback returned. */
    char got[sizeof(probe) + 8];
    size_t used = 0;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(300);
    while (used + 1 < sizeof(got) && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t byte = 0;
        if (uart_read_bytes(B220_BASE_UART_PORT, &byte, 1, pdMS_TO_TICKS(20)) == 1) {
            got[used++] = (char)byte;
        }
    }
    got[used] = '\0';
    (void)uart_set_loop_back(B220_BASE_UART_PORT, false);

    const bool ok = (send_err == ESP_OK) && (strcmp(got, probe) == 0);
    ESP_LOGW(TAG, "selftest %s: sent %u byte(s), looped back %u byte(s) %s", ok ? "PASS" : "FAIL",
             (unsigned)(sizeof(probe) - 1), (unsigned)used, ok ? "- UART0 works" : got);
    if (!ok) {
        ESP_LOGW(TAG, "  (a FAIL here means the UART path itself is wrong - pins, clock or the "
                      "console setting - not that the baseboard is silent)");
    }
    return ok ? ESP_OK : ESP_FAIL;
}

/*
 * THE LOCAL TEST CHANNEL, AND THE REPORTER.
 *
 * Two jobs in one task, because both are "look at what came in on UART0".
 *
 * REPORTING (always on): the baseboard is expected to answer some frames (AT+Ver) and may
 * announce itself unprompted, and until this existed not one of those bytes was ever
 * printed - which made "the baseboard never replied" indistinguishable from "we never
 * looked".
 *
 * TESTING (the interesting part): a line of the form
 *
 *     {BASE+MOVE,forward,60,800}
 *     {BASE+STOP}
 *     {BASE+LIGHT,blue}
 *
 * drives the baseboard from a serial or telnet console, WITHOUT the cloud.  That matters
 * for two reasons: the direction and steering conventions of this protocol are still
 * unverified on this unit, and settling them through the voice path costs a cloud round
 * trip per attempt; and when the network itself is what is broken, the assistant cannot
 * reach the tools at all.
 *
 * Note what it does NOT do: it does not talk to the UART itself.  Every command goes
 * through b220_baseboard_*, so the power and duration caps, the 600 ms heartbeat and the
 * timeout that stops the wheels all still apply.  A test channel that bypassed them would
 * be testing something other than the thing that ships.
 *
 * The command and its echo are kept out of the "baseboard says" log line, so the console
 * output stays readable: a command is a command, not a reply.
 */
static bool cmd_test_active = false;

/* Defined with the sweep machinery below; declared here because the command parser (which
 * sits above them in the file) starts them. */
static void base_raw_move_task(void* arg);
static void base_sweep_task(void* arg);
static void base_dirs_task(void* arg);
static void base_hold_task(void* arg);
static void base_light_sweep_task(void* arg);

bool b220_base_uart_echo_is_test_command(const char* frame)
{
    return frame != NULL && strstr(frame, "BASE+") != NULL;
}

void b220_base_uart_note_test_echo(void)
{
    cmd_test_active = true;
}

static void base_rx_report_task(void* arg)
{
    (void)arg;
    char line[B220_BASE_LAST_MAX];
    for (;;) {
        if (b220_base_uart_read_pending(line, sizeof(line), 500) != ESP_OK || line[0] == '\0') {
            continue;
        }
        if (cmd_test_active) {
            /* This is our own command coming back around: act on it, do not report it as
             * something the baseboard said. */
            cmd_test_active = false;
            b220_base_uart_handle_test_command(line);
            continue;
        }
        ESP_LOGW(TAG, "baseboard says: %s", line);
    }
}

/*
 * A SWEEP, because the parameter space is not worth guessing by hand one round trip at a
 * time.
 *
 * The first attempt at movement produced a hum and no rotation.  The reference project's
 * validated frames differ from this port's in two ways that both matter: their duration
 * field is 300-800 units (60-160 SECONDS, against our 0.6), and every steering angle they
 * use is 50, 90 or 170 - never 0 or 180, which is what this port has been sending.
 *
 * {BASE+SWEEP} therefore walks the combinations itself, logging each step, so a bench
 * observer only has to report which step number moved the wheels - or that none did, which
 * is just as informative.
 */
/*
 * Each step now carries the INTERVAL at which its frame is repeated, because that is the
 * one parameter left, and it is the one that decides whether motion is continuous.
 *
 * What the bench established about this baseboard:
 *
 *   - the duration field is MILLISECONDS and it is capped at about 1000 whatever you write
 *     (a step asking for 2000, and even one writing 999, each produced a single movement
 *     lasting about a second);
 *   - the third tuple field is a SPEED, and 255 is acceptable to it;
 *   - one frame = one burst of motion, whose length is that field.
 *
 * So continuous motion means re-sending the frame about once per second - and the interval
 * matters in both directions, which the first two attempts demonstrate:
 *
 *   interval 500 ms with a 600 ms field -> the bench counted 20 discrete movements in 10 s
 *                                          (a gap between every pair: a stutter)
 *   no repetition at all                -> exactly one movement, then nothing
 *
 * This sweep therefore walks the interval itself at a fixed known-good mode, so one run
 * says which cadence is smooth.  The overlap question is the whole point: refreshing
 * BEFORE the previous burst ends may blend into continuous motion, or may restart it.
 */
typedef struct {
    uint8_t mode;
    uint8_t steer;
    uint8_t power;      /* speed, as the baseboard names the field; 255 is its maximum */
    uint16_t field_ms;  /* duration field written into the frame; effectively capped at 1000 */
    uint16_t interval_ms;
    const char* what;
} b220_sweep_step_t;

static const b220_sweep_step_t s_sweep[] = {
    {3, 90, 255, 1000, 1000, "mode 3 fwd, speed 255, field 1000 ms, repeat EVERY 1000 ms (no overlap)"},
    {3, 90, 255, 1000,  900, "mode 3 fwd, speed 255, field 1000 ms, repeat every 900 ms (slight overlap)"},
    {3, 90, 255, 1000,  800, "mode 3 fwd, speed 255, field 1000 ms, repeat every 800 ms (overlap)"},
    {3, 90, 255, 1000,  600, "mode 3 fwd, speed 255, field 1000 ms, repeat every 600 ms (more overlap)"},
    {3, 90, 255,  900,  900, "mode 3 fwd, speed 255, field 900 ms, repeat every 900 ms"},
    {3, 90, 150, 1000,  900, "mode 3 fwd, speed 150, field 1000 ms, repeat every 900 ms"},
    {2, 90, 255, 1000,  900, "mode 2 (backward), speed 255, repeat every 900 ms"},
};

/*
 * {BASE+MOVE,<dir>,<power>,<ms>} sends a frame whose duration field SAYS exactly how long
 * the movement should last.  That makes the frame's internal length the user's length,
 * which the bench showed is not how this baseboard works: a frame of 600 ms produced 0.5 s
 * of motion and a frame of 10 s produced a single movement, so the duration field bounds a
 * fixed-length motion sequence rather than programming a continuous one.
 *
 * {BASE+HOLD,<dir>,<power>,<ms>} is the experiment that separates the two: it sends ONE
 * frame with a very long internal duration and then STOPS it after `ms`.  If the wheels run
 * continuously for `ms`, a long frame is the answer and the normal move path should be
 * built that way.
 */
/*
 * {BASE+RAW,SWEEP,<code>,<field>,<from>,<to>[,<step>]} - WALK ONE FIELD, holding the rest.
 *
 * Built because the bench's three frames showed that the control tuple's THIRD field is not a
 * steering angle in any simple sense:
 *
 *     (2,300,90,50,1)  only the right wheel turned
 *     (2,300,50,50,1)  right fast, left barely
 *     (2,300,10,50,1)  BOTH wheels turned, medium speed
 *     (2,300, 0,50,1)  both turned, a little slower than medium
 *
 * So that field decides how the drive is SPLIT between the two wheels, and the code picks
 * which wheel dominates.  Walking it from 0 to 90 with everything else fixed says where the
 * straight, the arc and the one-wheel end of that range actually are - one run instead of a
 * dozen hand-typed frames.
 *
 * `field` is 3 (steering), 4 (speed) or 5 (direction) - 1-based within the tuple, so
 * {BASE+RAW,SWEEP,2,3,0,90,10} is the walk described above.
 *
 * Each step runs for `ms` (default 3 s), announces the frame it is about to send, and is
 * followed by a stop, so the steps cannot blur together.  {BASE+STOP} cancels the whole walk.
 */
static void base_field_sweep_task(void* arg)
{
    char* spec = (char*)arg;
    int code = 2;
    int field = 3;
    int from = 0;
    int to = 90;
    int step = 10;
    const int got = sscanf(spec, "%d,%d,%d,%d,%d", &code, &field, &from, &to, &step);
    free(spec);
    if (got < 4 || step == 0 || field < 2 || field > 5) {
        ESP_LOGW(TAG, "SWEEP: bad spec (want SWEEP,<code>,<field 2..5>,<from>,<to>[,<step>])");
        vTaskDelete(nullptr);
        return;
    }

    const uint32_t generation = b220_baseboard_stop_generation();
    for (int value = from; (step > 0) ? (value <= to) : (value >= to); value += step) {
        if (b220_baseboard_stop_generation() != generation) {
            ESP_LOGW(TAG, "SWEEP: cancelled by STOP");
            vTaskDelete(nullptr);
            return;
        }
        int v[5] = {code, 300, 50, 50, 1};
        v[field - 1] = value;
        char cmd[B220_BASE_FRAME_MAX];
        const int n = snprintf(cmd, sizeof(cmd),
                               "AT+CMD=(1,1,3,1),(%d,%d,%d,%d,%d),(0,0,0,0),(0,0,0,0),(0,0,0,0),"
                               "(0,0,0,0)",
                               v[0], v[1], v[2], v[3], v[4]);
        if (n <= 0 || (size_t)n >= sizeof(cmd)) {
            continue;
        }
        ESP_LOGW(TAG, "SWEEP field %d = %d : %s", field, value, cmd);
        for (uint32_t elapsed = 0; elapsed < 3000; elapsed += 250) {
            if (b220_baseboard_stop_generation() != generation) {
                break;
            }
            (void)b220_base_uart_send(cmd, 120);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        (void)b220_base_stop();
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    ESP_LOGW(TAG, "SWEEP done - describe each value: only right wheel / both, right fast / "
                  "both, even / only left wheel / other");
    vTaskDelete(nullptr);
}

/*
 * {BASE+LRAW,SWEEP,<tuple field 2..4>,<from>,<to>[,<step>]} - walk one field of the light
 * tuples and watch.
 *
 * This is how "solid" has to be found.  The frame this port sends was reported as "blinks two
 * or three times, then off" - so a light frame is an ANIMATION, not a state, and re-sending
 * it cannot make it steady.  A light tuple is (id, colour, duration/effect, group): field 3
 * walks the duration/effect and field 4 the group, both of which the reference varies
 * ((7,c,2000,1) for its "right front" frames, (7,c,2000,2) for its pairs, (7,c,500,1) in its
 * "short flash" frame).
 */
static void base_light_sweep_task(void* arg)
{
    char* spec = (char*)arg;
    int which = 3;
    int from = 0;
    int to = 1000;
    int step = 100;
    int colour = 5;   /* blue, the colour the bench has been using */
    int effect = 2;   /* field 4: 1 slow flash, 2 medium, 3 fast; 0 and 4+ do nothing */
    const int got = sscanf(spec, "%d,%d,%d,%d,%d,%d", &which, &from, &to, &step, &colour,
                           &effect);
    free(spec);
    if (got < 3 || step == 0 || which < 2 || which > 4) {
        ESP_LOGW(TAG, "LSWEEP: bad spec (want SWEEP,<field 2..4>,<from>,<to>[,<step>[,<colour>"
                      "[,<effect>]]])");
        vTaskDelete(nullptr);
        return;
    }
    const uint32_t generation = b220_baseboard_stop_generation();
    for (int value = from; (step > 0) ? (value <= to) : (value >= to); value += step) {
        if (b220_baseboard_stop_generation() != generation) {
            ESP_LOGW(TAG, "LSWEEP: cancelled by STOP");
            vTaskDelete(nullptr);
            return;
        }
        int l[4] = {7, colour, 1000, effect};
        l[which - 1] = value;
        char frame[B220_BASE_FRAME_MAX];
        const int n = snprintf(frame, sizeof(frame),
                               "AT+CMD=(4,1,0,1),(0,0,0,0,0),(%d,%d,%d,%d),(%d,%d,%d,%d),"
                               "(0,0,0,0),(0,0,0,0)",
                               l[0], l[1], l[2], l[3], l[0], l[1], l[2], l[3]);
        if (n <= 0 || (size_t)n >= sizeof(frame)) {
            continue;
        }
        ESP_LOGW(TAG, "LSWEEP field %d = %d : %s", which, value, frame);
        (void)b220_base_uart_send(frame, 120);
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
    ESP_LOGW(TAG, "LSWEEP done - which value gave a STEADY light?");
    vTaskDelete(nullptr);
}

static void base_hold_task(void* arg)
{
    char* spec = (char*)arg;
    /* spec is "direction,power,ms" */
    char direction[12] = "forward";
    int power = 40;
    int ms = 3000;
    char* cursor = spec;
    char* f1 = strchr(cursor, ',');
    if (f1 != NULL) {
        *f1 = '\0';
        snprintf(direction, sizeof(direction), "%s", cursor);
        cursor = f1 + 1;
        char* f2 = strchr(cursor, ',');
        if (f2 != NULL) {
            *f2 = '\0';
            power = atoi(cursor);
            ms = atoi(f2 + 1);
        } else {
            power = atoi(cursor);
        }
    } else {
        snprintf(direction, sizeof(direction), "%s", cursor);
    }

    /* The mode/steer table lives in b220_baseboard.c; this is the bench spelling, so the
     * two straight-line modes are written out here rather than duplicated logic. */
    uint8_t mode = 3;
    uint8_t steer = 90;
    if (strcmp(direction, "backward") == 0) {
        mode = 2;
    }

    /* 999 units = 199.8 s: far longer than any test, so the frame cannot be the thing that
     * ends the motion. */
    char cmd[B220_BASE_FRAME_MAX];
    const int n = snprintf(cmd, sizeof(cmd),
                           "AT+CMD=(9,1,0,1),(%u,999,%u,%u,0),(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)",
                           (unsigned)mode, (unsigned)steer, (unsigned)power);
    ESP_LOGW(TAG, "HOLD: one long frame (mode %u, steer %u, %d%%), stopping after %d ms",
             (unsigned)mode, (unsigned)steer, power, ms);
    if (n > 0 && (size_t)n < sizeof(cmd)) {
        (void)b220_base_uart_send(cmd, 120);
        ESP_LOGW(TAG, "HOLD: frame sent: %s", cmd);
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    (void)b220_base_stop();
    ESP_LOGW(TAG, "HOLD: stopped after %d ms - was the motion continuous?", ms);
    free(spec);
    vTaskDelete(nullptr);
}

/*
 * Cadence for the bench tools.  A 1000 ms field repeated every 900 ms is the combination the
 * interval sweep proved produces continuous motion, so every test uses it - the only thing a
 * test changes is the mode and the steering angle.
 */
#define B220_BASE_DIRS_FIELD_MS   1000
#define B220_BASE_DIRS_REPEAT_MS   900
/* A turn needs 4 s to be unmistakable; the in-place candidates get 2 s more. */
#define B220_BASE_DIRS_WINDOW_MS  4000
#define B220_BASE_DIRS_SPEED       200
/* Where part 3 of the DIRS table - the in-place candidates - begins. */
#define B220_BASE_DIRS_INPLACE_FIRST 12

/*
 * Drive the two wheels with separate frames, the way the baseboard expects a manoeuvre.
 *
 * `arg` is "left,right": the control-tuple FIRST value for each wheel (2 = left channel,
 * 3 = right channel, 0 = that wheel idle) and the direction is taken as forward for
 * non-zero.  Both frames go out on every tick for the window, then both are stopped.
 */
/* Repeat one already-built frame for a fixed window, then stop.  Used by the raw MOVE form
 * so a candidate found by {BASE+DIRS} can be watched on its own, at length, without another
 * build. */
static void base_raw_move_task(void* arg)
{
    char* cmd = (char*)arg;
    ESP_LOGW(TAG, "RAW: %s", cmd);
    /*
     * Repeat at the cadence the frame itself asks for, not a fixed interval: the reference's
     * wheel frames carry a 300 ms duration, and repeating one every 900 ms would leave a
     * 600 ms hole in the motion.
     */
    const char* paren = strchr(cmd, '(');
    const char* comma = (paren != NULL) ? strchr(paren, ',') : NULL;
    uint32_t repeat_ms = 250;
    if (comma != NULL) {
        const int field = atoi(comma + 1);
        if (field > 0 && field <= 1000) {
            repeat_ms = (uint32_t)field;
        }
    }
    /*
     * INTERRUPTIBLE.  Two things end this loop, and both matter:
     *
     *   the window below, so a forgotten test stops by itself;
     *   {BASE+STOP}, which sets the normal path to "not moving" - and the bench found the
     *   hard way that nothing checked that.  A stop command that only reaches the normal
     *   path cannot stop a bench tool that is sending frames of its own, and the result was
     *   a machine that kept driving until the window expired.
     */
    const uint32_t generation = b220_baseboard_stop_generation();
    for (uint32_t elapsed = 0; elapsed < 5000; elapsed += repeat_ms) {
        if (b220_baseboard_stop_generation() != generation) {
            ESP_LOGW(TAG, "RAW: cancelled by STOP after %u ms", (unsigned)elapsed);
            break;
        }
        (void)b220_base_uart_send(cmd, 120);
        vTaskDelay(pdMS_TO_TICKS(repeat_ms));
    }
    (void)b220_base_stop();
    ESP_LOGW(TAG, "RAW: done - in place, arc, or nothing?");
    free(cmd);
    vTaskDelete(nullptr);
}

static void base_sweep_task(void* arg)
{
    (void)arg;
    for (size_t i = 0; i < sizeof(s_sweep) / sizeof(s_sweep[0]); i++) {
        const b220_sweep_step_t* s = &s_sweep[i];
        ESP_LOGW(TAG, "SWEEP %u/%u: %s", (unsigned)(i + 1),
                 (unsigned)(sizeof(s_sweep) / sizeof(s_sweep[0])), s->what);
        /*
         * Straight to the frame, not through b220_baseboard_move(): a sweep is a bench
         * tool and has to be able to try angles and durations the normal path deliberately
         * restricts.  It still stops between steps, so a step that does move cannot run on.
         */
        char cmd[B220_BASE_FRAME_MAX];
        const int n = snprintf(cmd, sizeof(cmd),
                               "AT+CMD=(9,1,0,1),(%u,%u,%u,%u,0),(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,0,0)",
                               (unsigned)s->mode, (unsigned)s->field_ms, (unsigned)s->steer,
                               (unsigned)s->power);
        if (n > 0 && (size_t)n < sizeof(cmd)) {
            /* Repeat the frame at the step's own interval for a fixed 3 s window, so the
             * only thing that differs between steps is the cadence. */
            const uint32_t window_ms = 3000;
            for (uint32_t elapsed = 0; elapsed < window_ms; elapsed += s->interval_ms) {
                (void)b220_base_uart_send(cmd, 120);
                vTaskDelay(pdMS_TO_TICKS(s->interval_ms));
            }
        }
        (void)b220_base_stop();
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    ESP_LOGW(TAG, "SWEEP done - report which step number moved the wheels");
    vTaskDelete(nullptr);
}

/*
 * {BASE+DIRS} - WHAT EACH MODE AND STEERING ANGLE ACTUALLY DOES.
 *
 * The documented mode table does not match this unit.  Six attempts across two bench runs
 * never produced a reliable BACKWARD, and mode 2 - which the reference protocol document
 * says is "backward / backward-left" - drove forward every time.  The only time the wheels
 * went backwards was mode 1 at steering 170, in an older sweep that was hunting something
 * else entirely.
 *
 * So the question "which combination is forward, backward, left and right" is answered here
 * by walking the candidates and watching, rather than by reading the table again:
 *
 *   part 1  one straight mode at a time, steering 90 - which of 0/1/2/3 goes forward and
 *           which goes backward.  2 and 3 have already failed once each, so 0 and 1 are the
 *           interesting ones.
 *   part 2  the in-place turn modes at both poles and both sides of the documented
 *           threshold (the table claims <15 and >165, the reference only ever sent 50/90/170)
 *           - looking for the combination where the wheels counter-rotate.
 *
 * Each step runs for `window_ms` with the frame repeated at B220_BASE_DIRS_REPEAT_MS, which
 * the interval sweep showed is continuous.  Steps are separated by a full stop.
 */
typedef struct {
    uint8_t mode;
    uint8_t steer;
    const char* what;
    /*
     * Header and the motion tuple's fifth field.  These exist because of a bench result that
     * the earlier tests could not explain: with the header this port has always used
     * ((9,1,0,1)) and a fifth field of 0, EVERY steering angle behaved like straight - mode 0
     * and 1 went backwards and modes 2 and 3 forwards, whatever the angle said, and nothing
     * ever turned.
     *
     * The reference project's frames that DID turn on hardware use the header (1,1,3,1) and
     * a fifth field of 1.  Its documented "pure motion" recipe (header 9, light tuples
     * cleared) is what it switched TO after giving up on turning, so this port has been
     * sending the give-up form and wondering why the wheels will not turn.
     */
    uint8_t header[4];
    uint8_t reserved;
} b220_dirs_step_t;

static const b220_dirs_step_t s_dirs[] = {
    /*
     * Part 1 - which mode is which way.  Steering straight for all of them.
     * RESULT (bench): step 3 (mode 0) and step 6? no - see the note below the table.
     */
    {2, 90,  "mode 2, steer 90 - document says BACKWARD",              {9, 1, 0, 1}, 0},
    {0, 90,  "mode 0, steer 90",                                       {9, 1, 0, 1}, 0},
    {1, 90,  "mode 1, steer 90",                                       {9, 1, 0, 1}, 0},
    {3, 90,  "mode 3, steer 90 - known FORWARD (control)",             {9, 1, 0, 1}, 0},
    /* Part 2 - steering angles, with the header that has never turned anything. */
    {0, 0,   "mode 0, steer 0",                                        {9, 1, 0, 1}, 0},
    {1, 180, "mode 1, steer 180",                                       {9, 1, 0, 1}, 0},
    {0, 170, "mode 0, steer 170",                                       {9, 1, 0, 1}, 0},
    {1, 170, "mode 1, steer 170",                                       {9, 1, 0, 1}, 0},
    {0, 50,  "mode 0, steer 50",                                        {9, 1, 0, 1}, 0},
    {1, 50,  "mode 1, steer 50",                                        {9, 1, 0, 1}, 0},
    {3, 170, "mode 3, steer 170",                                       {9, 1, 0, 1}, 0},
    {3, 50,  "mode 3, steer 50",                                        {9, 1, 0, 1}, 0},
    /*
     * Part 3 - in place, still with the header that has never turned anything.
     */
    {0, 0,   "IN-PLACE: mode 0, steer 0",                              {9, 1, 0, 1}, 0},
    {1, 180, "IN-PLACE: mode 1, steer 180",                            {9, 1, 0, 1}, 0},
    {1, 0,   "IN-PLACE: mode 1, steer 0",                              {9, 1, 0, 1}, 0},
    {0, 180, "IN-PLACE: mode 0, steer 180",                            {9, 1, 0, 1}, 0},
    {0, 15,  "IN-PLACE: mode 0, steer 15",                             {9, 1, 0, 1}, 0},
    {1, 170, "IN-PLACE: mode 1, steer 170",                            {9, 1, 0, 1}, 0},
    {0, 170, "IN-PLACE: mode 0, steer 170",                            {9, 1, 0, 1}, 0},
    {1, 90,  "IN-PLACE: mode 1, steer 90 (control)",                   {9, 1, 0, 1}, 0},
    /*
     * Part 4 - THE STEERING HYPOTHESIS.
     *
     * Every row above uses the header this port has always sent and a fifth field of 0, and
     * across two full runs NOT ONE of them turned: the steering angle made no difference to
     * anything, mode 0 and 1 went backwards at steering 90 and forward at every other angle,
     * modes 2 and 3 went forward.  A field that changes nothing is not being read.
     *
     * The reference project's frames that turned on real hardware carry the header (1,1,3,1)
     * and a fifth field of 1 - e.g. its validated "left wheel" frame
     *
     *     AT+CMD=(1,1,3,1),(2,300,50,30,1),(0,0,0,0),...
     *
     * So these rows vary exactly those two things while holding the known-good cadence and
     * speed.  If a turn appears anywhere, the header was the missing piece.
     */
    {3, 50,  "STEER-HYPOTHESIS: mode 3, steer 50, header 1,1,3,1, reserve 1",  {1, 1, 3, 1}, 1},
    {3, 170, "STEER-HYPOTHESIS: mode 3, steer 170, header 1,1,3,1, reserve 1", {1, 1, 3, 1}, 1},
    {0, 50,  "STEER-HYPOTHESIS: mode 0, steer 50, header 1,1,3,1, reserve 1",  {1, 1, 3, 1}, 1},
    {1, 50,  "STEER-HYPOTHESIS: mode 1, steer 50, header 1,1,3,1, reserve 1",  {1, 1, 3, 1}, 1},
    {2, 50,  "STEER-HYPOTHESIS: mode 2, steer 50, header 1,1,3,1, reserve 1",  {1, 1, 3, 1}, 1},
    {3, 50,  "STEER-HYPOTHESIS: mode 3, steer 50, header 9,1,0,1, reserve 1",  {9, 1, 0, 1}, 1},
    {3, 170, "STEER-HYPOTHESIS: mode 3, steer 170, header 9,1,0,1, reserve 1", {9, 1, 0, 1}, 1},
    {3, 50,  "STEER-HYPOTHESIS: mode 3, steer 50, header 1,1,3,1, reserve 0",  {1, 1, 3, 1}, 0},
};

static void base_dirs_task(void* arg)
{
    (void)arg;
    const size_t total = sizeof(s_dirs) / sizeof(s_dirs[0]);
    for (size_t i = 0; i < total; i++) {
        const b220_dirs_step_t* s = &s_dirs[i];
        const bool in_place = (i >= B220_BASE_DIRS_INPLACE_FIRST);
        ESP_LOGW(TAG, "DIRS %u/%u%s: %s  [mode %u, steer %u, speed %d, %u ms]",
                 (unsigned)(i + 1), (unsigned)total,
                 in_place ? " (IN-PLACE candidate)" : "",
                 s->what, (unsigned)s->mode, (unsigned)s->steer,
                 B220_BASE_DIRS_SPEED, B220_BASE_DIRS_WINDOW_MS);
        char cmd[B220_BASE_FRAME_MAX];
        const int n = snprintf(cmd, sizeof(cmd),
                               "AT+CMD=(%u,%u,%u,%u),(%u,%u,%u,%u,%u),(0,0,0,0),(0,0,0,0),"
                               "(0,0,0,0),(0,0,0,0)",
                               (unsigned)s->header[0], (unsigned)s->header[1],
                               (unsigned)s->header[2], (unsigned)s->header[3],
                               (unsigned)s->mode, B220_BASE_DIRS_FIELD_MS,
                               (unsigned)s->steer, B220_BASE_DIRS_SPEED,
                               (unsigned)s->reserved);
        if (n > 0 && (size_t)n < sizeof(cmd)) {
            const uint32_t window = in_place ? (B220_BASE_DIRS_WINDOW_MS + 2000)
                                             : B220_BASE_DIRS_WINDOW_MS;
            for (uint32_t elapsed = 0; elapsed < window;
                 elapsed += B220_BASE_DIRS_REPEAT_MS) {
                (void)b220_base_uart_send(cmd, 120);
                vTaskDelay(pdMS_TO_TICKS(B220_BASE_DIRS_REPEAT_MS));
            }
        }
        (void)b220_base_stop();
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    ESP_LOGW(TAG, "DIRS done - say, for each number, what the wheels did "
                  "(forward / backward / span left / spin right / nothing)");
    vTaskDelete(nullptr);
}

/* Split a comma field, advancing the cursor.  Returns NULL when there is nothing left. */
static char* next_field(char** cursor)
{
    if (cursor == NULL || *cursor == NULL) {
        return NULL;
    }
    char* field = *cursor;
    char* comma = strchr(field, ',');
    if (comma != NULL) {
        *comma = '\0';
        *cursor = comma + 1;
    } else {
        *cursor = NULL;
    }
    return field;
}

void b220_base_uart_handle_test_command(const char* line)
{
    if (line == NULL || strncmp(line, "{BASE+", 6) != 0) {
        return;
    }
    char buf[B220_BASE_LAST_MAX];
    snprintf(buf, sizeof(buf), "%s", line + 6);
    char* brace = strchr(buf, '}');
    if (brace != NULL) {
        *brace = '\0';
    }

    char* cursor = buf;
    const char* verb = next_field(&cursor);
    if (verb == NULL) {
        return;
    }

    /*
     * THE ONE COMMAND THAT NEEDS NO GUESSING.
     *
     * {BASE+RAW,<h1>,<h2>,<h3>,<h4>,<t1.1>..<t1.5>,<t2.1>..<t2.5>}
     *     every header field and two complete control tuples, set by hand:
     *       {BASE+RAW,1,1,3,1,2,300,50,255,1}
     *       {BASE+RAW,1,1,3,1,2,300,50,255,1,3,300,50,255,1}
     *
     * {BASE+RAW,AT+CMD=(...),(...)}
     *     a literal frame is sent VERBATIM, so a frame from the reference project or one
     *     worked out by hand can be pasted straight in.
     *
     * Omitted fields keep the values the bench validated: header 1,1,3,1 and tuple
     * 2,300,50,255,1.  The frame repeats every 250 ms for 5 s (a single frame is only 300 ms
     * of motion), then stops.
     */
    if (strcmp(verb, "RAW") == 0) {
        /* Sub-form: walk one tuple field over a range.  See base_field_sweep_task. */
        if (cursor != NULL && strncmp(cursor, "SWEEP", 5) == 0) {
            const char* spec = strchr(cursor, ',');
            char* copy = strdup(spec != NULL ? spec + 1 : "");
            if (copy == nullptr ||
                xTaskCreate(base_field_sweep_task, "b220_fsweep", 3072, copy, 4, nullptr) !=
                    pdPASS) {
                free(copy);
                ESP_LOGW(TAG, "test: could not start the field sweep");
            }
            return;
        }

        char frame[B220_BASE_FRAME_MAX];
        if (cursor != NULL && strncmp(cursor, "AT+CMD=", 7) == 0) {
            const int n = snprintf(frame, sizeof(frame), "%s", cursor);
            if (n <= 0 || (size_t)n >= sizeof(frame)) {
                ESP_LOGW(TAG, "test: RAW literal is too long (%d bytes, max %d)", n,
                         (int)sizeof(frame) - 1);
                return;
            }
        } else {
            /*
             * NUMBERS.  Which numbers they are depends on how many there are, and that is on
             * purpose: the first version always took four header fields first, so
             *
             *     {BASE+RAW,0,300,0,90,10}
             *
             * - which any reader would call "one control tuple" - was silently read as a
             * HEADER of 0,300,0,90 plus a tuple of 10,300,50,255,1, producing a malformed
             * frame with the (1,1,3,1) header missing.  The bench spotted it in the log and
             * asked "is (1,1,3,1) missing?" - it was.
             *
             * The rule now:
             *   4 or fewer numbers   -> they are the four header fields
             *   5 or more            -> they are TUPLE fields, header stays 1,1,3,1
             *   exactly 5 or 10      -> that is one or two whole tuples, which is the common case
             *   prefix "H,"          -> force the header reading
             */
            /*
             * WHICH NUMBERS ARE WHICH, SAID EXPLICITLY.
             *
             * Counting them was tried and does not work: treating five or more as tuple fields
             * makes {BASE+RAW,2,300,50,90,1} read the obvious way, but it silently breaks the
             * nine-number form this port has been using -
             * {BASE+RAW,1,1,3,1,2,300,50,90,1} - because (1,1,3,1) then becomes part of the
             * tuple.  The bench spotted exactly that in the log.
             *
             * Both spellings must keep working, so the caller says which is meant:
             *
             *   {BASE+RAW,T,...}   the numbers are TUPLE fields, header stays 1,1,3,1
             *   {BASE+RAW,H,...}   the numbers are the four HEADER fields
             *   {BASE+RAW,...}     no prefix: ten or more numbers are header + tuples (the
             *                      spelling used until now), otherwise they are tuples
             *
             *     {BASE+RAW,T,2,300,50,90,1}          one tuple, the short way
             *     {BASE+RAW,H,1,1,3,1}                the header only
             *     {BASE+RAW,1,1,3,1,2,300,50,90,1}    header + tuple, as before
             */
            int mode = 0;   /* 0 = auto, 1 = tuples only, 2 = header first */
            if (cursor != NULL && (cursor[1] == ',' || cursor[1] == '\0')) {
                if (cursor[0] == 'T' || cursor[0] == 't') {
                    mode = 1;
                } else if (cursor[0] == 'H' || cursor[0] == 'h') {
                    mode = 2;
                }
            }
            if (mode != 0) {
                const char* comma = strchr(cursor, ',');
                cursor = (comma != NULL) ? (char*)(comma + 1) : nullptr;
            }

            int nums[14] = {0};
            int got = 0;
            while (got < 14 && cursor != nullptr && *cursor != '\0') {
                nums[got++] = atoi(next_field(&cursor));
            }

            int v[14] = {1, 1, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            const bool as_header = (mode == 2) || (mode == 0 && got >= 10);
            if (as_header) {
                for (int i = 0; i < got && i < 14; i++) {
                    v[i] = nums[i];
                }
            } else {
                v[4] = nums[0];   /* tuples only, after the fixed header */
                for (int i = 1; i < got && (4 + i) < 14; i++) {
                    v[4 + i] = nums[i];
                }
            }
            const int n = snprintf(frame, sizeof(frame),
                                   "AT+CMD=(%d,%d,%d,%d),(%d,%d,%d,%d,%d),(%d,%d,%d,%d,%d),"
                                   "(0,0,0,0),(0,0,0,0),(0,0,0,0)",
                                   v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8],
                                   v[9], v[10], v[11], v[12], v[13]);
            if (n <= 0 || (size_t)n >= sizeof(frame)) {
                ESP_LOGW(TAG, "test: RAW frame did not fit (%d bytes)", n);
                return;
            }
        }
        ESP_LOGW(TAG, "test: RAW -> %s", frame);
        char* copy = strdup(frame);
        if (copy == nullptr ||
            xTaskCreate(base_raw_move_task, "b220_raw", 3072, copy, 4, nullptr) != pdPASS) {
            free(copy);
            ESP_LOGW(TAG, "test: could not start RAW");
        }
        return;
    }

    /* Is the local command channel alive at all?  Touches nothing else. */
    if (strcmp(verb, "PING") == 0) {
        ESP_LOGW(TAG, "test: PING - the local command channel is alive");
        return;
    }

    /*
     * {BASE+ACCEL} - read the on-board accelerometer once and print it.
     *
     * Read-only and one transaction, which is the whole design of b220_gsensor: the chip
     * shares I2C0 with the AW9106B (backlight and audio PA), so this reports what is there
     * rather than starting a sampler.  At rest exactly one axis should sit near +-1000 mg;
     * which axis that is says which way up the board is, and tilting the unit by hand says
     * which axis is which.
     */
    if (strcmp(verb, "ACCEL") == 0) {
        b220_gsensor_state_t state = {};
        const esp_err_t err = b220_gsensor_read(&state);
        char text[160];
        b220_gsensor_describe(&state, text, sizeof(text));
        ESP_LOGW(TAG, "test: ACCEL -> %s", text);
        ESP_LOGW(TAG, "test: ACCEL -> WHO_AM_I 0x%02X (0x%02X expected for an SC7A20/LIS3DH)%s%s",
                 state.who_am_i, 0x11, err == ESP_OK ? "" : " - error: ",
                 err == ESP_OK ? "" : esp_err_to_name(err));
        return;
    }

    /* Ask the baseboard for its version. */
    if (strcmp(verb, "VER") == 0) {
        char rx[96];
        const esp_err_t err = b220_base_uart_send_and_receive("AT+Ver", rx, sizeof(rx), 800);
        ESP_LOGW(TAG, "test: VER -> %s (%s)", rx[0] ? rx : "no reply", esp_err_to_name(err));
        return;
    }

    if (strcmp(verb, "STOP") == 0) {
        (void)b220_baseboard_stop();
        ESP_LOGW(TAG, "test: STOP");
        return;
    }

    /*
     * {BASE+MOVE,<direction>,<speed>,<ms>}
     *   drives through the direction table in b220_baseboard.c, i.e. whatever the normal path
     *   (and the voice assistant) uses.  Speed is the control tuple's speed field, 10..255.
     */
    if (strcmp(verb, "MOVE") == 0) {
        const char* direction = next_field(&cursor);
        if (direction == NULL) {
            ESP_LOGW(TAG, "test: MOVE needs a direction (forward/backward/left/right/"
                          "spin_left/spin_right)");
            return;
        }
        const char* speed_field = next_field(&cursor);
        const char* ms_field = next_field(&cursor);
        const int speed = (speed_field != NULL) ? atoi(speed_field) : 255;
        const int ms = (ms_field != NULL) ? atoi(ms_field) : 2000;
        const esp_err_t err = b220_baseboard_move(direction, speed, ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "test: MOVE rejected (%s)", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "test: MOVE %s speed %d for %d ms", direction, speed, ms);
        }
        return;
    }

    /*
     * {BASE+LRAW,<h1>,<h2>,<h3>,<h4>,<c1.1..c1.5>,<l1.1..l1.4>,<l2.1..l2.4>[,<ms>[,<repeat_ms>]]}
     *
     * A RAW for LIGHT frames.  Built because the bench established that a light frame is an
     * ANIMATION, not a state: {BASE+LIGHT,once,blue} was reported as "blinks two or three
     * times, then off", so re-sending cannot make it steady and the frame itself has to
     * change.  What can change is:
     *
     *   the header                    the reference uses (4,1,0,1) for group lights and
     *                                 (5,1,4,1) / (12,1,3,1) for its combined effects
     *   the first control tuple       the reference puts the not-a-light values here
     *   each light tuple's ID         the reference always writes 7, but 0..255 is untested
     *   each light tuple's DURATION   the reference uses 2000 for "on" and 500 for "short
     *                                 flash", so this may be the animation length or its rate
     *   each light tuple's GROUP      the reference uses 1 and 2; which is which is unknown
     *
     * The last two arguments repeat the frame (`ms` default 1000, `repeat_ms` default 0 = send
     * once), so both a single shot and a stream can be tried without another build.
     *
     * Defaults are the frame this port already sends for a blue front light.
     */
    if (strcmp(verb, "LRAW") == 0) {
        /* Sub-form: walk one field of the light tuples. */
        if (cursor != NULL && strncmp(cursor, "SWEEP", 5) == 0) {
            const char* spec = strchr(cursor, ',');
            char* copy = strdup(spec != NULL ? spec + 1 : "");
            if (copy == nullptr ||
                xTaskCreate(base_light_sweep_task, "b220_lsw", 3072, copy, 4, nullptr) !=
                    pdPASS) {
                free(copy);
                ESP_LOGW(TAG, "test: could not start the light sweep");
            }
            return;
        }
        int n14[14] = {4, 1, 0, 1, 0, 0, 0, 0, 0, 7, 5, 1000, 2, 0};
        int got = 0;
        while (got < 14 && cursor != nullptr && *cursor != '\0') {
            n14[got++] = atoi(next_field(&cursor));
        }
        if (got < 9) {
            ESP_LOGW(TAG, "test: LRAW needs at least 9 numbers (header 4 + control 5), got %d",
                     got);
            return;
        }
        /*
         * The remaining light tuples are whatever was given: two of four fields each is the
         * reference's shape, and a tuple of zeros means "this light is dark".
         */
        int l1[4] = {7, 5, 1000, 2};
        int l2[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4 && (9 + i) < got; i++) {
            l1[i] = n14[9 + i];
        }
        for (int i = 0; i < 4 && (13 + i) < got; i++) {
            l2[i] = n14[13 + i];
        }
        char frame[B220_BASE_FRAME_MAX];
        const int n = snprintf(frame, sizeof(frame),
                               "AT+CMD=(%d,%d,%d,%d),(%d,%d,%d,%d,%d),(%d,%d,%d,%d),(%d,%d,%d,%d),"
                               "(0,0,0,0),(0,0,0,0)",
                               n14[0], n14[1], n14[2], n14[3], n14[4], n14[5], n14[6], n14[7],
                               n14[8], l1[0], l1[1], l1[2], l1[3], l2[0], l2[1], l2[2], l2[3]);
        if (n <= 0 || (size_t)n >= sizeof(frame)) {
            ESP_LOGW(TAG, "test: LRAW frame did not fit (%d bytes, max %d)", n,
                     (int)sizeof(frame) - 1);
            return;
        }
        /* ONE frame, no repeat: these are pictures of an animation, and re-sending one would
         * restart it - which is what the flicker investigation had to rule out. */
        const esp_err_t err = b220_base_uart_send(frame, 120);
        ESP_LOGW(TAG, "test: LRAW -> %s (%s) - watch it", frame,
                 err == ESP_OK ? "sent" : "FAILED");
        return;
    }

    /* {BASE+LIGHT,<colour>} */
    if (strcmp(verb, "LIGHT") == 0) {
        const char* color = next_field(&cursor);
        if (color == NULL) {
            ESP_LOGW(TAG, "test: LIGHT needs a colour (off/white/red/yellow/green/blue/"
                          "deepblue/purple)");
            return;
        }
        /* {BASE+LIGHT,once,<colour>} sends a single frame, for timing how long it lasts. */
        if (strcmp(color, "once") == 0) {
            const char* c2 = next_field(&cursor);
            const esp_err_t err = b220_baseboard_lights_once(c2 != NULL ? c2 : "white");
            ESP_LOGW(TAG, "test: LIGHT once -> %s", esp_err_to_name(err));
            return;
        }
        const esp_err_t err = b220_baseboard_lights(color);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "test: LIGHT rejected - unknown colour");
        } else {
            ESP_LOGW(TAG, "test: LIGHT %s", color);
        }
        return;
    }

    /* Kept because they answer questions nothing else can: {BASE+REF,<code>} sends the
     * reference project's exact frame with only the control code changed. */
    if (strcmp(verb, "REF") == 0) {
        const char* which = next_field(&cursor);
        const int code = (which != NULL) ? atoi(which) : 2;
        char cmd[B220_BASE_FRAME_MAX];
        const int n = snprintf(cmd, sizeof(cmd),
                               "AT+CMD=(1,1,3,1),(%d,300,50,255,1),(0,0,0,0),(0,0,0,0),(0,0,0,0),"
                               "(0,0,0,0)",
                               code);
        if (n <= 0 || (size_t)n >= sizeof(cmd)) {
            return;
        }
        ESP_LOGW(TAG, "test: REF code %d -> %s", code, cmd);
        char* copy = strdup(cmd);
        if (copy == nullptr ||
            xTaskCreate(base_raw_move_task, "b220_ref", 3072, copy, 4, nullptr) != pdPASS) {
            free(copy);
            ESP_LOGW(TAG, "test: could not start REF");
        }
        return;
    }

    /* {BASE+HOLD,<direction>,<speed>,<ms>} - one long frame, stopped from the outside. */
    if (strcmp(verb, "HOLD") == 0) {
        char* spec = strdup(cursor != NULL ? cursor : "");
        if (spec == nullptr ||
            xTaskCreate(base_hold_task, "b220_hold", 3072, spec, 4, nullptr) != pdPASS) {
            free(spec);
            ESP_LOGW(TAG, "test: could not start HOLD");
        }
        return;
    }

    /* {BASE+DIRS} - the earlier mode/steering walk, kept as a bench tool. */
    if (strcmp(verb, "DIRS") == 0) {
        if (xTaskCreate(base_dirs_task, "b220_dirs", 3072, nullptr, 4, nullptr) != pdPASS) {
            ESP_LOGW(TAG, "test: could not start DIRS");
        }
        return;
    }

    /* {BASE+SWEEP} - the repeat-interval walk that established what makes motion continuous. */
    if (strcmp(verb, "SWEEP") == 0) {
        if (xTaskCreate(base_sweep_task, "b220_sweep", 3072, nullptr, 4, nullptr) != pdPASS) {
            ESP_LOGW(TAG, "test: could not start SWEEP");
        }
        return;
    }

    ESP_LOGW(TAG, "test: unknown verb '%s' (RAW / MOVE / STOP / LIGHT / PING / VER / REF / "
                  "HOLD / DIRS / SWEEP / ACCEL)", verb);
}

void b220_base_uart_start_rx_report(void)
{
    if (!s_rx_enabled) {
        return;
    }
    (void)xTaskCreate(base_rx_report_task, "b220_base_rx", 3072, nullptr, 3, nullptr);
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
    /*
     * With our own driver the frame goes through it; without one (the console owns the
     * port and its TX ring is zero-length) the ROM transmitter is the only way out - see
     * the block at the top of this file.
     */
    const int len = (int)strlen(frame);
    if (b220_base_uart_echo_is_test_command(frame)) {
        /* This is a local test command; the RX task will see it come back on the shared
         * wire (the console echoes it in the console build) and must act on it once. */
        b220_base_uart_note_test_echo();
    }
    int written;
    if (s_driver_ours) {
        written = uart_write_bytes(B220_BASE_UART_PORT, frame, (size_t)len);
        if (written == len) {
            (void)uart_wait_tx_done(B220_BASE_UART_PORT, pdMS_TO_TICKS(tx_timeout_ms));
        }
    } else {
        written = base_write_raw(frame, (size_t)len, tx_timeout_ms);
    }
    if (written == len) {
        snprintf(s_last_tx, sizeof(s_last_tx), "%s", cmd);
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "baseboard write short (%d of %d bytes) - the TX FIFO did not drain",
                 written, len);
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
