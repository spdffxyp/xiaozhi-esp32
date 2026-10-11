/****************************************************************************
* zl38063_dsp.c - board-facing bring-up API for the ZL38063 AcuEdge DSP
*
* Derived from esp-adf release/v2.x
* components/audio_hal/driver/zl38063/ (zl38063.c: reset GPIO handling and
* volume plumbing; api_lib/: status and config-record handling).  The parts
* that are a straight port of Microsemi code keep their MIT headers, see
* ZL38063-LICENSE.txt next to this file.
***************************************************************************/

#include "zl38063_dsp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

/*
 * config.h must come BEFORE the B220_ZL_DIAG block below: it is what defines
 * B220_ZL_DIAG, and a preprocessor conditional placed above it would silently
 * evaluate to 0 and drop the whole diagnostic (including the <stdio.h> the
 * sweep needs for snprintf()).
 */
#include "config.h"                 /* ZL38063_RESET_GPIO */
#include "zl38063_hbi.h"
#include "zl38063_vproc.h"
#include "zl38063_config_record.h"

#if B220_ZL_DIAG || B220_DSP_VERIFY_UPLOAD || B220_DSP_HANDS_OFF
#include <stdio.h>                  /* snprintf() in the HBI sweep, in the
                                     * upload read-back verify's hex dump and in
                                     * the hands-off one-line status poll */
#endif
#if B220_DSP_HANDS_OFF
#include <stdarg.h>                 /* va_list for the status-line appendf */
#endif

#if B220_ZL_DIAG
/* zl38063_dsp_status_dump() is DEFINED further down inside the B220_ZL_DIAG
 * block of this file and CALLED from zl38063_dsp_boot_addr_matrix() at the
 * bottom.  The copy of this declaration in zl38063_dsp.h is not visible here:
 * config.h (which defines B220_ZL_DIAG) is included after that header on
 * purpose, so the header's "#if B220_ZL_DIAG" evaluated to 0 for this
 * translation unit.  Hence this local declaration. */
void zl38063_dsp_status_dump(void);
#endif

#if B220_DSP_HANDS_OFF
/* Same reason as above: the header's "#if B220_DSP_HANDS_OFF" block was skipped
 * for this translation unit (config.h is included after the header), so the two
 * hands-off entry points are declared locally. */
esp_err_t zl38063_dsp_hands_off_attach(void);
int zl38063_dsp_status_line(unsigned elapsed_s);
#endif

/* NB: the firmware image header (zl38063/zl38063_firmware_image.h) is
 * deliberately NOT included here.  It is included exactly once, by
 * zl38063_fwimg_data.c, which is also where the static asserts that tie the
 * image to the scalars live; this file reads the resulting symbols through the
 * extern declarations in zl38063_vproc.h.  Including it here as well would
 * compile a second (unreferenced, and warned-about) static copy of the
 * 3699-record array into this object. */

#define TAG "ZL38063"

/* Reset/low/high pulse followed by the stock loader's 500 ms pause before the
 * first HBI access. */
#define ZL38063_RESET_LOW_MS   10
#define ZL38063_BOOT_WAIT_MS   500

/* Upstream tw_upload_dsp_firmware() sleeps 1000 ms before it reads the
 * application status, so VprocTwolfFirmwareStart() is allowed this long to get
 * the image running before anyone judges its status register. */
#define ZL38063_FW_START_WAIT_MS 1000

/* Extra settle pause the boot-address matrix leaves between a boot attempt and
 * the register dump, so the DSP has definitely started executing (or definitely
 * not) before reg 0x030 is judged. */
#define ZL38063_MATRIX_SETTLE_MS 1000

/* Raw gain register value for volume 0, i.e. -90 dB (0xA6 as int8_t gain).
 * Volume 100 would be +10 dB, but the DSP saturates at +6 dB, so the usable
 * range is 0..96 and anything above 96 is clamped by the register itself. */
#define ZL38063_VOLUME_ZERO_DB (-90)

static bool s_hbi_ready = false;

/*----------------------------------------------------------------------------
 * prgmBase / execAddr selection
 *
 * Without an override the two values come from the linked image itself
 * (zl38063_fwimg_data.c: programBaseAddress = 0x00080200, executionAddress =
 * 0x00083040), i.e. exactly what this port has always written.  One setter
 * (zl38063_dsp_set_boot_addresses(), public API below) flips to a caller-chosen
 * pair, so the "is execAddr really 0x00083040?" experiment needs no rebuild.
 *--------------------------------------------------------------------------*/
static bool     s_boot_addr_override = false;
static uint32_t s_boot_prgm_base = 0;
static uint32_t s_boot_exec_addr = 0;

static uint32_t dsp_prgm_base(void)
{
    return s_boot_addr_override ? s_boot_prgm_base : (uint32_t)programBaseAddress;
}

static uint32_t dsp_exec_addr(void)
{
    return s_boot_addr_override ? s_boot_exec_addr : (uint32_t)executionAddress;
}

/*----------------------------------------------------------------------------
 * Helpers
 *--------------------------------------------------------------------------*/

/* Read the application status register.  Returns -1 if the read failed, else
 * the masked status (0 = not running, 1 = running, 2 = running + mute). */
static int read_app_status(uint16 *out_status)
{
    uint16 status = 0;
    VprocStatusType st = VprocTwolfGetAppStatus(&status);

    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "reading the application status failed (%d)", (int)st);
        return -1;
    }
    if (out_status != NULL) {
        *out_status = status;
    }
    return (int)status;
}

/*----------------------------------------------------------------------------
 * HBI bring-up diagnostic (B220_ZL_DIAG)
 *
 * Read-only: this probe never writes a device register, so it can run even
 * after the host boot has failed.  See zl38063_dsp.h for how to read its
 * output and for the mapping onto hypotheses H1/H2/H3.
 *--------------------------------------------------------------------------*/
#if B220_ZL_DIAG

/* Where the diag gets the shared-SPI2 pin numbers from.  config.h defines them
 * for the display (which owns the bus); these defaults are the same pins and
 * only exist so the probe still compiles if the names ever move. */
#ifndef DISPLAY_SPI_SCK_PIN
#define DISPLAY_SPI_SCK_PIN  GPIO_NUM_32
#endif
#ifndef DISPLAY_SPI_MOSI_PIN
#define DISPLAY_SPI_MOSI_PIN GPIO_NUM_33
#endif
#ifndef DISPLAY_SPI_MISO_PIN
#define DISPLAY_SPI_MISO_PIN GPIO_NUM_34
#endif

#define ZL38063_DIAG_SCLK_GPIO ((int)DISPLAY_SPI_SCK_PIN)
#define ZL38063_DIAG_MOSI_GPIO ((int)DISPLAY_SPI_MOSI_PIN)
#define ZL38063_DIAG_MISO_GPIO ((int)DISPLAY_SPI_MISO_PIN)

/* The registers worth looking at once the link is supposed to be up:
 *   0x0006 host software flags / mailbox-adjacent, 0x0020/0x0022/0x0024 are
 *   part of the boot ROM's identification block, 0x0030 app status,
 *   0x0032 host command, 0x0034 command param/result (0xD3D3 = boot ROM ready),
 *   0x012C the image execution address.  A device that drives MISO must not
 *   return 0x0000 for all eight in both byte orders. */
static const uint16_t kDiagRegs[] = {
    0x0006, 0x0020, 0x0022, 0x0024, 0x0030, 0x0032, 0x0034, 0x012C,
};

static void diag_read_regs(const char *label)
{
    ESP_LOGI(TAG, "--- HBI dump %s ---", label);

    for (size_t i = 0; i < sizeof(kDiagRegs) / sizeof(kDiagRegs[0]); i++) {
        uint16_t reg = kDiagRegs[i];
        uint16_t value = 0;
        VprocStatusType st = VprocTwolfHbiRead((unsigned short)reg, 1,
                                               (unsigned short *)&value);
        if (st != VPROC_STATUS_SUCCESS) {
            /* Never abort the sweep on one bad register. */
            ESP_LOGI(TAG, "%s reg 0x%04X = <vproc error %d>", label,
                     (unsigned)reg, (int)st);
        } else {
            ESP_LOGI(TAG, "%s reg 0x%04X = 0x%04X", label, (unsigned)reg,
                     (unsigned)value);
        }
    }
}

void zl38063_dsp_hbi_diag(void)
{
    static const struct {
        const char *name;
        int gpio;
    } pins[] = {
        {"SCLK", ZL38063_DIAG_SCLK_GPIO},
        {"MOSI", ZL38063_DIAG_MOSI_GPIO},
        {"MISO", ZL38063_DIAG_MISO_GPIO},
        {"CS  ", ZL38063_HBI_CS_GPIO},
        {"RST ", ZL38063_RESET_GPIO},
    };

    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "hbi_diag called before zl38063_dsp_begin() - there is no "
                      "HBI device on SPI2 to read from");
        return;     /* not a read failure: nothing was ever attached */
    }

    ESP_LOGI(TAG, "===== ZL38063 HBI diagnostic (B220_ZL_DIAG) =====");
    ESP_LOGI(TAG, "byte-order swap is %s (default = ENABLED)",
             zl38063_hbi_get_swap() ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "register list (word offset = addr/2, page = addr>>8) is "
                  "read once per byte order; values are raw 16-bit words");

    /*
     * H1 test: the same eight registers in both byte orders.  A real device
     * answers with plausible words in exactly one block; a byte-order bug makes
     * the two blocks byte-mirrors of each other; no MISO at all yields 0x0000
     * (or 0xFFFF) in both.
     */
    diag_read_regs("swapped");
    zl38063_hbi_set_swap(false);
    diag_read_regs("raw");
    zl38063_hbi_set_swap(true);         /* restore the default behaviour */

    /*
     * Pin evidence for H2/H3.  gpio_get_level() only READS the pad level; it
     * does not call gpio_config()/gpio_set_direction(), so the pins the SPI
     * driver owns (SCLK/MOSI/MISO and the CS it drives for this device) keep
     * their SPI routing and the LCD's bus is left untouched.
     *
     * Caveat: while the SPI driver owns SCLK/MOSI/MISO/CS these are peripheral
     * pins and the input path is not guaranteed to be enabled, so a 0 here does
     * NOT prove the line is low.  The reset GPIO is a plain GPIO output, and at
     * the end of zl38063_dsp_begin() it should read 1 - a 0 there is a strong
     * hint that the DSP is held in reset.
     */
    ESP_LOGI(TAG, "ESP32 input levels (read only, no pin is reconfigured):");
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        ESP_LOGI(TAG, "%s GPIO%-2d = %d%s", pins[i].name, pins[i].gpio,
                 (int)gpio_get_level((gpio_num_t)pins[i].gpio),
                 (pins[i].gpio == (int)ZL38063_RESET_GPIO) ? "   (expect 1)" : "");
    }
    ESP_LOGI(TAG, "CS GPIO%d idles HIGH (deasserted) between transactions; a "
                  "low reading means the device is stuck selected or the pin is "
                  "shorted", (int)ZL38063_HBI_CS_GPIO);
    ESP_LOGI(TAG, "===== end ZL38063 HBI diagnostic =====");
}

/*----------------------------------------------------------------------------
 * GPIO forensics (B220_ZL_DIAG)
 *
 * The open question is whether the ESP32 actually OWNS the DSP reset net.  The
 * HBI diag reads GPIO19 back as 0 after zl38063_dsp_begin() drove it high.  A
 * plain read of an output pad is level-only, so it cannot tell "the pad really
 * is held low by something external" from "the read path is odd".  This probe
 * supplies the missing half: it drives the pin both ways and reads it back in
 * *every* electrical configuration, so a value that never follows the driver is
 * proof of an external low (another driver, a short, or a missing pull-up on a
 * pin that is not actually connected to this net).
 *
 * Reversible by construction: the function drives GPIO19 low/high and switches
 * it between output/input-pullup/input-pulldown, and its LAST action is to put
 * GPIO19 back the way our driver left it - output, driven HIGH (DSP out of
 * reset).  The only non-GPIO action is the AW9106 group-A write, which is
 * restored to 0x00 (the value InitializeBacklight() itself programmed).
 *--------------------------------------------------------------------------*/

/* AW9106 group A (reg 0x02 GPIO_OUTPUT_A), provided by the board file
 * xiaomodou_b220.cc, which owns the I2C bus the AW9106B sits on.  The HBI I2C
 * handle is not reachable from here, so the board exposes this one-line
 * accessor.  It is declared weak so a board build without that helper reports
 * "not linked in" instead of failing to link.  Returns ESP_OK, or
 * ESP_ERR_INVALID_STATE when the board has no AW9106 device registered. */
extern esp_err_t __attribute__((weak)) b220_aw9106_set_gpio_output_a(uint8_t value);

/* Busy-wait us.  The board uses esp_rom_delay_us() for this (zl38063_vproc.c),
 * and it is the portable spelling of the ROM ets_delay_us(). */
#define ZL38063_DIAG_DELAY_US(us) esp_rom_delay_us(us)

/* Offsets differ per mode only in these three fields, so build each step from
 * one template - that keeps the matrix obviously correct. */
static void diag_gpio19_config(gpio_mode_t mode, gpio_pullup_t pull_up,
                               gpio_pulldown_t pull_down)
{
    gpio_config_t cfg = {0};
    cfg.pin_bit_mask = (1ULL << ZL38063_RESET_GPIO);
    cfg.mode = mode;
    cfg.pull_up_en = pull_up;
    cfg.pull_down_en = pull_down;
    cfg.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "forensics: gpio_config(GPIO%d) failed: %s",
                 (int)ZL38063_RESET_GPIO, esp_err_to_name(err));
    }
}

/* One matrix row: apply the configuration, settle, read the pad back.  The
 * level is logged in the same line as the configuration it was read under, so a
 * capture can be read without cross-referencing. */
static int diag_gpio19_step(const char *what, gpio_mode_t mode,
                            gpio_pullup_t pull_up, gpio_pulldown_t pull_down,
                            int drive_level)
{
    diag_gpio19_config(mode, pull_up, pull_down);

    /* Drive AFTER the config, so the level is not lost across the pad mux. */
    if (mode == GPIO_MODE_OUTPUT) {
        gpio_set_level((gpio_num_t)ZL38063_RESET_GPIO, drive_level);
    }

    /* Short settle: 2 ms is orders of magnitude more than the RC of this net. */
    ZL38063_DIAG_DELAY_US(2000);

    int level = (int)gpio_get_level((gpio_num_t)ZL38063_RESET_GPIO);
    ESP_LOGI(TAG, "GPIO%d %-34s -> read %d", (int)ZL38063_RESET_GPIO, what,
             level);
    return level;
}

/* App status probe.  Reads register 0x030 only (VprocTwolfGetAppStatus() is a
 * pure read: VprocHALInit() has nothing left to do in this port), so it is safe
 * to call as often as we like and it writes no device register. */
static void diag_app_status(const char *label)
{
    uint16 status = 0;
    VprocStatusType st = VprocTwolfGetAppStatus(&status);
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "app status %-28s = <read error %d>", label, (int)st);
    } else {
        ESP_LOGI(TAG, "app status %-28s = 0x%02X (masked, reg 0x030 & 0x0F)",
                 label, (unsigned)status);
    }
}

void zl38063_dsp_gpio_forensics(void)
{
    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "gpio_forensics called before zl38063_dsp_begin() - the "
                      "HBI device is not attached to SPI2");
        return;
    }

    ESP_LOGI(TAG, "===== ZL38063 GPIO forensics (B220_ZL_DIAG) =====");
    ESP_LOGI(TAG, "deliberately re-configuring GPIO%d, which our own driver "
                  "owns; the last step restores it to output HIGH",
             (int)ZL38063_RESET_GPIO);

    /*
     * 1. Ownership matrix.  Read the pad back in every configuration:
     *      out LOW  -> reads 0 (expected), out HIGH -> must read 1 if the ESP32
     *                  owns the net;
     *      in PULLUP   -> reads 1 on a floating net, 0 means something is
     *                  pulling harder than the ~45k internal pull-up;
     *      in PULLDOWN -> reads 0 (expected) but confirms the pin is read as an
     *                  input at all, which rules out a stuck/odd read path.
     *    "out HIGH -> 0" together with "in PULLUP -> 0" is the external-hold
     *    signature: the net is driven low by something that is not the ESP32.
     */
    ESP_LOGI(TAG, "--- GPIO%d ownership matrix ---", (int)ZL38063_RESET_GPIO);
    diag_gpio19_step("output, no pulls, driven LOW", GPIO_MODE_OUTPUT,
                     GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, 0);
    diag_gpio19_step("output, no pulls, driven HIGH", GPIO_MODE_OUTPUT,
                     GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE, 1);
    diag_gpio19_step("input, internal pull-up", GPIO_MODE_INPUT,
                     GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, 0);
    diag_gpio19_step("input, internal pull-down", GPIO_MODE_INPUT,
                     GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_ENABLE, 0);

    /* Restore: output, driven HIGH = DSP out of reset.  Do this BEFORE the DSP
     * probes below, so they run with the reset line in its normal state. */
    diag_gpio19_config(GPIO_MODE_OUTPUT, GPIO_PULLUP_DISABLE,
                       GPIO_PULLDOWN_DISABLE);
    gpio_set_level((gpio_num_t)ZL38063_RESET_GPIO, 1);
    ZL38063_DIAG_DELAY_US(2000);
    ESP_LOGI(TAG, "GPIO%d restored: output, driven HIGH (read back %d)",
             (int)ZL38063_RESET_GPIO,
             (int)gpio_get_level((gpio_num_t)ZL38063_RESET_GPIO));

    /*
     * 2. Does the DSP answer now?  Same question zl38063_dsp_begin() already
     *    asked, but asked again on a freshly driven reset line.  No HBI writes,
     *    no reset pulse: this only re-reads register 0x030.
     */
    diag_app_status("after the matrix");

    /*
     * 3. AW9106 group-A outputs.  GPIO19 - the DSP reset - has been suspected of
     *    being shared with, or gated by, the LED driver's group A.  Drive five of
     *    the six group-A outputs high and re-read the DSP status, then put the
     *    register back to 0x00 - the exact value InitializeBacklight() writes for
     *    it - so the board is left as we found it.
     *
     *    BIT 1 IS DELIBERATELY EXCLUDED (0x3D, not 0x3F).  Group-A output OUT_A1 =
     *    reg 0x02 bit 1 is the AW8737S SHDN line, and that amplifier selects its
     *    operating mode from the NUMBER OF RISING EDGES on SHDN (1 edge = mode 1
     *    1.2 W, 4 edges = mode 4 NCN OFF = no output at all), so a 0 -> 1 on that
     *    one bit is a MODE CHANGE and not just a level.  With 0x3D the probe still
     *    exercises the other five outputs of the group and can still show whether
     *    group A gates the DSP, while bit 1 stays 0 the whole time: this
     *    diagnostic can no longer add a rising edge and cannot move the amplifier
     *    out of the mode the enable sequence selected.
     *
     *    This is the ONLY device write in this diagnostic, it touches the
     *    AW9106B (not the DSP), and it is undone in the same function.
     */
    if (b220_aw9106_set_gpio_output_a == NULL) {
        ESP_LOGW(TAG, "AW9106 group-A test SKIPPED: "
                      "b220_aw9106_set_gpio_output_a() is not linked in");
    } else {
        ESP_LOGI(TAG, "--- AW9106 group A (reg 0x02 GPIO_OUTPUT_A) ---");
        esp_err_t err = b220_aw9106_set_gpio_output_a(0x3D);
        ESP_LOGI(TAG, "AW9106 reg 0x02 <- 0x3D (bit 1 = SHDN left LOW on purpose) -> %s",
                 esp_err_to_name(err));
        if (err == ESP_OK) {
            /* 10 ms: the DSP status register is on the SPI side, the AW9106B on
             * I2C, so this is just a settle pause. */
            ZL38063_DIAG_DELAY_US(10000);
            diag_app_status("AW9106 group A = 0x3D");

            err = b220_aw9106_set_gpio_output_a(0x00);
            ESP_LOGI(TAG, "AW9106 reg 0x02 <- 0x00 (restore: all of group A low, which "
                          "is also the AW8737S SHDN low state) -> %s",
                     esp_err_to_name(err));
            ZL38063_DIAG_DELAY_US(10000);
            diag_app_status("AW9106 group A restored");
        } else {
            ESP_LOGW(TAG, "AW9106 group-A test aborted (write failed); the "
                          "register is untouched on a failed write");
        }
    }

    ESP_LOGI(TAG, "===== end ZL38063 GPIO forensics =====");
}

/*----------------------------------------------------------------------------
 * HBI transport sweep (B220_ZL_DIAG)
 *
 * The framing, the byte order, the reset line, the image and the config record
 * are all settled; what is still open is whether the DSP is being addressed at
 * all - i.e. which GPIO is really its HBI chip-select, and whether it wants SPI
 * mode 0 or mode 3 at 10 MHz or 1 MHz.  This probe brute-forces exactly that
 * space by re-binding the HBI device (zl38063_hbi_rebind(), which never frees
 * the LCD's SPI2 bus) and reading register 0x0034 - the boot ROM's 0xD3D3 magic
 * - once per combination.
 *
 * READ-ONLY on the device side: no HBI write happens here, so the DSP cannot be
 * left in a strange state.  The byte-order swap is deliberately NOT swept (it is
 * a proven-equal dimension) and stays ENABLED for every read.
 *--------------------------------------------------------------------------*/

/* Pins that are known to belong to another peripheral on this board and must
 * therefore never be taken over as an HBI chip-select: the LCD + its DC/RST and
 * the shared SPI2 bus itself, the AW9106B I2C bus, the I2S link to the DSP, the
 * UART console and the flash/PSRAM pads.
 *   32 33 34 27 21 22  LCD / HBI bus           18 23        AW9106B I2C
 *   5 25 26 35           I2S                   1 3          console UART
 *   6 7 8 9 10 11 16 17  flash / PSRAM
 * GPIO27 is the LCD's CS and would be a false hit, but it is kept as a control:
 * it proves the sweep can find a chip that really is selected.
 */
static const int kSweepCsUnused[] = {
    32, 33, 34, 27, 21, 22, 18, 23, 5, 25, 26, 35, 1, 3, 6, 7, 8, 9, 10, 11,
    16, 17,
};

/* CS candidates, in the order the sweep tries them.  GPIO27 (the LCD CS) is in
 * here on purpose, as the control case above. */
static const int kSweepCs[] = {
    0, 2, 4, 12, 13, 14, 15, 19, 20, 24, 27, 28, 29, 30, 31,
};

/* The cheap pass: 2 modes x 2 clocks x 15 pins = 60 reads.  Modes 1/2 are only
 * added when that pass finds nothing at all (see the second loop below), which
 * keeps the common case fast and the exhaustive case still under ~400 reads. */
