#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/*
 * Â∞èÈ≠îË±?B220 (XiaoMoDou B220) AI robot toy
 *   ESP32-D0WD rev1.0, 16 MB flash (GigaDevice GD25Q128), 8 MB PSRAM
 *
 * Every pin below was recovered from the factory firmware image; the evidence
 * (addresses, structs, disassembly) is documented in
 *   <workspace>\PORTING-FEASIBILITY.md
 * and the raw analysis in the same directory (DEVICE-ANALYSIS.md, GPIO-FINDINGS.md).
 */

/* ------------------------------------------------------------------ audio --
 * The stock audio chain is: 3 mics -> ZL38063 (AEC/NR/beamforming) -> I2S ->
 * ESP32 -> I2S -> ZL38063 cross-point -> internal DAC -> speaker.
 * The ESP32 is the I2S master on ONE full-duplex I2S0 link:
 *   i2s_pin_config_t @ DROM 0x3f4317ac = {bck 5, ws 25, dout 26, din 35}
 * The ZL38063 must be host-initialised over SPI HBI (reset = GPIO19, CS =
 * GPIO0 on the shared SPI2 bus) - that is Phase 3 work.  Phase 2 uses
 * DummyAudioCodec so the rest of the firmware can be brought up first.
 */
/* Phase 4 (production audio): these are the rates the ONE I2S0 link to the
 * ZL38063 really runs at.  B220_I2sUpBeforeDspBoot() configures that port at
 * 48000 Hz / 16-bit / stereo BEFORE the DSP reset (the DSP is an I2S slave and
 * needs BCLK/WS running before it answers over HBI), and the DSP does no rate
 * conversion of its own - the proven 1 kHz test tone written to this port at
 * 48 kHz was audible from the speaker at 1 kHz.  The audio service resamples
 * around these values (input: esp_ae_rate_cvt 48k -> 16k; output: the server's
 * rate -> 48k), so declaring 16000 for a 48 kHz port would run capture and
 * playback three times too fast.  The ZL38063 itself is driven by the board's
 * boot bring-up, not by this value - see xiaomodou_b220.cc / zl38063_dsp.c. */
#define AUDIO_INPUT_SAMPLE_RATE  48000
#define AUDIO_OUTPUT_SAMPLE_RATE 48000

#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_25
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_26
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_35

#define ZL38063_RESET_GPIO  GPIO_NUM_19
#define ZL38063_HBI_CS_GPIO GPIO_NUM_0

/* ---------------------------------------------------------------- display --
 * ILI9341 240x320 on SPI2 (10 MHz), sharing that bus with the ZL38063 HBI.
 * Pins from the factory lcd config struct @ DROM 0x3f401654.
 */
#define DISPLAY_SPI_HOST     SPI2_HOST
#define DISPLAY_SPI_SCK_PIN  GPIO_NUM_32
#define DISPLAY_SPI_MOSI_PIN GPIO_NUM_33
#define DISPLAY_SPI_MISO_PIN GPIO_NUM_34
#define DISPLAY_SPI_CS_PIN   GPIO_NUM_27
#define DISPLAY_DC_PIN       GPIO_NUM_21
#define DISPLAY_RST_PIN      GPIO_NUM_22

/* --- geometry -------------------------------------------------------------
 * The GLASS is 128 x 160 (measured on hardware, see the panel notes above).
 * DISPLAY_WIDTH / DISPLAY_HEIGHT are the LVGL *logical* screen: what Layout()
 * and LV_HOR_RES / LV_VER_RES see.  Everything the panel driver is told is
 * derived from these two plus DISPLAY_SWAP_XY / DISPLAY_MIRROR_*, so the
 * logical size and the transform can never drift apart.
 *
 *   DISPLAY_SWAP_XY == false -> logical 128 x 160 (portrait, the stock idea)
 *   DISPLAY_SWAP_XY == true  -> logical 160 x 128 (landscape, rotated 90 deg)
 *
 * B220_DISPLAY_ROTATE_90 is the knob that flips DISPLAY_SWAP_XY and recomputes
 * the logical size, because a 90 deg rotation is exactly "swap X and Y".
 * DISPLAY_MIRROR_X / _Y are then swapped with it so that the *relative*
 * handedness of the picture is unchanged - the result is a pure rotation and
 * not a rotation plus a mirror.
 */
#define B220_DISPLAY_ROTATE_90 1

#define B220_DISPLAY_PANEL_WIDTH   128
#define B220_DISPLAY_PANEL_HEIGHT  160

#if B220_DISPLAY_ROTATE_90
/* 90 deg: X and Y change places, so the logical canvas is 160 x 128.  This is
 * the meaningful rotation for this toy: the glass sits in a wide, short window,
 * so portrait artwork is cramped while landscape fits the "eyes" UI.
 *
 * The transform handed to the panel driver is a real MADCTL write, and the
 * flush path then draws straight into panel coordinates.  lvgl_port_disp.c
 * (_lvgl_port_transform / lvgl_port_disp_rotation_update) is what normally
 * applies this at LV_DISPLAY_ROTATION_0, so this switch is the conventional
 * way to rotate: the port is left at ROTATION_0 and the panel does the work. */
#define DISPLAY_WIDTH   B220_DISPLAY_PANEL_HEIGHT   /* 160 */
#define DISPLAY_HEIGHT  B220_DISPLAY_PANEL_WIDTH    /* 128 */
#define DISPLAY_SWAP_XY  true
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y true
#else
#define DISPLAY_WIDTH   B220_DISPLAY_PANEL_WIDTH    /* 128 */
#define DISPLAY_HEIGHT  B220_DISPLAY_PANEL_HEIGHT   /* 160 */
#define DISPLAY_SWAP_XY  false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#endif

/* The factory init table issues 0x21 (INVON) itself and sets MADCTL (0x36)=0x00
 * (RGB order, no mirror/swap), so the panel driver must not invert or re-map
 * the colour order.  Note the driver does NOT re-read MADCTL after the factory
 * table: it caches the value written by its own init sequence, so the
 * mirror/swap calls below compose correctly on top of the table's 0x00. */
#define DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0
#define DISPLAY_SPI_MODE 0

/* Compile-time guard: the two numbers below must be the same canvas.  If they
 * ever stop matching, the panel transform and the LVGL resolution describe
 * different screens and the picture tears or wraps at the edges. */
#define B220_DISPLAY_LOGICAL_IS_VALID()                                        \
    ((DISPLAY_WIDTH == (B220_DISPLAY_ROTATE_90 ? B220_DISPLAY_PANEL_HEIGHT     \
                                               : B220_DISPLAY_PANEL_WIDTH)) && \
     (DISPLAY_HEIGHT == (B220_DISPLAY_ROTATE_90 ? B220_DISPLAY_PANEL_WIDTH    \
                                                : B220_DISPLAY_PANEL_HEIGHT)))

/* The stock LCD config struct carries no backlight pin: the panel backlight is
 * either tied on or driven from the baseboard.  Leave it uncontrolled for now. */
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_NC
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

/* ---------------------------------------------------------------- buttons --
 * This board has NO button wired to the ESP32:
 *   - the ÂêØÂä®Èî?is a soft power on/off latch on the baseboard (press once =
 *     power off, press again = power on); the MCU cannot read it;
 *   - GPIO0 is the ESP32 boot strapping pin and, on the stock hardware, the
 *     ZL38063 HBI chip-select.  The toy's own micro-USB bridge pulls it low at
 *     power-on, which is why plugging that USB in puts the chip into UART
 *     download mode instead of booting the app.  Unplug it to run.
 * xiaozhi therefore runs button-less here: it enters WiFi config mode by itself
 * when no credentials are stored, and the wake word becomes the way to talk to
 * it once the ZL38063 audio path is up (Phase 3).
 */

/* ------------------------------------------------------------------- diag --
 * Bring-up diagnostics.  The backlight question is answered (AW9106B DIM0-2),
 * so both are off; set B220_BACKLIGHT_SWEEP=1 (with B220_DIAGNOSTIC=1) if an
 * unknown enable pin ever needs hunting again.
 */
#define B220_DIAGNOSTIC       0
#define B220_BACKLIGHT_SWEEP  0

/* One-shot orientation card at boot (8 colour bars + two white corners).
 * Geometry is confirmed (vertical bars, white corners TL+BR), so it is off.
 * NOTE: a raw esp_lcd_panel_draw_bitmap() buffer must be byte-swapped by hand
 * (the panel expects the high byte first) - that is why the card looked
 * colour-swapped while the LVGL UI does not. */
#define B220_ORIENT_CARD      0