static const int kSweepModes[] = {0, 3};
static const int kSweepModesExtra[] = {1, 2};
static const int kSweepClocks[] = {10000000, 1000000};

/* Any read that actually transferred returns a value < 0x10000; SWEEP_NO_VALUE
 * marks "the HBI transfer itself failed", which is NOT the same thing as "the
 * device answered 0x0000" - keeping the two apart is the whole point of the
 * sweep. */
#define SWEEP_NO_VALUE  0x10000u

static uint16_t sweep_read_reg(uint16_t reg, int *err_out)
{
    unsigned short value = 0;
    VprocStatusType st = VprocTwolfHbiRead((unsigned short)reg, 1, &value);
    if (err_out != NULL) {
        *err_out = (int)st;
    }
    if (st != VPROC_STATUS_SUCCESS) {
        return (uint16_t)SWEEP_NO_VALUE;
    }
    return (uint16_t)value;
}

/* Format one register result as "=0x1234" / "=(vproc error 7)" / "=(skipped)". */
static void sweep_value_text(char *buf, size_t len, uint16_t value, int err)
{
    if (value == (uint16_t)SWEEP_NO_VALUE) {
        snprintf(buf, len, "=(vproc error %d)", err);
    } else {
        snprintf(buf, len, "=0x%04X", (unsigned)value);
    }
}

/* One combination: re-bind the HBI device, read @value_reg, and if that returned
 * a value that is neither 0x0000 nor 0xFFFF also read @aux_reg.  Returns true
 * when the combination produced such a value (i.e. the DSP answered). */
static bool sweep_probe(int cs, int mode, int clock, uint16_t value_reg,
                        uint16_t aux_reg)
{
    char vtext[24];
    char atext[24];
    int err = 0;
    int aux_err = 0;
    uint16_t value;
    uint16_t aux = (uint16_t)SWEEP_NO_VALUE;
    bool answered;

    if (zl38063_hbi_rebind(cs, mode, clock) != ESP_OK) {
        ESP_LOGI(TAG, "sweep cs=%-2d mode%d %2dMHz -> ATTACH FAILED", cs, mode,
                 clock / 1000000);
        return false;
    }

    value = sweep_read_reg(value_reg, &err);
    answered = (value != (uint16_t)SWEEP_NO_VALUE) && (value != 0x0000u) &&
               (value != 0xFFFFu);
    if (answered) {
        /* Something answered: ask for the application status as well. */
        aux = sweep_read_reg(aux_reg, &aux_err);
    }

    sweep_value_text(vtext, sizeof(vtext), value, err);
    if (!answered) {
        snprintf(atext, sizeof(atext), "=(skipped)");
    } else {
        sweep_value_text(atext, sizeof(atext), aux, aux_err);
    }

    if (answered) {
        ESP_LOGI(TAG, "sweep HIT: cs=%d mode%d %2dMHz reg0x%02X%s reg0x%02X%s",
                 cs, mode, clock / 1000000, (unsigned)value_reg, vtext,
                 (unsigned)aux_reg, atext);
        return true;
    }

    ESP_LOGI(TAG, "sweep cs=%-2d mode%d %2dMHz -> reg0x%02X%s reg0x%02X%s", cs,
             mode, clock / 1000000, (unsigned)value_reg, vtext,
             (unsigned)aux_reg, atext);
    return false;
}

void zl38063_dsp_hbi_sweep(void)
{
    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "hbi_sweep called before zl38063_dsp_begin() - the HBI "
                      "device is not attached to SPI2");
        return;
    }

    ESP_LOGI(TAG, "===== ZL38063 HBI sweep (cs x mode x clock) =====");
    ESP_LOGI(TAG, "framing and 16-bit byte-order swap stay ENABLED and are NOT "
                  "swept (both verified); reads only, no device register is "
                  "written");
    ESP_LOGI(TAG, "reg0x34 = boot-ROM magic (expect 0xD3D3); reg0x30 is read "
                  "only when reg0x34 is neither 0x0000 nor 0xFFFF");
    ESP_LOGI(TAG, "skipping pins already owned by LCD/HBI, I2C, I2S, console or "
                  "flash/PSRAM:");
    {
        /* One line so the excluded set is visible in a capture, not just in the
         * source.  Built incrementally to avoid a long printf argument list. */
        char line[192] = {0};
        size_t used = 0;
        for (size_t i = 0;
             (i < sizeof(kSweepCsUnused) / sizeof(kSweepCsUnused[0])) &&
             (used < sizeof(line) - 8);
             i++) {
            int n = snprintf(line + used, sizeof(line) - used, "%s%d",
                             (used == 0) ? "" : " ", kSweepCsUnused[i]);
            if ((n <= 0) || ((size_t)n >= (sizeof(line) - used))) {
                break;      /* truncated: stop instead of running off the end */
            }
            used += (size_t)n;
        }
        ESP_LOGI(TAG, "  %s", line);
    }

    /* One bit per CS candidate: set when that CS produced any value that is
     * neither 0x0000 nor 0xFFFF. */
    uint32_t hit_mask = 0;
    int combos = 0;
    int hits = 0;

    /*
     * cs is the OUTER loop on purpose, so a capture that is cut short still
     * contains every mode/clock for the pins tried so far.
     */
    for (int pass = 0; pass < 2; pass++) {
        if ((pass == 1) && (hits > 0)) {
            break;      /* the cheap pass found something: modes 1/2 not needed */
        }
        if (pass == 1) {
            ESP_LOGI(TAG, "no hit with modes 0/3 - adding the exhaustive pass "
                          "with SPI modes 1 and 2");
        }

        for (size_t c = 0; c < sizeof(kSweepCs) / sizeof(kSweepCs[0]); c++) {
            int cs = kSweepCs[c];
            const int *modes = (pass == 0) ? kSweepModes : kSweepModesExtra;
            size_t nmodes = (pass == 0)
                            ? (sizeof(kSweepModes) / sizeof(kSweepModes[0]))
                            : (sizeof(kSweepModesExtra) /
                               sizeof(kSweepModesExtra[0]));

            for (size_t m = 0; m < nmodes; m++) {
                for (size_t k = 0;
                     k < sizeof(kSweepClocks) / sizeof(kSweepClocks[0]); k++) {
                    int mode = modes[m];
                    int clock = kSweepClocks[k];
                    combos++;
                    if (sweep_probe(cs, mode, clock, 0x0034, 0x0030)) {
                        hits++;
                        hit_mask |= (1u << (unsigned)c);
                    }
                }
            }
        }
    }

    ESP_LOGI(TAG, "===== sweep done: %d combos, %d hits =====", combos, hits);

    /* Summary: every CS that ever produced a non-0x0000/0xFFFF value. */
    if (hit_mask == 0) {
        ESP_LOGI(TAG, "sweep summary: no CS produced a value other than "
                      "0x0000/0xFFFF; the DSP is not being addressed, or it is "
                      "not driving MISO (power/reset/wiring)");
    } else {
        char line[160] = {0};
        size_t used = 0;
        for (size_t c = 0; c < sizeof(kSweepCs) / sizeof(kSweepCs[0]); c++) {
            if ((hit_mask & (1u << (unsigned)c)) == 0) {
                continue;
            }
            int n = snprintf(line + used, sizeof(line) - used, "%s%d",
                             (used == 0) ? "" : ",", kSweepCs[c]);
            if ((n <= 0) || ((size_t)n >= (sizeof(line) - used))) {
                break;      /* truncated: stop instead of running off the end */
            }
            used += (size_t)n;
        }
        ESP_LOGI(TAG, "sweep summary: CS with a non-0x0000/0xFFFF value: %s",
                 line);
    }

    /*
     * Restore the production device (CS 0, mode 0, 10 MHz, swap enabled) so the
     * rest of the boot behaves exactly as it did before the sweep.  The swap
     * was never changed; set it again anyway to be explicit about the known-good
     * state.
     */
    zl38063_hbi_set_swap(true);
    esp_err_t err = zl38063_hbi_rebind((int)ZL38063_HBI_CS_GPIO,
                                       ZL38063_HBI_MODE,
                                       (int)ZL38063_HBI_CLOCK_HZ);
    ESP_LOGI(TAG, "sweep: normal HBI device restored (cs=%d, mode %d, %d MHz) -> %s",
             (int)ZL38063_HBI_CS_GPIO, ZL38063_HBI_MODE,
             (int)(ZL38063_HBI_CLOCK_HZ / 1000000), esp_err_to_name(err));
    ESP_LOGI(TAG, "===== end ZL38063 HBI sweep =====");
}

/*----------------------------------------------------------------------------
 * Post-boot register dump (B220_ZL_DIAG)
 *
 * The narrow question after a successful-looking upload is "what does the DSP
 * say about itself NOW?".  This is the read-only answer: the app status, the
 * host command/result pair, the mailbox flags, the register 0x12C entry point
 * and the boot ROM's three identification words, raw, one line each.  No HBI
 * write happens anywhere in here, so it is callable at any time after
 * zl38063_dsp_begin() - including straight after a failed or half-finished
 * boot, which is exactly when it is wanted.
 *--------------------------------------------------------------------------*/

/* Register list and the label each value is logged under.  Order is the order
 * the lines appear in the log. */
static const struct {
    uint16_t reg;
    const char *label;
} kStatusDumpRegs[] = {
    {0x0030, "app status"},
    {0x0032, "host command / status"},
    {0x0034, "cmd result (0xD3D3 = boot ROM)"},
    {0x0006, "host flags / mailbox"},
    {0x012C, "entry point, hi word"},
    {0x0020, "boot ROM id word 0"},
    {0x0022, "boot ROM id word 1"},
    {0x0024, "boot ROM id word 2"},
};

void zl38063_dsp_status_dump(void)
{
    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "status_dump called before zl38063_dsp_begin() - there is "
                      "no HBI device on SPI2 to read from");
        return;     /* not a read failure: nothing was ever attached */
    }

    ESP_LOGI(TAG, "===== ZL38063 status dump (read-only, no HBI writes) =====");

    bool app_status_valid = false;
    uint16_t app_status = 0;

    for (size_t i = 0; i < sizeof(kStatusDumpRegs) / sizeof(kStatusDumpRegs[0]);
         i++) {
        uint16_t reg = kStatusDumpRegs[i].reg;
        unsigned short value = 0;
        VprocStatusType st = VprocTwolfHbiRead((unsigned short)reg, 1, &value);

        if (st != VPROC_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "status dump reg 0x%04X (%-28s) = <vproc error %d>",
                     (unsigned)reg, kStatusDumpRegs[i].label, (int)st);
        } else {
            ESP_LOGI(TAG, "status dump reg 0x%04X (%-28s) = 0x%04X",
                     (unsigned)reg, kStatusDumpRegs[i].label, (unsigned)value);
            if (reg == 0x0030) {
                app_status = (uint16_t)value;
                app_status_valid = true;
            }
        }
    }

    /*
     * Register 0x12C is a 32-bit entry point written as two words, high first
     * (HbiSrecBoot_alt(): HBI write 0x12C, 2 words).  The single word above is
     * therefore not enough to tell the two candidates apart - 0x00083040 and
     * 0x00080200 share the high word 0x0008 and differ only in the low one - so
     * read the pair as well.  Still read-only.
     */
    unsigned short entry[2] = {0, 0};
    VprocStatusType st = VprocTwolfHbiRead(0x012C, 2, entry);
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "status dump entry point (0x012C/0x012D, 2 words) = "
                      "<vproc error %d>", (int)st);
    } else {
        ESP_LOGI(TAG, "status dump entry point (0x012C/0x012D, 2 words) = "
                      "0x%04X%04X",
                 (unsigned)entry[0], (unsigned)entry[1]);
    }

    if (app_status_valid) {
        ESP_LOGI(TAG, "status dump app status (reg 0x0030 & 0x0F) = 0x%02X (%s)",
                 (unsigned)(app_status & 0x0F),
                 ((app_status & 0x0F) != ZL38063_APP_STATUS_NOT_RUNNING)
                 ? "running" : "not running");
    }

    ESP_LOGI(TAG, "===== end ZL38063 status dump =====");
}

#endif /* B220_ZL_DIAG */

/*----------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

/* Runtime selection of the prgmBase / execAddr pair (see zl38063_dsp.h).  Only
 * host-side state is touched here; nothing is written to the device. */
void zl38063_dsp_set_boot_addresses(uint32_t prgm_base, uint32_t exec_addr)
{
    ESP_LOGI(TAG, "boot addresses set: prgmBase 0x%08lX execAddr 0x%08lX "
                  "(image declares prgmBase 0x%08lX execAddr 0x%08lX)",
             (unsigned long)prgm_base, (unsigned long)exec_addr,
             (unsigned long)programBaseAddress,
             (unsigned long)executionAddress);
    s_boot_prgm_base = prgm_base;
    s_boot_exec_addr = exec_addr;
    s_boot_addr_override = true;
}

/* Host (HBI) boot of the recovered stock image.
 *
 * The ZL38063 on this board does NOT self-boot: nothing in the stock firmware
 * ever relies on the device's own slave flash, and the factory power-up path
 * (stock 0x40124454 -> 0x40126240 -> VprocTwolfHbiBoot 0x40125c64) always
 * uploads the image.  This function is that path, expressed with the ported
 * Microsemi API.
 *
 * Register order, stock vs. this implementation (see
 * zl38063/zl38063_firmware_image.md sections 1 and 4):
 *
 *   stock step                                  ported call
 *   ------------------------------------------------------------------------
 *   reg 0x14 <- 1 (force boot ROM)              VprocTwolfHbiBootPrepare(),
 *   300 ms wait                                 called from inside
 *   reg 0x34 must read 0xD3D3                   VprocTwolfHbiBoot_alt()
 *   per record: reg 0x0C <- 2 words (target)    HbiSrecBoot_alt(), called from
 *               reg 0xFF <- numWords (data)     inside VprocTwolfHbiBoot_alt()
 *   reg 0x12C <- execAddr (2 words, hi/lo)
 *   reg 0x32 <- 0x000D (host load complete)     VprocTwolfcmdRegWr(0x000D)
 *   poll reg 0x06 until it releases             VprocTwolfMailboxAcquire()
 *   poll reg 0x06/0x32, verify result           VprocTwolfCheckCmdResult()
 *   --------------------------------------------------------------- then
 *   reg 0x32 <- 0x0008 ("GO")                   VprocTwolfFirmwareStart()
 *
 * Note there is no separate prepare step at this level: the ported
 * VprocTwolfHbiBoot_alt() already performs VprocTwolfHbiBootPrepare()
 * internally (it is a straight port of Microsemi's single-shot loader, which
 * does the same).  Calling VprocTwolfHbiBootPrepare() here as well would only
 * repeat reg 0x14 <- 1 and the 300 ms wait.
 *
 * On success the firmware is in DSP RAM and has been told to start (reg 0x32
 * <- 0x0008), so the device begins executing it; the caller still has to push
 * the configuration record (zl38063_dsp_load_config()) for the audio path to be
 * configured.
 */
esp_err_t zl38063_dsp_upload_firmware(void)
{
    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "upload_firmware called before zl38063_dsp_begin()");
        return ESP_ERR_INVALID_STATE;
    }

    if (firmwareStreamLen == 0) {
        ESP_LOGE(TAG, "no ZL38063 firmware image linked in (firmware image "
                      "symbols are still the weak empty ones)");
        return ESP_ERR_NOT_FOUND;
    }

    /* Fill in the structure VprocTwolfHbiBoot_alt() expects.  The cast drops
     * the const on the generated array: the loader only reads it (upstream
     * ADF does the same cast).
     *
     * prgmBase / execAddr come from dsp_prgm_base() / dsp_exec_addr(): the
     * linked image's own values unless zl38063_dsp_set_boot_addresses() has
     * selected another pair.  execAddr is what HbiSrecBoot_alt() writes to
     * register 0x12C (the RAM entry point the boot ROM jumps to); prgmBase only
     * reaches the loader's debug log, because the page-255 base register is
     * re-written from every record's own targetAddr. */
    twFirmware fw = {0};
    fw.st_Fwr = (twFwr *)st_twFirmware;
    fw.twFirmwareStreamLen = (uint16)firmwareStreamLen;
    fw.execAddr = dsp_exec_addr();
    fw.havePrgmBase = (uint8)haveProgramBaseAddress;
    fw.prgmBase = dsp_prgm_base();

    ESP_LOGI(TAG, "host boot addresses: prgmBase 0x%08lX execAddr 0x%08lX (%s)",
             (unsigned long)fw.prgmBase, (unsigned long)fw.execAddr,
             s_boot_addr_override ? "set by zl38063_dsp_set_boot_addresses()"
                                  : "from the linked image");
    ESP_LOGI(TAG, "host boot: %u records, prgmBase 0x%08lX, execAddr 0x%08lX",
             (unsigned)fw.twFirmwareStreamLen,
             (unsigned long)fw.prgmBase, (unsigned long)fw.execAddr);

    /* reg 0x14 <- 1, 300 ms, reg 0x34 == 0xD3D3, transfer the records
     * (reg 0x0C target + reg 0xFF data), reg 0x12C <- execAddr,
     * reg 0x32 <- 0x000D, poll reg 0x06, verify. */
    VprocStatusType st = VprocTwolfHbiBoot_alt(&fw);
    if (st != VPROC_STATUS_SUCCESS) {
        if (zl38063_vproc_release_cmd_host_load_complete_is_suppressed()) {
            /* B220_DSP_RELEASE_MATRIX / B220_DSP_ADF_RELEASE_TX path with the
             * host-load-complete command suppressed: VprocTwolfHbiBoot_alt() then
             * returns VPROC_STATUS_FW_LOAD_FAILED on purpose (it cannot verify a
             * command it was told not to send), but the transfer itself succeeded -
             * the records and reg 0x12C are in place and the caller's release
             * sequence follows.  Nothing is reported as an error here. */
            ESP_LOGI(TAG, "firmware image transferred (%d records); the upload path's "
                          "reg 0x32 <- 0x000D was suppressed for the release path "
                          "(B220_DSP_RELEASE_MATRIX / B220_DSP_ADF_RELEASE_TX), so "
                          "the transfer is reported as '%d' by design - the records "
                          "and reg 0x12C are in place, which is the state the release "
                          "sequence starts from",
                     (int)fw.twFirmwareStreamLen, (int)st);
            /* The suppression is the only reason this is not "success": the boot
             * ROM was never told the host load is complete, so the loader's
             * result check cannot pass.  The image WAS transferred, so report it
             * the same way the successful path below does. */
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "firmware transfer failed (%d): the DSP did not enter "
                          "boot ROM (reg 0x34 != 0xD3D3) or an HBI access failed",
                     (int)st);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGI(TAG, "firmware image transferred (%d records)",
                 (int)fw.twFirmwareStreamLen);
    }

    /* reg 0x32 <- 0x0008: start executing the image we just loaded - unless the
     * release path (B220_DSP_RELEASE_MATRIX / B220_DSP_ADF_RELEASE_TX) has asked
     * for this one write to be suppressed at its call site, because that sequence
     * emits it itself (or deliberately does not emit it) after its own 0x000D /
     * config record.
     * VprocTwolfFirmwareStart() itself is untouched, so every other caller still
     * gets the write; the flag is 0 in every build that does not run a release
     * path, i.e. the default path is byte-for-byte unchanged.  Upstream's 1 s wait
     * and the status read below are skipped with the GO: with no GO there is
     * nothing to settle and no application that could be running yet. */
    if (zl38063_vproc_firmware_start_is_suppressed()) {
        ESP_LOGI(TAG, "host boot: reg 0x32 <- 0x0008 (GO) suppressed at this call "
                      "site for the release path (B220_DSP_RELEASE_MATRIX / "
                      "B220_DSP_ADF_RELEASE_TX); that sequence issues it or "
                      "deliberately does not");
        return ESP_OK;
    }

    st = VprocTwolfFirmwareStart();
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "VprocTwolfFirmwareStart failed (%d)", (int)st);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "execution started at 0x%08lX", (unsigned long)fw.execAddr);

    /* Upstream waits 1 s here before it looks at the status register. */
    vTaskDelay(pdMS_TO_TICKS(ZL38063_FW_START_WAIT_MS));

    /* Report what the device says now (reg 0x030).  No status check here on
     * purpose: the loader has already verified reg 0x34 via
     * VprocTwolfCheckCmdResult(). */
    uint16 status = 0;
    if (read_app_status(&status) < 0) {
        ESP_LOGW(TAG, "could not read the application status after the upload");
    } else {
        ESP_LOGI(TAG, "app status after upload = 0x%04X", (unsigned)status);
    }
    return ESP_OK;
}

/*----------------------------------------------------------------------------
 * Page-255 window addressing shared by the calibration and the upload verify
 * (B220_DSP_PAGE255_CALIB / B220_DSP_VERIFY_UPLOAD)
 *
 * Register 0x0C = PAGE_255_BASE_HI_REG, the page-255 window base: the full 32-bit
 * DSP target address, written as 2 words (hi16 then lo16).  The same address is a
 * private define in zl38063_vproc.c; it is repeated here so the board layer does
 * not have to depend on a vproc-internal macro.
 *
 * The target-address-to-window-offset arithmetic is NOT here: it lives in
 * zl38063_hbi_page255_window_offset() (zl38063_hbi.c/.h) and both diagnostics
 * below go through it.
 *
 * s_page255_calib_encoding is the encoding the verify reads the window with: the
 * calibration's verdict once it has run, and ZL38063_PAGE255_OFF_UPLOAD (what the
 * upload's own 0xFF00|lowbyte write emits on the wire) until then.
 *--------------------------------------------------------------------------*/
#if B220_DSP_PAGE255_CALIB || B220_DSP_VERIFY_UPLOAD
#define B220_DSP_PAGE255_BASE_REG 0x000C
static bool         s_page255_calibrated = false;
static unsigned int s_page255_calib_encoding = ZL38063_PAGE255_OFF_UPLOAD;

/* The window read framing the verify below uses - and therefore the one the
 * pre-GO observer in the board file (B220_DSP_VERIFY_BEFORE_GO) has to use too,
 * because it reads the same window at a different moment in the upload.  Before
 * the calibration has run this is ZL38063_PAGE255_OFF_UPLOAD, i.e. what the
 * upload's own 0xFF00|lowbyte write puts on the wire. */
unsigned int zl38063_dsp_page255_calib_encoding(void)
{
    return s_page255_calib_encoding;
}

bool zl38063_dsp_page255_calibrated(void)
{
    return s_page255_calibrated;
}
#endif