/* ------------------------------------------------------- button discovery --
 * B220_BUTTON_DISCOVERY turns the firmware into a probe for the toy's three
 * physical keys (record / volume-up / volume-down).
 *
 * Why it is needed: nothing in the stock image or in the reversals so far says
 * which chip samples those keys.  The stock button code takes its pins from a
 * runtime configuration (so they are not immediates in the image), the stock
 * ZL38063 configuration record leaves the whole DSP GPIO register block
 * (0x0700..0x0A22) at zero, and the ESP32 pins this port already owns cover the
 * LCD, I2S, HBI, I2C and the DSP reset - which leaves GPIO 20 / 36 / 39 as the
 * only plausibly free ones.
 *
 * So rather than guess, the probe samples EVERY input source the board could
 * use and logs the moment one changes:
 *   - ESP32 GPIOs 20, 36, 39 (plus 0 only if B220_BUTTON_PROBE_GPIO0 = 1),
 *     each read with the internal pull-up on and with every pull off;
 *   - the AW9106B (I2C 0x5B) input-data registers 0x00/0x01 and its mode/output
 *     registers;
 *   - the ZL38063 over HBI: its whole register address space, pages
 *     0..B220_BUTTON_DSP_PAGES-1;
 *   - the SC7A20 accelerometer (I2C 0x19) CLICK/INT1 status registers.
 *
 * It ONLY reads: no DSP reg 0x0014, no reset, no page-255 window base, no
 * AW9106B reg 0x02 (so the AW8737S keeps its one SHDN rising edge).  The task it
 * starts is compiled out entirely when this is 0, which is the production value.
 *
 * Procedure: flash with it at 1, leave the unit alone ~30 s, then press and hold
 * one key at a time; the `btn-disc: CHANGE` lines name the source. */
#define B220_BUTTON_DISCOVERY   0
/* Deepest HBI page swept.  Pages 0..3 are the documented register range; the
 * config record lives in 0x02..0x0A.  0x40 pages x 128 words = 8192 addresses,
 * which is the whole HBI space the 8-bit page field can address. */
#define B220_BUTTON_DSP_PAGES   0x40
/* Pages re-read on the fast tick; the rest of the space is swept on the slow
 * tick and at boot.  4 pages = 512 reads per fast sweep. */
#define B220_BUTTON_DSP_LIVE_PAGES 4
#define B220_BUTTON_FAST_PERIOD_MS 300
#define B220_BUTTON_DEEP_PERIOD_MS 30000
#define B220_BUTTON_TOP_PERIOD_MS  30000
/* The TWO ZL38063 registers the stock firmware's own buttonTask polls:
 * HBI 0x0000 ("status") and HBI 0x0002 (the event/key code).  Reverse
 * engineering of the stock image found that this - not an ESP32 GPIO and not the
 * AW9106B - is where the stock gets its key input from (BUTTON-HARDWARE-REVERSE.md),
 * and the stock polls it every 10 ms (gpioclear = 10 ticks, with a 50-tick
 * debounce and a 2000-tick long press).  A key event can be a short pulse, so
 * these two are also polled here, separately from the coarse whole-register
 * sweep, at the stock cadence. */
#define B220_BUTTON_BTN_PERIOD_MS  10
/* Deep whole-address-space sweep (0..B220_BUTTON_DSP_PAGES-1).  The first
 * hardware run showed it is not needed and actively harmful: the two registers
 * that carry the keys are polled on their own at B220_BUTTON_BTN_PERIOD_MS, and
 * the deep sweep's thousands of extra HBI word reads ring onto the input-only
 * GPIOs and produce a wall of `CHANGE zl38063-hbi: reg 0x00DC ...` lines that
 * are just the HBI command word being read back.  Set to 1 only to go looking
 * for an unknown register again. */
#define B220_BUTTON_DEEP_SWEEP     0
/* GPIO0 is the ZL38063 HBI chip select AND the boot strapping pin.  Sampling it
 * is very likely still fine (a read with a pull-up only ever helps it stay
 * high), but a board that drives it low would produce a permanent "change", so
 * it is opt-in and off by default. */
#define B220_BUTTON_PROBE_GPIO0 0

/* ------------------------------------------------------- the three keys -----
 * The keys are read THROUGH the ZL38063 audio front-end, not from an ESP32 GPIO
 * and not from the AW9106B (see b220_buttons.h / .c and BUTTON-HARDWARE-REVERSE.md).
 *
 * THE TWO REGISTERS (measured on this unit, then confirmed independently):
 *
 *   HBI 0x0000  "status"  0x0000 idle, 0x0003 PRESS, 0x0002 RELEASE.
 *                         **bit 0 tells a press from the release.**
 *   HBI 0x0002  "key"     the key code, valid while status != 0:
 *                         0x0008 = volume up, 0x000A = volume down,
 *                         0x000B = record / MIC (the talk key).
 *
 * ONE PHYSICAL PRESS PRODUCES TWO EVENTS - a press (status bit0 = 1) and a
 * release (status = 0x0002) - and the driver acts only on the press.  Acting on
 * both is what made an early revision run every function twice.
 *
 * THE MAPPING IS CONFIRMED TWICE:
 *   - the hardware owner pressed one key at a time and reported 0x000B = record,
 *     0x0008 = volume up, 0x000A = volume down;
 *   - the independent open-source port of this same toy, ref/Flower_modou
 *     (components/modou_drivers/modou_dsp_keys.c), names exactly the same codes:
 *     `case 0x08: return "VOL+"; case 0x0a: return "VOL-"; case 0x0b: return "MIC";`
 *
 * If a key ever does the wrong thing, these three values are the only thing to
 * swap: the driver logs the raw key and status of every action it takes. */
#define B220_BUTTON_KEY_1          0x000B  /* record / talk key */
#define B220_BUTTON_KEY_2          0x0008  /* volume up         */
#define B220_BUTTON_KEY_3          0x000A  /* volume down       */

/* Poll cadence and long-press threshold, both copied from the stock firmware:
 * gpioclear = 10 ticks (10 ms), gpiolongpress = 2000 ticks (2 s). */
#define B220_BUTTON_POLL_MS        10
/* THERE IS NO LONG PRESS ON THIS HARDWARE - and no macro for one any more.
 *
 * Measured with an instrumented build: a deliberate 4-second hold and a
 * 0.3-second tap produce exactly the same sequence on this interface - one
 * 0x0003 carrying the key code, then nothing but 0x0000, and 0x0002 about
 * 140..200 ms later regardless of how long the key was really held.  GPIO38 reads
 * HIGH throughout a hold.  The hold duration is simply not observable.
 *
 * Three attempts to time a hold (level, event gaps, then the GPIO38 line) all
 * failed on hardware; the record is in docs/B220-LED-AND-BASE-UART.md ¬ß2.6.
 * Every key press is therefore one action: B220_BUTTON_VOLUME_STEP per press.
 *
 * If a "volume to max/min" gesture is wanted later, the honest way is a
 * double-press or a sequence of presses - not a long press. */
/* Volume step per key press, and the range it is clamped to. */
#define B220_BUTTON_VOLUME_STEP    10
/* Double-press window for the volume keys: one press steps the volume, two
 * presses of the SAME key inside this window jump straight to the end of the
 * range.  (A double press is used instead of a long press because this front-end
 * cannot report how long a key was held - see b220_buttons.h.)
 *
 * The single-press step is deferred by this long, so that a double press never
 * moves one step first and then jumps.  Shorter feels snappier but is easier to
 * miss; 400 ms is the usual double-click budget. */
#define B220_BUTTON_DOUBLE_CLICK_MS 400
#define B220_BUTTON_VOLUME_MIN     0
#define B220_BUTTON_VOLUME_MAX     100

/* ------------------------------------------------------------- indicator --
 * The board's own indicator lights are AW9106B constant-current channels, not
 * GPIOs (see b220_led.h).  b220_led.c drives registers 0x23/0x24 and leaves the
 * LCD backlight channels 0x20/0x21/0x22 to InitializeBacklight(), which sets
 * them to 0x3F from the stock sequence.
 *
 * The two indicator channels were left at 0x80 by the factory replay and nothing
 * ever touched them again before this.  Confirming WHICH physical light each
 * channel is still needs a bench check: the independent port of this toy treats
 * 0x20/0x21/0x22 as a top RGB LED, and that mapping DOES hold on this unit: the
 * six RGB LEDs are in parallel, common anode, with R/G/B on OUT0/OUT1/OUT2, i.e.
 * reg 0x20/0x21/0x22.  They need the output driver configured (0x15/0x16) and
 * GPMD_A = 0x0A to sink current at all - see B220-LED-AND-BASE-UART.md.
 *
 * 0x23/0x24 are the LCD BACKLIGHT on this unit (measured), and this port parks
 * them at 0x80, the factory value. */
#define B220_LED_INDICATOR_IDLE_LEVEL 0x00   /* dark while idle (power saving) */

/*
 * LCD backlight, in the framework's 0..100 percent units (not raw register
 * values).  The panel is on AW9106B DIM3/DIM4 (reg 0x23/0x24), which take 0..255;
 * B220_Backlight in xiaomodou_b220.cc converts and writes them.
 *
 * Measured 2026-10-10: the panel is brighter at every value up to 0xFF, i.e. the
 * backlight uses the same 8-bit range as the RGB channels, so 0xFF (100%) is the
 * hardware maximum - hence a default of full brightness.
 *
 * These are DEFAULTS ONLY, used the first time the device boots.  Afterwards the
 * user's choice lives in NVS under "display"/"brightness" (the framework's
 * Backlight base class owns that key, plus the fade and the clamp to 0..100), and
 * the board restores it at start-up.  To change the out-of-box default, change
 * this number; to change what a device does today, that is a setting, not a build.
 */
#define B220_LED_BACKLIGHT_DEFAULT_PERCENT 100
#define B220_LED_BACKLIGHT_MAX_PERCENT     100

/* ONE-SHOT BENCH MEASUREMENT, off in production: with this set to 1, the boot
 * ramps the LCD backlight through 0x3F/0x80/0xBF/0xE0/0xFF (3 s each) and logs
 * every step, to establish the backlight's usable range.  Measured 2026-10-10:
 * the panel gets brighter at every step and 0xFF is the brightest, i.e. the
 * backlight uses the same 8-bit DIM range as the RGB channels (datasheet 9.2).
 * The port still PARKS it at 0x80 - the factory value - because raising it is a
 * power/heat decision, not a correctness one (see B220_LED_BACKLIGHT_MAX below). */
/* The debug log is available over TCP (port 3333) as well as on the serial port, so
 * the serial connector can be handed to the baseboard once it is plugged in - the
 * baseboard covers that connector, and the port is also what makes baseboard REPLIES
 * unreadable while the console owns it.  b220_tcp_log.cc installs the hook before
 * app_main(), so the buffered stream contains the whole boot.
 *
 * With this set to 1, a volume-down press also dumps the recent log to the SERIAL
 * port: the network is the convenient channel, not a dependable one, and "the network
 * will not come up" is exactly the kind of fault worth being able to see.  Set to 0 to
 * give the key back its single meaning. */
#define B220_TCP_LOG_DUMP_ON_VOLUME_DOWN 1

#define B220_LED_WALK 0

/* TEMPORARY bring-up diagnostic: print the FreeRTOS task list (name/state/priority/
 * stack/core) every 10 s from the indicator task, so a suspected CPU hog can be
 * identified from real data instead of from the watchdog's "currently running"
 * line.  Set to 0 for production. */
#define B220_DIAG_TASK_STATS 0

/* ------------------------------------------------------- baseboard UART ----
 * The baseboard MCU (motors, head/tail lights) is on UART0 at 115200 8N1, the
 * same pins as the console and the flashing port: TX GPIO1 / RX GPIO3.  This is
 * confirmed by the porting guide's reading of the stock firmware and by the
 * independent port of this toy (ref/Flower_modou), which drives UART_NUM_0 on
 * GPIO1/3.
 *
 * B220_BASE_UART_HANDSHAKE sends "AT+Pon" once shortly after boot; the stock uses
 * that to wake the baseboard and it is what unlocks the motors.  It is safe with
 * the console sharing the wire: the baseboard ignores text it does not recognise,
 * which is how the stock firmware's own log spam coexists with it. */
#define B220_BASE_UART_HANDSHAKE   1
/* Wait this long before the first frame so the ROM banner and the early log burst
 * are out of the way. */
#define B220_BASE_UART_HANDSHAKE_DELAY_MS 3000

/* Panel backlight lives on the AW9106B LED driver, not on a GPIO. */
#define B220_AW9106_I2C_SDA   GPIO_NUM_18
#define B220_AW9106_I2C_SCL   GPIO_NUM_23
#define B220_AW9106_ADDR      0x5B

/* Phase 3-‚ë?: bring the ZL38063 HBI link up at boot and report what it says
 * (reset pulse + register probe + config record).  No I2S yet - the audio
 * codec stays a stub until the link is proven on hardware. */
#define B220_AUDIO_BRINGUP    1

/* Phase 3-‚ë?: start the ESP32 I2S master BEFORE the ZL38063 reset/boot.
 *
 * Hypothesis under test: the ZL38063 takes its master clock from the ESP32 I2S
 * BCLK (pin 5), so it stays mute on HBI - all register reads 0x0000, reg 0x34
 * never 0xD3D3 - as long as no BCLK is present, even though its rails and its
 * GPIO19 reset are fine.  The stock firmware's order is
 *   [codec/I2S init] -> [GPIO19 reset pulse] -> [host boot over HBI].
 *
 * With this at 1, xiaomodou_b220.cc brings up a full-duplex 48 kHz / 16-bit /
 * stereo I2S0 master (bck 5, ws 25, dout 26, din 35) before it calls
 * zl38063_dsp_begin(), and then retries zl38063_dsp_begin() up to
 * B220_DSP_BOOT_ATTEMPTS times while logging every attempt.  Set to 0 to get the
 * old order back (HBI with no I2S at all).
 *
 * MCLK is NOT driven: the stock pin map has no MCLK pin and the ZL38063 clocking
 * is documented as BCLK-derived here (i2s_pin_config_t @ DROM 0x3f4317ac). */
#define B220_I2S_BEFORE_DSP   1

/* How many times the ZL38063 reset + host-boot sequence is attempted when the
 * DSP has not answered yet.  6 mirrors the stock firmware's retry count. */
#define B220_DSP_BOOT_ATTEMPTS 6

/* Phase 3-‚ë?: prgmBase / execAddr boot-address matrix.
 *
 * State of the bring-up when this was added: with I2S up BEFORE the DSP boot the
 * host upload now runs to completion (reg 0x14 <- 1, the reg 0x34 == 0xD3D3 gate
 * passes, all 3699 records transferred, reg 0x12C <- execAddr, reg 0x32 <-
 * 0x000D, FirmwareStart), but reg 0x030 & 0x0F still reads 0x0000 - the DSP does
 * not report a running application.  Our two reverse-engineering passes disagree
 * about which of 0x00080200 / 0x00083040 is the RAM entry point written to
 * register 0x12C (see zl38063/PROTOCOL-REPORT.md vs zl38063_firmware_image.md),
 * and today's build writes 0x00083040.
 *
 * When 1, xiaomodou_b220.cc calls zl38063_dsp_boot_addr_matrix() once at boot,
 * after the normal attempt and after the B220_ZL_DIAG probes, and only while the
 * DSP still reports "not running".  Each matrix attempt is a full reset + host
 * boot + ~1 s settle + register dump (~4-5 s of boot time each): first today's
 * pairing (0x00080200 / 0x00083040), then the swap (0x00083040 / 0x00080200).
 * The matrix stops at the first pairing that reports a running application and
 * leaves those addresses in place, so the config-record path that follows works
 * against whichever pairing won.  Set to 0 for the single-pairing boot again. */
#define B220_DSP_ADDR_MATRIX 0

/* Phase 3-‚ë?: ZL38063 HBI byte-order / MISO diagnostic.  When 1,
 * xiaomodou_b220.cc calls zl38063_dsp_hbi_diag() right after
 * zl38063_dsp_begin() and the unit prints the interesting HBI registers twice,
 * once with the 16-bit word byte-swap on and once with it off.  A register map
 * that reads meaningfully in exactly one of the two blocks proves the wire
 * order; two identical blocks of 0x0000/0xFFFF mean MISO is not being driven
 * at all (pins, CS, power) rather than a byte-order problem.
 *
 * It also enables two further probes in the same call group:
 *   zl38063_dsp_gpio_forensics() - GPIO19 ownership matrix + AW9106 group A;
 *   zl38063_dsp_hbi_sweep()      - brute-force CS x SPI mode x clock, one
 *                                  read-only register read per combination,
 *                                  then the normal device is restored.
 * The sweep tries CS 0, 2, 4, 12-15, 19, 20, 24, 27 (the LCD's CS, as a
 * control), 28-31 with modes 0/3 and 10/1 MHz; pins owned by the LCD, I2C, I2S,
 * the console and flash/PSRAM are excluded (see zl38063_dsp.c). */
#define B220_ZL_DIAG 0

/* Phase 3-‚ë?: host-upload read-back verification.  One-line kill switch.
 *
 * When 1, xiaomodou_b220.cc calls zl38063_dsp_verify_uploaded_image() once,
 * after the boot attempts have all reported "no application running" and before
 * the HBI diagnostics.  It re-points the page-255 window base at each sampled
 * record's targetAddr exactly the way the upload does and reads the record's
 * words back out of DSP RAM to compare them with the image we sent, so the open
 * hypothesis "the transferred image content is wrong (bad record replay / wrong
 * write target / wrong word count), so the DSP executes garbage" gets a number
 * in the log instead of another guess: mismatching_words = 0 means our image
 * really is in DSP RAM and the problem is elsewhere.
 *
 * It samples every 4th record (always including 0, 1, 2 and the last two) and
 * costs roughly a third of the upload itself, i.e. ~0.4-0.7 s of extra boot time
 * when the DSP does not come up; nothing at all when an application is running.
 * Read-only on the device apart from the window-base register writes the upload
 * already performs, so it leaves the DSP as the boot attempt left it.
 * Set to 0 to get the previous boot behaviour back. */
#define B220_DSP_VERIFY_UPLOAD 0