/*----------------------------------------------------------------------------
 * Page-255 window calibration (B220_DSP_PAGE255_CALIB)
 *
 * Why: the upload reports success yet reg 0x030 never reports a running
 * application, so the first read-back verify tried to compare the image in DSP
 * RAM with the image we sent.  Its read side was an INFERENCE - Microsemi's
 * driver only ever writes the page-255 window (TwolfHbiPage255Write(), no read
 * anywhere in adf_ref/) - and its own log gave the inference away: it printed
 * "window base low byte 0x200" for target 0x00080200, i.e. it printed the low 16
 * bits where a low byte belongs, and it fed the raw low byte into the command
 * word where zl38063_hbi_write()/hbi_send_cmd() actually puts low byte / 2.  Its
 * 13780/14856 mismatching words are therefore not evidence of anything yet.
 *
 * What this does instead: write a known 16-word pattern into a scratch DSP RAM
 * address USING EXACTLY THE UPLOAD'S WRITE PATH (reg 0x0C <- {hi16,lo16}, then
 * the window write 0xFF00|lowbyte through VprocTwolfHbiWrite()), then read it
 * back under every plausible read encoding and log which one reproduces it:
 *
 *   enc=a      offset field = targetAddr & 0xFF          (raw low byte)
 *   enc=b      offset field = (targetAddr & 0xFF) / 2    (word offset - what the
 *              upload's write path itself puts in bits 14..8)
 *   preselect  1 = send SELECT_PAGE(0xFF) = 0xFEFF first, 0 = do not
 *   base       hi_lo = reg 0x0C <- {hi16, lo16} (what the loader does) or lo_hi
 *
 * Cost: 8 combinations x (base write + command + 16 word reads) plus a few probe
 * reads, i.e. a few milliseconds - nothing next to the 3699-record upload.
 *--------------------------------------------------------------------------*/
#if B220_DSP_PAGE255_CALIB

/* Scratch DSP RAM target: the first word of the one documented hole in the range
 * our image covers.  zl38063/zl38063_firmware_image.md §2 validates the 3699
 * records as covering 0x00080128..0x0009f4e6 with exactly one hole,
 * 0x00080A24..0x00082FFF (9694 bytes), and records exist on BOTH sides of that
 * hole - so the DSP demonstrably has RAM there, while none of our records writes
 * it.  0x000A0000 (the other suggestion) lies 0xB1A bytes past the last word the
 * image addresses, which only proves "outside our image", not "backed by RAM".
 * The low byte 0x24 is even, so it is a legal word boundary, and the two
 * candidate encodings differ for it (0x24 byte vs 0x12 word), which is precisely
 * the ambiguity being tested. */
#define B220_DSP_PAGE255_CALIB_TARGET   0x00080A24u
#define B220_DSP_PAGE255_CALIB_WORDS    16u
#define B220_DSP_PAGE255_WINDOW_WORDS   128u  /* 7-bit field = 256-byte window */
#define B220_DSP_PAGE255_CALIB_PRE_MS   0u    /* no settle delay: pure HBI reads */

/* 16 distinct, non-trivial words: 0xC0DE ^ (0x1111 * i).  Neither 0x0000 nor
 * 0xFFFF can occur, so a dead MISO line or an unbacked window cannot match it. */
static unsigned short calib_pattern_word(unsigned int i)
{
    return (unsigned short)(0xC0DEu ^ (unsigned short)(0x1111u * (unsigned short)i));
}

/* Write the window base register the way HbiSrecBoot_alt() does (2 words, hi16
 * then lo16), or with the halves swapped when @lo_hi is set. */
static VprocStatusType calib_write_window_base(uint32_t target, unsigned int lo_hi)
{
    unsigned short base[2];

    base[0] = (unsigned short)(lo_hi ? (target & 0xFFFFu) : ((target >> 16) & 0xFFFFu));
    base[1] = (unsigned short)(lo_hi ? ((target >> 16) & 0xFFFFu) : (target & 0xFFFFu));
    return VprocTwolfHbiWrite(B220_DSP_PAGE255_BASE_REG, 2, base);
}

/* One window read under one framing, built from the low-level primitives so the
 * SELECT_PAGE(0xFF) step can be included or left out:
 *   [0xFEFF,] PAGED_READ(offset field, n-1), then n x 16-bit word reads - the
 *   same sequence zl38063_hbi_read() emits, minus the register-address decode.
 * The offset field comes from zl38063_hbi_page255_window_offset(), never from
 * local arithmetic. */