/* Phase 3-10: page-255 window addressing calibration.  One-line kill switch.
 *
 * When 1, xiaomodou_b220.cc calls zl38063_dsp_page255_calibrate() once, right
 * before zl38063_dsp_verify_uploaded_image().  That verify reads the uploaded
 * image back out of DSP RAM through the page-255 window, and the read side is our
 * own inference - Microsemi's driver only ever WRITES the window, there is no
 * reference read anywhere in zl38063/adf_ref/ - so its numbers cannot mean
 * anything until the addressing is measured.  The calibration writes a known
 * 16-word pattern to scratch DSP RAM (0x00080A24, the first word of the single
 * documented hole 0x00080A24..0x00082FFF inside the range the image covers) using
 * exactly the write path the upload uses (reg 0x0C <- {hi16,lo16}, then the
 * 0xFF00 | lowbyte window write), then reads it back under all 8 combinations of
 *   enc  = offset field is the raw low byte (a) vs low byte / 2 (b),
 *   preselect = send SELECT_PAGE(0xFF) = 0xFEFF first (1) or not (0),
 *   base = reg 0x0C <- {hi16,lo16} (hi_lo) vs {lo16,hi16} (lo_hi),
 * logging one line per combination
 *   calib enc=<a|b> preselect=<0|1> base=<hi_lo|lo_hi> -> matched m/16
 * plus a conclusion, and the verify then reads the window with the encoding the
 * calibration reproduced the pattern with.
 *
 * It writes 16 words of scratch DSP RAM, so it runs only on the "no application
 * running" path and only once per boot; ~8 small HBI round trips, milliseconds.
 * Set to 0 to get the previous boot behaviour back (the verify then falls back to
 * the encoding the upload's own write produces on the wire).
 */
#define B220_DSP_PAGE255_CALIB 0

/* Phase 3-‚ë?: "hands off" the DSP - let it auto-boot from its own flash.

 * The hardware finding that forced this switch: a full 60 s boot log of the
 * STOCK firmware on the real board (baseboard unplugged, sound works) contains
 * only {AT+Pon} traffic after startup.  It never prints
 * `1- Firmware boot loading started ....`, `prgmBase 0x‚Ä¶`, `execAddr 0x‚Ä¶` or
 * `2- Loading the image to RAM....done`, so the stock firmware never host-boots
 * the ZL38063 at all: the DSP auto-boots from its own slave SPI flash and the
 * stock only configures it and uses it over I2S.
 *
 * Our firmware does the opposite - GPIO19 reset pulse, reg 0x14 <- 1 (which
 * forces boot-ROM mode), 3699 record writes, entry point, GO - and that
 * sequence may be destroying a perfectly running DSP application, which would
 * explain why the upload "succeeds" while reg 0x030 never reports a running app.
 *
 * When 1, InitializeAudioDsp() takes the hands-off path instead:
 *   1. I2S up first, with the unchanged B220_I2sUpBeforeDspBoot() call (the DSP
 *      keeps its BCLK);
 *   2. zl38063_hbi_init() + VprocTwolfHbiInit() only: attach the HBI device to
 *      SPI2 and write the single HBI configuration register (0xFD80), which is
 *      the only device write the stock driver itself makes.  GPIO19 is NOT
 *      configured, driven or read, so the DSP reset net is left exactly as the
 *      board powers it up: no reset pulse, no reg 0x14 <- 1, no host-boot
 *      upload, no boot-address matrix, no config record, no volume write, and
 *      no diagnostic that writes a device register;
 *   3. poll reg 0x0030 / 0x0032 / 0x0034 / 0x0006 / 0x0020 / 0x0022 / 0x0024
 *      once per second for B220_DSP_HANDS_OFF_POLLS seconds (read-only), log the
 *      first non-zero app status and then the verdict - does the auto-booted
 *      application report RUNNING on its own, or does it never come up?
 *
 * Cost: ~20 s of boot time for the polls plus the unchanged I2S bring-up; it
 * replaces the reset + 3699-record upload + verify + diagnostics, which cost
 * more than that here.  Set this back to 0 to restore today's host-boot
 * behaviour exactly (one-line revert): with 0, every existing diagnostic, the
 * verify-upload path and the boot-address matrix are still compiled and
 * unchanged. */
#define B220_DSP_HANDS_OFF 0

/* Number of 1 s hands-off polls logged before the verdict (20 ‚â?20 s). */
#define B220_DSP_HANDS_OFF_POLLS 20

/* Phase 3-11: verify the image in DSP RAM BEFORE the GO.

 * State of the bring-up when this was added (hardware-proven, do not
 * re-investigate):
 *   - the page-255 WINDOW WRITE path is correct: a 16-word pattern written to
 *     scratch DSP RAM 0x00080A24 with the upload's own write path reads back
 *     16/16 exact with enc=b, and the scale probe proves field+1 = one word;
 *   - reg 0x0C reads back {0x0008,0x0A00} when written {0x0008,0x0A24} (the low
 *     byte is ignored / 256-byte aligned - harmless);
 *   - YET the upload's own data is not in DSP RAM afterwards: with that same
 *     calibrated encoding, target 0x00080200 reads 0x0000 although we sent
 *     0x1C0F 0x0010 0x0041 ..., and reg 0x0034 goes 0xD3D3 (boot ROM waiting) ->
 *     0x0000 (boot ROM left) while reg 0x0030 never reports a running app.
 *
 * The two surviving explanations - "the image IS transferred and started, but the
 * DSP crashes/resets afterwards and its RAM is cleared" and "the transfer never
 * writes the image records at all" - are indistinguishable in a post-GO-only log.
 * This switch separates them: it installs a hook that the upload path calls
 * IMMEDIATELY AFTER the last image record has been written and BEFORE register
 * 0x12C (entry point) and register 0x32 <- 0x000D (host load complete) are
 * written, i.e. before any host command can have reached the device.  The hook
 * runs the same calibrated read-back on at least 8 records including record 0 and
 * logs one line per record plus a summary, then the state of the read-only
 * registers 0x0034 / 0x0030 / 0x0032 / 0x0006.
 *
 * With B220_DSP_VERIFY_UPLOAD = 1 as well, one boot then shows BOTH states:
 * pre-GO (inside the upload) and post-GO (after the retries) - that comparison is
 * the whole point.  The hook is a no-op when this is 0 (nothing is registered and
 * the upload path's call site compiles away), so the default behaviour is
 * unchanged.
 *
 * Cost: ~8 records x (window base write + one HBI word read per word), a few
 * milliseconds, once per upload - i.e. once per boot attempt, not once per
 * retry-loop iteration.  Read-only apart from the page-255 window base register
 * 0x0C, which the upload itself writes for every record anyway. */
#define B220_DSP_VERIFY_BEFORE_GO 0
/* Phase 3-12: ZL38063 hook-release matrix (B220_DSP_RELEASE_MATRIX).

 * State of the bring-up when this was added (hardware-proven, do not
 * re-investigate):
 *   - the host-boot UPLOAD is CORRECT: the pre-GO hook (B220_DSP_VERIFY_BEFORE_GO)
 *     verifies 9 records spanning the whole image BEFORE any release command is
 *     sent and reports mismatching_words=0 with the read-back identical to what
 *     was sent, while reg 0x0034 still reads 0xD3D3 (boot ROM still waiting);
 *   - after the release sequence (reg 0x12C <- 0x00083040, reg 0x32 <- 0x000D,
 *     VprocTwolfFirmwareStart() -> reg 0x32 <- 0x0008) the DSP leaves its boot
 *     ROM (reg 0x0034 -> 0x0000) but reg 0x0030 never reports a running
 *     application, and a later read of the image area returns zeros: the DSP
 *     appears to reset/crash at or after the release.

 * The open question is which of the two host commands actually releases the boot
 * ROM.  The early RE marked the vendor's VprocTwolfFirmwareStart (0x40126210) as
 * a DELAY STUB (ets_delay_us), i.e. the stock firmware may never write 0x0008 at
 * all and the GO may be implied by 0x000D alone - while this port always writes
 * both.

 * When 1, xiaomodou_b220.cc calls zl38063_dsp_release_matrix() instead of the
 * normal post-upload sequence (and instead of the boot-address matrix below).
 * It runs one FULL fresh attempt per variant - reset pulse, HBI, HBI config,
 * complete 3699-record upload - and then issues exactly that variant's release
 * writes, waits ~1 s and logs reg 0x0030 / 0x0032 / 0x0034 / 0x0006 plus the
 * read-back of the same 9 sampled image records.  It stops at the first variant
 * whose masked app status (reg 0x0030 & 0x0F) is non-zero.  Variants:
 *   1. reg 0x12C only
 *   2. reg 0x12C + reg 0x32 <- 0x000D                    (no 0x0008 anywhere)
 *   3. reg 0x12C + reg 0x32 <- 0x0008                    (no 0x000D)
 *   4. reg 0x12C + reg 0x32 <- 0x000D + reg 0x32 <- 0x0008  (today's behaviour)
 *   5. reg 0x12C + reg 0x32 <- 0x000D + the 260-entry config record +
 *      reg 0x32 <- 0x0008                                (config before the GO)

 * To be able to emit or suppress each of 0x000D and 0x0008 individually, the
 * upload path's own two writes are gated at their call SITES by
 * zl38063_vproc_release_*_from_host_boot() (zl38063_vproc.c/.h): the record
 * transfer itself (HbiSrecBoot_alt(), including reg 0x12C) is untouched, and
 * with no variant set every byte on the wire is the one the default path always
 * wrote.

 * Cost: one full reset + upload per variant (~4-5 s each, ~25 s worst case for
 * all five, less when one of them comes up running) plus ~1 s settle per variant
 * and the 9-record read-back.  It refuses to run while an application is already
 * running.  The switch is 0 by default and a 0 build compiles the matrix and its
 * gate calls out completely. */
#define B220_DSP_RELEASE_MATRIX 0

/* Phase 3-12: the prgmBase/execAddr boot-address matrix is DISABLED while the
 * release matrix above runs.  Both are full reset + 3699-record uploads, and
 * both are chasing "the DSP never reports a running application"; running them in
 * the same boot would add 2 extra complete attempts (~10 s), re-upload the image
 * after the release matrix has already settled on a verdict, and interleave two
 * sets of app-status lines in the log.  The matrix already uses today's pairing
 * (prgmBase 0x00080200 / execAddr 0x00083040), which is all it needs; the
 * boot-address question can be re-opened by setting this back to 1 and
 * B220_DSP_RELEASE_MATRIX back to 0.  The code, its API and its behaviour with
 * this at 1 are unchanged. */
#define B220_DSP_ADDR_MATRIX 0
/* Phase 3-13: the release variants the esp-adf reference names (see
 * zl38063/adf_ref/, and the full esp-adf copy under ref\zl38063\).

 * Hardware result that defines this phase (do not re-investigate): the release
 * matrix of Phase 3-12 showed that 0x12C alone, +0x000D, +0x0008 and
 * 0x000D+0x0008 ALL leave the sampled image RAM matching (mismatching_words=0)
 * while reg 0x0030 & 0x0F stays 0x00, i.e. the missing step is something else.
 * The in-tree reference for this same chip (LyraTD-MSC) is:

 *   zl38063_codec_init()            ref\zl38063\zl38063.c:70-76
 *     -> tw_upload_dsp_firmware(0)  ref\zl38063\example_apps\tw_spi_access.c:43
 *        - 1000 ms, VprocTwolfGetAppStatus(): if it is already non-zero the
 *          whole upload is skipped (tw_spi_access.c:49-57)
 *        - VprocTwolfHbiInit()                             (tw_spi_access.c:64)
 *        - VprocTwolfHbiBoot_alt() = the record upload      (tw_spi_access.c:80;
 *          inside it reg 0x12C <- execAddr and reg 0x32 <- 0x000D,
 *          vprocTwolf_access.c:533 and :588-596)
 *        - VprocTwolfFirmwareStart() = reg 0x32 <- 0x0008   (tw_spi_access.c:99)
 *        - VprocTwolfLoadConfig() = the config record       (tw_spi_access.c:111)
 *        - VprocTwolfReset(VPROC_RST_SOFTWARE)              (tw_spi_access.c:130)
 *          = ONE device write: reg 0x006 <- 0x0002 (buf = 0x02;
 *          VprocTwolfHbiWrite(0x006, 1, &buf), vprocTwolf_access.c:866-872),
 *          then Vproc_msDelay(200)                     (vprocTwolf_access.c:877)

 * So the new ingredient is that soft reset, and it comes LAST - after the GO and
 * after the config record.  Two further steps of the reference are also missing
 * from this port and are variant-tested as well:
 *   - reg 0x32 <- 0x000B (HOST_CMD_HOST_FLASH_INIT): the first command of
 *     VprocTwolfSaveImgToFlash(), which ADF calls BETWEEN the record upload and
 *     VprocTwolfFirmwareStart() (vprocTwolf_access.c:944-956, called at
 *     tw_spi_access.c:90).  Only the discovery command is emitted here - the rest
 *     of that function programs the device's own slave flash (0x0009 delete all
 *     applications, 0x0004 save image+CR) and would also stall ~10-20 s per
 *     command when no flash answers;
 *   - the soft reset sent a SECOND time: "if the ZL38040 is an I2S slave, if the
 *     I2S master is not stable at the time of this reset, then that reset will
 *     not take effect.  In that case the host has to simply resend the reset
 *     command once the I2S master is up and running and is at a stable state"
 *     (ref\zl38063\example_apps\tw_hal_verify.c:126-134).

 * Variants (zl38063_dsp.c, zl38063_dsp_release_matrix()), RUN in this order -
 * the reference-shaped ones first, because 1..4 are already known failures - and
 * the matrix stops at the first non-zero masked app status:
 *   5.  0x12C + 0x000D + config record + 0x0008              (config before GO)
 *   6.  as 5 + reg 0x0006 <- 0x0002                          (the ADF order)
 *   7.  0x12C + 0x000D + 0x0008 + reg 0x0006 <- 0x0002       (no config load)
 *   8.  as 6 but with the config record AFTER the GO         (order isolation)
 *   9.  as 6 with reg 0x0006 <- 0x0002 sent twice, 500 ms apart
 *  10.  as 6 with reg 0x000B (host flash init) before the config record
 *   1.  0x12C only | 2. +0x000D | 3. +0x0008 | 4. +0x000D+0x0008 (control)

 * The reg 0x0006 <- 0x0002 step is issued through the already-ported
 * VprocTwolfReset(VPROC_RST_SOFTWARE) (zl38063_vproc.c), not through a raw HBI
 * write: it writes 0x0006 <- 0x0002 exactly once, waits 200 ms, and reports a
 * failure when that write fails - the same code the production path would use.

 * Cost: one full reset + 3699-record upload per variant (~4-5 s each) plus ~1 s
 * settle and the 9-record read-back; stops at the first success.  B220_DSP_
 * ADDR_MATRIX stays 0 while this runs (see the Phase 3-12 block above).  With
 * B220_DSP_RELEASE_MATRIX at 0, none of this is compiled in. */
#define B220_DSP_RELEASE_MATRIX_V2 0
/* Phase 3-14: standalone audio LOOPBACK test (B220_AUDIO_LOOPBACK).

 * Goal: the audio path itself is the test, not reg 0x0030 ("app status"), which
 * this product's application may simply never update.

 * When 1, xiaomodou_b220.cc starts ONE diagnostic task right after the DSP
 * bring-up attempt (whatever that attempt's verdict is - the early returns in
 * InitializeAudioDsp() do not skip it).  The task:
 *   - enables the audio PA (I2C write to the AW9106B at 0x5B, reg 0x02 <-
 *     0x02; 0x00 would mute it) and logs the result;
 *   - reads from the I2S RX channel and writes the very same frames back to the
 *     I2S TX channel (a plain echo), on the SAME full-duplex handles the
 *     existing B220_I2sUpBeforeDspBoot() created for I2S0 at 48 kHz / 16-bit /
 *     stereo - it never creates a second channel, which on one port would fail;
 *   - logs one rate line every ~2 s with the frames in/out, the
 *     i2s_channel_read / i2s_channel_write error counts and the peak / mean
 *     absolute sample value of the most recent block, so "silence" and "signal"
 *     are told apart in the log before anyone listens to a speaker.

 * Off (0) compiles the whole task, its buffers and its constructor call out and
 * the normal path is byte-for-byte what it was.  That is the one line to flip
 * for normal operation.

 * NOTE: this build wants the SHORT boot, so B220_DSP_RELEASE_MATRIX is 0 and
 * B220_DSP_ADDR_MATRIX is 0 below: one GPIO19 reset + one host-boot upload and
 * then the normal release sequence (reg 0x12C <- execAddr, reg 0x32 <- 0x000D,
 * GO reg 0x32 <- 0x0008), with no matrix re-uploading the image afterwards.
 * (B220_DSP_RELEASE_MATRIX_V2 above is a separate, later switch and is left as
 * its own author set it.) */
/* Phase 4 (production): 0 - the diagnostic loopback task is OFF.  The audio
 * path is the real codec now (Zl38063AudioCodec in zl38063_audio_codec.cc,
 * returned by GetAudioCodec()); it owns the same I2S0 RX/TX DMA the echo task
 * used, so the two must not both run.  The one AW8737S PA enable that the
 * loopback used to issue is issued by the board constructor instead (see the
 * production block after InitializeAudioDsp()). */