static int calib_window_read(uint32_t target,
                             unsigned int encoding,
                             unsigned int preselect,
                             unsigned int numWords,
                             unsigned short *out)
{
    const uint16_t off = (uint16_t)zl38063_hbi_page255_window_offset(target, encoding);

    if (preselect != 0u) {
        if (zl38063_hbi_wr16_cmd(ZL38063_HBI_SELECT_PAGE(0xFF)) != 0) {
            return -1;
        }
    }
    if (zl38063_hbi_wr16_cmd(ZL38063_HBI_PAGED_READ(off, (uint8_t)(numWords - 1u))) != 0) {
        return -1;
    }
    for (unsigned int i = 0; i < numWords; i++) {
        if (zl38063_hbi_rd16(&out[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static unsigned int calib_count_matches(const unsigned short *back,
                                        const unsigned short *expect,
                                        unsigned int numWords)
{
    unsigned int m = 0;

    for (unsigned int i = 0; i < numWords; i++) {
        if (back[i] == expect[i]) {
            m++;
        }
    }
    return m;
}

esp_err_t zl38063_dsp_page255_calibrate(void)
{
    const uint32_t target = B220_DSP_PAGE255_CALIB_TARGET;
    const unsigned int n = B220_DSP_PAGE255_CALIB_WORDS;
    const unsigned int low_byte =
        (unsigned int)zl38063_hbi_page255_window_offset(target, ZL38063_PAGE255_OFF_BYTE);
    const unsigned int word_off =
        (unsigned int)zl38063_hbi_page255_window_offset(target, ZL38063_PAGE255_OFF_WORD);
    unsigned short pattern[B220_DSP_PAGE255_CALIB_WORDS];
    unsigned short back[B220_DSP_PAGE255_CALIB_WORDS];
    unsigned short base_rb[2];
    bool found = false;
    unsigned int best_enc = ZL38063_PAGE255_OFF_UPLOAD;
    unsigned int best_pre = 1u;
    unsigned int best_base = 0u;
    unsigned int hit_field = 0xFFFFFFFFu;   /* set when the fallback scan finds it */
    /* Preference order for "several encodings match": the write path's own
     * encoding first, then the upload's own framing (preselect, base order). */
    static const unsigned int enc_order[2] = { ZL38063_PAGE255_OFF_WORD,
                                               ZL38063_PAGE255_OFF_BYTE };
    static const unsigned int pre_order[2] = { 1u, 0u };
    static const unsigned int base_order[2] = { 0u, 1u };   /* 0 = {hi,lo} */

    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "page255 calibrate called before zl38063_dsp_begin() - "
                      "there is no HBI device on SPI2 to talk to");
        return ESP_ERR_INVALID_STATE;
    }
    /* Same backstop as the verify: a live application owns its RAM, so writing a
     * scratch pattern into it would corrupt the very thing under test. */
    if (zl38063_dsp_is_running()) {
        ESP_LOGI(TAG, "page255 calibrate: skipped, an application is already "
                      "running (its RAM is not scratch space)");
        return ESP_OK;
    }

    for (unsigned int i = 0; i < n; i++) {
        pattern[i] = calib_pattern_word(i);
    }
    /* For this target the two encodings differ, so the arithmetic is
     * hand-checkable: low byte 0x24 -> enc=a field 0x24 (read command
     * PAGED_READ(0x24,15) = 0x240F), enc=b field 0x12 (0x120F).  The write the
     * upload path performs is HBI address 0xFF00|0x24 = 0xFF24, which
     * zl38063_hbi_write() turns into SELECT_PAGE(0xFF) = 0xFEFF + PAGED_WRITE of
     * field 0x12 = 0x128F + 16 data words.  All of it is recomputed from the
     * macros below; only these log lines print it. */
    ESP_LOGI(TAG, "----- ZL38063 page-255 window calibration (B220_DSP_PAGE255_CALIB) -----");
    ESP_LOGI(TAG, "page255 calib: scratch target 0x%08lX (first word of the documented image hole "
                  "0x00080A24..0x00082FFF, low byte 0x%02X, image occupies 0x00080128..0x0009f4e6)",
             (unsigned long)target, low_byte);
    ESP_LOGI(TAG, "page255 calib: pattern 16 words = 0xC0DE ^ (0x1111 * i): "
                  "%04X %04X %04X %04X %04X %04X %04X %04X "
                  "%04X %04X %04X %04X %04X %04X %04X %04X",
             (unsigned)pattern[0], (unsigned)pattern[1], (unsigned)pattern[2],
             (unsigned)pattern[3], (unsigned)pattern[4], (unsigned)pattern[5],
             (unsigned)pattern[6], (unsigned)pattern[7], (unsigned)pattern[8],
             (unsigned)pattern[9], (unsigned)pattern[10], (unsigned)pattern[11],
             (unsigned)pattern[12], (unsigned)pattern[13], (unsigned)pattern[14],
             (unsigned)pattern[15]);

    /* ---- the write: byte for byte what the upload does ------------------- */
    VprocStatusType st = calib_write_window_base(target, 0u);
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "page255 calib: writing the window base 0x%08lX to reg 0x0C failed (%d)",
                 (unsigned long)target, (int)st);
        return ESP_FAIL;
    }
    const uint16_t wr_cmd = (uint16_t)(0xFF00u | (uint16_t)low_byte);
    st = VprocTwolfHbiWrite(wr_cmd, (unsigned char)n, pattern);
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "page255 calib: window write of the pattern (HBI 0x%04X) failed (%d)",
                 (unsigned)wr_cmd, (int)st);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "page255 calib: write = upload path: reg 0x0C <- {0x%04X,0x%04X} (2 words) then "
                  "VprocTwolfHbiWrite(0x%04X, 16) -> SELECT_PAGE(0xFF)=0xFEFF then "
                  "PAGED_WRITE(offset field 0x%02X, 15)=0x%04X then 16 data words",
             (unsigned)((target >> 16) & 0xFFFFu), (unsigned)(target & 0xFFFFu),
             (unsigned)wr_cmd, word_off,
             (unsigned)ZL38063_HBI_PAGED_WRITE(word_off, (uint8_t)(n - 1u)));
    if (VprocTwolfHbiRead(B220_DSP_PAGE255_BASE_REG, 2, base_rb) == VPROC_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "page255 calib: reg 0x0C reads back {0x%04X,0x%04X} (wrote {0x%04X,0x%04X})",
                 (unsigned)base_rb[0], (unsigned)base_rb[1],
                 (unsigned)((target >> 16) & 0xFFFFu), (unsigned)(target & 0xFFFFu));
    } else {
        ESP_LOGW(TAG, "page255 calib: reg 0x0C could not be read back");
    }
    if (B220_DSP_PAGE255_CALIB_PRE_MS != 0u) {
        vTaskDelay(pdMS_TO_TICKS(B220_DSP_PAGE255_CALIB_PRE_MS));
    }

    /* ---- the read matrix: 2 encodings x 2 preselect x 2 base orders ------- */
    for (unsigned int b = 0; b < 2u; b++) {
        for (unsigned int p = 0; p < 2u; p++) {
            for (unsigned int e = 0; e < 2u; e++) {
                const unsigned int enc = enc_order[e];
                const unsigned int lo_hi = base_order[b];
                const char enc_char = (enc == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a';
                const uint16_t cmd_word =
                    ZL38063_HBI_PAGED_READ((uint16_t)zl38063_hbi_page255_window_offset(target, enc),
                                           (uint8_t)(n - 1u));
                unsigned int m = 0;

                memset(back, 0, sizeof(back));
                st = calib_write_window_base(target, lo_hi);
                if (st == VPROC_STATUS_SUCCESS) {
                    if (calib_window_read(target, enc, pre_order[p], n, back) == 0) {
                        m = calib_count_matches(back, pattern, n);
                    } else {
                        ESP_LOGW(TAG, "page255 calib: enc=%c preselect=%u base=%s read failed",
                                 enc_char, pre_order[p], lo_hi ? "lo_hi" : "hi_lo");
                    }
                }
                ESP_LOGI(TAG, "calib enc=%c preselect=%u base=%s -> matched %u/%u "
                              "(read %04X %04X %04X %04X, expected %04X %04X %04X %04X)",
                         enc_char, pre_order[p], lo_hi ? "lo_hi" : "hi_lo", m, n,
                         (unsigned)back[0], (unsigned)back[1], (unsigned)back[2],
                         (unsigned)back[3], (unsigned)pattern[0], (unsigned)pattern[1],
                         (unsigned)pattern[2], (unsigned)pattern[3]);
                if ((m == n) && !found) {
                    found = true;
                    best_enc = enc;
                    best_pre = pre_order[p];
                    best_base = lo_hi;
                    ESP_LOGI(TAG, "page255 calib: enc=%c preselect=%u base=%s matched 16/16 with "
                                  "window command word 0x%04X",
                             enc_char, best_pre, lo_hi ? "lo_hi" : "hi_lo", (unsigned)cmd_word);
                }
            }
        }
    }

    /* ---- what the offset field means (only meaningful once something read) -- */
    if (found) {
        unsigned short probe[2];
        const uint32_t next = target + 2u;   /* low byte 0x26 -> field 0x13 = word_off + 1 */
        bool word_scale = false;

        memset(probe, 0, sizeof(probe));
        if ((calib_write_window_base(target, best_base) == VPROC_STATUS_SUCCESS) &&
            (calib_window_read(next, ZL38063_PAGE255_OFF_WORD, 1u, 2u, probe) == 0) &&
            (probe[0] == pattern[1]) && (probe[1] == pattern[2])) {
            word_scale = true;
        }
        ESP_LOGI(TAG, "page255 calib: scale probe target 0x%08lX (low byte 0x%02X -> field 0x%02X) "
                      "read %04X %04X, pattern[1..2] = %04X %04X -> field+1 steps one %s",
                 (unsigned long)next, (unsigned)(next & 0xFFu),
                 (unsigned)zl38063_hbi_page255_window_offset(next, ZL38063_PAGE255_OFF_WORD),
                 (unsigned)probe[0], (unsigned)probe[1],
                 (unsigned)pattern[1], (unsigned)pattern[2],
                 word_scale ? "WORD (low byte is a byte offset, field = low byte / 2)"
                            : "byte (or the window does not answer for the neighbouring word)");
    } else {
        /* Nothing reproduced the pattern.  Before concluding that the write path
         * never reached DSP RAM, sweep the whole 256-byte window under the framing
         * that mirrors the upload (enc=b, SELECT_PAGE first, base {hi,lo}) and see
         * whether the pattern is in there at some other field value. */
        unsigned short scan[4];
        bool base_ok = (calib_write_window_base(target, 0u) == VPROC_STATUS_SUCCESS);

        for (unsigned int w = 0; base_ok && (w < B220_DSP_PAGE255_WINDOW_WORDS); w++) {
            const uint32_t scan_target = (target & ~0xFFu) | (uint32_t)(w * 2u);

            memset(scan, 0, sizeof(scan));
            if (calib_window_read(scan_target, ZL38063_PAGE255_OFF_WORD, 1u, 4u, scan) != 0) {
                break;
            }
            if ((scan[0] == pattern[0]) && (scan[1] == pattern[1])) {
                hit_field = w;
                ESP_LOGW(TAG, "page255 calib: pattern[0..1] found at offset field 0x%02X "
                              "(read %04X %04X %04X %04X, expected %04X %04X %04X %04X)",
                         w, (unsigned)scan[0], (unsigned)scan[1], (unsigned)scan[2],
                         (unsigned)scan[3], (unsigned)pattern[0], (unsigned)pattern[1],
                         (unsigned)pattern[2], (unsigned)pattern[3]);
                break;
            }
        }
    }

    s_page255_calibrated = true;
    s_page255_calib_encoding = found ? best_enc : (unsigned int)ZL38063_PAGE255_OFF_UPLOAD;

    if (found) {
        ESP_LOGI(TAG, "page255 calib: CONCLUSION enc=%c preselect=%u base=%s reproduces the pattern "
                      "16/16 -> a window READ must put %s in command bits 14..8; the verify will use "
                      "enc=%c",
                 (best_enc == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a',
                 best_pre, best_base ? "lo_hi" : "hi_lo",
                 (best_enc == ZL38063_PAGE255_OFF_WORD) ? "(targetAddr & 0xFF) / 2"
                                                        : "(targetAddr & 0xFF)",
                 (s_page255_calib_encoding == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a');
    } else {
        if (hit_field != 0xFFFFFFFFu) {
            ESP_LOGE(TAG, "page255 calib: CONCLUSION no encoding reproduced the pattern AT the target "
                          "0x%08lX, but it IS in the window at offset field 0x%02X: the write reached "
                          "DSP RAM and the offset field's scale/position is what is wrong, not the "
                          "transfer",
                     (unsigned long)target, hit_field);
        } else {
            ESP_LOGE(TAG, "page255 calib: CONCLUSION none of the 8 encodings reproduced the 16-word "
                          "pattern at 0x%08lX and the pattern is nowhere in the window's %u words: the "
                          "write path we mirror does NOT deposit our words in the page-255 window at this "
                          "base address (or this part cannot read the window at all - Microsemi's driver "
                          "never reads it).  The verify's mismatching words are therefore not evidence "
                          "about the uploaded image.",
                     (unsigned long)target, (unsigned)B220_DSP_PAGE255_WINDOW_WORDS);
        }
    }
    ESP_LOGI(TAG, "page255 calib: window base left at 0x%08lX (the verify re-points it per record)",
             (unsigned long)target);
    ESP_LOGI(TAG, "----- end ZL38063 page-255 window calibration -----");
    return ESP_OK;
}

#endif /* B220_DSP_PAGE255_CALIB */

/*----------------------------------------------------------------------------
 * Shared page-255 read-back of sampled image records
 *
 * The pre-GO observer (B220_DSP_VERIFY_BEFORE_GO, implemented in
 * xiaomodou_b220.cc) and the hook-release matrix (B220_DSP_RELEASE_MATRIX, below)
 * both have to answer the same question at two different moments: "are the image
 * records we sent actually in DSP RAM?"  This is the one implementation of that
 * read, so both report the same numbers through the same framing (SELECT_PAGE
 * 0xFF, PAGED_READ with the offset field from
 * zl38063_hbi_page255_window_offset(), one HBI word read per word) and with the
 * one encoding the calibration proved (zl38063_dsp_page255_calib_encoding(),
 * enc=b).
 *
 * It WRITES nothing except the page-255 window base register 0x0C, exactly as
 * many times as HbiSrecBoot_alt() writes it for the records it visits - a state
 * every upload already produces - so it is safe both inside the upload (pre-GO)
 * and after it (post-release).
 *
 * Cost: one 2-word base write plus one 16-bit transaction per word per record.
 * @stats may be NULL.
 *--------------------------------------------------------------------------*/
#if B220_DSP_PAGE255_CALIB
#define B220_SAMPLE_WORDS_BYTES   (4u * 16u + 1u)
#define B220_SAMPLE_READ_MAX      16u    /* words per "read/expected" line */

struct zl38063_sample_stats {
    unsigned int records;          /* records actually read                 */
    unsigned int records_ok;       /* records whose every word matched      */
    unsigned int words;            /* words compared                        */
    unsigned int matched;          /* words identical to the image          */
    unsigned long mismatching;     /* words that differ                     */
    unsigned int read_errors;      /* records whose window read failed      */
    bool         image_missing;    /* firmwareStreamLen == 0                */
};

/* One sample set for both read-back passes (the pre-GO hook in
 * xiaomodou_b220.cc and the release matrix below): record 0, record 1 when the
 * image has one, @n evenly spaced records across the whole image (k/n of it for
 * k = 1..n) and always the LAST record.  Duplicates are dropped, so a short image
 * yields fewer entries, and at most @max entries are written.  Recording 0 is
 * always first: it is the record the hardware probes saw as 0x0000 and the one the
 * image starts with. */
unsigned int zl38063_dsp_sample_index_set(unsigned int rec_cnt, unsigned int n,
                                          unsigned int *idx, unsigned int max)
{
    unsigned int count = 0;

    if ((idx == NULL) || (max == 0u) || (rec_cnt == 0u)) {
        return 0u;
    }
    if (n == 0u) {
        n = 1u;
    }

    idx[count++] = 0u;
    if ((rec_cnt > 1u) && (count < max)) {
        idx[count++] = 1u;
    }
    for (unsigned int k = 1u; (k <= n) && (count < max); k++) {
        unsigned int i = (rec_cnt * k) / n;
        bool dup = false;
        if (i >= rec_cnt) {
            i = rec_cnt - 1u;
        }
        for (unsigned int j = 0; j < count; j++) {
            if (idx[j] == i) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            idx[count++] = i;
        }
    }
    {
        const unsigned int last = rec_cnt - 1u;
        bool dup = false;
        for (unsigned int j = 0; j < count; j++) {
            if (idx[j] == last) {
                dup = true;
                break;
            }
        }
        if (!dup && (count < max)) {
            idx[count++] = last;
        }
    }
    return count;
}

/* "<first 4 words read back> / <same 4 words as sent>" of one record, both in
 * the same "XXXX XXXX ..." shape the pre-GO log uses. */
static void b220_sample_words_text(const twFwr *rec,
                                   const unsigned short *back,
                                   char *read_txt, size_t read_sz,
                                   char *sent_txt, size_t sent_sz)
{
    const unsigned int n = (rec->numWords > B220_SAMPLE_READ_MAX)
                               ? B220_SAMPLE_READ_MAX
                               : (unsigned int)rec->numWords;
    size_t rp = 0;
    size_t sp = 0;

    read_txt[0] = '\0';
    sent_txt[0] = '\0';
    for (unsigned int w = 0; (w < n) && (w < 4u); w++) {
        (void)snprintf(&read_txt[rp], read_sz - rp, "%04X ", (unsigned)back[w]);
        rp += 5u;
        (void)snprintf(&sent_txt[sp], sent_sz - sp, "%04X ", (unsigned)rec->buf[w]);
        sp += 5u;
    }
    if (rp >= read_sz) { read_txt[read_sz - 1u] = '\0'; }
    if (sp >= sent_sz) { sent_txt[sent_sz - 1u] = '\0'; }
}

static void b220_sample_indices_text(const unsigned int *idx, unsigned int nidx,
                                     unsigned int rec_cnt,
                                     char *line, size_t len)
{
    size_t off = 0;

    (void)rec_cnt;
    line[0] = '\0';
    for (unsigned int k = 0; k < nidx; k++) {
        int written = snprintf(&line[off], len - off, "%u ", idx[k]);
        if ((written < 0) || ((size_t)written >= (len - off))) {
            break;
        }
        off += (size_t)written;
    }
}

static void b220_sample_targets_text(const unsigned int *idx, unsigned int nidx,
                                     char *line, size_t len)
{
    size_t off = 0;

    line[0] = '\0';
    for (unsigned int k = 0; k < nidx; k++) {
        int written = snprintf(&line[off], len - off, "0x%08lX ",
                               (unsigned long)st_twFirmware[idx[k]].targetAddr);
        if ((written < 0) || ((size_t)written >= (len - off))) {
            break;
        }
        off += (size_t)written;
    }
}

/* Read @idx[] (values into st_twFirmware[], clamped to the linked image) back
 * through the page-255 window, compare word for word against what the upload
 * sent and log "record <i> target=0x... matched m/n (read ... expected ...)" for
 * every mismatch, then one SUMMARY line.  @tag names the moment the sample was
 * taken ("pre-GO" or "post-release-v<N>") and goes into the log prefix. */
esp_err_t zl38063_dsp_sample_records_readback(const char *tag,
                                              const unsigned int *idx,
                                              unsigned int nidx,
                                              struct zl38063_sample_stats *stats)
{
    struct zl38063_sample_stats local = {0};
    struct zl38063_sample_stats *st_out = (stats != NULL) ? stats : &local;

    *st_out = local;

    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "%s: no HBI device on SPI2 (zl38063_dsp_begin() not run)",
                 tag);
        return ESP_ERR_INVALID_STATE;
    }

    const unsigned int rec_cnt = (unsigned int)firmwareStreamLen;
    if (rec_cnt == 0u) {
        ESP_LOGE(TAG, "%s: no ZL38063 firmware image linked in "
                      "(firmwareStreamLen == 0), nothing to compare against", tag);
        st_out->image_missing = true;
        return ESP_ERR_NOT_FOUND;
    }

    const unsigned int enc = s_page255_calib_encoding;

    for (unsigned int k = 0; k < nidx; k++) {
        unsigned int i = idx[k];
        if (i >= rec_cnt) {
            i = rec_cnt - 1u;
        }
        const twFwr *rec = &st_twFirmware[i];

        if (rec->numWords == 0u) {
            ESP_LOGW(TAG, "%s: record %u target=0x%08lX is an address-only "
                          "record, nothing to read back", tag, i,
                     (unsigned long)rec->targetAddr);
            continue;
        }

        const uint32_t target = rec->targetAddr;
        if (rec->useTargetAddr != 0u) {
            unsigned short base[2];
            base[0] = (unsigned short)((target >> 16) & 0xFFFFu);
            base[1] = (unsigned short)(target & 0xFFFFu);
            VprocStatusType wst = VprocTwolfHbiWrite(B220_DSP_PAGE255_BASE_REG, 2,
                                                     base);
            if (wst != VPROC_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "%s: record %u target=0x%08lX: window base write to "
                              "reg 0x0C failed (%d); stopping", tag, i,
                         (unsigned long)target, (int)wst);
                break;
            }
        }

        unsigned short back[sizeof(rec->buf) / sizeof(rec->buf[0])];
        memset(back, 0, sizeof(back));
        VprocStatusType rst = VprocTwolfHbiPage255Read(
            target, (unsigned char)rec->numWords, back, enc);
        if (rst != VPROC_STATUS_SUCCESS) {
            st_out->read_errors++;
            ESP_LOGE(TAG, "%s: record %u target=0x%08lX: window read of %u word(s) "
                          "FAILED (%d)", tag, i, (unsigned long)target,
                     (unsigned)rec->numWords, (int)rst);
            continue;
        }

        unsigned int rec_match = 0;
        unsigned int rec_bad = 0;
        for (unsigned int w = 0; w < (unsigned int)rec->numWords; w++) {
            st_out->words++;
            if (back[w] == rec->buf[w]) {
                st_out->matched++;
                rec_match++;
            } else {
                rec_bad++;
            }
        }
        st_out->mismatching += rec_bad;
        st_out->records++;
        if (rec_bad == 0u) {
            st_out->records_ok++;
        }

        char read_txt[B220_SAMPLE_WORDS_BYTES];
        char sent_txt[B220_SAMPLE_WORDS_BYTES];
        b220_sample_words_text(rec, back, read_txt, sizeof(read_txt),
                               sent_txt, sizeof(sent_txt));
        ESP_LOGI(TAG, "%s: record %u target=0x%08lX matched %u/%u "
                      "(read %sexpected %s)",
                 tag, i, (unsigned long)target, rec_match,
                 (unsigned)rec->numWords, read_txt, sent_txt);

        unsigned int logged = 0;
        for (unsigned int w = 0; (w < (unsigned int)rec->numWords) && (logged < 4u);
             w++) {
            if (back[w] == rec->buf[w]) {
                continue;
            }
            ESP_LOGE(TAG, "%s: record %u target=0x%08lX word %u: sent 0x%04X "
                          "read 0x%04X", tag, i, (unsigned long)target, w,
                     (unsigned)rec->buf[w], (unsigned)back[w]);
            logged++;
        }
    }

    ESP_LOGI(TAG, "%s: SUMMARY records=%u visited=%u ok=%u words=%u matched=%u "
                  "mismatching_words=%lu read_errors=%u window_enc=%c calibrated=%u",
             tag, rec_cnt, st_out->records, st_out->records_ok, st_out->words,
             st_out->matched, st_out->mismatching, st_out->read_errors,
             (enc == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a',
             (unsigned)(s_page255_calibrated ? 1u : 0u));
    return ESP_OK;
}
#endif /* B220_DSP_PAGE255_CALIB */

/*----------------------------------------------------------------------------
 * Host-upload read-back verification (B220_DSP_VERIFY_UPLOAD)
 *
 * The upload path reports success (reg 0x34 gate passed, all 3699 records
 * written, reg 0x12C <- execAddr, reg 0x32 <- 0x000D, FirmwareStart) yet the DSP
 * keeps answering reg 0x030 & 0x0F with 0x0000.  Hypothesis under test: the
 * image the DSP actually received is not the image we meant to send, so the
 * application executes garbage (or never starts).
 *
 * This walks the SAME twFwr record array the upload walked (st_twFirmware[] /
 * firmwareStreamLen from zl38063_fwimg_data.c) and, for each sampled record,
 * sets the page-255 window base to that record's targetAddr exactly the way
 * HbiSrecBoot_alt() does (reg 0x0C <- {hi16, lo16}, 2 words, only when
 * useTargetAddr is set) and then READS the record's numWords words back through
 * the page-255 window and compares them word for word.
 *
 * Everything except that window-base setup is a read: no device register is
 * written, no record data is re-sent, so the DSP is left exactly as the failed
 * boot attempt left it (the final window base is the last useTargetAddr record
 * the loop touched, which is a state the upload itself produces every run).
 *
 * The read command's offset field comes from
 * zl38063_hbi_page255_window_offset() - the single place that mask/scale lives -
 * with the encoding the calibration in this file
 * (zl38063_dsp_page255_calibrate(), B220_DSP_PAGE255_CALIB) proved reproduces the
 * upload's own write.  The first version of this probe fed the target's raw low
 * byte to the command word and printed the low 16 bits as a "low byte" (the
 * "0x200" line), so its 13780/14856 mismatching words were an artefact of the
 * read side and proved nothing about the upload.
 *
 * Sampling: a record costs one 16-bit SPI transaction per word plus the window
 * base write, so a full 3699-record pass is roughly a third of the upload's own
 * ~3 s.  To keep the boot time bounded this samples every 4th record while
 * always including 0, 1, 2 and the last two.  The chosen indices are logged.
 *--------------------------------------------------------------------------*/
#if B220_DSP_VERIFY_UPLOAD

#define B220_DSP_VERIFY_STRIDE      4u   /* sample every 4th record          */
#define B220_DSP_VERIFY_MAX_MISMATCH 8u  /* how many mismatches get logged   */
#define B220_DSP_VERIFY_DUMP_WORDS  16u  /* hex dump of the first record     */

static bool s_verify_no_image_logged = false;

esp_err_t zl38063_dsp_verify_uploaded_image(void)
{
    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "verify_uploaded_image called before zl38063_dsp_begin() "
                      "- there is no HBI device on SPI2 to read from");
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t rec_cnt = (uint16_t)firmwareStreamLen;
    if (rec_cnt == 0) {
        if (!s_verify_no_image_logged) {
            ESP_LOGE(TAG, "upload verify: no ZL38063 firmware image linked in "
                          "(firmwareStreamLen == 0), nothing to compare against");
            s_verify_no_image_logged = true;
        }
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * Read-only, but pointless while an application is running: the live
     * application may itself write over the uploaded image, so a mismatch would
     * prove nothing.  The caller is expected to check, this is the backstop.
     */
    if (zl38063_dsp_is_running()) {
        ESP_LOGI(TAG, "upload verify: skipped, an application is already running "
                      "(its own RAM writes would make the comparison meaningless)");
        return ESP_OK;
    }

    /* Register the upload's own state for the conclusion line below. */
    uint16_t app_status = 0;
    (void)read_app_status(&app_status);
    ESP_LOGI(TAG, "----- ZL38063 host-upload read-back verify (B220_DSP_VERIFY_UPLOAD) -----");
    ESP_LOGI(TAG, "upload verify: %u records (3714-byte image), %u-byte twFwr "
                  "record stride; app status 0x%04X",
             (unsigned)rec_cnt, (unsigned)sizeof(twFwr), (unsigned)app_status);
    ESP_LOGI(TAG, "upload verify: NOTE this is a SAMPLED pass - every %uth "
                  "record, plus records 0, 1, 2, %u and %u",
             (unsigned)B220_DSP_VERIFY_STRIDE, (unsigned)(rec_cnt - 2u),
             (unsigned)(rec_cnt - 1u));
    ESP_LOGI(TAG, "upload verify: page-255 read = SELECT_PAGE(0xFF) + "
                  "PAGED_READ(offset = %s, n-1); the offset field is the one "
                  "zl38063_hbi_page255_window_offset() computes",
             (s_page255_calib_encoding == ZL38063_PAGE255_OFF_WORD)
                 ? "(targetAddr & 0xFF) / 2 [enc=b]"
                 : "(targetAddr & 0xFF) [enc=a]");
    ESP_LOGI(TAG, "upload verify: window encoding enc=%c, %s; base = reg 0x0C <- "
                  "{hi16, lo16} for every record with useTargetAddr set",
             (s_page255_calib_encoding == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a',
             s_page255_calibrated
                 ? "taken from the page-255 calibration that ran just before this pass"
                 : "the upload's own wire encoding (calibration did not run)");

    unsigned int rec_visited = 0;       /* records this pass actually looked at */
    unsigned int rec_ok = 0;            /* records with every word matching     */
    unsigned int words_compared = 0;
    unsigned int words_matched = 0;
    unsigned int mismatches = 0;
    unsigned int mismatched_records = 0;
    unsigned int read_errors = 0;
    bool in_ram = false;
    uint16_t dump[B220_DSP_VERIFY_DUMP_WORDS];
    unsigned int dump_valid = 0;

    for (unsigned int idx = 0; idx < (unsigned int)rec_cnt; idx++) {
        bool sample = (idx == 0u) || (idx == 1u) || (idx == 2u) ||
                      (idx == (unsigned int)(rec_cnt - 2u)) ||
                      (idx == (unsigned int)(rec_cnt - 1u)) ||
                      ((idx % B220_DSP_VERIFY_STRIDE) == 0u);
        if (!sample) {
            continue;
        }

        const twFwr *rec = &st_twFirmware[idx];
        if (rec->numWords == 0u) {
            continue;               /* address-only record: nothing to compare */
        }
        rec_visited++;

        const uint32_t target = rec->targetAddr;
        const uint16_t hi = (uint16_t)((target >> 16) & 0xFFFFu);
        const uint16_t lo = (uint16_t)(target & 0xFFFFu);

        /* Exactly HbiSrecBoot_alt(): only records with useTargetAddr re-point
         * the window base.  Records without one are skipped by that loader too,
         * so they are skipped here rather than compared at a stale base. */
        if (rec->useTargetAddr != 0u) {
            unsigned short base[2];
            base[0] = (unsigned short)hi;
            base[1] = (unsigned short)lo;
            VprocStatusType st = VprocTwolfHbiWrite(B220_DSP_PAGE255_BASE_REG, 2,
                                                    base);
            if (st != VPROC_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "upload verify: record %u: writing window base "
                              "0x%08lX to reg 0x0C failed (%d); stopping",
                         idx, (unsigned long)target, (int)st);
                break;
            }
        }
        if (!in_ram) {
            /* The low byte comes from the one helper, not from a raw "%02X" of
             * the low 16 bits: the old line printed 0x200 for target 0x00080200
             * and that mislabelled number is what sent this probe off the rails. */
            ESP_LOGI(TAG, "upload verify: record 0 target 0x%08lX -> page-255 "
                          "window base low byte 0x%02X, offset field 0x%02X (enc=%c)",
                     (unsigned long)target,
                     (unsigned)zl38063_hbi_page255_window_offset(target, ZL38063_PAGE255_OFF_BYTE),
                     (unsigned)zl38063_hbi_page255_window_offset(target, s_page255_calib_encoding),
                     (s_page255_calib_encoding == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a');
            in_ram = true;
        }

        unsigned short back[sizeof(rec->buf) / sizeof(rec->buf[0])];
        memset(back, 0, sizeof(back));
        VprocStatusType st = VprocTwolfHbiPage255Read(target,
                                                      (unsigned char)rec->numWords,
                                                      back, s_page255_calib_encoding);
        if (st != VPROC_STATUS_SUCCESS) {
            read_errors++;
            ESP_LOGE(TAG, "upload verify: record %u target 0x%08lX: window read "
                          "of %u words failed (%d)",
                     idx, (unsigned long)target, (unsigned)rec->numWords, (int)st);
            continue;
        }

        /* First sampled record gets a hex dump so the log shows whether
         * anything at all is sitting in DSP RAM where the image should be. */
        if (dump_valid == 0u) {
            unsigned int n = rec->numWords;
            if (n > B220_DSP_VERIFY_DUMP_WORDS) {
                n = B220_DSP_VERIFY_DUMP_WORDS;
            }
            for (unsigned int w = 0; w < n; w++) {
                dump[w] = back[w];
            }
            dump_valid = n;
        }

        unsigned int rec_bad = 0;
        for (unsigned int w = 0; w < rec->numWords; w++) {
            words_compared++;
            if (back[w] == rec->buf[w]) {
                words_matched++;
                continue;
            }
            mismatches++;
            rec_bad++;
            if (mismatches <= B220_DSP_VERIFY_MAX_MISMATCH) {
                ESP_LOGE(TAG, "upload verify: mismatch: <%u> target=0x%08lX "
                              "word %u: sent 0x%04X read 0x%04X",
                         idx, (unsigned long)target, w,
                         (unsigned)rec->buf[w], (unsigned)back[w]);
            }
        }        if (rec_bad == 0u) {
            rec_ok++;
        } else {
            mismatched_records++;
        }
    }

    ESP_LOGI(TAG, "upload verify: %u records read back, %u compared, %u word(s) "
                  "compared, %u matching",
             rec_visited, rec_ok, words_compared, words_matched);
    ESP_LOGI(TAG, "upload verify: SUMMARY records=%u verified=%u words=%u "
                  "mismatching_words=%u (mismatching_records=%u, read_errors=%u) "
                  "window_enc=%c calibrated=%u",
             (unsigned)rec_cnt, rec_visited, words_compared, mismatches,
             mismatched_records, read_errors,
             (s_page255_calib_encoding == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a',
             (unsigned)(s_page255_calibrated ? 1u : 0u));

    if (dump_valid > 0u) {
        char dump_line[4u * B220_DSP_VERIFY_DUMP_WORDS + 1u];
        unsigned int pos = 0;
        for (unsigned int w = 0; w < dump_valid; w++) {
            int n = snprintf(&dump_line[pos], sizeof(dump_line) - pos, "%04X ",
                             (unsigned)dump[w]);
            if (n <= 0 || (unsigned int)n >= (sizeof(dump_line) - pos)) {
                break;
            }
            pos += (unsigned int)n;
        }
        ESP_LOGI(TAG, "upload verify: first record (index 0) target 0x%08lX, "
                      "first %u words read back: %s",
                 (unsigned long)st_twFirmware[0].targetAddr, dump_valid,
                 dump_line);
    }

    if ((mismatches == 0u) && (read_errors == 0u)) {
        ESP_LOGI(TAG, "upload verify: every sampled word came back identical to "
                      "the image we sent; the DSP really does hold our image, so "
                      "the image CONTENT is not why the application never "
                      "reports running");
        ESP_LOGI(TAG, "----- end ZL38063 upload read-back verify -----");
        return ESP_OK;
    }

    if ((words_matched == 0u) || (read_errors > 0u)) {
        ESP_LOGE(TAG, "upload verify: RESULT no recovered word matched the image: "
                      "%u word(s) compared, %u mismatching across %u of %u records "
                      "read back, %u record(s) unreadable",
                 words_compared, mismatches, mismatched_records, rec_visited,
                 read_errors);
    } else {
        ESP_LOGE(TAG, "upload verify: RESULT mismatches are scattered inside an "
                      "image that partly matches (%u of %u words wrong in %u of %u "
                      "records read back), so the read side is working and part of "
                      "the transfer itself went wrong",
                 mismatches, words_compared, mismatched_records, rec_visited);
    }

    /*
     * A zero-match pass is still ambiguous after the calibration: either the DSP
     * never received the data, or the whole window-read framing is unusable on
     * this part.  The calibration (when compiled in) has already tested every
     * plausible encoding against a known pattern, so all that is left here is a
     * bounded cross-check with the OTHER encoding on the two records whose low
     * byte makes the two encodings differ (0x00080200 -> field 0x00 vs 0x00 -
     * equal, so record 0 cannot separate them; 0x00080220 -> 0x20 vs 0x10).
     */
    if (words_matched == 0u) {
        const unsigned int other_enc = (s_page255_calib_encoding == ZL38063_PAGE255_OFF_WORD)
                                           ? (unsigned int)ZL38063_PAGE255_OFF_BYTE
                                           : (unsigned int)ZL38063_PAGE255_OFF_WORD;
        unsigned int alt_matched = 0;
        unsigned int alt_words = 0;

        ESP_LOGW(TAG, "upload verify: no sampled word matched at all; re-reading "
                      "records 0..1 with the OTHER window offset encoding (enc=%c, "
                      "field = %s) to tell 'the DSP never got the data' apart from "
                      "'this read command is wrong'",
                 (other_enc == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a',
                 (other_enc == ZL38063_PAGE255_OFF_WORD) ? "(targetAddr & 0xFF) / 2"
                                                         : "(targetAddr & 0xFF)");

        for (unsigned int idx = 0; (idx < 2u) && (idx < (unsigned int)rec_cnt); idx++) {
            const twFwr *rec = &st_twFirmware[idx];
            unsigned short alt[sizeof(rec->buf) / sizeof(rec->buf[0])];
            unsigned short base[2] = { (unsigned short)((rec->targetAddr >> 16) & 0xFFFFu),
                                       (unsigned short)(rec->targetAddr & 0xFFFFu) };

            if (rec->numWords == 0u) {
                continue;
            }
            VprocStatusType st = VprocTwolfHbiWrite(B220_DSP_PAGE255_BASE_REG, 2, base);
            if (st != VPROC_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "upload verify: alt-encoding probe: window base "
                              "write failed (%d)", (int)st);
                break;
            }
            memset(alt, 0, sizeof(alt));
            st = VprocTwolfHbiPage255Read(rec->targetAddr,
                                          (unsigned char)rec->numWords, alt, other_enc);
            if (st != VPROC_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "upload verify: alt-encoding probe: record %u read "
                              "failed (%d)", idx, (int)st);
                continue;
            }
            unsigned int m = 0;
            for (unsigned int w = 0; w < rec->numWords; w++) {
                alt_words++;
                if (alt[w] == rec->buf[w]) {
                    m++;
                }
            }
            alt_matched += m;
            ESP_LOGW(TAG, "upload verify: alt enc=%c record %u target 0x%08lX: "
                          "%u/%u words matched; first 4 read %04X %04X %04X %04X "
                          "(sent %04X %04X %04X %04X)",
                     (other_enc == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a',
                     idx, (unsigned long)rec->targetAddr, m,
                     (unsigned)rec->numWords,
                     (unsigned)alt[0],
                     (unsigned)((rec->numWords > 1u) ? alt[1] : 0u),
                     (unsigned)((rec->numWords > 2u) ? alt[2] : 0u),
                     (unsigned)((rec->numWords > 3u) ? alt[3] : 0u),
                     (unsigned)rec->buf[0],
                     (unsigned)((rec->numWords > 1u) ? rec->buf[1] : 0u),
                     (unsigned)((rec->numWords > 2u) ? rec->buf[2] : 0u),
                     (unsigned)((rec->numWords > 3u) ? rec->buf[3] : 0u));
        }

        if (alt_matched > 0u) {
            ESP_LOGE(TAG, "upload verify: CONCLUSION the OTHER window offset "
                          "encoding matches (%u/%u words) while enc=%c does not: "
                          "either the calibration did not run or the record's low "
                          "byte makes the two encodings differ for this record",
                     alt_matched, alt_words,
                     (s_page255_calib_encoding == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a');
        } else if (s_page255_calibrated) {
            ESP_LOGE(TAG, "upload verify: CONCLUSION neither encoding reproduced the "
                          "image (%u words checked) although the calibration did "
                          "reproduce a known pattern with enc=%c: the window reads "
                          "work, so the page-255 window simply does not hold our "
                          "payload at these target addresses - the uploaded image "
                          "CONTENT/write target is wrong, not the read framing",
                     alt_words,
                     (s_page255_calib_encoding == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a');
        } else {
            ESP_LOGE(TAG, "upload verify: CONCLUSION neither window offset encoding "
                          "reproduced the image (%u words checked) and no calibration "
                          "ran, so 'the DSP never got the data' and 'the read framing "
                          "is wrong' are still indistinguishable - enable "
                          "B220_DSP_PAGE255_CALIB", alt_words);
        }
    }

    ESP_LOGI(TAG, "----- end ZL38063 upload read-back verify -----");
    return ESP_OK;
}

#endif /* B220_DSP_VERIFY_UPLOAD */

/*----------------------------------------------------------------------------
 * Hands-off bring-up (B220_DSP_HANDS_OFF)
 *
 * The stock firmware never host-boots this DSP: a 60 s boot log captured on the
 * real board (baseboard unplugged, sound works) has no
 * "1- Firmware boot loading started ...." / "prgmBase 0x…" / "execAddr 0x…" /
 * "2- Loading the image to RAM....done" in it at all.  The ZL38063 auto-boots
 * from its own slave SPI flash and the stock only configures it.  This pair of
 * entry points is the "do not disturb it" path: attach the HBI, write the one
 * HBI configuration register, then only ever READ.
 *
 * NOTHING in this block may write a device register other than the 0xFD80 of
 * VprocTwolfHbiInit(), and nothing in it may touch GPIO19 (the DSP reset) in any
 * way - not even a read.
 *--------------------------------------------------------------------------*/
#if B220_DSP_HANDS_OFF

esp_err_t zl38063_dsp_hands_off_attach(void)
{
    /* 1. Attach to the SPI2 bus the LCD already initialised.  This touches CS
     *    GPIO0 only (spi_bus_add_device + a floating pull); GPIO19 is not
     *    referenced anywhere in this function. */
    esp_err_t err = zl38063_hbi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hands-off: HBI attach failed: %s", esp_err_to_name(err));
        return err;
    }
    s_hbi_ready = true;

    /* 2. The one and only device write: 0xFD80, the HBI port configuration
     *    register, exactly as VprocTwolfHbiInit() does upstream.  No reset, no
     *    boot-ROM entry, no image, no config record. */
    VprocStatusType st = VprocTwolfHbiInit();
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "hands-off: VprocTwolfHbiInit (HBI config 0xFD80) failed "
                      "(%d) - the HBI port did not answer", (int)st);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "hands-off: HBI attached (cs=%d) and HBI config written; "
                  "GPIO%d (reset) untouched, no reset pulse, no reg 0x14 <- 1, "
                  "no host-boot upload - the DSP keeps whatever application it "
                  "auto-booted from its own flash",
             (int)ZL38063_HBI_CS_GPIO, (int)ZL38063_RESET_GPIO);
    return ESP_OK;
}

/* Append to a bounded one-line buffer; returns the new offset, clamped to the
 * buffer size.  Used to build the compact status line without a fixed-layout
 * snprintf (whose per-register error text would otherwise need a sentinel). */
static size_t hands_off_appendf(char *buf, size_t len, size_t off,
                                const char *fmt, ...)
{
    va_list ap;

    if (off >= len) {
        return len;
    }
    va_start(ap, fmt);
    int written = vsnprintf(buf + off, len - off, fmt, ap);
    va_end(ap);
    if (written < 0) {
        return off;
    }
    if ((size_t)written > (len - off)) {
        return len;     /* truncated: saturate instead of overflowing */
    }
    return off + (size_t)written;
}

int zl38063_dsp_status_line(unsigned elapsed_s)
{
    /* The hands-off register set, in log order: app status first, then the host
     * command/result pair, the host flags and the boot ROM id block.  The label
     * goes straight into the line so the log stays self-describing. */
    static const struct {
        uint16_t reg;
        const char *label;
    } regs[] = {
        {0x0030, "app status"},
        {0x0032, "host command"},
        {0x0034, "cmd result"},
        {0x0006, "host flags"},
        {0x0020, "id word 0"},
        {0x0022, "id word 1"},
        {0x0024, "id word 2"},
    };

    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "hands-off status line called before the HBI attach - "
                      "there is no HBI device on SPI2 to read from");
        return -1;
    }

    char line[320];
    size_t off = hands_off_appendf(line, sizeof(line), 0, "hands-off +%us:",
                                  (unsigned)elapsed_s);

    int masked = -1;

    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        unsigned short value = 0;
        VprocStatusType st = VprocTwolfHbiRead((unsigned short)regs[i].reg, 1,
                                              &value);
        if (st != VPROC_STATUS_SUCCESS) {
            off = hands_off_appendf(line, sizeof(line), off,
                                    " 0x%04X=<err %d>(%s)",
                                    (unsigned)regs[i].reg, (int)st,
                                    regs[i].label);
            continue;
        }
        if (regs[i].reg == 0x0030) {
            /* The one register the whole experiment is about: report the masked
             * application status (0 = not running, non-zero = running) inline. */
            masked = (int)((uint16_t)value & 0x0F);
            off = hands_off_appendf(line, sizeof(line), off,
                                    " 0x%04X=0x%04X(%s: %s)",
                                    (unsigned)regs[i].reg, (unsigned)value,
                                    regs[i].label,
                                    (masked != ZL38063_APP_STATUS_NOT_RUNNING)
                                    ? "RUNNING" : "not running");
            continue;
        }
        off = hands_off_appendf(line, sizeof(line), off, " 0x%04X=0x%04X(%s)",
                                (unsigned)regs[i].reg, (unsigned)value,
                                regs[i].label);
    }
    (void)off;

    ESP_LOGI(TAG, "%s", line);
    return masked;
}

#endif /* B220_DSP_HANDS_OFF */

esp_err_t zl38063_dsp_begin(void)
{    /* 1. hardware reset: GPIO19 low 10 ms, then high. */
    gpio_config_t rst = {0};
    rst.pin_bit_mask = (1ULL << ZL38063_RESET_GPIO);
    rst.mode = GPIO_MODE_OUTPUT;
    rst.pull_up_en = GPIO_PULLUP_DISABLE;
    rst.pull_down_en = GPIO_PULLDOWN_DISABLE;
    rst.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&rst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(reset GPIO%d) failed: %s",
                 (int)ZL38063_RESET_GPIO, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "reset: GPIO%d low %d ms", (int)ZL38063_RESET_GPIO,
             ZL38063_RESET_LOW_MS);
    gpio_set_level((gpio_num_t)ZL38063_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(ZL38063_RESET_LOW_MS));
    gpio_set_level((gpio_num_t)ZL38063_RESET_GPIO, 1);

    /* 2. the 500 ms the stock loader waits after releasing the reset before it
     * touches the HBI; the device runs its boot ROM (it does not self-boot). */
    vTaskDelay(pdMS_TO_TICKS(ZL38063_BOOT_WAIT_MS));

    /* 3. attach to the SPI2 bus the LCD already initialised. */
    err = zl38063_hbi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HBI attach failed: %s", esp_err_to_name(err));
        return err;
    }
    s_hbi_ready = true;

    /* 4. HBI configuration register (0xFD80). */
    VprocStatusType st = VprocTwolfHbiInit();
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "VprocTwolfHbiInit failed (%d)", (int)st);
        return ESP_FAIL;
    }

    /* 5. report what the device says about itself. */
    uint16 status = 0;
    int app_status = read_app_status(&status);
    if (app_status < 0) {
        ESP_LOGW(TAG, "ZL38063 did not answer on HBI (check CS GPIO%d, reset "
                      "GPIO%d and that SPI2 is up)",
                 (int)ZL38063_HBI_CS_GPIO, (int)ZL38063_RESET_GPIO);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "app status = 0x%04X (masked 0x%02X)", (unsigned)status,
             (unsigned)app_status);

    /* 6. host-boot the DSP exactly like the stock firmware, unless an
     * application is already running (then a reset + upload would only throw
     * a working image away). */
    if ((app_status & 0x0F) != ZL38063_APP_STATUS_NOT_RUNNING) {
        ESP_LOGI(TAG, "the ZL38063 application is already running (status "
                      "0x%02X); skipping the host-boot upload", (unsigned)app_status);
    } else {
        err = zl38063_dsp_upload_firmware();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "host-boot upload failed: %s; the DSP is not running "
                          "an application", esp_err_to_name(err));
            return err;
        }
    }

    /* 7. report the status again, now that the image has been uploaded. */
    status = 0;
    app_status = read_app_status(&status);
    if (app_status < 0) {
        ESP_LOGW(TAG, "the ZL38063 stopped answering after the upload");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "app status = 0x%04X (masked 0x%02X) after the host boot",
             (unsigned)status, (unsigned)app_status);
    if ((app_status & 0x0F) == ZL38063_APP_STATUS_NOT_RUNNING) {
        ESP_LOGW(TAG, "the ZL38063 application still reports 'not running' "
                      "after the host-boot upload; the image transfer itself "
                      "reported success, so check the entry point "
                      "(0x%08lX) and the I2S/clock wiring",
                 (unsigned long)dsp_exec_addr());
    }
    return ESP_OK;
}