#define B220_AUDIO_LOOPBACK 0
/* Phase 3-15: the DECISIVE combination - ONE clean path that releases the DSP the
 * way the reference does and then hands it straight to the audio loopback
 * (B220_DSP_ADF_RELEASE_TX).

 * What it selects (zl38063_dsp.c, zl38063_dsp_release_adf_tx()):
 *   I2S up (unchanged, B220_I2S_BEFORE_DSP 1)
 *     -> ONE GPIO19 reset + ONE complete host-boot upload (reg 0x14 <- 1, 300 ms,
 *        reg 0x34 == 0xD3D3, all 3699 records, reg 0x12C <- execAddr), with the
 *        upload path's OWN two release writes suppressed at their call sites, so
 *        the four writes below are the complete release;
 *     -> reg 0x32 <- 0x000D               (host load complete)
 *     -> the 260-register config record   (BEFORE the GO, as ADF does it)
 *     -> reg 0x32 <- 0x0008               (the GO)
 *     -> VprocTwolfReset(VPROC_RST_SOFTWARE) = reg 0x0006 <- 0x0002 (+200 ms)
 *     -> +500 ms, ONE READ-ONLY line with reg 0x0030 / 0x0032 / 0x0034 / 0x0006
 *     -> the audio loopback starts (B220_AudioLoopbackStart(), board file).

 * That is matrix variant 6 (B220_DSP_RELEASE_MATRIX), run once and followed by
 * the loopback instead of by the other nine variants.

 * Off (0) restores the previous boot: the switch is only read by
 * zl38063_dsp_release_adf_tx(), the board's ADF branch and the loopback's "which
 * release ran" line; with 0 they all compile away and the switches below decide
 * the boot exactly as before.

 * The other switches are set for this variant too, because the point of the
 * experiment is that NOTHING else touches the device:
 *   B220_ZL_DIAG              0  no hbi_diag / gpio_forensics / hbi_sweep /
 *                                status_dump
 *   B220_DSP_VERIFY_UPLOAD    0  no post-GO image read-back
 *   B220_DSP_PAGE255_CALIB    0  no page-255 calibration (it writes scratch RAM)
 *   B220_DSP_VERIFY_BEFORE_GO 0  no pre-GO image read-back
 *   B220_DSP_RELEASE_MATRIX   0  no ten-variant release matrix
 *   B220_DSP_RELEASE_MATRIX_V2 0 unused by the code, off together with it
 *   B220_DSP_ADDR_MATRIX      0  (both defines) no boot-address matrix
 *   B220_DSP_HANDS_OFF        0  the reset + upload path, not hands-off
 *   B220_AUDIO_LOOPBACK       1  the echo task, the PA and the rate line

 * Belt and braces: the ADF path ALSO gates the mailbox probe (which writes
 * reg 0x014 <- 1 and would force the DSP back into its boot ROM), the release
 * matrix, the boot-address matrix, the ZL_DIAG probe group and the
 * calibration+verify block on !B220_DSP_ADF_RELEASE_TX in xiaomodou_b220.cc, so
 * leaving any switch above at 1 still cannot put a diagnostic on this path. */
/* Phase 3-17: SELECTABLE DOWNLINK TEST (B220_AUDIO_TEST_MODE).

 * Why: the DSP is running (the ADF release order works), the uplink works (I2S RX
 * carries real microphone audio, peak 20000-32000) and the PA enable is accepted
 * (AW9106B 0x5B reg 0x02 <- 0x02), yet NOTHING is heard from the speaker even
 * though the same audio is written back to I2S TX.  The remaining unknowns are
 * (a) the ESP32 -> speaker leg itself - of the four I2S lines only DOUT (GPIO26)
 * was never continuity-tested - and (b) the DSP's internal routing.  One flash
 * has to tell those two apart, so the loopback's sample SOURCE becomes a switch:

 *   0  the current behaviour: plain echo, RX -> TX, byte for byte as today.
 *   1  a 1 kHz FULL-SCALE sine on TX only - RX is never read at all.  If this is
 *      audible the ESP32/I2S/PA/speaker leg is fine and the fault is on the DSP's
 *      routing or on its drive of the RX leg; if it is silent the downlink leg
 *      (DOUT GPIO26 -> DSP -> PA -> speaker) is the broken one.
 *   2  a frequency sweep on TX: 1 kHz -> 8 kHz, 1 s per step, full scale.  Tells
 *      a partly-working downlink (some band audible, some not) from a dead one.
 *   3  echo RX -> TX as in mode 0 PLUS a 0.5 s 1 kHz beep burst every 2 s, so the
 *      listener can distinguish "no sound at all" from "sound but wrong" (and the
 *      log carries both the RX and the TX peak of the same window).

 * The tone is generated with no allocation and no floating point in the loop: one
 * 48-sample full-scale sine cycle (48 kHz / 48 = 1 kHz) is precomputed into a
 * static table with 4 fractional bits, and each block renders 240 frames (5 ms)
 * out of it into a static int16 buffer, the same sample to L and R.  The block is
 * handed to i2s_channel_write() as one 960-byte DMA chunk while the DMA chain
 * holds 6 x 240 frames = 30 ms, so the writer stays ~25 ms ahead of the audio
 * clock; on top of that auto_clear_after_cb is already 1 (the DMA emits silence
 * rather than stale data if a block is ever late).  Amplitude ramps in over the
 * first ~20 ms of every burst, so the tone starts without a click.  Mode 1 and 2
 * are the only modes whose sample source does not touch RX, and their rate line
 * reports the TX peak/mean and the frame counts instead of the RX ones.

 * Off (0) is bit-for-bit today's echo; 1/2/3 change only the sample source, the
 * task, its buffers and the ADF release path are otherwise untouched.  Anything
 * outside 0..3 fails the build in xiaomodou_b220.cc.

 * The other switches are left exactly as the ADF-release experiment had them:
 * B220_ZL_DIAG 0, B220_DSP_VERIFY_UPLOAD 0, B220_DSP_PAGE255_CALIB 0,
 * B220_DSP_VERIFY_BEFORE_GO 0, B220_DSP_RELEASE_MATRIX 0, B220_DSP_ADDR_MATRIX 0,
 * B220_DSP_HANDS_OFF 0, B220_AUDIO_LOOPBACK 1, B220_DSP_ADF_RELEASE_TX 1. */
#define B220_AUDIO_TEST_MODE 0
#define B220_DSP_ADF_RELEASE_TX 1
/*
 * B220_DSP_DOWNLINK_TRIES - one-build downlink bring-up experiment.
 *
 * Symptom it exists for: the ZL38063 application is running and the UPLINK is
 * alive (I2S DIN / GPIO35 carries real microphone audio into the ESP32), but the
 * DOWNLINK is dead - a full-scale 1 kHz sine written to I2S TX (GPIO26) reaches
 * the DSP's I2S_SDIA continuously and yet neither the DSP DAC pins nor the
 * speaker terminals show any waveform.  So the DSP never converts the I2S input
 * into DAC output: the downlink path inside the DSP is not (fully) configured.
 *
 * The experiment (implemented in zl38063_dsp.c, driven from the audio loopback
 * task in xiaomodou_b220.cc) runs ONE candidate per ~2 s window while the 1 kHz
 * TX tone keeps playing, so the DAC output can be watched on a scope and the
 * speaker listened to at the same time.  Every device write is logged as
 *
 *     downlink try N: reg 0x.... <- 0x.... -> ok|FAILED
 *
 * followed by a read-only readback line and a per-try summary.
 *
 * The candidate order is set by what the 1042-entry record dig uncovered (see
 * zl38063/zl38063_config_record_full.h):
 *
 *   The stock image's configuration record is 1042 {reg,value} pairs, regs
 *   0x0200..0x0A22, and the stock host loader writes ALL of them (its loop is
 *   `addx4 end = data + 4*numElements` with numElements = the record's length
 *   field 0x0412 = 1042).  The esp-adf object zl38063_config.o agrees:
 *   configStreamLen = 1042 next to a 1042-element st_twConfig.  Our build until
 *   now wrote only the first 260 pairs (zl38063_config_record.h, COUNT 260), so
 *   registers 0x0408..0x0A22 - 782 entries, 157 of them non-zero - were NEVER
 *   written.  That is the prime suspect for a dead downlink: the uplink lives in
 *   the first 260 registers, the downlink/DAC/crosspoint configuration does not.
 *
 * The stock *playback* path makes the same point directly.  The stock's
 * "enable playback" routine (VA 0x40125358) ends with audio_pa_on(1) and then
 * ops[+44](50), where ops is the audio-HAL table at 0x3ffbf348 and ops[+44] is
 * 0x401256D8, the ZL38063 volume/route routine.  At volume 50 that routine writes
 *     0x238 <- 0xECEC, 0x23A <- 0xECEC, 0x24E <- 0xF3F3,
 *     0x494 <- 0x0666, 0x49A <- 0x0002, then reg 0x0006 <- 0x0002
 * Three more stock routines (0x400D6493, 0x4012953C, 0x40128D40) write a
 * handful of scenario-profile registers - including 0x04C0 and 0x0622, both in
 * the omitted tail - through a second helper 0x40125578(reg, value), and then
 * soft-reset.  Those are tries 7..9.
 * (the 0x494/0x49A pair is gated on VprocTwolfHbiRead(0x0014) == 0x200;
 * volume >= 80 selects 0x16C3 / 0x0001 instead).  Our 260-element record covers
 * 0x238/0x23A and 0x24E but NOT 0x494 (record index 330) or 0x49A (index 333) -
 * both in the omitted tail, and the values the stock writes for them are exactly
 * the values the stock record carries.  That is the mechanistic link between
 * "record truncated to 260" and "downlink dead".
 *
 * The tries, in order:
 *   1  read-only probe (app status, host flags, 0x0014, gains, 0x0494/0x049A/0x0A1C)
 *   2  the 782 omitted record entries (0x0408..0x0A22) + soft reset
 *   3  the WHOLE 1042-entry record + soft reset
 *   4  the STOCK PLAYBACK-START WRITE LIST (0x238/0x23A <- 0xECEC,
 *      0x24E <- 0xF3F3, 0x494 <- 0x0666, 0x49A <- 0x0002) + soft reset
 *   5  0x494 <- 0x0666, 0x49A <- 0x0002   (the two tail registers alone)
 *   6  0x494 <- 0x16C3, 0x49A <- 0x0001   (the volume >= 80 branch)
 *   7  STOCK SCENARIO PROFILE A (0x400D6493): 0x4C0<-0x0022, 0x202<-0x1C0F,
 *      0x22A<-0x0000 + soft reset
 *   8  STOCK SCENARIO PROFILE B (0x4012953C): 0x206<-0x5000, 0x202<-0x1C0C,
 *      0x2A0<-0x0005, 0x2A2<-0xC000, 0x300<-0x3838, 0x622<-0x0187 + soft reset
 *   9  0x04C0 <- 0x0033 then 0x0022   (the one profile register in the tail)
 *  10  0x0238/0x023A <- 0x0000   (0 dB, unity output gain)
 *  11  0x0238/0x023A <- 0x0606   (+6 dB, maximum)
 *  12  0x0238/0x023A <- 0x0202   (the esp-adf value)
 *  13  0x0200 <- 0x0000  (esp-adf value; stock has 0x0001 - the stock's set-mute
 *      routine drives exactly this register)
 *  14  0x0232 <- 0x0006          (esp-adf value; stock has 0x0000)
 *  15  0x0212 <- 0x0605          (esp-adf value; stock has 0x0000)
 *  16  0x0224/0x0228/0x022A <- 1/2/3   (the esp-adf enum triple)
 *  17  the remaining page-2 esp-adf deltas, 0x02B2..0x02E6
 *  18  control: the soft reset alone (reg 0x0006 <- 0x0002)
 *  19  reg 0x0032 <- 0x0008 (GO again, framed host command)
 *
 * 0 means the switch, the table, the record array and the call site are all
 * compiled out and the build is byte-for-byte the previous one.  1 (the default
 * here) costs 4168 bytes of .rodata for the full record plus the stepper.
 *
 * Needs B220_AUDIO_LOOPBACK 1 (it is the loopback task that paces the tries) and
 * is only meaningful on the B220_DSP_ADF_RELEASE_TX path, which is what performs
 * the release the tries build on.
 */
#define B220_DSP_DOWNLINK_TRIES 0

/* Phase 3-19: I2S TX SIGNAL-LEVEL VARIANT SWEEP (B220_I2S_TX_VARIANT_SWEEP).

 * Why: everything on the DSP side measures correct - the application runs and
 * reports ready (reg 0x0014 = 0x0200, the stock firmware's own route-gate
 * condition), the full 1042-entry config record plus the stock playback-start
 * values (0x494 <- 0x0666, 0x49A <- 0x0002, ...) all read back, and the uplink
 * carries real microphone audio on I2S DIN/GPIO35 - yet the downlink is dead: a
 * 1 kHz full-scale sine written to I2S TX/GPIO26 (tx_peak=32759, write_err=0)
 * shows a square wave on GPIO26, which is continuously connected to the DSP's
 * I2S_SDIA, while DAC1_P/DAC1_M stay flat (a stock unit playing audio shows a
 * 100 mV square wave there).  Rate and width already match the stock unit
 * (measured: WS 48 kHz, BCLK 1.54 MHz = 32 BCLK/frame = 16-bit slots, and
 * I2S_SDIA carries data during stock playback), so the remaining ESP32-side
 * variable is the SIGNAL LEVEL on that link: BCLK/WS polarity, slot width, slot
 * placement and the Philips 1-bit data delay.

 * What it does: with the switch at 1 the loopback task's sample source is pinned
 * to the fixed 1 kHz full-scale tone (B220_AUDIO_TEST_MODE is overridden to 1)
 * and, ~6 s after the task starts, it reconfigures ONE I2S TX parameter at a
 * time - each held ~3 s - through
 *   i2s_channel_disable() -> i2s_channel_reconfig_std_slot() /
 *   i2s_channel_reconfig_std_gpio() -> i2s_channel_enable(),
 * logging "i2s-tx variant N/9: <description> -> ok|FAILED (err)" and, ~2 s into
 * the hold, "i2s-tx variant N/9: tx_peak=.. tx_frames=.. write_err=..".  The tone
 * runs throughout (a few ms of glitch per step, expected), so DAC1_P/DAC1_M can
 * be watched on a scope while the steps go by:

 *   1  baseline: 16-bit slot, STEREO, slot_mask=BOTH, bit_shift=1, no invert
 *      (the current configuration - nothing is reconfigured)
 *   2  invert_flags.bclk_inv = true           (BCLK phase flipped, 1.536 MHz)
 *   3  invert_flags.ws_inv   = true           (WS phase flipped, 48 kHz)
 *   4  both inverted
 *   5  slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT with 16-bit data still
 *      MSB-aligned (BCLK 3.072 MHz, WS stays 48 kHz, 64 BCLK/frame)
 *   6  slot_mode = I2S_SLOT_MODE_MONO, data in the LEFT slot
 *   7  slot_mode = I2S_SLOT_MODE_MONO, data in the RIGHT slot
 *   8  bit_shift = false (MSB format, no Philips 1-bit delay)
 *   9  back to baseline

 * Every one of those is runtime-changeable in IDF 6.1 through the two reconfigure
 * APIs above (i2s_std.h:352 i2s_channel_reconfig_std_slot() for
 * slot_bit_width/slot_mode/slot_mask/bit_shift, i2s_std.h:367
 * i2s_channel_reconfig_std_gpio() for invert_flags.bclk_inv/ws_inv; both require
 * the channel to be disabled first, which is why every step brackets them with
 * disable/enable).  data_bit_width is deliberately NOT swept: it is the DMA word
 * size that i2s_get_buf_size() keys the DMA chain off, and the wire parameter the
 * DSP sees is slot_bit_width.

 * MONO note for steps 6/7: in the TX direction mono means the buffer holds ONE
 * slot of data, and the driver really does halve the DMA frame (active_slot = 1,
 * i2s_get_buf_size() bytes_per_frame = bytes_per_sample * active_slot), so the
 * tone is written as 240 x 16-bit there and 240 x 2 x 16-bit in stereo.  IDF's
 * own test suite skips the 16-bit TX-mono case on this chip ("#if
 * !CONFIG_IDF_TARGET_ESP32 // the 16 bit channel sequence on ESP32 is incorrect",
 * test_apps/i2s/main/test_i2s.c:799), so steps 6/7 may show a driver quirk rather
 * than a DSP verdict.

 * 0 leaves the build exactly as it was (the tone still plays, the loopback still
 * logs its rate line); anything else fails the build in xiaomodou_b220.cc when
 * B220_AUDIO_LOOPBACK or B220_I2S_BEFORE_DSP is 0, because the sweep reconfigures
 * the I2S0 TX channel only those create.

 * B220_DSP_DOWNLINK_TRIES is set to 0 in the same pass: its 19-step register
 * matrix is exhausted on hardware (state above), and it is paced from the very
 * same loopback task, so leaving it on would mix DSP register writes into the I2S
 * reconfiguration windows this run is measuring.  The code, the table and its
 * call site are untouched and come straight back with the switch at 1.

 * Everything else is left exactly as the ADF-release build had it:
 * B220_ZL_DIAG 0, B220_DSP_VERIFY_UPLOAD 0, B220_DSP_PAGE255_CALIB 0,
 * B220_DSP_VERIFY_BEFORE_GO 0, B220_DSP_RELEASE_MATRIX 0, B220_DSP_ADDR_MATRIX 0,
 * B220_DSP_HANDS_OFF 0, B220_AUDIO_LOOPBACK 1, B220_I2S_BEFORE_DSP 1,
 * B220_DSP_ADF_RELEASE_TX 1, B220_AUDIO_TEST_MODE 1. */