esp_err_t zl38063_dsp_end(void)
{
    zl38063_hbi_deinit();
    s_hbi_ready = false;
    return ESP_OK;
}

esp_err_t zl38063_dsp_mailbox_probe(void)
{
    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "mailbox_probe called before zl38063_dsp_begin()");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * This probe is destructive by construction: it stops the running firmware
     * and drops the device into its boot ROM (reg 0x014 <- 1).  After a
     * successful host boot that would throw the working image away, so refuse
     * to do it while an application is running - there is nothing to probe
     * then anyway.
     */
    if (zl38063_dsp_is_running()) {
        ESP_LOGW(TAG, "mailbox probe skipped: an application is already running "
                      "(probing would reset the DSP into boot ROM)");
        return ESP_OK;
    }

    /*
     * Stop the running firmware and drop the device into its boot ROM.  This
     * is exactly what the stock firmware does before handing over an image.
     */
    unsigned short buf = 1;
    VprocStatusType st = VprocTwolfHbiWrite(0x014, 1, &buf);
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "mailbox probe: write 0x014 failed (%d)", (int)st);
        return ESP_FAIL;
    }

    /* The device needs 300 ms to come up in boot mode. */
    Vproc_msDelay(300);

    buf = 0;
    st = VprocTwolfHbiRead(0x034, 1, &buf);
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "mailbox probe: read 0x034 failed (%d)", (int)st);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "mailbox probe: reg 0x034 = 0x%04X (boot ROM magic 0xD3D3)",
             (unsigned)buf);
    if (buf == ZL38063_BOOTROM_MAGIC) {
        ESP_LOGW(TAG, "the DSP is sitting in its boot ROM waiting for a "
                      "host-booted image - call zl38063_dsp_upload_firmware() "
                      "(the image is linked in from zl38063_fwimg_data.c)");
    } else {
        ESP_LOGI(TAG, "the DSP did not answer with the boot ROM magic; it is "
                      "running an application (the stock firmware always "
                      "host-boots this DSP, so this is the uploaded one)");
    }
    return ESP_OK;
}

esp_err_t zl38063_dsp_load_config(void)
{
    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "load_config called before zl38063_dsp_begin()");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * zl38063_config_record[][] is layout-identical to Microsemi's dataArr
     * ({uint16_t reg; uint16_t value;}), so no conversion is needed.
     */
    VprocStatusType st = VprocTwolfLoadConfig((dataArr *)zl38063_config_record,
                                              ZL38063_CONFIG_RECORD_COUNT);
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "config record load failed (%d) after %d registers",
                 (int)st, ZL38063_CONFIG_RECORD_COUNT);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "config record loaded: %d registers (0x%04X..0x%04X)",
             ZL38063_CONFIG_RECORD_COUNT,
             ZL38063_CONFIG_RECORD_FIRST_REG,
             ZL38063_CONFIG_RECORD_LAST_REG);
    return ESP_OK;
}

esp_err_t zl38063_dsp_set_volume(uint8_t vol)
{
    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "set_volume called before zl38063_dsp_begin()");
        return ESP_ERR_INVALID_STATE;
    }
    if (vol > 100) {
        ESP_LOGW(TAG, "volume %u out of range (0..100)", (unsigned)vol);
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Volume -> gain, on a PERCEPTUAL curve rather than a straight line.
     *
     * The straight line this used to be (`gain = vol - 90`) put 100 equally spaced
     * values on 100 dB, i.e. a constant +10 dB per 10% step.  Loudness is roughly
     * logarithmic, so a fixed 10 dB step is a 3.2x amplitude jump at EVERY level: fine
     * near mute, unpleasantly large near the top, and the last steps also run into the
     * DSP's +6 dB saturation.  Reported from the bench exactly that way ("the jump
     * feels much bigger at high volume").
     *
     * THE STOCK FIRMWARE DID NOT DO THIS.  Its volume routine (0x401256D8) takes
     * `index = vol / 10` - TEN levels, not a hundred - and looks the gain byte up in a
     * table.  Two things follow, and both matter:
     *
     *   1. the stock top level was NOT +10 dB: the boot config record it loads leaves
     *      0x0238 at 0xF1 = -15 dB, and its own play path writes 0xEC = -20 dB, so +10
     *      dB was never in its range at all.  This port's mapping put 100% at +10 dB,
     *      which is both louder than stock and in the region where the limiter works.
     *   2. above 80 the stock routine ALSO writes the second attenuator
     *      (0x0494 = 0x16C3, 0x049A = 1) instead of 0x0666/2 below it - a coarse/fine
     *      split this port has never implemented (see PORTING-GUIDE-PART2 §6.3).
     *
     * Without that second stage, the honest thing is a curve that keeps the useful
     * range usable and stops short of saturation, so equal steps feel equal:
     *
     *     gain_dB = -90 + 96 * (vol/100)^1.7      ->  -90 dB at 0, about -13 dB at 70,
     *                                                +0.6 dB at 95, +6 dB at 100
     *
     * The top now lands on the +6 dB the DSP can actually deliver instead of asking for
     * +10 dB and being clipped, and the steps shrink as the volume rises.  It is an
     * approximation of a loudness curve, not the stock table: when the 0x0494 stage is
     * implemented, this should be replaced by the table itself.
     */
    {
        /* Integer power 1.7 without floating point or libm: x^1.7 = x * x^0.7, and
         * x^0.7 is taken from a 33-entry table (3% steps) with linear interpolation,
         * so the curve is monotonic and every volume value maps to a stable gain. */
        static const uint8_t kPow07[33] = {
            0, 6, 10, 13, 16, 19, 21, 24, 26, 28, 30, 32, 34, 36, 38,
            39, 41, 43, 44, 46, 47, 49, 50, 52, 53, 55, 56, 57, 59, 60,
            61, 62, 64
        };
        const int v = (int)vol;                 /* 0..100, checked above */
        const int idx = v * 32 / 100;           /* 0..32 */
        const int rem = (v * 32) % 100;
        int p = kPow07[idx];
        if (idx < 32) {
            p += (kPow07[idx + 1] - kPow07[idx]) * rem / 100;
        }
        /* 96 dB of range over the curve, anchored at -90 dB so that 0 is still mute. */
        const int gain_db = ZL38063_VOLUME_ZERO_DB + (96 * p) / 64;
        const int8_t gain = (int8_t)gain_db;

        VprocStatusType st = VprocTwolfSetVolume((uint8)gain);
        if (st != VPROC_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "set volume failed (%d)", (int)st);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "volume %u -> gain 0x%02X (%d dB, perceptual curve)", (unsigned)vol,
                 (unsigned)(uint8)gain, gain_db);
        return ESP_OK;
    }
}

bool zl38063_dsp_is_running(void)
{
    if (!s_hbi_ready) {
        return false;
    }
    int app_status = read_app_status(NULL);
    if (app_status < 0) {
        return false;
    }
    /* Upstream masks 0x030 & 0x0F; 0 means the application is not loaded. */
    return ((app_status & 0x0F) != ZL38063_APP_STATUS_NOT_RUNNING);
}

/*----------------------------------------------------------------------------
 * Single clean ADF-order release + TX (B220_DSP_ADF_RELEASE_TX)
 *
 * The decisive combination: ONE reset + ONE full upload, then exactly the
 * release sequence matrix variant 6 ran (B220_DSP_RELEASE_MATRIX, below), and
 * then straight to the audio loopback.  Nothing else touches the device.
 *
 * In order, on the device:
 *   1. zl38063_dsp_begin() - the production boot: GPIO19 reset pulse (10 ms low,
 *      500 ms settle), HBI attach on SPI2, VprocTwolfHbiInit() (0xFD80),
 *      reg 0x14 <- 1 + the 300 ms wait + the reg 0x34 == 0xD3D3 gate, the
 *      complete 3699-record transfer, and reg 0x12C <- execAddr.  Its OWN two
 *      release writes are suppressed at their call sites first, so step 2 owns
 *      them - with no release write suppressed the transfer would end with
 *      0x000D and the GO would follow from inside it, which is NOT the ADF
 *      order this experiment needs;
 *   2. reg 0x32 <- 0x000D   HOST_CMD_HOST_LOAD_CMP (VprocTwolfHostCmdWr());
 *   3. the 260-register config record (VprocTwolfLoadConfig());
 *   4. reg 0x32 <- 0x0008   the GO (VprocTwolfHostCmdWr(), the write
 *      VprocTwolfFirmwareStart() would emit);
 *   5. VprocTwolfReset(VPROC_RST_SOFTWARE) - one device write, reg 0x0006 <-
 *      0x0002, plus its own 200 ms wait ("firmware reset - in order for the
 *      configuration to take effect");
 *   6. +500 ms, then ONE read-only line with reg 0x0030 / 0x0032 / 0x0034 /
 *      0x0006.  No window-base write, no page-255 read, no other access.
 *
 * The same five entry points, in the same order, as matrix variant 6: the
 * difference is that the matrix runs up to ten variants back to back, each with
 * its own full reset + 3699-record upload and its own 9-record read-back, while
 * this runs one variant once and then hands the device to the loopback.
 *
 * Both release writes of the upload path are suppressed AT THEIR CALL SITE
 * through the zl38063_vproc.c setters (VprocTwolfHbiBoot_alt() writes 0x000D,
 * zl38063_dsp_upload_firmware() writes 0x0008); the suppression is cleared
 * before this function returns, so it can never leak into a later boot.  Note
 * that with 0x000D suppressed VprocTwolfHbiBoot_alt() reports FW_LOAD_FAILED by
 * design and zl38063_dsp_upload_firmware() maps that back to ESP_OK - the
 * transfer itself did happen, which is why step 2 must send 0x000D itself.
 *
 * Device accesses on this path, exhaustively: the reset GPIO, VprocTwolfHbiInit
 * (0xFD80), reg 0x14 <- 1, the record writes with their reg 0x0C window bases,
 * reg 0x12C, reg 0x32 <- 0x000D, the 260 config-register writes, reg 0x32 <-
 * 0x0008, reg 0x0006 <- 0x0002, and four HBI reads for the status line.  There
 * is no zl38063_dsp_mailbox_probe() (it would write reg 0x014 <- 1 and force the
 * device back into its boot ROM), no zl38063_dsp_hbi_diag(), no
 * zl38063_dsp_gpio_forensics(), no zl38063_dsp_hbi_sweep(), no
 * zl38063_dsp_page255_calibrate(), no zl38063_dsp_verify_uploaded_image(), no
 * pre-GO hook and no release / boot-address matrix: the ones guarded by
 * B220_ZL_DIAG / B220_DSP_PAGE255_CALIB / B220_DSP_VERIFY_UPLOAD /
 * B220_DSP_VERIFY_BEFORE_GO / B220_DSP_RELEASE_MATRIX / B220_DSP_ADDR_MATRIX are
 * not even compiled with those switches off, and the board file additionally
 * gates every one of those call sites on !B220_DSP_ADF_RELEASE_TX.
 *--------------------------------------------------------------------------*/
#if B220_DSP_ADF_RELEASE_TX

/* How long after the release the read-only status line is taken.  Step 5 has
 * already waited the reset's own 200 ms when this starts. */
#define B220_ADF_TX_STATUS_WAIT_MS 500u

/* Set once the sequence has really run, so zl38063_dsp_release_name() cannot
 * claim the ADF order for a boot in which it was skipped or failed. */
static bool s_adf_release_ran = false;

const char *zl38063_dsp_release_name(void)
{
    return s_adf_release_ran
               ? "ADF order (variant 6): reg 0x12C (upload) + reg 0x32 <- 0x000D + "
                 "config record (260 reg) + reg 0x32 <- 0x0008 (GO) + "
                 "VprocTwolfReset(VPROC_RST_SOFTWARE) = reg 0x0006 <- 0x0002"
               : "ADF order requested (B220_DSP_ADF_RELEASE_TX=1) but the sequence "
                 "did NOT run this boot (see the adf-tx lines above)";
}