/* Phase 3-20: AW8737S SPEAKER-AMPLIFIER SHDN PULSE COUNT (the reason this build
 * has B220_I2S_TX_VARIANT_SWEEP at 0).

 * The speaker amplifier is an AW8737S.  Its SHDN pin (A4) is active-low and is
 * driven by the AW9106B group-A output OUT_A1 = register 0x02 bit 1.  It is not a
 * plain enable: after SHDN has been held low for longer than TOFF (the datasheet
 * recommends 1 ms) the chip selects its OPERATING MODE from the NUMBER OF RISING
 * EDGES that follow:
 *     1 rising edge  -> mode 1, 1.2 W   <- what this board wants
 *     2 rising edges -> mode 2, 1.0 W
 *     3 rising edges -> mode 3, 0.8 W
 *     4 rising edges -> mode 4, NCN function OFF = NO OUTPUT AT ALL
 *     more than four is not allowed.
 * The "DAC1_P/DAC1_M carry the 1 kHz tone, the speaker shows nothing" symptom is
 * mode 4: our boot used to write register 0x02 in three places (the stock
 * backlight init sequence, the ZL38063 group-A diagnostic and the loopback's
 * PA-on), and every 0 -> 1 transition on bit 1 counted as another edge.
 *
 * The board file now owns reg 0x02 alone: one audited writer, one enable sequence
 * per boot (0x02 <- 0x00, hold 2 ms, 0x02 <- 0x02 = exactly one rising edge), a
 * monotonic write counter, a rising-edge counter, and a loud warning if a second
 * edge would ever be produced.  See the "AW8737S SHDN pulse" block in
 * xiaomodou_b220.cc, and zl38063_dsp.c: the group-A diagnostic now writes 0x3D
 * (bit 1 cleared) so it cannot add an edge either.

 * This build: B220_DSP_ADF_RELEASE_TX 1, B220_AUDIO_LOOPBACK 1,
 * B220_AUDIO_TEST_MODE 1 (the fixed 1 kHz full-scale sine on TX only),
 * B220_I2S_TX_VARIANT_SWEEP 0 - the sweep is OFF on purpose, because it would
 * reconfigure the TX channel every ~3 s (BCLK/WS polarity, slot width, mono) and
 * this run needs one STABLE 1 kHz sine so that "still no sound" can only mean the
 * amplifier, not a shifting I2S format.  Everything else is unchanged:
 * B220_ZL_DIAG 0, B220_DSP_VERIFY_UPLOAD 0, B220_DSP_PAGE255_CALIB 0,
 * B220_DSP_VERIFY_BEFORE_GO 0, B220_DSP_RELEASE_MATRIX 0, B220_DSP_ADDR_MATRIX 0,
 * B220_DSP_HANDS_OFF 0, B220_DSP_DOWNLINK_TRIES 0, B220_I2S_BEFORE_DSP 1. */
#define B220_I2S_TX_VARIANT_SWEEP 0
/* Phase 3-21: AW9106B / AW8737S PA ENABLE - THE FIX (B220_PA_BIT_FINDER back to 0,
 * because the Phase 3-20 walk below answered its question).

 * The walk proved the root cause on hardware: with GPMD_A (AW9106B reg 0x12) at
 * 0x08 the group-A outputs drive NOTHING - steps 1..9, including 0x02 <- 0xFF,
 * were silent - and the 1 kHz tone became audible EXACTLY at the steps that wrote
 * 0x12 <- 0x02 together with 0x04 (GPIO_CFG_A) <- 0x00, and stayed audible until
 * power-off.  Our InitializeBacklight() replayed the stock tail 0x11(CTL)<-0x1B +
 * 0x12(GPMD_A)<-0x08 AFTER the PA enable, so it switched the amplifier straight
 * back off: that overwrite is the difference between no sound and sound here.

 * The board file now does the whole thing itself, in this order:
 *   1. 0x12 (GPMD_A)     <- 0x02 (group A in the mode in which it drives SHDN)
 *   2. 0x04 (GPIO_CFG_A) <- 0x00 (all group-A pins plain outputs)
 *   3. 0x02 <- 0x00, hold >= 2 ms (the AW8737S shutdown/TOFF period)
 *   4. 0x02 <- 0x02 (exactly ONE rising edge -> mode 1, 1.2 W)
 *   5. latch 0x12/0x04/0x02: any later CHANGE is refused and logged
 * 0x12/0x04 go through the same audited, logging writers as 0x02, the
 * {0x12, 0x08} stock-tail entry is deleted, and the boot ends with
 *   pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02 |
 *   0x02 writes=N, SHDN rising edges=1 (AW8737S mode 1)

 * BACKLIGHT: the DIM registers (0x20..0x25) that light the panel are untouched and
 * only 0x02/0x12/0x04 are frozen.  If the panel ever needs the group-A DIM/PWM
 * engine that 0x08 selects, the only way to have both is a GPMD_A value that keeps
 * bit 1 in the output mode AND the DIM pins in theirs (0x0A is the candidate) -
 * that combination has never been tested on this hardware and is NOT used here:
 * sound first.

 * This build (the clean audio test): B220_AUDIO_LOOPBACK 1, B220_AUDIO_TEST_MODE 1
 * (stable 1 kHz full-scale sine on TX only), B220_DSP_ADF_RELEASE_TX 1,
 * B220_I2S_BEFORE_DSP 1, B220_AUDIO_BRINGUP 1, and 0 for everything else:
 * B220_PA_BIT_FINDER 0, B220_I2S_TX_VARIANT_SWEEP 0, B220_DSP_DOWNLINK_TRIES 0,
 * B220_DSP_ADDR_MATRIX 0, B220_DSP_RELEASE_MATRIX 0, B220_DSP_RELEASE_MATRIX_V2 0,
 * B220_DSP_VERIFY_UPLOAD 0, B220_DSP_VERIFY_BEFORE_GO 0, B220_DSP_PAGE255_CALIB 0,
 * B220_DSP_HANDS_OFF 0, B220_ZL_DIAG 0, B220_DIAGNOSTIC 0,
 * B220_BACKLIGHT_SWEEP 0, B220_ORIENT_CARD 0. */
#define B220_PA_BIT_FINDER 0

/* =========================================================================== *
 * Phase 5 : AUTO LISTEN / CONTINUOUS CONVERSATION (B220_AUTO_LISTEN) - the one
 * line to build with.
 *
 * Why this board used to need it: the B220 has no readable button - the power
 * key is a baseboard soft-latch the ESP32 cannot read, GPIO0 is the boot strap /
 * ZL38063 HBI chip-select, and BOOT_BUTTON_GPIO was deliberately removed from
 * this board's config.h - so nothing ever put the device into
 * kDeviceStateListening: it booted, activated, reached idle and stayed there.
 *
 * Why it is 0 now (2026-10-09): voice wake-up does work on this chip.  esp-sr
 * 2.4.7 allows WakeNet9 on a classic ESP32 with PSRAM (main/Kconfig.projbuild,
 * USE_ESP_WAKE_WORD: IDF_TARGET_ESP32 && SPIRAM), sdkconfig.defaults.esp32
 * already picks CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS, and config.json now adds
 * CONFIG_USE_ESP_WAKE_WORD=y.  The model rides inside the existing assets
 * partition (no partition-table change, no new offset), so the phrase
 * "ni hao xiao zhi" is what starts a conversation now.  Auto-listen would be a
 * second, competing way to open the microphone, hence this is 0.
 *
 * What 1 does: a small driver in xiaomodou_b220.cc (B220_AutoListenStart())
 * watches the application state and, after EVERY transition into
 * kDeviceStateIdle, re-arms listening 800 ms later, so the user can simply keep
 * talking.  The trigger is Application::StartListening() - the same event-bit
 * API a button or a wake word uses (application.h:100-103) - and the state is
 * read with Application::GetDeviceState() (application.h:71).  Nothing in the
 * core (application.cc, device_state_machine.*) is touched: this is a board-level
 * observer.
 *
 * What 0 does: the driver, its task and its logs are compiled out completely (no
 * runtime cost, no "-Wunused" warnings); B220_AutoListenStart() collapses to a
 * stub that only logs that auto-listen is off.
 *
 * Way out: B220_AUTO_LISTEN 0 is the build-time stop, and it compiles the whole
 * watcher (task, state and logs) out.  At runtime this board has nothing to
 * press, so the loop's only stop is power: the guards keep it from becoming a
 * busy loop (an arm only on a transition into idle, an 800 ms delay, and a stop
 * after B220_AUTO_LISTEN_MAX_MISSES consecutive arms that do not move the device
 * out of idle - e.g. no protocol/audio channel yet).  While the device sits in
 * idle the audio service powers the codec output down by itself after
 * AUDIO_POWER_TIMEOUT_MS; the watcher re-enables it once per idle episode before
 * arming, because on this board that is the difference between a heard reply and
 * a silent one (the codec's SHDN enable is latched, so this adds no PA edge).
 *
 * The rest of the build is untouched by this switch: B220_DSP_ADF_RELEASE_TX 1,
 * B220_I2S_BEFORE_DSP 1, B220_AUDIO_BRINGUP 1, AUDIO_INPUT/OUTPUT_SAMPLE_RATE
 * 48000, B220_AUDIO_LOOPBACK 0, B220_AUDIO_TEST_MODE 0 and every diagnostic 0.
 * ========================================================================== */
#define B220_AUTO_LISTEN 0

/* Delay between "the device reached idle" and the re-arm, in milliseconds.  It
 * is long enough for the server's goodbye / the last playback tail and short
 * enough that a user starting to talk right after a reply is still captured. */
#define B220_AUTO_LISTEN_DELAY_MS 800

/* State polling period of the supervisor task (ms).  It is a sleep, not a spin:
 * one Application::GetDeviceState() read per period. */
#define B220_AUTO_LISTEN_POLL_MS 100

/* How many consecutive arms may fail to move the device out of idle before the
 * driver stops for good (e.g. no audio channel and therefore no protocol). */
#define B220_AUTO_LISTEN_MAX_MISSES 5
#endif // _BOARD_CONFIG_H_