esp_err_t zl38063_dsp_release_adf_tx(void)
{
    /* Phase 3-16 (2/2): NO s_hbi_ready check at entry, and that is the fix.
     *
     * The guard that used to sit here tested a flag whose only setter is this
     * function's own step 1/5 (zl38063_dsp_begin() -> zl38063_hbi_init() sets
     * s_hbi_ready=true): the function could never run, it returned
     * ESP_ERR_INVALID_STATE on every boot ("adf-tx: called before
     * zl38063_dsp_begin() - there is no HBI device on SPI2 to boot through"),
     * the DSP was never reset or booted, and the loopback read pure silence.
     * Moving the call site after the I2S bring-up (xiaomodou_b220.cc) was
     * necessary - the ZL38063 takes its clock from this port's BCLK - but not
     * sufficient, because the I2S bring-up does not attach the HBI device:
     * only step 1/5 below does.
     *
     * So step 1/5 is allowed to attach the bus.  Steps 2/5..5/5 genuinely need
     * it and are guarded by the check right after step 1/5 (same condition, but
     * now after the one step that can satisfy it, and each failure line names
     * the condition that failed). */
    ESP_LOGW(TAG, "===== ZL38063 ADF-order release + TX (B220_DSP_ADF_RELEASE_TX): "
                  "ONE reset + ONE full upload, then the esp-adf release order, then "
                  "the audio loopback =====");

    /* Step 2 owns both release writes: suppress the two the upload path would
     * otherwise send (0x000D inside VprocTwolfHbiBoot_alt(), 0x0008 through
     * VprocTwolfFirmwareStart()).  reg 0x12C is NOT suppressed - it is part of
     * the record transfer and is the entry point the ADF order leaves in place. */
    zl38063_vproc_release_cmd_host_load_complete_from_host_boot(true);
    zl38063_vproc_firmware_start_from_host_boot(true);
    ESP_LOGI(TAG, "adf-tx: the upload path's own release writes are suppressed at "
                  "their call sites (0x000D=%u 0x0008=%u), so the sequence below is "
                  "the complete set of release writes; reg 0x12C is written by the "
                  "upload's own record transfer",
             (unsigned)(zl38063_vproc_release_cmd_host_load_complete_is_suppressed()
                            ? 1u : 0u),
             (unsigned)(zl38063_vproc_firmware_start_is_suppressed() ? 1u : 0u));

    /* Step 1: one hardware reset + the complete image transfer. */
    esp_err_t err = zl38063_dsp_begin();
    if (err != ESP_OK) {
        zl38063_vproc_release_cmd_host_load_complete_from_host_boot(false);
        zl38063_vproc_firmware_start_from_host_boot(false);
        ESP_LOGE(TAG, "adf-tx: FAILED at condition 'step 1/5 = GPIO19 reset + HBI "
                      "attach + full image upload completes' (%s); no release sequence "
                      "issued and the suppression is cleared again",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "adf-tx: step 1/5 done: GPIO19 reset + %u record(s) uploaded + "
                  "reg 0x12C <- 0x%08lX; no release write has reached the device yet",
             (unsigned)firmwareStreamLen, (unsigned long)dsp_exec_addr());

    /* The later check that DOES belong here: steps 2/5..5/5 are raw HBI register
     * writes (0x000D, the 260-register config record, the GO, the soft reset),
     * so from step 2 on the HBI device on SPI2 must exist.  Step 1/5 above was
     * supposed to create it; if it did not - one condition, one named log line,
     * so a boot log can never be misread as "the sequence ran". */
    if (!s_hbi_ready) {
        zl38063_vproc_release_cmd_host_load_complete_from_host_boot(false);
        zl38063_vproc_firmware_start_from_host_boot(false);
        ESP_LOGE(TAG, "adf-tx: FAILED at condition 'HBI device attached to SPI2' - "
                      "s_hbi_ready is still false AFTER step 1/5 (zl38063_dsp_begin() "
                      "returned ESP_OK but no HBI device exists on SPI2); steps 2/5..5/5 "
                      "need that bus, so NO release sequence was issued (no reg 0x32 <- "
                      "0x000D, no config record, no reg 0x32 <- 0x0008, no "
                      "VprocTwolfReset) and the upload-path suppression is cleared again");
        return ESP_ERR_INVALID_STATE;
    }

    /* Step 2: reg 0x32 <- 0x000D, "host application load complete". */
    VprocStatusType st = VprocTwolfHostCmdWr(0x000D);
    ESP_LOGI(TAG, "adf-tx: step 2/5: reg 0x32 <- 0x000D (host load complete) -> %s",
             (st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED");

    /* Step 3: the 260-register config record, in the ADF position - after
     * 0x000D and before the GO.  Same array and same entry point the production
     * path uses (zl38063_dsp_load_config() is this call plus its log line). */
    VprocStatusType cfg_st = VprocTwolfLoadConfig((dataArr *)zl38063_config_record,
                                                  ZL38063_CONFIG_RECORD_COUNT);
    ESP_LOGI(TAG, "adf-tx: step 3/5: config record (%u registers, 0x%04X..0x%04X) "
                  "loaded between 0x000D and the GO -> %s",
             (unsigned)ZL38063_CONFIG_RECORD_COUNT,
             (unsigned)ZL38063_CONFIG_RECORD_FIRST_REG,
             (unsigned)ZL38063_CONFIG_RECORD_LAST_REG,
             (cfg_st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED");

    /* Step 4: reg 0x32 <- 0x0008, the GO. */
    VprocStatusType go_st = VprocTwolfHostCmdWr(0x0008);
    ESP_LOGI(TAG, "adf-tx: step 4/5: reg 0x32 <- 0x0008 (GO) -> %s",
             (go_st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED");

    /* Step 5: the soft reset, through the ported entry point - its only device
     * write is reg 0x0006 <- 0x0002, followed by its own 200 ms wait. */
    VprocStatusType rst_st = VprocTwolfReset(VPROC_RST_SOFTWARE);
    ESP_LOGI(TAG, "adf-tx: step 5/5: VprocTwolfReset(VPROC_RST_SOFTWARE) = "
                  "reg 0x0006 <- 0x0002 (+ its own 200 ms wait) -> %s",
             (rst_st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED");

    /* The suppression must never outlive this sequence. */
    zl38063_vproc_release_cmd_host_load_complete_from_host_boot(false);
    zl38063_vproc_firmware_start_from_host_boot(false);

    /* s_adf_release_ran is set while ALL FIVE steps are known to have run: step 1
     * returned ESP_OK (its failure path above returns before this point) and 2..5
     * are status codes reported by the device, not host errors.  Only a failure
     * that returns from this function leaves the flag false, so
     * zl38063_dsp_release_name() can never say "did NOT run this boot" for a boot
     * in which the sequence was issued - and the completion line below can never
     * claim the five steps for a boot in which one of them failed. */
    const bool steps_ok = (st == VPROC_STATUS_SUCCESS) && (cfg_st == VPROC_STATUS_SUCCESS) &&
                          (go_st == VPROC_STATUS_SUCCESS) && (rst_st == VPROC_STATUS_SUCCESS);
    s_adf_release_ran = true;
    ESP_LOGI(TAG, "adf-tx: release sequence COMPLETE: all five steps ran in the ADF / "
                  "variant-6 order (1/5 begin = GPIO19 reset + full image upload + reg "
                  "0x12C; 2/5 reg 0x32 <- 0x000D; 3/5 config record; 4/5 reg 0x32 <- "
                  "0x0008 GO; 5/5 VprocTwolfReset(VPROC_RST_SOFTWARE) = reg 0x0006 <- "
                  "0x0002) - all five reported success: %s; per step: 1/5 upload=done, "
                  "2/5 0x000D=%s, 3/5 config=%s, 4/5 GO=%s, 5/5 soft reset=%s; "
                  "upload-path suppression cleared (0x000D=%u 0x0008=%u)",
             steps_ok ? "yes"
                      : "no - see the FAILED line(s) above this one for the step(s) that "
                        "reported it (all five were still issued)",
             (st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED",
             (cfg_st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED",
             (go_st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED",
             (rst_st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED",
             (unsigned)(zl38063_vproc_release_cmd_host_load_complete_is_suppressed() ? 1u
                                                                                     : 0u),
             (unsigned)(zl38063_vproc_firmware_start_is_suppressed() ? 1u : 0u));

    /* +500 ms, read-only: the first state the released device is judged in on
     * this path.  Four HBI reads, nothing else. */
    vTaskDelay(pdMS_TO_TICKS(B220_ADF_TX_STATUS_WAIT_MS));

    unsigned short r30 = 0;
    unsigned short r32 = 0;
    unsigned short r34 = 0;
    unsigned short r06 = 0;
    const bool ok30 = VprocTwolfHbiRead(0x0030u, 1, &r30) == VPROC_STATUS_SUCCESS;
    const bool ok32 = VprocTwolfHbiRead(0x0032u, 1, &r32) == VPROC_STATUS_SUCCESS;
    const bool ok34 = VprocTwolfHbiRead(0x0034u, 1, &r34) == VPROC_STATUS_SUCCESS;
    const bool ok06 = VprocTwolfHbiRead(0x0006u, 1, &r06) == VPROC_STATUS_SUCCESS;

    ESP_LOGI(TAG, "adf-tx: ~%u ms after the release (read-only, no device write): "
                  "reg 0x0030 (app status)=0x%04X reg 0x0032 (host cmd)=0x%04X "
                  "reg 0x0034 (cmd result)=0x%04X reg 0x0006 (host flags)=0x%04X "
                  "(read ok: 0x0030=%u 0x0032=%u 0x0034=%u 0x0006=%u) -> masked app "
                  "status=0x%02X",
             (unsigned)B220_ADF_TX_STATUS_WAIT_MS, (unsigned)r30, (unsigned)r32,
             (unsigned)r34, (unsigned)r06, (unsigned)(ok30 ? 1u : 0u),
             (unsigned)(ok32 ? 1u : 0u), (unsigned)(ok34 ? 1u : 0u),
             (unsigned)(ok06 ? 1u : 0u), (unsigned)(r30 & 0x0Fu));

    ESP_LOGI(TAG, "adf-tx: done - the audio loopback starts next (board constructor, "
                  "right after this call); no further device access happens here");
    return ESP_OK;
}

#endif /* B220_DSP_ADF_RELEASE_TX */

/*----------------------------------------------------------------------------
 * Hook-release matrix (B220_DSP_RELEASE_MATRIX)
 *
 * The upload is proven correct: the pre-GO hook reads its 9 sampled records back
 * identical before any release command is sent.  What is NOT known is which of
 * the two host commands actually releases the boot ROM, because the early RE
 * marked the vendor's VprocTwolfFirmwareStart (0x40126210) as a delay stub
 * (ets_delay_us) - i.e. the stock firmware may never write reg 0x32 <- 0x0008 and
 * the GO may be implied by the 0x000D "host load complete" alone, while this port
 * always writes both.
 *
 * Variants 5..10 (run first) follow the in-tree reference for this same chip,
 * esp-adf's zl38063_codec_init() -> tw_upload_dsp_firmware() (the full esp-adf copy
 * at ref\zl38063\, mirrored into zl38063/adf_ref/): check app status -> HBI init ->
 * host-boot the image -> GO (VprocTwolfFirmwareStart(), reg 0x32 <- 0x0008) ->
 * VprocTwolfLoadConfig() (the config record) -> VprocTwolfReset(VPROC_RST_SOFTWARE),
 * which writes reg 0x0006 <- 0x0002 once and waits 200 ms.  The main new ingredient
 * is that reset; 7 and 8 isolate it in both orderings, and 9 / 10 add the two other
 * reference steps our port does not have (the reset resent on a stable I2S master,
 * tw_hal_verify.c:126-134, and the host flash-discovery command ADF sends before
 * the GO, vprocTwolf_access.c:950-956 via tw_spi_access.c:90).
 *
 * One variant = one FULL fresh attempt + one release sequence:
 *   a. zl38063_dsp_boot_attempt(): the complete production boot - GPIO19 reset
 *      pulse (10 ms low, 500 ms settle), HBI attach, VprocTwolfHbiInit()
 *      (0xFD80), reg 0x14 <- 1 + 300 ms + the reg 0x34 == 0xD3D3 gate, the 3699
 *      record transfer with the pre-GO verify hook inside it, reg 0x12C <-
 *      execAddr - with the upload path's OWN two release writes suppressed (see
 *      below), so the variant owns them;
 *   b. b220_release_issue(): the variant's step list in the variant's order -
 *      reg 0x32 <- 0x000D (2, 4, 5, 6, 7, 8, 9, 10), the 260-entry config record
 *      (5, 6, 8, 9, 10), reg 0x32 <- 0x0008 (3, 4, 5, 6, 7, 8, 9, 10), reg 0x32
 *      <- 0x000B (10) and, for 6..10, VprocTwolfReset(VPROC_RST_SOFTWARE) =
 *      reg 0x0006 <- 0x0002 (sent twice, 500 ms apart, in 9) - then a 1000 ms
 *      settle;
 *   c. reg 0x0030 / 0x0032 / 0x0034 / 0x0006, then the same 9 sampled image
 *      records the pre-GO hook reads, through the calibrated page-255 encoding.
 *
 * How each of the two release writes is emitted or suppressed, and why nothing
 * else moves:
 *   - the reg 0x32 <- 0x000D that VprocTwolfHbiBoot_alt() writes after the record
 *     transfer is skipped AT ITS CALL SITE by
 *     zl38063_vproc_release_cmd_host_load_complete_from_host_boot(true)
 *     (zl38063_vproc.c).  That call site additionally skips the
 *     VprocTwolfCheckCmdResult() which verifies that command - with no command
 *     sent, reg 0x0034 still holds the boot ROM's 0xD3D3 magic and the check
 *     would correctly fail;
 *   - the reg 0x32 <- 0x0008 that zl38063_dsp_upload_firmware() writes through
 *     VprocTwolfFirmwareStart() is skipped at its call site by
 *     zl38063_vproc_firmware_start_from_host_boot(true) in the uploaded path
 *     below.  VprocTwolfFirmwareStart() itself is untouched, so FirmwareStop and
 *     every other caller keep working exactly as before;
 *   - the variant's own writes go through VprocTwolfHostCmdWr(), which is
 *     VprocTwolfcmdRegWr() - the loader's own framing (mailbox acquire, reg 0x32
 *     <- cmd, reg 0x0006 <- 1, poll reg 0x32 back to 0) - so a write the matrix
 *     emits is byte-identical to the one the default path would have emitted;
 *     the config record goes through VprocTwolfLoadConfig() and the soft reset
 *     through VprocTwolfReset(VPROC_RST_SOFTWARE), i.e. the same ported entry
 *     points the production path uses, never a raw write;
 *   - nothing else in the sequence is gated.  With no variant set (the default
 *     build, and every code path outside this matrix) both setters leave the mask
 *     at 0 and the upload writes exactly the bytes it always wrote.
 *--------------------------------------------------------------------------*/
#if B220_DSP_RELEASE_MATRIX

#if !B220_DSP_PAGE255_CALIB
/* The read-back below compares the records through the page-255 window with the
 * CALIBRATED encoding (the read side of that window is our inference from the
 * write side, so the comparison is only meaningful once the encoding has been
 * measured on hardware).  That encoding and its helpers only exist with the
 * calibration compiled in. */
#error "B220_DSP_RELEASE_MATRIX needs B220_DSP_PAGE255_CALIB 1 (config.h): the post-release record read-back must use the calibrated page-255 encoding"
#endif

/* How long the release is given before it is judged - the same 1 s upstream
 * waits after VprocTwolfFirmwareStart(). */
#define B220_RELEASE_FW_START_WAIT_MS 1000u

/* The one-shot calibration hook the BOARD layer installs.  Weak on purpose: this
 * file must compile and run without a board-side definition (a build that never
 * calls the matrix), and the calibration itself is a board-layer one-shot
 * (s_dsp_page255_calibrated in xiaomodou_b220.cc) which the default path also
 * uses, so it cannot be reached from here by name. */
extern void B220_ReleaseMatrixCalibrate(void) __attribute__((weak));

static bool s_release_matrix_app_running = false;

/* Read one status register for the variant's status line (0x0030 app status,
 * 0x0032 host command, 0x0034 cmd result, 0x0006 host flags).  Returns false when
 * the HBI read failed; the caller logs that as an unreadable register. */
static bool b220_release_read_reg(uint16_t reg, uint16_t *value)
{
    unsigned short v = 0;

    if (VprocTwolfHbiRead((unsigned short)reg, 1, &v) != VPROC_STATUS_SUCCESS) {
        return false;
    }
    *value = (uint16_t)v;
    return true;
}

/* Variants 5, 6 and 8: the 260-entry board config record, written exactly like
 * zl38063_dsp_load_config() writes it (VprocTwolfLoadConfig(), one register per
 * HBI write), i.e. between the 0x000D host-load-complete and the 0x0008 GO for 5
 * and 6, and after the GO for 8 (the isolating ordering). */
static esp_err_t b220_release_load_config(void)
{
    VprocStatusType st = VprocTwolfLoadConfig((dataArr *)zl38063_config_record,
                                              ZL38063_CONFIG_RECORD_COUNT);
    if (st != VPROC_STATUS_SUCCESS) {
        ESP_LOGE(TAG, "release matrix: the config record load failed (%d)",
                 (int)st);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "release matrix: config record loaded (%d registers, "
                  "0x%04X..0x%04X) between 0x000D and the GO",
             ZL38063_CONFIG_RECORD_COUNT, ZL38063_CONFIG_RECORD_FIRST_REG,
             ZL38063_CONFIG_RECORD_LAST_REG);
    return ESP_OK;
}

/* How long the resend variant leaves between the two reg 0x0006 <- 0x0002 writes:
 * long enough for the I2S master (already started before the DSP boot,
 * B220_I2S_BEFORE_DSP 1) to be unambiguously stable, short enough not to matter
 * next to a full upload. */
#define B220_RELEASE_RESET_RESEND_MS 500u

/* The ported VprocTwolfReset(VPROC_RST_SOFTWARE) (zl38063_vproc.c, a straight
 * port of api_lib/vprocTwolf_access.c:866-872 in the full esp-adf copy at
 * ref\zl38063\, and of zl38063/adf_ref/api_lib/vprocTwolf_access.c:866-872), which
 * is the "firmware reset - in order for the configuration to take effect" step of
 * ADF's upload:
 *
 *     } else if (mode == VPROC_RST_SOFTWARE) {   buf = 0x02;
 *         status = VprocTwolfHbiWrite(0x006, 1, &buf);   ... Vproc_msDelay(200);
 *
 * i.e. exactly one device write, reg 0x0006 <- 0x0002, then a 200 ms wait - and
 * failure detection (the status is 0 only when the HBI write itself succeeded).
 * This wrapper is used instead of a raw VprocTwolfHbiWrite(0x006, ...) so the new
 * ingredient is the ported function, not a re-spelling of it; it adds no device
 * access of its own.
 *
 * @again: emit the same reset a second time, @gap_ms later.  Not an invention:
 * esp-adf's example_apps/tw_hal_verify.c:126-134 documents the failure mode and
 * the remedy for this exact step - "if the ZL38040 is an I2S slave, if the I2S
 * master is not stable at the time of this reset, then that reset will not take
 * effect.  In that case the host has to simply resend the reset command once the
 * I2S master is up and running and is at a stable state." */
static esp_err_t b220_release_soft_reset(const char *tag, bool again,
                                         unsigned int gap_ms)
{
    VprocStatusType st = VprocTwolfReset(VPROC_RST_SOFTWARE);

    ESP_LOGI(TAG, "%s: VprocTwolfReset(VPROC_RST_SOFTWARE) -> %s (its only "
                  "device write is reg 0x0006 <- 0x0002, followed by its own "
                  "200 ms wait)",
             tag, (st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED");
    if (st != VPROC_STATUS_SUCCESS) {
        return ESP_FAIL;
    }
    if (again) {
        /* The remedy the reference names for "the reset did not take effect
         * because the I2S master was not stable yet": wait, then resend. */
        vTaskDelay(pdMS_TO_TICKS(gap_ms));
        st = VprocTwolfReset(VPROC_RST_SOFTWARE);
        ESP_LOGI(TAG, "%s: reg 0x0006 <- 0x0002 RESENT after %u ms (the I2S "
                      "master is up by now - tw_hal_verify.c:126-134) -> %s",
                 tag, gap_ms, (st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED");
        if (st != VPROC_STATUS_SUCCESS) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* One full fresh attempt: the complete production boot (zl38063_dsp_begin()) with
 * both of the upload path's own release writes suppressed at their call sites, so
 * whatever the variant wants to send afterwards is the complete set of release
 * writes that reached the device.  zl38063_dsp_begin() itself builds the twFirmware
 * exactly as before (linked image -> dsp_prgm_base() / dsp_exec_addr()) and its
 * upload path skips the reg 0x32 <- 0x0008 at its call site while the suppression
 * is set, which is why nothing here has to re-implement the transfer. */
static esp_err_t b220_release_attempt_upload(unsigned int variant)
{
    zl38063_vproc_release_cmd_host_load_complete_from_host_boot(true);
    zl38063_vproc_firmware_start_from_host_boot(true);

    ESP_LOGI(TAG, "release matrix: variant %u: fresh attempt = full reset + HBI "
                  "attach + HBI config + complete upload (see the boot lines that "
                  "follow); the upload path's own release writes are suppressed "
                  "before it runs: 0x000D=%u 0x0008=%u",
             variant,
             (unsigned)(zl38063_vproc_release_cmd_host_load_complete_is_suppressed()
                            ? 1u : 0u),
             (unsigned)(zl38063_vproc_firmware_start_is_suppressed() ? 1u : 0u));

    esp_err_t err = zl38063_dsp_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "release matrix: variant %u: the fresh attempt failed (%s) - "
                      "the DSP did not enter boot ROM or an HBI access failed; no "
                      "release sequence issued",
                 variant, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "release matrix: variant %u: fresh attempt finished (reset pulse, "
                  "HBI attach, HBI config, upload, reg 0x12C set, no release write "
                  "yet)", variant);
    return ESP_OK;
}

/* The release steps a variant can be built from.  A variant is an ordered list of
 * these (b220_release_issue() executes the list in the order it is written down),
 * which is what makes the step ORDER - not just the step set - expressible:
 *   - R_HLC   reg 0x32 <- 0x000D, "Host Application Load Complete"
 *             (HOST_CMD_HOST_LOAD_CMP, vprocTwolf_access.c:54);
 *   - R_CFG   the 260-entry config record (VprocTwolfLoadConfig());
 *   - R_GO    reg 0x32 <- 0x0008, "GO" (VprocTwolfFirmwareStart());
 *   - R_FLASH reg 0x32 <- 0x000B, HOST_CMD_HOST_FLASH_INIT, "Host Application
 *             flash discovery" (vprocTwolf_access.c:55) - the command ADF sends
 *             BEFORE the GO inside VprocTwolfSaveImgToFlash() (vprocTwolf_access.c
 *             :944-981, called at tw_spi_access.c:90, i.e. between the record
 *             upload and VprocTwolfFirmwareStart());
 *   - R_RST   VprocTwolfReset(VPROC_RST_SOFTWARE) = reg 0x0006 <- 0x0002;
 *   - R_RST2  the same reset, resent after B220_RELEASE_RESET_RESEND_MS.
 * dsp_step_name[] is the log text for each and is kept in the same order. */
enum {
    R_HLC = 0,
    R_CFG,
    R_GO,
    R_FLASH,
    R_RST,
    R_RST2,
    R_STEP_COUNT
};
/* A variant's step list is spelled out literally in the table below; these two
 * macros only keep those rows short and readable. */
#define R_STEPS(...) { __VA_ARGS__ }

static const char *const dsp_step_name[R_STEP_COUNT] = {
    "reg 0x32 <- 0x000D",
    "config record (260 registers)",
    "reg 0x32 <- 0x0008 (GO)",
    "reg 0x32 <- 0x000B (host flash init, save-image-before-GO)",
    "reg 0x0006 <- 0x0002 (soft reset)",
    "reg 0x0006 <- 0x0002 (soft reset RESENT)",
};

/* The variant's release sequence: reg 0x12C is already in the device (the upload
 * wrote it), and @steps[] are executed in the order the variant lists them,
 * through the loader's own framing (VprocTwolfHostCmdWr() = VprocTwolfcmdRegWr())
 * and the ported config / reset entry points - never a raw register write. */
static void b220_release_issue(unsigned int variant, const unsigned char *steps,
                               unsigned int n_steps)
{
    char seq[192];
    size_t off = 0;

    /* One description line that mirrors, step for step, what is emitted below. */
    seq[0] = '\0';
    for (unsigned int i = 0; i < n_steps; i++) {
        const char *clause = dsp_step_name[steps[i] % R_STEP_COUNT];
        int written = snprintf(&seq[off], sizeof(seq) - off, "%s%s",
                               (off == 0u) ? "" : ", ", clause);
        if ((written < 0) || ((size_t)written >= (sizeof(seq) - off))) {
            break;
        }
        off += (size_t)written;
    }

    ESP_LOGI(TAG, "release matrix: variant %u: release sequence = reg 0x12C "
                  "(already written by the upload), %s",
             variant, (seq[0] != '\0') ? seq : "(nothing further)");

    for (unsigned int i = 0; i < n_steps; i++) {
        VprocStatusType st;

        switch (steps[i]) {
        case R_HLC:
            st = VprocTwolfHostCmdWr(0x000D);
            ESP_LOGI(TAG, "release matrix: variant %u: reg 0x32 <- 0x000D -> %s",
                     variant, (st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED");
            break;
        case R_CFG:
            (void)b220_release_load_config();
            break;
        case R_GO:
            st = VprocTwolfHostCmdWr(0x0008);
            ESP_LOGI(TAG, "release matrix: variant %u: reg 0x32 <- 0x0008 (GO) -> "
                          "%s", variant,
                     (st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED");
            break;
        case R_FLASH:
            /* The one command of ADF's VprocTwolfSaveImgToFlash() that precedes
             * the GO (vprocTwolf_access.c:950-956).  The rest of that function is
             * the 0xAA55 param write, the 0x0009 "delete applications" command and
             * the 0x0004 IMG_CFG_SAVE command that actually programs the device's
             * own slave flash; on this board that flash is not ours to program and
             * the stock firmware never saves an image, so only the "is there a
             * flash?" discovery command is emitted here.  It is still a change
             * from the default path (0x000D -> GO directly), which is the point. */
            st = VprocTwolfHostCmdWr(0x000B);
            ESP_LOGI(TAG, "release matrix: variant %u: reg 0x32 <- 0x000B (host "
                          "flash init, as ADF sends before the GO) -> %s", variant,
                     (st == VPROC_STATUS_SUCCESS) ? "ok" : "FAILED");
            break;
        case R_RST:
            (void)b220_release_soft_reset("release matrix", false, 0u);
            break;
        case R_RST2:
            (void)b220_release_soft_reset("release matrix", true,
                                          B220_RELEASE_RESET_RESEND_MS);
            break;
        default:
            ESP_LOGE(TAG, "release matrix: variant %u: unknown release step %u",
                     variant, (unsigned)steps[i]);
            break;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(B220_RELEASE_FW_START_WAIT_MS));
}

esp_err_t zl38063_dsp_release_matrix(void)
{
    /*
     * Variants 5..10 are the REFERENCE-shaped ones and run first: 1..4 are already
     * known failures on this hardware (each of 0x000D alone, 0x0008 alone and both
     * together leaves the sampled image RAM matching but reg 0x0030 & 0x0F at
     * 0x00), so they only cost a full upload each while the reference order and
     * its extra steps - the 260-register config record written BEFORE the GO, the
     * host flash-discovery command ADF sends before the GO, and
     * VprocTwolfReset(VPROC_RST_SOFTWARE) (reg 0x0006 <- 0x0002) - are the
     * untested ones.  The matrix stops at the first non-zero masked app status, so
     * a working variant 5..10 also keeps the boot short; 1..4 still run afterwards
     * in their original order (nothing is dropped) when none of 5..10 comes up.
     *
     * Each variant's steps run in the order written here; the source of each is
     * named in the entry and in dsp_step_name[] above.
     */
    static const struct {
        unsigned int variant;
        unsigned char steps[6];
        unsigned int n_steps;
        const char  *what;
    } variants[] = {
        /* esp-adf order, tw_spi_access.c: 0x000D -> config -> GO (-> soft reset). */
        {5u, R_STEPS(R_HLC, R_CFG, R_GO), 3u,
         "reg 0x12C + reg 0x000D + config record + reg 0x0008 (config before the GO)"},
        {6u, R_STEPS(R_HLC, R_CFG, R_GO, R_RST), 4u,
         "reg 0x12C + reg 0x000D + config record + reg 0x0008 + reg 0x0006 <- 0x0002 "
         "(ADF order, tw_spi_access.c:111,130)"},
        {7u, R_STEPS(R_HLC, R_GO, R_RST), 3u,
         "reg 0x12C + reg 0x000D + reg 0x0008 + reg 0x0006 <- 0x0002 (no config load)"},
        {8u, R_STEPS(R_HLC, R_CFG, R_GO, R_RST), 4u,
         "reg 0x12C + reg 0x000D + config record + reg 0x0008 + reg 0x0006 <- 0x0002 "
         "(config AFTER the GO - isolates the config/GO order)"},
        /* The two extra reference steps (see the report): the reset resent once the
         * I2S master is stable (tw_hal_verify.c:126-134) and the host flash
         * discovery command ADF sends before the GO (vprocTwolf_access.c:950-956,
         * called at tw_spi_access.c:90). */
        {9u, R_STEPS(R_HLC, R_CFG, R_GO, R_RST2), 4u,
         "reg 0x12C + reg 0x000D + config record + reg 0x0008 + reg 0x0006 <- 0x0002 "
         "twice, 500 ms apart (reset resent on a stable I2S master)"},
        {10u, R_STEPS(R_HLC, R_FLASH, R_CFG, R_GO, R_RST), 5u,
         "reg 0x12C + reg 0x000D + reg 0x000B (host flash init) + config record + "
         "reg 0x0008 + reg 0x0006 <- 0x0002 (ADF save-image-before-GO shape)"},
        {1u, R_STEPS(), 0u, "reg 0x12C only"},
        {2u, R_STEPS(R_HLC), 1u,
         "reg 0x12C + reg 0x32 <- 0x000D (no 0x0008 anywhere)"},
        {3u, R_STEPS(R_GO), 1u,
         "reg 0x12C + reg 0x32 <- 0x0008 (no 0x000D)"},
        {4u, R_STEPS(R_HLC, R_GO), 2u,
         "reg 0x12C + reg 0x000D + reg 0x0008 (today's behaviour, control)"},
    };
    const unsigned int n_variants =
        (unsigned int)(sizeof(variants) / sizeof(variants[0]));
    const unsigned int rec_cnt = (unsigned int)firmwareStreamLen;
    unsigned int sample_idx[11];
    unsigned int n_sample = 0;

    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "release_matrix called before zl38063_dsp_begin() - there is "
                      "no HBI device on SPI2 to boot through");
        return ESP_ERR_INVALID_STATE;
    }

    if (zl38063_dsp_is_running()) {
        ESP_LOGI(TAG, "release matrix: skipped, an application is already running");
        s_release_matrix_app_running = true;
        return ESP_OK;
    }

    if (rec_cnt == 0u) {
        ESP_LOGE(TAG, "release matrix: no ZL38063 firmware image linked in "
                      "(firmwareStreamLen == 0), nothing to upload");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "===== ZL38063 hook-release matrix (B220_DSP_RELEASE_MATRIX): %u "
                  "variant(s), one FULL reset + host-boot upload each =====",
             n_variants);
    ESP_LOGW(TAG, "matrix (order 5,6,7,8,9,10,1,2,3,4 - the reference-shaped "
                  "variants first): 5: 0x12C+0x000D+config+0x0008 (ADF, config "
                  "first) | 6: =5+0x0006<-0x0002 (ADF complete) | 7: "
                  "0x12C+0x000D+0x0008+0x0006<-0x0002 (no config) | 8: "
                  "=6 with the config AFTER the GO | 9: =6 with the soft reset "
                  "resent 500 ms later | 10: =6 with reg 0x000B (host flash init) "
                  "before the GO | 1: 0x12C | 2: 0x12C+0x000D | 3: 0x12C+0x0008 | "
                  "4: 0x12C+0x000D+0x0008 (control); stops at the first variant "
                  "whose masked app status (reg 0x0030 & 0x0F) is non-zero");
    ESP_LOGW(TAG, "matrix: reg 0x0006 <- 0x0002 is emitted through the ported "
                  "VprocTwolfReset(VPROC_RST_SOFTWARE) (its only device write), not "
                  "by a raw HBI write");
    ESP_LOGW(TAG, "matrix: read-back = the same 9 sampled image records as the "
                  "pre-GO verify (of %u records), through the page-255 window with "
                  "the calibrated encoding, taken ~%u ms after each release",
             rec_cnt, (unsigned)B220_RELEASE_FW_START_WAIT_MS);

    /* Calibrate the page-255 read encoding once, before the variants: the
     * calibration writes a known 16-word pattern to scratch DSP RAM and reads it
     * back under every plausible framing, so the record comparison below is a
     * measurement and not an assumption.  The board layer owns the one-shot (it
     * also serves the default verify path), hence the weak hook. */
    if (B220_ReleaseMatrixCalibrate != NULL) {
        ESP_LOGI(TAG, "release matrix: running the page-255 calibration once, before "
                      "the variants, so every read-back below uses the measured "
                      "encoding");
        B220_ReleaseMatrixCalibrate();
    } else {
        ESP_LOGW(TAG, "release matrix: no calibration hook linked in; the read-back "
                      "uses the current page-255 encoding (calibrated=%u), which may "
                      "make the record comparison meaningless",
                 (unsigned)(zl38063_dsp_page255_calibrated() ? 1u : 0u));
    }

    /* Full-image sample, built by the ONE sample-set helper the pre-GO hook uses
     * too (zl38063_dsp_sample_index_set()): records 0 and 1, then the 1/9th steps
     * and the last record - i.e. "9 records spanning the whole image", the same
     * records whose pre-GO read-back the hook just logged. */
    n_sample = zl38063_dsp_sample_index_set(rec_cnt, 9u, sample_idx,
                                            sizeof(sample_idx) /
                                                sizeof(sample_idx[0]));

    {
        char idx_txt[128];
        char tgt_txt[160];
        b220_sample_indices_text(sample_idx, n_sample, rec_cnt, idx_txt,
                                 sizeof(idx_txt));
        b220_sample_targets_text(sample_idx, n_sample, tgt_txt, sizeof(tgt_txt));
        ESP_LOGI(TAG, "release matrix: sampled records (%u) = %s(targets: %s)",
                 n_sample, idx_txt, tgt_txt);
    }

    for (unsigned int v = 0; v < n_variants; v++) {
        const unsigned int variant = variants[v].variant;

        ESP_LOGW(TAG, "----- release variant %u/%u: %s -----", variant, n_variants,
                 variants[v].what);

        esp_err_t err = b220_release_attempt_upload(variant);
        if (err != ESP_OK) {
            zl38063_vproc_release_cmd_host_load_complete_from_host_boot(false);
            zl38063_vproc_firmware_start_from_host_boot(false);
            ESP_LOGW(TAG, "release matrix: VERDICT variant %u (%s): SKIPPED - the "
                          "fresh attempt's upload failed", variant, variants[v].what);
            continue;
        }

        /* The variant's release sequence, in the order the variant lists it. */
        b220_release_issue(variant, variants[v].steps, variants[v].n_steps);

        /* What the device says now: the four registers, then the records. */
        uint16_t r30 = 0;
        const bool ok30 = b220_release_read_reg(0x0030u, &r30);
        const unsigned int masked = ok30 ? ((unsigned int)r30 & 0x0Fu) : 0xFFu;
        {
            uint16_t r32 = 0, r34 = 0, r06 = 0;
            const bool ok32 = b220_release_read_reg(0x0032u, &r32);
            const bool ok34 = b220_release_read_reg(0x0034u, &r34);
            const bool ok06 = b220_release_read_reg(0x0006u, &r06);

            ESP_LOGI(TAG, "release matrix: variant %u: ~%u ms after the release: "
                          "reg 0x0030 (app status)=0x%04X reg 0x0032 (host "
                          "command)=0x%04X reg 0x0034 (cmd result)=0x%04X reg 0x0006 "
                          "(host flags)=0x%04X (read ok: 0x0030=%u 0x0032=%u "
                          "0x0034=%u 0x0006=%u) -> masked app status=0x%02X",
                     variant, (unsigned)B220_RELEASE_FW_START_WAIT_MS,
                     (unsigned)r30, (unsigned)r32, (unsigned)r34, (unsigned)r06,
                     (unsigned)(ok30 ? 1u : 0u), (unsigned)(ok32 ? 1u : 0u),
                     (unsigned)(ok34 ? 1u : 0u), (unsigned)(ok06 ? 1u : 0u),
                     (masked == 0xFFu) ? 0xFFu : masked);
        }

        {
            struct zl38063_sample_stats stats;
            char tag[32];
            (void)snprintf(tag, sizeof(tag), "post-release-v%u", variant);
            esp_err_t rerr = zl38063_dsp_sample_records_readback(tag, sample_idx,
                                                                 n_sample, &stats);
            const bool ram_matches = (rerr == ESP_OK) &&
                                     (stats.mismatching == 0ul) &&
                                     (stats.read_errors == 0u) &&
                                     (stats.records == n_sample);
            const bool running = (masked != 0xFFu) &&
                                 (masked !=
                                  (unsigned int)ZL38063_APP_STATUS_NOT_RUNNING);

            ESP_LOGW(TAG, "release matrix: VERDICT variant %u (%s): app status "
                          "0x0030 & 0x0F = 0x%02X (%s), sampled RAM read-back %s "
                          "(mismatching_words=%lu read_errors=%u over %u record(s))",
                     variant, variants[v].what, (masked == 0xFFu) ? 0xFFu : masked,
                     running ? "RUNNING" : "not running",
                     (rerr != ESP_OK)
                         ? "could not be compared"
                         : (ram_matches ? "STILL MATCHES" : "does NOT match"),
                     (rerr == ESP_OK) ? stats.mismatching : 0ul,
                     (rerr == ESP_OK) ? stats.read_errors : 0u, n_sample);

            /* The suppression only ever affects the upload path; clear it before
             * anything else runs so the rest of the boot is byte-for-byte the
             * default path (both queries must read 0 afterwards). */
            zl38063_vproc_release_cmd_host_load_complete_from_host_boot(false);
            zl38063_vproc_firmware_start_from_host_boot(false);

            if (running) {
                s_release_matrix_app_running = true;
                ESP_LOGW(TAG, "release matrix: STOPPING at variant %u - it produced "
                              "a non-zero masked app status (0x%02X)%s", variant,
                         masked,
                         ram_matches ? " and the sampled image records still match"
                                     : " (but the sampled image records no longer "
                                       "all match)");
                return ESP_OK;
            }
            if (zl38063_vproc_release_cmd_host_load_complete_is_suppressed() ||
                zl38063_vproc_firmware_start_is_suppressed()) {
                ESP_LOGE(TAG, "release matrix: internal error - the release-write "
                              "suppression did not clear after variant %u", variant);
            }
        }
    }

    ESP_LOGE(TAG, "release matrix: no variant produced a non-zero app status: the "
                  "boot ROM is released (reg 0x0034 -> 0x0000) but neither 0x000D "
                  "alone, nor 0x0008 alone, nor both together - with or without the "
                  "260-entry config record before/after the GO, and with or without "
                  "VprocTwolfReset(VPROC_RST_SOFTWARE) (reg 0x0006 <- 0x0002) as the "
                  "last release write - brings the application up");
    return ESP_FAIL;
}

bool zl38063_dsp_release_matrix_app_running(void)
{
    return s_release_matrix_app_running;
}

#endif /* B220_DSP_RELEASE_MATRIX */

/*----------------------------------------------------------------------------
 * prgmBase / execAddr boot-attempt matrix (B220_DSP_ADDR_MATRIX)
 *
 * The host upload now completes (reg 0x34 == 0xD3D3 gate passes, 3699 records
 * transferred, reg 0x12C <- execAddr, reg 0x32 <- 0x000D, FirmwareStart) yet the
 * DSP still answers reg 0x030 & 0x0F with 0x0000, i.e. "no application
 * running".  The remaining unknown is which of the two recovered values is
 * really the RAM entry point: PROTOCOL-REPORT.md says prgmBase = 0x00083040 /
 * execAddr = 0x00080200, while zl38063_firmware_image.md (and the log strings,
 * and today's build) says prgmBase = 0x00080200 / execAddr = 0x00083040.
 *
 * This matrix settles it on hardware: one full boot per pairing, in order
 * "today's values" then "the swap", stopping at the first pairing whose
 * application reports running.  Each round is exactly the production sequence -
 * zl38063_dsp_begin() does the reset pulse, the HBI attach, the HBI
 * configuration and the complete upload - so nothing here is a special path.
 *
 * It is deliberately NOT called from zl38063_dsp_begin(): the board calls it
 * once, after the normal attempt.  Calling it from inside begin() would recurse.
 *--------------------------------------------------------------------------*/
#if B220_DSP_ADDR_MATRIX

esp_err_t zl38063_dsp_boot_addr_matrix(void)
{
    if (!s_hbi_ready) {
        ESP_LOGE(TAG, "boot_addr_matrix called before zl38063_dsp_begin() - "
                      "there is no HBI device on SPI2 to boot through");
        return ESP_ERR_INVALID_STATE;
    }

    /* Never disturb a running application: the caller is expected to have
     * checked, but the check belongs here too. */
    if (zl38063_dsp_is_running()) {
        ESP_LOGI(TAG, "boot-address matrix: skipped, an application is already "
                      "running");
        return ESP_OK;
    }

    /*
     * Pairing A is whatever the boot path uses today (the linked image's own
     * values: prgmBase 0x00080200, execAddr 0x00083040), pairing B is the same
     * two numbers with their roles swapped.  Built from the live values rather
     * than hard-coded, so A always means "what this build would have used".
     */
    const uint32_t base_a = dsp_prgm_base();
    const uint32_t exec_a = dsp_exec_addr();
    const struct {
        uint32_t prgm_base;
        uint32_t exec_addr;
        const char *what;
    } pairs[] = {
        {base_a, exec_a, "today's values"},
        {exec_a, base_a, "the swap"},
    };
    const int n_pairs = (int)(sizeof(pairs) / sizeof(pairs[0]));

    ESP_LOGW(TAG, "===== ZL38063 boot-address matrix (B220_DSP_ADDR_MATRIX): "
                  "%d pairings, one full reset + host boot each =====", n_pairs);
    ESP_LOGW(TAG, "matrix pairing A = prgmBase 0x%08lX / execAddr 0x%08lX "
                  "(today's values)", (unsigned long)base_a,
             (unsigned long)exec_a);
    ESP_LOGW(TAG, "matrix pairing B = prgmBase 0x%08lX / execAddr 0x%08lX "
                  "(the swap)", (unsigned long)exec_a, (unsigned long)base_a);

    for (int i = 0; i < n_pairs; i++) {
        ESP_LOGW(TAG, "matrix attempt %d/%d: %s - prgmBase 0x%08lX execAddr "
                      "0x%08lX", i + 1, n_pairs, pairs[i].what,
                 (unsigned long)pairs[i].prgm_base,
                 (unsigned long)pairs[i].exec_addr);

        /* Select the pairing, then run the production reset + host boot.  The
         * image is transferred in full on every attempt (~4 s). */
        zl38063_dsp_set_boot_addresses(pairs[i].prgm_base, pairs[i].exec_addr);
        esp_err_t err = zl38063_dsp_begin();
        ESP_LOGI(TAG, "matrix attempt %d/%d: reset + host boot -> %s", i + 1,
                 n_pairs, esp_err_to_name(err));

        /* Let the freshly started image settle before judging it. */
        vTaskDelay(pdMS_TO_TICKS(ZL38063_MATRIX_SETTLE_MS));

#if B220_ZL_DIAG
        /* Every status/boot register, raw.  Read-only. */
        zl38063_dsp_status_dump();
#endif
        uint16 status = 0;
        int app_status = read_app_status(&status);
        if (app_status < 0) {
            ESP_LOGW(TAG, "matrix attempt %d/%d: application status read "
                          "failed", i + 1, n_pairs);
            continue;
        }

        ESP_LOGI(TAG, "matrix attempt %d/%d: app status 0x%04X (masked 0x%02X) "
                      "-> %s", i + 1, n_pairs, (unsigned)status,
                 (unsigned)(app_status & 0x0F),
                 ((app_status & 0x0F) != ZL38063_APP_STATUS_NOT_RUNNING)
                 ? "RUNNING" : "not running");

        if ((app_status & 0x0F) != ZL38063_APP_STATUS_NOT_RUNNING) {
            ESP_LOGW(TAG, "matrix: the application is RUNNING with prgmBase "
                          "0x%08lX / execAddr 0x%08lX (%s) - keeping this "
                          "pairing", (unsigned long)pairs[i].prgm_base,
                     (unsigned long)pairs[i].exec_addr, pairs[i].what);
            ESP_LOGW(TAG, "===== end ZL38063 boot-address matrix (success) =====");
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "matrix: neither pairing produced a running application; the "
                  "boot addresses stay at the last attempt (prgmBase 0x%08lX "
                  "execAddr 0x%08lX) and the entry point is probably not the "
                  "problem", (unsigned long)dsp_prgm_base(),
             (unsigned long)dsp_exec_addr());
    ESP_LOGE(TAG, "===== end ZL38063 boot-address matrix (no pairing worked) =====");
    return ESP_FAIL;
}

#endif /* B220_DSP_ADDR_MATRIX */

/*----------------------------------------------------------------------------
 * Downlink bring-up experiment (B220_DSP_DOWNLINK_TRIES, config.h)
 *
 * WHY
 * ---
 * The ZL38063 application is running and the UPLINK works - the ESP32 reads real
 * microphone audio from I2S DIN (GPIO35).  The DOWNLINK does not: a full-scale
 * 1 kHz sine written to I2S TX (GPIO26) reaches the DSP's I2S_SDIA continuously
 * (scope: square wave on GPIO26) and yet neither the DSP's DAC outputs nor the
 * speaker terminals show any waveform.  The DSP therefore never turns its I2S
 * input into DAC output - the downlink path inside the DSP is not configured.
 *
 * WHAT THE RECORD DIG FOUND (the reason try 2/3 exist)
 * ---------------------------------------------------
 * The stock image stores its configuration record at wifi.bin file offset
 * 0x059EA8 as
 *
 *     u16  length  = 0x0412 = 1042        <- an ELEMENT count
 *     dataArr[1042] = 1042 x {u16 reg; u16 value}, regs 0x0200..0x0A22, step +2
 *
 * and the stock host loader VprocTwolfLoadConfig() (VA 0x40125c30) walks it with
 *
 *     40125c33: extui a3, a3, 0, 16      ; a3 = numElements = the length field
 *     40125c36: addx4 a3, a3, a2         ; a3 = data + 4 * numElements
 *     40125c56: bne   a2, a3, 40125c3c   ; one 4-byte step per iteration
 *
 * addx4 is `a3 = a2 + (a3 << 2)`, so the loop runs 1042 times: the stock writes
 * the WHOLE 1042-pair record.  Independent confirmation: the esp-adf object
 * zl38063_config.o (ref/analysis/zl38063_adf/libfirmware.a) carries
 * configStreamLen = 1042 next to a 1042-element st_twConfig, and its array starts
 * at reg 0x0200 and ends at reg 0x0A22 exactly like the stock image's, with 39
 * value differences.
 *
 * Our build until now wrote only the first 260 pairs (zl38063_config_record.h,
 * ZL38063_CONFIG_RECORD_COUNT 260 - the length field had been read as a BYTE
 * count).  Registers 0x0408..0x0A22 - 782 entries, 157 of them non-zero - were
 * therefore NEVER written.  The uplink lives in the first 260 registers; the
 * downlink / DAC / crosspoint configuration is a prime candidate for living in
 * the omitted tail, which is exactly the observed asymmetry (uplink alive,
 * downlink dead).  zl38063/zl38063_config_record_full.h carries all 1042 pairs.
 *
 * WHAT THE STOCK *PLAYBACK PATH* ADDS (try 4, and the reason the tail matters)
 * --------------------------------------------------------------------------
 * The stock's "enable playback" routine is at VA 0x40125358.  Its last device
 * action before it marks playback started is
 *     401253ed: movi.n a10, 1                ; audio_pa_on(1)
 *     401253f6: movi.n a10, 50               ; volume = 50
 *     401253f8: l32i.n a3, a4, 44            ; a4 = 0x3ffbf348 = audio-HAL ops
 *     401253fa: callx8  a3                   ; ops[+44](50)
 * and ops[+44] is 0x401256D8, the ZL38063 volume/route routine (the same table
 * slot order puts the ZL38063 set-mute routine at ops[+60] = 0x40125788, which
 * drives reg 0x0200 <- 0/1).  With volume 50, 0x401256D8 writes, in order:
 *
 *     0x238 <- 0xECEC   0x23A <- 0xECEC      (VprocTwolfSetVolume, tblA[50/10])
 *     0x24E <- 0xF3F3                        (tblB[50/10])
 *     0x494 <- 0x0666   0x49A <- 0x0002      (only when VprocTwolfHbiRead(0x14)
 *                                             == 0x200; volume>=80 selects
 *                                             0x16C3 / 0x0001 instead)
 *     reg 0x0006 <- 0x0002                   (VprocTwolfReset SOFTWARE)
 *
 * Against our 260-element record: 0x238/0x23A and 0x24E ARE covered (indices 28,
 * 29, 39), while **0x494 (index 330) and 0x49A (index 333) are NOT** - they are
 * in the omitted tail, and the values stock writes for them (0x0666 / 0x0002) are
 * exactly the values the stock record carries.  0x494 is also one of the 39
 * stock-vs-esp-adf record differences (esp-adf 0x2026).  That is the mechanistic
 * link between "record truncated to 260" and "downlink dead", and it is what try
 * 4 (the exact stock write list) and try 5 (just those two registers) isolate.
 *
 * HOW IT IS DRIVEN
 * ----------------
 * The audio loopback task (xiaomodou_b220.cc) calls
 * zl38063_dsp_downlink_tries_step() once per ~2 s window, so ONE candidate is
 * applied at a time while the 1 kHz TX tone keeps playing: the DAC output can be
 * watched on a scope and the speaker listened to between and across tries.
 * Every device write is logged as
 *
 *     downlink try N: reg 0x.... <- 0x.... -> ok|FAILED
 *
 * Each try is bracketed by a header line and a summary, and followed by a
 * read-only readback of the registers that say the most (0x0030 app status,
 * 0x0032 host command, 0x0034 command result, 0x0006 host flags, 0x0238/0x023A
 * output gains, and 0x0600 / 0x0A1C - both ABOVE 0x0406, so they show whether the
 * omitted tail ever reached the device).
 *--------------------------------------------------------------------------*/
#if B220_DSP_DOWNLINK_TRIES
#include "zl38063/zl38063_config_record_full.h"
/* The readback line is built with snprintf(); <stdio.h> above is only pulled in
 * for the diagnostics, so this block brings its own. */
#include <stdio.h>
#endif

/* Number of registers the readback line samples. */
#define B220_DL_READBACK_REGS 10

/* 1-based number of the try currently running, and its write counters.  Only
 * touched from the loopback task, so no locking is needed.  Kept inside the
 * switch so the B220_DSP_DOWNLINK_TRIES 0 build has no unused statics. */
#if B220_DSP_DOWNLINK_TRIES
static unsigned s_dl_try = 0u;
static unsigned s_dl_ok = 0u;
static unsigned s_dl_bad = 0u;
#endif

#if B220_DSP_DOWNLINK_TRIES

/* One HBI register write, logged in the exact shape the experiment is read in.
 * Failures are always logged; @verbose only suppresses the success line, which
 * the bulk-record tries need (782 lines would stall the tone in the UART). */
static bool dl_wr_q(unsigned short reg, unsigned short value, bool verbose)
{
    unsigned short buf = value;
    const VprocStatusType st = VprocTwolfHbiWrite(reg, 1, &buf);
    const bool ok = (st == VPROC_STATUS_SUCCESS);
    if (ok) {
        ++s_dl_ok;
    } else {
        ++s_dl_bad;
    }
    if (verbose || !ok) {
        ESP_LOGI(TAG, "downlink try %u: reg 0x%04X <- 0x%04X -> %s", s_dl_try,
                 (unsigned)reg, (unsigned)value, ok ? "ok" : "FAILED");
    }
    return ok;
}

static bool dl_wr(unsigned short reg, unsigned short value)
{
    return dl_wr_q(reg, value, true);
}

/* The registers whose value after a try says the most.  0x0494 / 0x049A and
 * 0x0A1C are deliberately ABOVE 0x0406: they are what the stock's own playback
 * path writes (see dl_stock_playback() below) and what the omitted tail carries,
 * so they read the stock record's values once the tail really reaches the device
 * - and the power-on default until then.  0x0014 is the register the stock
 * volume/route routine gates its route writes on (it proceeds only when 0x0014
 * reads 0x200). */
static const unsigned short s_dl_readback_regs[B220_DL_READBACK_REGS] = {
    0x0030u, 0x0032u, 0x0034u, 0x0006u, 0x0014u,
    0x0238u, 0x024Eu, 0x0494u, 0x049Au, 0x0A1Cu,
};

/* Read-only: never a device write, so it cannot disturb the try it reports on.
 * A register that cannot be read is marked "??" instead of being dropped. */
static void dl_readback(void)
{
    char line[208];
    size_t off = 0u;
    line[0] = '\0';
    for (unsigned i = 0u; i < B220_DL_READBACK_REGS; ++i) {
        unsigned short v = 0u;
        const VprocStatusType st = VprocTwolfHbiRead(s_dl_readback_regs[i], 1, &v);
        const int w = snprintf(&line[off], sizeof(line) - off,
                               (i == 0u) ? "0x%04X=%s0x%04X" : " 0x%04X=%s0x%04X",
                               (unsigned)s_dl_readback_regs[i],
                               (st == VPROC_STATUS_SUCCESS) ? "" : "??", (unsigned)v);
        if ((w < 0) || ((size_t)w >= (sizeof(line) - off))) {
            break;
        }
        off += (size_t)w;
    }
    ESP_LOGI(TAG, "downlink try %u: readback (read-only) %s", s_dl_try, line);
}

/* The software reset every configuration change needs to take effect, through
 * the same single device write VprocTwolfReset(VPROC_RST_SOFTWARE) makes
 * (reg 0x0006 <- 0x0002) plus that call's own 200 ms wait.  Written through
 * dl_wr() so it appears in the log in the same shape as everything else. */
static void dl_soft_reset(void)
{
    if (dl_wr(0x0006u, 0x0002u)) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* A run of record entries: the first few successes are logged individually, all
 * failures always are, and the run ends with its own count so a truncated record
 * is visible in the log even when nothing failed. */
static void dl_write_pairs(unsigned from, unsigned to)
{
    const unsigned ok0 = s_dl_ok;
    const unsigned bad0 = s_dl_bad;
    for (unsigned i = from; i < to; ++i) {
        dl_wr_q(zl38063_config_record_full[i][0], zl38063_config_record_full[i][1],
                (i - from) < 8u);
    }
    ESP_LOGI(TAG, "downlink try %u: record entries [%u..%u) = regs 0x%04X..0x%04X, "
                  "%u register write(s) -> %u ok, %u FAILED",
             s_dl_try, from, to, (unsigned)zl38063_config_record_full[from][0],
             (unsigned)zl38063_config_record_full[to - 1u][0], to - from,
             s_dl_ok - ok0, s_dl_bad - bad0);
}

/* ---- the candidates ------------------------------------------------------ */

/* 1: read-only probe.  Nothing is written, so this is the baseline the readback
 * lines of every later try are compared against. */
static void dl_probe(void)
{
    ESP_LOGI(TAG, "downlink try %u: read-only probe - no device write in this try",
             s_dl_try);
}

/* 2: the 782 entries the 260-element header never wrote. */
static void dl_tail(void)
{
    dl_write_pairs(ZL38063_CONFIG_RECORD_TAIL_FIRST_INDEX,
                   ZL38063_CONFIG_RECORD_FULL_COUNT);
}

/* 3: the whole record, in the stock order. */
static void dl_full(void)
{
    dl_write_pairs(0u, ZL38063_CONFIG_RECORD_FULL_COUNT);
}

/* 4: THE STOCK PLAYBACK-START SEQUENCE, reproduced instruction for instruction.
 *
 * The stock B220 function at VA 0x40125358 is the "enable playback" routine:
 * it ends with
 *     401253ed: movi.n a10, 1
 *     401253f0: l32r   a8, 0x40207ae8        ; audio_pa_on()
 *     401253f3: callx8  a8                   ; audio_pa_on(1)  <- PA on
 *     401253f6: movi.n a10, 50               ; volume = 50
 *     401253f8: l32i.n a3, a4, 44            ; a4 = 0x3ffbf348, the audio-HAL ops
 *     401253fa: callx8  a3                   ; ops[+44](50)
 * i.e. IMMEDIATELY after it enables the PA it calls ops[+44] with volume 50, and
 * ops[+44] is 0x401256D8 - the ZL38063 volume/route routine.  0x401256D8(50):
 *     idx = 50/10 = 5                       (vol < 10 clamps to 0, vol > 99 to 100)
 *     40125710: VprocTwolfSetVolume(tblA[5])       -> 0x238, 0x23A
 *     4012571c: r = VprocTwolfHbiRead(0x0014)      -> the route writes below are
 *              40125748: bne r, 0x200, return          SKIPPED unless 0x14==0x200
 *     40125756: 0x24E <- tblB[5] | (tblB[5]<<8)
 *     40125762/73: 0x494 <- 0x16C3 (idx>=8) else 0x0666
 *     4012577c: 0x49A <- 0x0001 (idx>=8) else 0x0002
 *     40125781: VprocTwolfReset(VPROC_RST_SOFTWARE)  -> 0x006 <- 0x0002
 *
 * The two tables are initialised .data (ELF .dram0.data, verified):
 *     tblA (base 0x3ffbf3d0, index vol/10):
 *         BF C4 DB E1 E7 EC F1 F3 F5 F7 F9   (-65..-7 dB, 1 dB/LSB signed)
 *     tblB (base 0x3ffbf3c5, index vol/10):
 *         C6 CB E2 E8 EE F3 F8 FA FA FA FA
 * so volume 50 (idx 5) is tblA[5] = 0xEC and tblB[5] = 0xF3.
 *
 * This try is therefore the exact ordered write list of the stock playback path.
 * Which of it our 260-element record already covered, at what value:
 *     0x238/0x23A : COVERED (record 0xF1F1) - this writes 0xECEC
 *     0x24E       : COVERED (record 0xFAFA) - this writes 0xF3F3
 *     0x494       : NOT COVERED (record idx 330, in the omitted tail)
 *     0x49A       : NOT COVERED (record idx 333, in the omitted tail)
 *     0x006       : covered by the release's own soft reset (ADF step 5/5)
 * and the values this writes for the two uncovered registers are EXACTLY the ones
 * the stock record carries (0x494 = 0x0666, 0x49A = 0x0002) - which is why
 * loading the full record and this replay are the two halves of the same fix. */
static void dl_stock_playback(void)
{
    dl_wr(0x0238u, 0xECECu);   /* tblA[5] | tblA[5]<<8  (SetVolume, Gain A/B) */
    dl_wr(0x023Au, 0xECECu);
    dl_wr(0x024Eu, 0xF3F3u);   /* tblB[5] | tblB[5]<<8 */
    dl_wr(0x0494u, 0x0666u);   /* idx 5 < 8 -> the low-volume branch */
    dl_wr(0x049Au, 0x0002u);
}

/* 5: the two registers the stock playback path writes that our 260-element
 * record never wrote, on their own and with no soft reset - so "the tail
 * registers did it" is separated from "the reset did it". */
static void dl_route_tail(void)
{
    dl_wr(0x0494u, 0x0666u);
    dl_wr(0x049Au, 0x0002u);
}

/* 6: the same pair with the volume>=80 branch's values. */
static void dl_route_tail_hi(void)
{
    dl_wr(0x0494u, 0x16C3u);
    dl_wr(0x049Au, 0x0001u);
}

/* 7/8/9: the stock's SCENARIO-PROFILE write sequences.  Three more stock
 * routines write a handful of DSP registers and then soft-reset, i.e. they
 * select a processing profile before starting audio.  They reach the device
 * through a second helper, 0x40125578(reg, value) (lock, ops[+72](reg,value),
 * unlock), whose 23 call sites are the only other DSP register writes in the
 * image - and two of the registers they touch, 0x04C0 and 0x0622, are in the
 * record tail our 260-element load never wrote:
 *
 *   profile A - 0x400D6493: 0x4C0 <- 0x0022, 0x202 <- 0x1C0F, 0x22A <- 0x0000
 *                            then VprocTwolfReset(SOFTWARE)
 *   profile B - 0x4012953C: 0x206 <- 0x5000, 0x202 <- 0x1C0C, 0x2A0 <- 0x0005,
 *                            0x2A2 <- 0xC000, 0x300 <- 0x3838,
 *                            0x622 <- read(0x622) ^ 0x0100, then reset
 *   0x40128D40 / 0x4012A430: 0x2A2 <- 0xC000, 0x300 <- 0x3808 (and, in the
 *                            other one, 0x4C0 <- x, 0x202 <- 0x3C0F, 0x22A <- 1)
 *
 * The stock record's own values for the same registers: 0x4C0 = 0x0033,
 * 0x202 = 0x3C0F, 0x22A = 0x0001, 0x206 = 0x5000, 0x2A0 = 0xC005, 0x2A2 = 0xC000,
 * 0x300 = 0x3808, 0x622 = 0x0087.  So profile A/B are genuinely DIFFERENT
 * profiles, not re-assertions - which is exactly why they are worth trying. */
static void dl_profile_a(void)
{
    dl_wr(0x04C0u, 0x0022u);
    dl_wr(0x0202u, 0x1C0Fu);
    dl_wr(0x022Au, 0x0000u);
}

static void dl_profile_b(void)
{
    dl_wr(0x0206u, 0x5000u);
    dl_wr(0x0202u, 0x1C0Cu);
    dl_wr(0x02A0u, 0x0005u);
    dl_wr(0x02A2u, 0xC000u);
    dl_wr(0x0300u, 0x3838u);
    dl_wr(0x0622u, 0x0187u);   /* record 0x0087 with the routine's ^0x0100 */
}

/* 10: 0x04C0 is the one profile register that lives in the omitted tail, so try
 * it on its own, both ways round (profile A's value, then the record's). */
static void dl_reg04c0(void)
{
    dl_wr(0x04C0u, 0x0033u);
    dl_wr(0x04C0u, 0x0022u);
}

/* 10/11/12: the output gains.  VprocTwolfSetVolume() proves the encoding: the
 * 16-bit register is two signed 8-bit gains (low = Gain A, high = Gain B), 1 dB
 * per LSB, +6 dB maximum and about -90 dB at the bottom (the esp-adf codec
 * wrapper documents 0xA6 = -90 dB, 0x00 = 0 dB, 0x06 = +6 dB).  The stock record
 * ships 0xF1F1 = -15 dB in both; the esp-adf reference record ships 0x0202 = +2 dB. */
static void dl_vol_zero(void)
{
    dl_wr(0x0238u, 0x0000u);
    dl_wr(0x023Au, 0x0000u);
}

static void dl_vol_max(void)
{
    dl_wr(0x0238u, 0x0606u);
    dl_wr(0x023Au, 0x0606u);
}

static void dl_vol_adf(void)
{
    dl_wr(0x0238u, 0x0202u);
    dl_wr(0x023Au, 0x0202u);
}

/* 13/14/15: individual page-2 registers where the stock record and the esp-adf
 * reference record disagree.  All 39 differences are on page 2; these are the
 * ones that can plausibly be an enable / selector / rate field. */
static void dl_reg0200_adf(void)
{
    dl_wr(0x0200u, 0x0000u);
}

static void dl_reg0232_adf(void)
{
    dl_wr(0x0232u, 0x0006u);
}

static void dl_reg0212_adf(void)
{
    dl_wr(0x0212u, 0x0605u);
}

/* 16: the small enum triple 0x0224/0x0228/0x022A (a port/mode selector shape). */
static void dl_enum_adf(void)
{
    dl_wr(0x0224u, 0x0001u);
    dl_wr(0x0228u, 0x0002u);
    dl_wr(0x022Au, 0x0003u);
}

/* 17: the remaining page-2 esp-adf deltas below 0x0300. */
static void dl_page2_rest_adf(void)
{
    dl_wr(0x02B2u, 0x0000u);
    dl_wr(0x02B6u, 0x0140u);
    dl_wr(0x02D8u, 0x0500u);
    dl_wr(0x02E0u, 0x0FF7u);
    dl_wr(0x02E2u, 0xFFFFu);
    dl_wr(0x02E4u, 0x0000u);
    dl_wr(0x02E6u, 0x0000u);
}

/* 18: the reset-only control - no register write of its own, only the soft reset
 * that follows it.  It separates "the writes did it" from "the reset did it". */
static void dl_nothing(void)
{
}

/* 19: the GO again, through the framed host command the release uses (mailbox
 * acquire, reg 0x32 <- cmd, reg 0x0006 <- 1, poll reg 0x32 back to 0) rather
 * than a bare register write. */
static void dl_go_again(void)
{
    const VprocStatusType st = VprocTwolfHostCmdWr(0x0008u);
    const bool ok = (st == VPROC_STATUS_SUCCESS);
    if (ok) {
        ++s_dl_ok;
    } else {
        ++s_dl_bad;
    }
    ESP_LOGI(TAG, "downlink try %u: reg 0x0032 <- 0x0008 (GO, framed host command) "
                  "-> %s", s_dl_try, ok ? "ok" : "FAILED");
}

typedef struct {
    const char *what;  /* the try's header line */
    void (*fn)(void);  /* its device writes (may be none) */
    bool reset;        /* follow them with the software reset? */
} b220_dl_try_t;

/* ORDER MATTERS: the two record tries come first because the record dig named
 * them the prime suspect, then an exact replay of the stock playback-start write
 * list (which is what isolates the two registers in the omitted tail), then the
 * output gains, then the individual page-2 esp-adf deltas, then the controls. */
static const b220_dl_try_t s_dl_tries[] = {
    {"read-only probe: app status, host command/result, host flags, 0x0014 (the "
     "route gate), the output gains and the tail registers 0x0494/0x049A/0x0A1C",
     dl_probe, false},
    {"the 782 record entries the 260-element header omitted (regs 0x0408..0x0A22) "
     "+ soft reset",
     dl_tail, true},
    {"the WHOLE 1042-entry record from the stock image + soft reset",
     dl_full, true},
    {"the STOCK PLAYBACK-START WRITE LIST (VA 0x40125358 -> 0x401256D8(volume 50)): "
     "0x238/0x23A <- 0xECEC, 0x24E <- 0xF3F3, 0x494 <- 0x0666, 0x49A <- 0x0002 "
     "+ soft reset",
     dl_stock_playback, true},
    {"the two tail registers that replay writes and our 260-entry record never did: "
     "0x494 <- 0x0666, 0x49A <- 0x0002 (no reset)",
     dl_route_tail, false},
    {"the same pair, volume>=80 branch: 0x494 <- 0x16C3, 0x49A <- 0x0001",
     dl_route_tail_hi, false},
    {"STOCK SCENARIO PROFILE A (0x400D6493): 0x4C0 <- 0x0022, 0x202 <- 0x1C0F, "
     "0x22A <- 0x0000 + soft reset",
     dl_profile_a, true},
    {"STOCK SCENARIO PROFILE B (0x4012953C): 0x206 <- 0x5000, 0x202 <- 0x1C0C, "
     "0x2A0 <- 0x0005, 0x2A2 <- 0xC000, 0x300 <- 0x3838, 0x622 <- 0x0187 + soft reset",
     dl_profile_b, true},
    {"0x04C0 (the one profile register in the omitted tail): 0x0033 then 0x0022",
     dl_reg04c0, false},
    {"0x0238/0x023A <- 0x0000  (0 dB, unity output gain)", dl_vol_zero, false},
    {"0x0238/0x023A <- 0x0606  (+6 dB, maximum output gain)", dl_vol_max, false},
    {"0x0238/0x023A <- 0x0202  (the esp-adf value, +2 dB)", dl_vol_adf, false},
    {"0x0200 <- 0x0000  (esp-adf value; the stock record has 0x0001 - the stock's "
     "set-mute routine drives exactly this register)",
     dl_reg0200_adf, false},
    {"0x0232 <- 0x0006  (esp-adf value; the stock record has 0x0000)",
     dl_reg0232_adf, false},
    {"0x0212 <- 0x0605  (esp-adf value; the stock record has 0x0000)",
     dl_reg0212_adf, false},
    {"0x0224/0x0228/0x022A <- 1/2/3  (the esp-adf enum triple)", dl_enum_adf, false},
    {"the remaining page-2 esp-adf deltas, 0x02B2..0x02E6", dl_page2_rest_adf, false},
    {"control: the soft reset alone, reg 0x0006 <- 0x0002, with no other write",
     dl_nothing, true},
    {"reg 0x0032 <- 0x0008  (GO again, through the framed host command)",
     dl_go_again, false},
};

unsigned zl38063_dsp_downlink_tries_count(void)
{
    return (unsigned)(sizeof(s_dl_tries) / sizeof(s_dl_tries[0]));
}

void zl38063_dsp_downlink_tries_reset(void)
{
    s_dl_try = 0u;
    s_dl_ok = 0u;
    s_dl_bad = 0u;
}

/* Apply the next candidate and return its 0-based index, or -1 once the table is
 * exhausted.  Called once per ~2 s loopback window, so the caller's tone keeps
 * running across tries. */
int zl38063_dsp_downlink_tries_step(void)
{
    const unsigned n = zl38063_dsp_downlink_tries_count();
    if (s_dl_try >= n) {
        return -1;
    }
    const b220_dl_try_t *t = &s_dl_tries[s_dl_try];
    ++s_dl_try;
    s_dl_ok = 0u;
    s_dl_bad = 0u;

    ESP_LOGW(TAG, "===== downlink try %u/%u: %s =====", s_dl_try, n, t->what);
    t->fn();
    if (t->reset) {
        dl_soft_reset();
    }
    dl_readback();
    ESP_LOGW(TAG, "downlink try %u/%u DONE: %u device write(s), %u ok, %u FAILED "
                  "-> check the DSP DAC output / speaker now; the next try comes "
                  "after the next ~2 s window and the 1 kHz tone keeps running "
                  "throughout",
             s_dl_try, n, s_dl_ok + s_dl_bad, s_dl_ok, s_dl_bad);
    if (s_dl_try >= n) {
        ESP_LOGW(TAG, "downlink: ALL %u tries exhausted - this experiment emits no "
                      "further device writes; the 1 kHz tone keeps running so the "
                      "DAC and the speaker can still be re-checked by hand", n);
    }
    return (int)(s_dl_try - 1u);
}

#else /* !B220_DSP_DOWNLINK_TRIES: the experiment is compiled out entirely */

unsigned zl38063_dsp_downlink_tries_count(void)
{
    return 0u;
}

void zl38063_dsp_downlink_tries_reset(void)
{
}

int zl38063_dsp_downlink_tries_step(void)
{
    return -1;
}

#endif /* B220_DSP_DOWNLINK_TRIES */

