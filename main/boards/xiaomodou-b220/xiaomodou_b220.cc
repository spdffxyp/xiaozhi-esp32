#include "wifi_board.h"
/*
 * The production audio codec of this board.  It is NOT a chip codec: it moves
 * PCM over the I2S0 link the bring-up below owns and switches the AW8737S PA on
 * through b220_audio_pa_enable_once() (see b220_audio_share.h).  DummyAudioCodec
 * is no longer used here.
 */
#include "zl38063_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "b220_baseboard_probe.h"
#if B220_BUTTON_DISCOVERY
#include "b220_button_discovery.h"
#else
#include "b220_buttons.h"
#endif
#include "b220_led.h"
#include "settings.h"
#include "b220_led_policy.h"

#include "mcp_server.h"   /* self.base.* tools */
#include "b220_baseboard.h"

#include <cstdlib>   /* strtod, for the seconds argument */
#include "b220_tcp_log.h"
#include "b220_base_uart.h"
/*
 * config.h must come BEFORE zl38063_dsp.h: zl38063_dsp.h exposes the
 * B220_DSP_* guarded declarations only if the macro is already defined when it
 * is parsed (see the include-order note at the top of zl38063_dsp.c).
 */
#include "config.h"
#include "zl38063_dsp.h"
/*
 * The pre-GO verify (B220_DSP_VERIFY_BEFORE_GO above) reads the firmware records
 * through the page-255 window, i.e. with the same two lower layers the upload
 * itself uses: `const twFwr st_twFirmware[]` / `firmwareStreamLen` and the Vproc
 * calls (VprocTwolfHbiWrite, VprocTwolfHbiPage255Read, VprocTwolfHbiRead,
 * VprocStatusType) come from zl38063_vproc.h, and the
 * ZL38063_PAGE255_OFF_* encodings from zl38063_hbi.h.  zl38063_dsp.h deliberately
 * does not re-export either - the board file is the layer that composes them
 * (zl38063_dsp.c includes both the same way).
 */
#include "zl38063_vproc.h"
#include "zl38063_hbi.h"
/*
 * The codec bridge: b220_audio_share.h declares the accessors defined in this
 * file, so including it here is what makes a definition/declaration drift a
 * compile error.  (The codec includes the same header.)
 */
#include "b220_audio_share.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include <driver/spi_common.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
/* esp_timer_get_time() - the PA SHDN pulse timing and the loopback's rate lines
 * both need a microsecond clock.  (The board file also included this locally far
 * below, next to the tone generator; one include at the top is enough.) */
#include <esp_timer.h>
/* esp_rom_delay_us() - the >= 2 ms SHDN low hold of the PA enable sequence.  It
 * is declared here explicitly rather than relied on transitively: it is the one
 * call in this file whose declaration the include set does not obviously carry. */
#include <esp_rom_sys.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "XiaoMoDouB220"

/*
 * Phase 3-15 : the single clean ADF-order release (B220_DSP_ADF_RELEASE_TX) ends
 * in the audio loopback, which runs on the I2S0 handles that
 * B220_I2sUpBeforeDspBoot() creates - there is no other I2S channel on this
 * board (one port, one duplex pair).  Fail the build rather than boot a board
 * that silently has nothing to loop back.
 */
#if B220_DSP_ADF_RELEASE_TX && !B220_I2S_BEFORE_DSP
#error "B220_DSP_ADF_RELEASE_TX 1 needs B220_I2S_BEFORE_DSP 1 (config.h): the audio loopback reuses the I2S0 handles that call creates"
#endif

/*
 * Phase 3-20 : the AW9106B SHDN bit finder (B220_PA_BIT_FINDER) is started from
 * B220_AudioLoopbackStart(), because it needs the loopback task that keeps the
 * 1 kHz tone running for the whole walk (see the finder block far below).  Fail
 * the build rather than boot a board that would start neither.
 */
#if B220_PA_BIT_FINDER && !(B220_AUDIO_LOOPBACK && B220_I2S_BEFORE_DSP)
#error "B220_PA_BIT_FINDER 1 needs B220_AUDIO_LOOPBACK 1 and B220_I2S_BEFORE_DSP 1 (config.h): the finder is started next to the loopback task and the 1 kHz tone must run during the whole walk"
#endif

/*
 * Phase 3-17 : the downlink test switch (B220_AUDIO_TEST_MODE in config.h).
 *
 * The ADF release works and the UPLINK is proven (I2S RX carries real mic audio,
 * peak 20000-32000) while the DOWNLINK is not (nothing is heard even though the
 * very same frames are written back to I2S TX and the PA enable was accepted).
 * One flash therefore has to separate "the ESP32 -> speaker leg is broken" - of
 * the four I2S lines only DOUT (GPIO26) was never continuity-tested - from "the
 * DSP's internal routing is wrong".  The switch changes only the loopback task's
 * SAMPLE SOURCE:
 *
 *   B220_AUDIO_TEST_MODE 0 : plain echo, RX -> TX, byte for byte as before.
 *   B220_AUDIO_TEST_MODE 1 : 1 kHz full-scale sine on TX only, RX never read.
 *   B220_AUDIO_TEST_MODE 2 : 1 kHz -> 8 kHz sweep on TX, 1 s per step, full scale.
 *   B220_AUDIO_TEST_MODE 3 : echo as mode 0, plus a 0.5 s 1 kHz beep every 2 s.
 *
 * The two #defines below must match the meanings in config.h; the #error is the
 * build-time check that they do.
 */
#define B220_TEST_ECHO       0
#define B220_TEST_TONE       1
#define B220_TEST_SWEEP      2
#define B220_TEST_ECHO_BEEP  3

#if (B220_AUDIO_TEST_MODE < B220_TEST_ECHO) || (B220_AUDIO_TEST_MODE > B220_TEST_ECHO_BEEP)
#error "B220_AUDIO_TEST_MODE (config.h) must be 0 (echo), 1 (1 kHz tone), 2 (sweep) or 3 (echo + beep)"
#endif

/*
 * Phase 3-19 : I2S TX SIGNAL-LEVEL VARIANT SWEEP (B220_I2S_TX_VARIANT_SWEEP).
 *
 * The state it steps from (all hardware-measured): the DSP application runs and
 * reports ready (reg 0x0014 = 0x0200), every DSP config/route/volume register we
 * know of reads back correctly, the uplink works (mic audio on I2S DIN/GPIO35)
 * and the downlink does not - a 1 kHz full-scale sine written to I2S TX/GPIO26
 * (tx_peak=32759, write_err=0) shows up as a square wave on GPIO26, which is
 * continuously connected to the DSP's I2S_SDIA, yet DAC1_P/DAC1_M stay flat while
 * a stock unit playing audio shows a 100 mV square wave there.  The rate and the
 * width already match the stock unit (WS 48 kHz, BCLK 1.54 MHz = 32 BCLK/frame =
 * 16-bit slots), so what is left on the ESP32 side of the link is the SIGNAL
 * LEVEL: phase/polarity of BCLK and WS, slot width, slot placement and the
 * Philips 1-bit data delay.  This switch sweeps exactly those, one at a time,
 * while the tone keeps playing, so DAC1_P/M can be watched on a scope:
 *
 *   step 1  baseline: what this build has always sent (no hardware change)
 *   step 2  invert_flags.bclk_inv = true
 *   step 3  invert_flags.ws_inv   = true
 *   step 4  both inverted
 *   step 5  slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT (data still 16-bit,
 *           MSB-aligned; BCLK becomes 3.072 MHz, WS stays 48 kHz)
 *   step 6  slot_mode = I2S_SLOT_MODE_MONO with the data in the LEFT slot
 *   step 7  slot_mode = I2S_SLOT_MODE_MONO with the data in the RIGHT slot
 *   step 8  bit_shift = false (MSB-first, no Philips 1-bit delay)
 *   step 9  back to baseline
 *
 * Every parameter above is runtime-changeable in IDF 6.1 through the APIs this
 * file uses - i2s_channel_disable() -> i2s_channel_reconfig_std_slot() /
 * i2s_channel_reconfig_std_gpio() -> i2s_channel_enable():
 *   - the slot fields (slot_bit_width/slot_mode/slot_mask/bit_shift/msb_right)
 *     are i2s_std_slot_config_t, i2s_channel_reconfig_std_slot() @
 *     components/esp_driver_i2s/include/driver/i2s_std.h:352;
 *   - bclk_inv/ws_inv are i2s_std_gpio_config_t.invert_flags,
 *     i2s_channel_reconfig_std_gpio() @ the same header line 367.
 * Both require the channel state READY ("`i2s_channel_disable` should be called
 * before calling this function if I2S has started"), which is why every step
 * brackets them with disable/enable.  data_bit_width is deliberately NOT swept:
 * it is the DMA word size and i2s_get_buf_size() (i2s_common.c) keys the DMA
 * chain off it, so changing it mid-run would re-allocate the descriptors for no
 * diagnostic gain - the frame width is what the DSP sees, and that is
 * slot_bit_width.
 *
 * MONO note (steps 6/7): in the TX direction "mono means the written buffer
 * contains only one slot data" (i2s_std.h:232-235), and the driver really does
 * halve the DMA frame - i2s_std_set_slot() sets active_slot = 1 for MONO
 * (i2s_std.c:150) and i2s_get_buf_size() computes
 * bytes_per_frame = bytes_per_sample * active_slot (i2s_common.c:624-632) - so
 * the tone is written as 240 x 16-bit (480 bytes) in mono and as 240 x 2 x
 * 16-bit (960 bytes) in stereo.  Worth knowing when reading the scope: IDF's own
 * test suite skips the 16-bit TX-mono case on this chip ("#if
 * !CONFIG_IDF_TARGET_ESP32 // the 16 bit channel sequence on ESP32 is incorrect",
 * test_apps/i2s/main/test_i2s.c:799), so steps 6/7 may show an IDF quirk rather
 * than a DSP verdict.
 *
 * 0 (the default) compiles every line of this phase out and leaves the build as
 * it was; the tone still plays and the loopback still logs its rate line.
 */
#if B220_I2S_TX_VARIANT_SWEEP && !(B220_AUDIO_LOOPBACK && B220_I2S_BEFORE_DSP)
#error "B220_I2S_TX_VARIANT_SWEEP 1 needs B220_AUDIO_LOOPBACK 1 and B220_I2S_BEFORE_DSP 1 (config.h): the stepper reconfigures the I2S0 TX channel that only B220_I2sUpBeforeDspBoot() creates, and it is paced by the loopback task that keeps the tone running"
#endif

#if B220_I2S_TX_VARIANT_SWEEP
/* True while the sweep has the TX channel in I2S_SLOT_MODE_MONO.  Read by the
 * tone path, which then writes half a block: in mono the DMA frame is ONE 16-bit
 * word, not two (see the note above). */
static bool s_b220_tx_mono = false;
#define B220_TX_MONO_NOW() (s_b220_tx_mono)
#else
#define B220_TX_MONO_NOW() (false)
#endif

#if B220_I2S_BEFORE_DSP
/* ------------------------------------------------------------------------- *
 * Phase 3-⑥ : I2S master that has to be running BEFORE the ZL38063 is reset.
 *
 * The DSP's master clock is the ESP32 I2S BCLK on GPIO5: the stock pin map has
 * no MCLK pin at all (i2s_pin_config_t @ DROM 0x3f4317ac carries bck/ws/dout/din
 * only), so with no BCLK the device never leaves its boot ROM and answers every
 * HBI read with 0x0000 - exactly the symptom we are chasing.  The stock order is
 * therefore [codec/I2S init] -> [GPIO19 reset pulse] -> [host boot over HBI].
 *
 * Full-duplex on I2S0 because the ZL38063 has both a data-in and a data-out leg
 * on this one link, and because a TX-only port is the one thing that could be
 * left without a clock.  i2s_new_channel() is called with BOTH handle pointers
 * non-NULL, which is what makes the driver put the two channels on one port and
 * share BCLK/WS between them ("If both tx_handle and rx_handle are not NULL, it
 * means this I2S controller will work at full-duplex mode, the RX and TX channels
 * will be allocated on a same I2S port in this case ... they can only share the
 * clock signals for now" - driver/i2s_common.h).
 *
 * On ESP32 (I2S hardware v1) the driver demotes the second channel of a duplex
 * pair from master to slave and loops BCLK/WS on-chip
 * (i2s_channel_try_to_constitute_duplex(): "the %s channel on I2S%d is switched
 * from master to slave for full-duplex mode"), so the TX channel is the one that
 * actually generates the clock and it must stay enabled (and fed).
 * ------------------------------------------------------------------------- */
#define B220_I2S_PORT I2S_NUM_0
#define B220_I2S_SAMPLE_RATE_HZ 48000
#define B220_I2S_SILENCE_FRAMES 512 /* 512 stereo frames = 2048 bytes */

/* Static and zero-initialised: i2s_channel_preload_data() only reads it, and
 * .bss in internal SRAM is always DMA-capable.  PSRAM is deliberately not used
 * (the ESP32's DMA cannot reach it safely, and this I2S port has never been
 * driven on this board before). */
static uint8_t s_i2s_silence[B220_I2S_SILENCE_FRAMES * 2 * sizeof(uint16_t)];

/* Kept so a later audio path can take over the already-running port instead of
 * fighting over it.  The pair is only valid once B220_I2sUpBeforeDspBoot() has
 * returned ESP_OK. */
static i2s_chan_handle_t s_i2s_tx = nullptr;
static i2s_chan_handle_t s_i2s_rx = nullptr;

#if B220_DSP_VERIFY_UPLOAD && !B220_DSP_HANDS_OFF && !B220_DSP_RELEASE_MATRIX
/* Phase 3-⑧ : the host-upload read-back verify is a one-shot diagnostic.  It
 * costs ~0.4-0.7 s and its answer cannot change between the boot retries, so it
 * runs on the first "no application running" outcome and never again.  (Only the
 * host-boot path uses it, hence the B220_DSP_HANDS_OFF term: in hands-off mode it
 * would be an unused static variable.  Phase 3-12 adds the same term for
 * B220_DSP_RELEASE_MATRIX: the release matrix compiles that verify's call site out
 * - it does its own 9-record read-back five times - so the flag would be unused
 * and would warn.) */
static bool s_dsp_upload_verified = false;
#endif

#if B220_DSP_PAGE255_CALIB && !B220_DSP_HANDS_OFF
/* Phase 3-10 : the page-255 window calibration WRITES 16 words of scratch DSP RAM
 * (plus the 2-word window base register the upload writes anyway) to decide which
 * window read encoding reproduces the upload's own write path, and its verdict is
 * remembered for the verify that consumes it.  One-shot, like the verify above,
 * and only on the host-boot path (B220_DSP_HANDS_OFF === 0). */
static bool s_dsp_page255_calibrated = false;
#endif

#if B220_DSP_VERIFY_BEFORE_GO && !B220_DSP_HANDS_OFF && !B220_DSP_ADF_RELEASE_TX
/* Phase 3-11 : the pre-GO verify runs INSIDE the host-upload transfer, so it can
 * fire more than once per boot (once per boot attempt, i.e. per
 * zl38063_dsp_begin()).  The counters below therefore add up over the attempts
 * and the counter is printed in the summary, so two passes are never mistaken
 * for one - which matters because a LATER attempt runs after the page-255
 * calibration has happened, while the first one does not. */
static unsigned s_dsp_pre_go_runs = 0;

/* Read one device register for the pre-GO state line.  Returns 0xFFFF when the
 * read failed (no register of this set carries 0xFFFF in the states that matter
 * here, so it reads as "unreadable" rather than as a plausible value). */
static uint16_t B220_PreGoReadRegister(uint16_t reg) {
    unsigned short v = 0;
    if (VprocTwolfHbiRead(reg, 1, &v) != VPROC_STATUS_SUCCESS) {
        return 0xFFFFu;
    }
    return (uint16_t)v;
}

/*
 * The record sample the pre-GO pass reads back, and the read-back itself.
 *
 * Phase 3-12 : both are zl38063_dsp.c's shared implementation
 * (ZL38063_DSP_SAMPLE_MAX, zl38063_dsp_sample_index_set(),
 * zl38063_dsp_sample_records_readback()) when the page-255 calibration is
 * compiled in, because that is exactly what the hook-release matrix uses for its
 * post-release read-back - the same helper, the same sample of the image, the
 * same encoding, so the pre-GO and post-release numbers of one boot differ only
 * if the device differs.
 *
 * With B220_DSP_PAGE255_CALIB 0 the shared helper is not compiled (it lives in
 * the calibration's block), so this file falls back to a small local version with
 * the same namesake behaviour ("enc=b", the upload's own wire encoding).  The
 * capability test is on the macro, not on the header: zl38063_dsp.h declares the
 * shared API only under the same macro.
 */
#define B220_PRE_GO_SAMPLE_MAX 16u

#if B220_DSP_PAGE255_CALIB
static unsigned int B220_PreGoSampleIndexSet(unsigned int rec_cnt, unsigned int n,
                                             unsigned int *idx, unsigned int max) {
    return zl38063_dsp_sample_index_set(rec_cnt, n, idx, max);
}
#else
static unsigned int B220_PreGoSampleIndexSet(unsigned int rec_cnt, unsigned int n,
                                             unsigned int *idx, unsigned int max) {
    unsigned int count = 0;
    if ((idx == nullptr) || (max == 0u) || (rec_cnt == 0u)) {
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
#endif

/* Phase 3-12 : when the hook-release matrix is compiled in it asks this file to
 * run the page-255 calibration once, before its first variant, and to report
 * whether an application came up.  Both are thin forwarders onto state this file
 * already owns (s_dsp_page255_calibrated / s_dsp_upload_verified), so the matrix
 * never re-runs a one-shot calibration and the default path's bookkeeping stays
 * authoritative.  The calibration is a write of 16 scratch words, which is why it
 * may not simply be called twice: the first one already chose the encoding. */
#if B220_DSP_RELEASE_MATRIX && B220_DSP_PAGE255_CALIB
extern "C" void B220_ReleaseMatrixCalibrate(void) {
    if (s_dsp_page255_calibrated) {
        ESP_LOGI(TAG, "release matrix: page-255 calibration already ran this boot; "
                      "keeping its encoding (re-running it would write the scratch "
                      "pattern a second time)");
        return;
    }
    s_dsp_page255_calibrated = true;
    zl38063_dsp_page255_calibrate();
}
#endif

/*
 * The pre-GO verify itself: read a sample of the image back out of DSP RAM in the
 * one instant at which the image is supposed to be in RAM and NO host command has
 * been sent yet (see zl38063_dsp_set_pre_go_hook()).
 *
 * It walks the SAME record array the upload walked (st_twFirmware[] /
 * firmwareStreamLen, the records HbiSrecBoot_alt() just fed to the device) with
 * the SAME framing as the post-GO pass (zl38063_dsp_verify_uploaded_image()):
 * re-point the page-255 window base with reg 0x0C <- {hi16, lo16} for records
 * whose useTargetAddr is set, then read the record's words through the window
 * with VprocTwolfHbiPage255Read() and the encoding
 * zl38063_dsp_page255_calib_encoding() reports.  That encoding matters: on the
 * first boot attempt the calibration has not run yet, so read those lines as
 * "genuinely 0x0000" (an encoding-independent result) and the later attempts as
 * the calibrated measurement.
 *
 * The read itself is zl38063_dsp_sample_records_readback() (zl38063_dsp.c), which
 * is also what the hook-release matrix uses for its post-release read-back - so
 * the pre-GO numbers and the post-release numbers of the same boot are produced
 * by the same code, through the same window and with the same encoding, and a
 * difference between them can only come from the device.
 *
 * Phase 3-12: the sample is now B220_DSP_PRE_GO_SAMPLES records spread evenly over
 * the whole image (the first and the last record included) instead of every 4th
 * record.  That is exactly the "9 records spanning the whole image" the release
 * matrix reads back after the release, so the two passes are directly comparable;
 * the every-4th pass with its per-record lines and hex dump is still what the
 * post-GO verify (zl38063_dsp_verify_uploaded_image()) does.
 */
#define B220_DSP_PRE_GO_SAMPLES 9u

#if B220_DSP_PAGE255_CALIB
/* The shared read-back with its counters (struct + function from zl38063_dsp.h). */
static esp_err_t B220_PreGoSampleReadback(const unsigned int *idx, unsigned int n,
                                          bool *all_match) {
    struct zl38063_sample_stats stats;
    const esp_err_t err = zl38063_dsp_sample_records_readback("pre-GO", idx, n,
                                                             &stats);
    if (all_match != nullptr) {
        *all_match = (err == ESP_OK) && (stats.mismatching == 0ul) &&
                     (stats.read_errors == 0u);
    }
    return err;
}
#else
/* No calibration compiled in: the shared helper does not exist, so read the sample
 * here with the same framing the old pre-GO pass used - the window base write the
 * upload itself performs, then VprocTwolfHbiPage255Read() with the encoding the
 * upload's own write path emits (ZL38063_PAGE255_OFF_UPLOAD). Note that
 * zl38063_dsp_page255_calib_encoding() is not available either (its declaration in
 * zl38063_dsp.h is guarded by B220_DSP_PAGE255_CALIB || B220_DSP_VERIFY_UPLOAD),
 * which is why the encoding constant is used directly. */
static esp_err_t B220_PreGoSampleReadback(const unsigned int *idx, unsigned int n,
                                          bool *all_match) {
    unsigned int words = 0;
    unsigned int mismatching = 0;
    unsigned int read_errors = 0;

    for (unsigned int k = 0; k < n; k++) {
        const twFwr *rec = &st_twFirmware[idx[k]];
        if (rec->numWords == 0u) {
            continue;
        }
        if (rec->useTargetAddr != 0u) {
            unsigned short base[2];
            base[0] = (unsigned short)((rec->targetAddr >> 16) & 0xFFFFu);
            base[1] = (unsigned short)(rec->targetAddr & 0xFFFFu);
            if (VprocTwolfHbiWrite(0x000Cu, 2, base) != VPROC_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "pre-GO verify: record %u: window base write for "
                              "0x%08lX failed; stopping", idx[k],
                         (unsigned long)rec->targetAddr);
                break;
            }
        }
        unsigned short back[sizeof(rec->buf) / sizeof(rec->buf[0])];
        const unsigned int nw = (rec->numWords >
                                 (sizeof(rec->buf) / sizeof(rec->buf[0])))
                                    ? (unsigned int)(sizeof(rec->buf) /
                                                     sizeof(rec->buf[0]))
                                    : (unsigned int)rec->numWords;
        memset(back, 0, sizeof(back));
        if (VprocTwolfHbiPage255Read(rec->targetAddr, (unsigned char)nw, back,
                                     ZL38063_PAGE255_OFF_UPLOAD) !=
            VPROC_STATUS_SUCCESS) {
            read_errors++;
            ESP_LOGE(TAG, "pre-GO verify: record %u target=0x%08lX: window read of "
                          "%u word(s) FAILED", idx[k],
                     (unsigned long)rec->targetAddr, nw);
            continue;
        }
        unsigned int rec_match = 0;
        for (unsigned int w = 0; w < nw; w++) {
            words++;
            if (back[w] == rec->buf[w]) {
                rec_match++;
            } else {
                mismatching++;
            }
        }
        char read_txt[5 * 5];
        char sent_txt[5 * 5];
        int rp = 0, sp = 0;
        for (unsigned int w = 0; (w < 4u) && (w < nw); w++) {
            rp += snprintf(&read_txt[rp], sizeof(read_txt) - (size_t)rp, "%04X ",
                           (unsigned)back[w]);
            sp += snprintf(&sent_txt[sp], sizeof(sent_txt) - (size_t)sp, "%04X ",
                           (unsigned)rec->buf[w]);
        }
        ESP_LOGI(TAG, "pre-GO verify: record %u target=0x%08lX matched %u/%u "
                      "(read %sexpected %s)",
                 idx[k], (unsigned long)rec->targetAddr, rec_match, nw, read_txt,
                 sent_txt);
    }

    ESP_LOGI(TAG, "pre-GO verify: SUMMARY visited=%u words=%u mismatching_words=%u "
                  "read_errors=%u window_enc=b (calibration not compiled in)",
             n, words, mismatching, read_errors);
    if (all_match != nullptr) {
        *all_match = (mismatching == 0u) && (read_errors == 0u);
    }
    return ESP_OK;
}
#endif

static void B220_PreGoVerify() {
    /*
     * No "is the HBI attached?" test here: the hook only ever runs from inside a
     * host upload, which zl38063_dsp_upload_firmware() refuses to start unless the
     * HBI device is attached (s_hbi_ready is a static of zl38063_dsp.c and not
     * reachable from this file).  The image check is the one that is really
     * reachable from a board-side translation unit, and
     * zl38063_dsp_sample_records_readback() repeats it as its backstop.
     */
    if (firmwareStreamLen == 0u) {
        ESP_LOGE(TAG, "pre-GO verify: no firmware image linked in (firmwareStreamLen "
                      "== 0); skipped");
        return;
    }

    const unsigned int rec_cnt = (unsigned int)firmwareStreamLen;
#if B220_DSP_PAGE255_CALIB
    const unsigned int enc = zl38063_dsp_page255_calib_encoding();
    const char enc_char = (enc == ZL38063_PAGE255_OFF_WORD) ? 'b' : 'a';
#else
    /* No calibration compiled in: the encoding the upload's own write emits, which
     * is the value ZL38063_PAGE255_OFF_UPLOAD stands for. */
    const char enc_char = 'b';
#endif

    unsigned int idx[B220_PRE_GO_SAMPLE_MAX];
    const unsigned int n_idx = B220_PreGoSampleIndexSet(rec_cnt,
                                                        B220_DSP_PRE_GO_SAMPLES,
                                                        idx,
                                                        B220_PRE_GO_SAMPLE_MAX);

    s_dsp_pre_go_runs++;

    ESP_LOGI(TAG, "----- pre-GO verify (run %u, B220_DSP_VERIFY_BEFORE_GO): called "
                  "from INSIDE the upload, all image records written, no host command "
                  "sent yet (reg 0x12C and reg 0x32 <- 0x000D come after this) -----",
             s_dsp_pre_go_runs);
    ESP_LOGI(TAG, "pre-GO verify: %u records, sampling %u of them evenly (record 0 "
                  "and the last record always), window read enc=%c, base = reg 0x0C "
                  "<- {hi16, lo16}",
             rec_cnt, n_idx, enc_char);

    bool all_match = false;
    const esp_err_t rerr = B220_PreGoSampleReadback(idx, n_idx, &all_match);
    if (rerr != ESP_OK) {
        ESP_LOGE(TAG, "pre-GO verify: the sampled read-back could not run (%s)",
                 esp_err_to_name(rerr));
    } else if (all_match) {
        ESP_LOGI(TAG, "pre-GO verify: the sampled records ARE in DSP RAM before any "
                      "host command was sent; the transfer works and whatever clears "
                      "or fails later happens at/after the start sequence");
    } else {
        ESP_LOGE(TAG, "pre-GO verify: at least one sampled word is NOT in DSP RAM "
                      "even though the records were just written - see the SUMMARY "
                      "line above (mismatching_words / read_errors); the GO never had "
                      "a fully correct image to start");
    }

    /* Read-only register state at the very same instant, before the GO. */
    ESP_LOGI(TAG, "pre-GO state (read-only): reg 0x0034 (boot ROM param/result)=0x%04X "
                  "reg 0x0030 (app status)=0x%04X reg 0x0032 (host cmd)=0x%04X "
                  "reg 0x0006 (host flags)=0x%04X",
             (unsigned)B220_PreGoReadRegister(0x0034),
             (unsigned)B220_PreGoReadRegister(0x0030),
             (unsigned)B220_PreGoReadRegister(0x0032),
             (unsigned)B220_PreGoReadRegister(0x0006));

    ESP_LOGI(TAG, "----- end pre-GO verify -----");
}
#endif  /* B220_DSP_VERIFY_BEFORE_GO && !B220_DSP_HANDS_OFF && !B220_DSP_ADF_RELEASE_TX */

#if B220_I2S_TX_VARIANT_SWEEP
/* Phase 3-19: the exact standard-mode configuration the TX channel was
 * initialised with (B220_I2sUpBeforeDspBoot() below).  It is the base every sweep
 * step is built from and it is updated on every successful reconfig, so "the
 * configuration that is on the wire right now" is always known without reaching
 * into the driver's private mode_info. */
static i2s_std_config_t s_b220_tx_variant_cfg;
#endif

/*
 * Bring up the full-duplex I2S0 master (48 kHz, 16-bit, 2 slots) and leave both
 * channels ENABLED, so BCLK/WS run continuously for the ZL38063.
 *
 * Clock guarantee: silence is preloaded while the TX channel is still in the
 * READY state - "Only allowed to be called when the channel state is READY ...
 * this function is used to preload the data into the DMA buffer, so that the
 * valid data can be transmitted immediately after the channel is enabled"
 * (i2s_channel_preload_data()) - and only then is the channel enabled - "Enable
 * the channel can start the I2S communication on hardware. It will start
 * outputting BCLK and WS signal" (i2s_channel_enable()).  The TX DMA therefore
 * never starts from an empty descriptor chain, and the clock is on GPIO5/GPIO25
 * before the first HBI access.
 */
static esp_err_t B220_I2sUpBeforeDspBoot() {
    if (s_i2s_tx != nullptr) {
        return ESP_OK; /* already up - do not re-create the port */
    }

    i2s_chan_config_t chan_cfg = {};
    chan_cfg.id = B220_I2S_PORT;
    chan_cfg.role = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear_after_cb = true; /* keep clocking zeros if the TX DMA
                                          * is ever starved by a callback */
    chan_cfg.auto_clear_before_cb = false;
    chan_cfg.allow_pd = false;
    chan_cfg.intr_priority = 0;
    chan_cfg.tx_destination = I2S_DESTINATION_DMA;
    chan_cfg.rx_destination = I2S_DESTINATION_DMA;

    /* Both handles non-NULL => full-duplex on one port (see the note above). */
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S: i2s_new_channel failed (%s)", esp_err_to_name(err));
        s_i2s_tx = nullptr;
        s_i2s_rx = nullptr;
        return err;
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg.sample_rate_hz = B220_I2S_SAMPLE_RATE_HZ;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.clk_cfg.bclk_div = 8; /* slave-role value; unused while master */
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = 16;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;
    /* The pin map carries no MCLK: the ZL38063 is clocked from BCLK here. */
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = AUDIO_I2S_GPIO_BCLK;
    std_cfg.gpio_cfg.ws = AUDIO_I2S_GPIO_WS;
    std_cfg.gpio_cfg.dout = AUDIO_I2S_GPIO_DOUT;
    std_cfg.gpio_cfg.din = AUDIO_I2S_GPIO_DIN;

    /* TX first: it is the channel that stays master and therefore owns BCLK/WS. */
    err = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(s_i2s_rx, &std_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S: std-mode init failed (%s)", esp_err_to_name(err));
        i2s_del_channel(s_i2s_tx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_tx = nullptr;
        s_i2s_rx = nullptr;
        return err;
    }

#if B220_I2S_TX_VARIANT_SWEEP
    /* Phase 3-19: the sweep's baseline is literally what was handed to the driver
     * just above, not a second transcription of the same constants. */
    s_b220_tx_variant_cfg = std_cfg;
#endif

    /* (a) Preload silence while the TX channel is still READY, so the very first
     * thing the port clocks out is valid data rather than an empty descriptor. */
    size_t preloaded = 0;
    err = i2s_channel_preload_data(s_i2s_tx, s_i2s_silence, sizeof(s_i2s_silence), &preloaded);
    if (err != ESP_OK) {
        /* Not fatal: step (b) below queues the same silence after enabling. */
        ESP_LOGW(TAG, "I2S: preload of %u silent bytes failed (%s)",
                 (unsigned)sizeof(s_i2s_silence), esp_err_to_name(err));
    } else if (preloaded < sizeof(s_i2s_silence)) {
        ESP_LOGW(TAG, "I2S: only %u of %u silent bytes preloaded", (unsigned)preloaded,
                 (unsigned)sizeof(s_i2s_silence));
    }

    /* TX first: this enable is the call that starts BCLK/WS. */
    err = i2s_channel_enable(s_i2s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S: TX enable failed (%s)", esp_err_to_name(err));
        i2s_del_channel(s_i2s_tx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_tx = nullptr;
        s_i2s_rx = nullptr;
        return err;
    }
    err = i2s_channel_enable(s_i2s_rx);
    if (err != ESP_OK) {
        /* The RX leg is a want, not a must: BCLK/WS come from the TX channel.
         * Keep the port up rather than tearing down a running clock. */
        ESP_LOGW(TAG, "I2S: RX enable failed (%s); TX keeps the clock running",
                 esp_err_to_name(err));
    }

    /* (b) Top the TX DMA up with one more silent block now that the channel is
     * RUNNING.  i2s_channel_write() only blocks until the DMA has taken the
     * buffer, so this costs a few ms of boot time. */
    size_t written = 0;
    err = i2s_channel_write(s_i2s_tx, s_i2s_silence, sizeof(s_i2s_silence), &written, 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "I2S: silence write failed (%s); relying on the preloaded "
                 "DMA buffers",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG,
             "I2S up before DSP boot: 48 kHz, 16-bit, stereo "
             "(bck=%d ws=%d dout=%d din=%d)",
             (int)AUDIO_I2S_GPIO_BCLK, (int)AUDIO_I2S_GPIO_WS, (int)AUDIO_I2S_GPIO_DOUT,
             (int)AUDIO_I2S_GPIO_DIN);
    return ESP_OK;
}
#endif  // B220_I2S_BEFORE_DSP

/* ------------------------------------------------------------------------- *
 * Phase 3-14 / Phase 4 : the AW9106B + AW8737S PA path, and (guarded
 * separately, further down) the standalone audio LOOPBACK diagnostic.
 *
 * STRUCTURE (Phase 4, production): this region is deliberately NOT behind
 * B220_AUDIO_LOOPBACK any more.  The AW9106B writers below, the audited
 * reg-0x02 path and b220_pa_enable_one_pulse() are PRODUCTION code:
 *   - InitializeBacklight() calls the 0x12/0x04/0x02 writers unconditionally;
 *   - b220_aw9106_set_gpio_output_a() (called from zl38063_dsp.c under its own
 *     switch) reads s_board_aw9106_dev and calls the reg-0x02 writer;
 *   - the production codec's EnableOutput(true) reaches the PA enable through
 *     b220_audio_pa_enable_once() (b220_audio_share.h).
 * With B220_AUDIO_LOOPBACK 0 - the production setting - the old guard around
 * this whole region removed exactly those definitions while every one of those
 * call sites stayed, so the file did not compile at all.  The guard now sits on
 * the DIAGNOSTIC only: from "s_b220_loopback_buf" down (the tone table and
 * generator, the sweep stepper, the raw-sample logger, the echo/tone task and
 * B220_AudioLoopbackStart()) it is `#if B220_AUDIO_LOOPBACK &&
 * B220_I2S_BEFORE_DSP` - the same condition this region used to carry, because
 * the diagnostic borrows the I2S0 handles and must never run next to the
 * production codec, which owns that same RX/TX DMA.
 *
 * Phase 3-14 : standalone audio LOOPBACK diagnostic (B220_AUDIO_LOOPBACK).
 * Why a loopback and not another register: the link work has proven everything
 * it can prove without audio - HBI answers, the host-boot upload is byte-correct
 * (mismatching_words=0 before any release write) - while reg 0x0030 ("app
 * status") has NEVER gone non-zero.  This product's application may simply never
 * update that register, so the AUDIO PATH is the test now, not the register: if
 * the DSP is running its application, whatever it sends on I2S RX must come back
 * out of I2S TX (the DSP is the audio source/sink here, the ESP32 is just the
 * I2S clock master), and the PA has to be on for a human to hear it.
 *
 * Standalone by design: B220_AudioLoopbackStart() is called from the board
 * constructor AFTER InitializeAudioDsp() - i.e. after the DSP bring-up attempt
 * has finished, whatever its verdict (the early returns in that function do not
 * skip this).  It creates exactly ONE task, once per boot, only when
 * B220_AUDIO_LOOPBACK is 1.
 *
 * The echo reuses the handles the bring-up already left open; see
 * B220_AudioLoopbackStart() below.  A second i2s_new_channel() on I2S0 would
 * fail ("the %s channel on I2S%d is switched from master to slave..." is the
 * duplex-demotion path, and the port itself can only be created once), so this
 * file never creates a channel of its own.
 * ------------------------------------------------------------------------- */
#include <esp_timer.h>
#include <math.h>   /* cos() - used ONCE per boot, in B220_ToneTableInit() only */
#include <stdio.h>  /* snprintf() for the raw-sample hex line below */

#define B220_LOOPBACK_FRAMES       240 /* 240 stereo frames = 5 ms @ 48 kHz */
#define B220_LOOPBACK_FRAME_BYTES  (2 * sizeof(int16_t)) /* L + R, 16-bit each */
#define B220_LOOPBACK_BLOCK_BYTES  (B220_LOOPBACK_FRAMES * B220_LOOPBACK_FRAME_BYTES)
#define B220_LOOPBACK_READ_TIMEOUT_MS  100
#define B220_LOOPBACK_WRITE_TIMEOUT_MS 100
#define B220_LOOPBACK_LOG_PERIOD_US    2000000 /* one rate line every ~2 s */
/* prio 4 = xiaozhi's own audio_output task (main/audio/audio_service.cc), i.e.
 * below audio_input (8) and equal to the playback path, and the task blocks in
 * i2s_channel_read() most of the time - it does not need to outrank anything to
 * keep a 30 ms DMA buffer (6 descriptors x 240 frames) fed. */
#define B220_LOOPBACK_TASK_PRIO    4
#if B220_I2S_TX_VARIANT_SWEEP
/* Phase 3-19: the sweep reconfigures the TX channel from inside this task
 * (i2s_channel_reconfig_std_slot() -> i2s_std_set_slot() -> DMA re-allocation)
 * and formats the long step/stat lines, so it gets a little more headroom than
 * the plain "memcpy-ish loop" below. */
#define B220_LOOPBACK_TASK_STACK   4096 /* bytes */
#else
#define B220_LOOPBACK_TASK_STACK   3072 /* bytes; the body is a memcpy-ish loop */
#endif
#define B220_LOOPBACK_PA_REG       0x02 /* AW9106B reg 0x02 = GPIO_OUTPUT_A */
#define B220_LOOPBACK_PA_ON        0x02 /* bit 1 of group A = audio PA on */
#define B220_LOOPBACK_PA_OFF       0x00 /* mute/off */

/* Registers 0x12 (GPMD_A) and 0x04 (GPIO_CFG_A): the group-A DRIVE MODE, i.e. the
 * precondition without which register 0x02 above does not move a single pin.
 *
 * Proven on hardware with the B220_PA_BIT_FINDER walk (its step table is further
 * down this file): with GPMD_A = 0x08 - where InitializeBacklight()'s replayed
 * stock tail used to leave it - the group-A outputs drive NOTHING, so there was no
 * sound for steps 1..9 including reg 0x02 <- 0xFF.  The 1 kHz tone became audible
 * EXACTLY at the steps that wrote GPMD_A <- 0x02 together with GPIO_CFG_A <- 0x00
 * (and stayed audible until power-off).  GPMD_A = 0x02 is therefore not a
 * preference but the switch that makes SHDN reach the AW8737S at all:
 *   - it must be written BEFORE the SHDN pulse (otherwise the pulse is a no-op on
 *     a pin that never moves), and
 *   - nothing may write 0x12/0x04 again afterwards (the stock tail's 0x12 <- 0x08
 *     ran AFTER the enable and silently switched the PA back off - the whole
 *     difference between no sound and sound on this board).
 * Both halves are enforced below: the enable sequence writes them first, and the
 * post-enable freeze in b220_aw9106_frozen_refuses() refuses any later change. */
#define B220_AW9106_REG_OUT_A       B220_LOOPBACK_PA_REG /* 0x02 GPIO_OUTPUT_A  */
#define B220_AW9106_REG_GPIO_CFG_A  0x04 /* GPIO_CFG_A: per-pin GPIO config      */
#define B220_AW9106_REG_GPMD_A      0x12 /* GPMD_A: group-A drive mode select    */
/*
 * GPMD_A = 0x0A, NOT 0x02.
 *
 * 0x02 is the group-A "GPIO/logic output" bit, which is what lets group-A OUT1
 * move the AW8737S SHDN line (the PA enable walk proved that).  0x08 is the
 * group-A "LED driver" bit (AW9106B datasheet tables 4/5: OUTx power up as GPIO
 * and are switched to LED driver mode through GPMD_A/GPMD_B), which is what the
 * six common-anode RGB LEDs on OUT0/OUT1/OUT2 need - and the stock tail's
 * 0x12 <- 0x08 was deleted from this port precisely because on its own it
 * silenced the PA.
 *
 * 0x0A sets BOTH bits, and was measured on this hardware: with GPMD_A = 0x0A the
 * RGB channels light (0x20 = red, 0x21 = green, 0x22 = blue, all three = white)
 * AND the speaker still works.  So the LED driver bit and the SHDN logic bit do
 * not have to be exclusive after all; the value this port pins GPMD_A to is the
 * combination.  Everything that writes 0x12 must use THIS value, because a second
 * different value would be a mode change on a latching amplifier input.
 */
#define B220_AW9106_GPMD_A_GPIO     0x0A /* GPIO bit | LED-driver bit (see above) */
#define B220_AW9106_GPIO_CFG_A_GPIO 0x00 /* every group-A pin a plain output     */

/* ============================ AW8737S SHDN pulse ==========================
 * The speaker amplifier is an AW8737S.  Its SHDN pin (A4) is active-low and is
 * driven by the AW9106B group-A output OUT_A1 = register 0x02 bit 1.  The chip
 * is NOT a plain enable: after it has registered a shutdown (SHDN held low for
 * longer than TOFF, the datasheet recommends 1 ms) it selects its OPERATING
 * MODE from the NUMBER OF RISING EDGES that follow:
 *
 *     1 rising edge  -> mode 1, 1.2 W   <- the only mode this board wants
 *     2 rising edges -> mode 2, 1.0 W
 *     3 rising edges -> mode 3, 0.8 W
 *     4 rising edges -> mode 4, NCN function OFF = NO OUTPUT AT ALL
 *     more than four is not allowed.
 *
 * Four edges is therefore exactly the "DAC1_P/DAC1_M carry the 1 kHz tone and
 * the speaker connector shows nothing" defect: the amplifier is in mode 4.  The
 * cause is that more than one place used to write register 0x02 (backlight
 * init, the ZL38063 group-A diagnostic, the loopback's PA-on), each 0 -> 1
 * transition counting as an edge.
 *
 * The rule this file now enforces: exactly ONE controlled enable sequence per
 * boot - 0x02 <- 0x00, hold >= 2 ms, 0x02 <- 0x02 - and nothing else may ever
 * write 0x02, because any later 0 -> 1 transition on bit 1 adds an edge and
 * re-selects the mode.  Every write that reaches the chip goes through
 * b220_aw9106_write_reg02(), which counts them, counts the rising edges, and
 * shouts when the count exceeds one. */
#define B220_PA_SHDN_MAX_RISING_EDGES 1u
/* The low hold.  The datasheet's TOFF floor is 1 ms, but the AW9106B is written
 * over I2C and this is also the margin for the chip's own debounce, so the
 * sequence holds 2 ms - twice the requirement, and the value the log prints. */
#define B220_PA_SHDN_LOW_HOLD_US      2000

/* ---------------------------------------------------------------------------
 * The single board instance's AW9106B device handle.  InitializeBacklight() sets
 * it; the audited reg-0x02 writer, the PA enable sequence and the ZL38063
 * accessor are its only readers.  It stays null when the AW9106B did not come up,
 * in which case every writer above reports ESP_ERR_INVALID_STATE and no reg 0x02
 * write is counted (nothing reached the bus).  It is declared HERE, before the
 * PA helpers, because they are defined before InitializeBacklight().
 *
 * This is deliberately NOT behind `#if B220_ZL_DIAG || B220_AUDIO_LOOPBACK` any
 * more: the handle is the PA path's own state now, and a build that wants neither
 * the diagnostic nor the loopback still needs b220_aw9106_set_gpio_output_a() to
 * exist (the ZL38063 accessor symbol is weak on the caller's side, so leaving the
 * definition in always is what keeps every switch combination linking).
 * ------------------------------------------------------------------------- */
static i2c_master_dev_handle_t s_board_aw9106_dev = nullptr;
/* File-scope mirror of the I2C0 master bus handle, filled in by
 * InitializeBacklight() the moment the bus exists.  Read by the two accessors at
 * the bottom of this file: b220_led_i2c_bus() (always) and
 * b220_button_discovery_i2c_bus() (discovery builds only).  Borrowers only ever
 * ADD devices to this bus - none of them creates or frees it. */
static i2c_master_bus_handle_t s_board_i2c_bus = nullptr;

/* ---------------------------------------------------------------------------
 * The audit state.
 *
 * NOT volatile, on purpose: `++` on a volatile-qualified object is deprecated in
 * C++20 and this project builds with -Werror, and there is no real concurrency to
 * protect against - every write to reg 0x02 happens either from the boot task
 * (InitializeBacklight(), inside the board constructor) or from the single PA
 * enable sequence, which is itself latched by s_pa_enable_done.  The counters and
 * the guard are all touched from one thread at a time, and the enable sequence is
 * the only code that runs later (from the loopback task).
 *
 * s_pa_enable_done is the singleton guard that makes the enable sequence
 * unrepeatable even if a future call site is added by accident; s_pa_enable_result
 * remembers its verdict for the repeat call.
 * ------------------------------------------------------------------------- */
static uint32_t s_aw9106_reg02_writes;        /* every 0x02 write           */
static uint32_t s_aw9106_reg02_rising_edges;  /* bit 1: 0 -> 1              */
static uint32_t s_aw9106_reg02_warned_edges;  /* loud-warning latch         */
static int s_aw9106_reg02_last_bit1 = -1;     /* -1 = no write seen yet     */
/* The same tracking for the two drive-mode registers and the last VALUE of 0x02:
 * the boot log has to state the final state of 0x12/0x04/0x02, and the post-enable
 * freeze has to know what "unchanged" means.  A read-back is NOT used for either:
 * the AW9106B on this bus does not reliably answer register reads, while the
 * tracked value is exactly what went over the wire (and the write log above it
 * names every one of them). */
static int s_aw9106_reg02_last_value = -1;    /* last 0x02 value written    */
static uint32_t s_aw9106_gpmd_a_writes;       /* every 0x12 write           */
static int s_aw9106_gpmd_a_last = -1;         /* last 0x12 value written    */
static uint32_t s_aw9106_gpio_cfg_a_writes;   /* every 0x04 write           */
static int s_aw9106_gpio_cfg_a_last = -1;     /* last 0x04 value written    */
static bool s_aw9106_pa_locked;               /* PA latched: freeze 0x12/0x04/0x02 */
static bool s_pa_enable_done;                 /* one edge per boot          */
static esp_err_t s_pa_enable_result = ESP_ERR_INVALID_STATE;

/*
 * The ZL38063 diagnostic (zl38063_dsp.c) cannot reach the board's I2C bus, so
 * the board lends it this one accessor.  It is DEFINED at the bottom of this
 * file, because it needs s_board_aw9106_dev (set by InitializeBacklight()) -
 * and it is the same weak declaration the diagnostic carries, so a build that
 * drops the diagnostic still links.
 */
extern "C" esp_err_t b220_aw9106_set_gpio_output_a(uint8_t value) __attribute__((weak));

/* "0x02" / "unset" - one tracked register value, as the final-state line and the
 * freeze refusal below print it. */
static void b220_aw9106_fmt_reg(char* out, size_t out_len, int last)
{
    if (last < 0) {
        snprintf(out, out_len, "unset");
    } else {
        snprintf(out, out_len, "0x%02X", (unsigned)last);
    }
}

/* ---------------------------------------------------------------------------
 * The post-enable freeze: the enforcement half of "from the moment the PA is
 * enabled, nothing else touches 0x12/0x04/0x02".
 *
 * The enable sequence ends by latching GPMD_A = 0x02, GPIO_CFG_A = 0x00 and
 * 0x02 = 0x02 - the only state in which this board has ever made sound.  From
 * then on a write that would CHANGE one of those three registers is refused here
 * (and logged loudly) instead of reaching the chip, because
 *    - a new 0 -> 1 on reg 0x02 bit 1 is one more SHDN rising edge, i.e. one more
 *      AW8737S mode selection (mode 2, mode 3, mode 4 = NCN function OFF = NO
 *      OUTPUT AT ALL), and
 *    - moving GPMD_A off 0x02 switches the group-A outputs off again, which is
 *      exactly what the stock tail's 0x12 <- 0x08 did to the PA before.
 * Writing the SAME value again is allowed on purpose: it cannot move the pin, so
 * it cannot add an edge, and it is the "re-assert 0x12 <- 0x02" idiom that the
 * enable sequence itself uses.
 *
 * One caller may lift the freeze: the bring-up walk under B220_PA_BIT_FINDER,
 * whose whole purpose is to write these registers on purpose, and which says so
 * in the log (b220_aw9106_pa_unlock()).  With that switch at 0 nothing can lift
 * it, and every write below that would matter is a refusal in the boot log.
 * ------------------------------------------------------------------------- */
static bool b220_aw9106_frozen_refuses(uint8_t reg, uint8_t value, int last, const char* why)
{
    if (!s_aw9106_pa_locked) {
        return false;
    }
    if ((last >= 0) && (value == (uint8_t)last)) {
        ESP_LOGI(TAG, "pa: no-op re-assert of reg 0x%02X <- 0x%02X (%s): the PA enable is latched "
                      "and the value does not change, so SHDN does not move (allowed)",
                 (unsigned)reg, (unsigned)value, (why != nullptr) ? why : "?");
        return false;
    }

    char prev[12];
    b220_aw9106_fmt_reg(prev, sizeof(prev), last);
    ESP_LOGE(TAG, "pa: REFUSED a change of reg 0x%02X <- 0x%02X by %s (latched value %s): the PA "
                  "enable is live and changing this register would move the AW8737S out of mode 1 "
                  "or switch the group-A outputs off - the write was NOT sent to 0x%02X",
             (unsigned)reg, (unsigned)value, (why != nullptr) ? why : "?", prev,
             (unsigned)B220_AW9106_ADDR);
    return true;
}

/*
 * The line the boot is read from: the state of the three registers that decide
 * whether the speaker can work at all, plus the two counters.  Called at the end
 * of the enable sequence on success AND on failure, so the log always ends with
 * what the chip was last told.  The tracked values are what actually went over
 * the wire (the AW9106B on this bus does not reliably answer read-backs, and the
 * per-write lines above name every write that produced them).
 */
static void b220_pa_log_final_state(void)
{
    char gpmd[12];
    char cfg[12];
    char out_a[12];
    b220_aw9106_fmt_reg(gpmd, sizeof(gpmd), s_aw9106_gpmd_a_last);
    b220_aw9106_fmt_reg(cfg, sizeof(cfg), s_aw9106_gpio_cfg_a_last);
    b220_aw9106_fmt_reg(out_a, sizeof(out_a), s_aw9106_reg02_last_value);

    const uint32_t edges = s_aw9106_reg02_rising_edges;
    ESP_LOGI(TAG, "pa: final AW9106 state 0x%02X(GPMD_A)=%s 0x%02X(GPIO_CFG_A)=%s 0x%02X=%s | "
                  "0x%02X writes=%lu, SHDN rising edges=%lu%s",
             (unsigned)B220_AW9106_REG_GPMD_A, gpmd, (unsigned)B220_AW9106_REG_GPIO_CFG_A, cfg,
             (unsigned)B220_AW9106_REG_OUT_A, out_a, (unsigned)B220_AW9106_REG_OUT_A,
             (unsigned long)s_aw9106_reg02_writes, (unsigned long)edges,
             (edges == B220_PA_SHDN_MAX_RISING_EDGES) ? " (AW8737S mode 1)"
                                                      : " (NOT mode 1 - check the log above)");
}

#if B220_PA_BIT_FINDER
/* The walk exists to write 0x12/0x04/0x02 on purpose, so it may lift the freeze -
 * loudly, because lifting it means the AW8737S can be moved out of mode 1 again.
 * Guarded by the switch: with B220_PA_BIT_FINDER 0 nothing calls it and the
 * function does not exist (-Wunused-function would fail the -Werror build). */
static void b220_aw9106_pa_unlock(const char* why)
{
    if (!s_aw9106_pa_locked) {
        return;
    }
    s_aw9106_pa_locked = false;
    ESP_LOGW(TAG, "pa: post-enable freeze LIFTED by %s - 0x%02X/0x%02X/0x%02X are writable again "
                  "and the AW8737S can be moved out of mode 1 (diagnostic only)",
             (why != nullptr) ? why : "?", (unsigned)B220_AW9106_REG_GPMD_A,
             (unsigned)B220_AW9106_REG_GPIO_CFG_A, (unsigned)B220_AW9106_REG_OUT_A);
}
#endif /* B220_PA_BIT_FINDER */

/*
 * The audited writers for the two DRIVE-MODE registers.  Same shape as the 0x02
 * writer right below: they log every write with the register's name, count it,
 * keep the last value for the final-state line, and refuse a change once the PA
 * is latched.  They exist so that the 0x12/0x04 writes of the enable sequence -
 * the ones that decide whether SHDN can move at all - are as visible in the boot
 * log as the 0x02 writes always were.
 */
static esp_err_t b220_aw9106_write_gpmd_a(i2c_master_dev_handle_t dev, uint8_t value,
                                          const char* why)
{
    if (dev == nullptr) {
        /* No device at 0x5B: nothing was written, so nothing is counted. */
        return ESP_ERR_INVALID_STATE;
    }
    if (b220_aw9106_frozen_refuses(B220_AW9106_REG_GPMD_A, value, s_aw9106_gpmd_a_last, why)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t b[2] = {B220_AW9106_REG_GPMD_A, value};
    const esp_err_t err = i2c_master_transmit(dev, b, sizeof(b), 100);

    const uint32_t n = ++s_aw9106_gpmd_a_writes;
    if (err == ESP_OK) {
        s_aw9106_gpmd_a_last = value;
    }
    ESP_LOGI(TAG, "pa: AW9106B 0x%02X reg 0x%02X (GPMD_A) <- 0x%02X (%s) -> %s | write #%lu, "
                  "group-A drive mode %s",
             (unsigned)B220_AW9106_ADDR, (unsigned)B220_AW9106_REG_GPMD_A, (unsigned)value,
             (why != nullptr) ? why : "?", esp_err_to_name(err), (unsigned long)n,
             (value == B220_AW9106_GPMD_A_GPIO)
                 ? "= GPIO/output: SHDN can reach the AW8737S"
                 : "!= 0x02: the group-A outputs stay idle and SHDN cannot move");
    return err;
}

static esp_err_t b220_aw9106_write_gpio_cfg_a(i2c_master_dev_handle_t dev, uint8_t value,
                                              const char* why)
{
    if (dev == nullptr) {
        /* No device at 0x5B: nothing was written, so nothing is counted. */
        return ESP_ERR_INVALID_STATE;
    }
    if (b220_aw9106_frozen_refuses(B220_AW9106_REG_GPIO_CFG_A, value, s_aw9106_gpio_cfg_a_last,
                                   why)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t b[2] = {B220_AW9106_REG_GPIO_CFG_A, value};
    const esp_err_t err = i2c_master_transmit(dev, b, sizeof(b), 100);

    const uint32_t n = ++s_aw9106_gpio_cfg_a_writes;
    if (err == ESP_OK) {
        s_aw9106_gpio_cfg_a_last = value;
    }
    ESP_LOGI(TAG, "pa: AW9106B 0x%02X reg 0x%02X (GPIO_CFG_A) <- 0x%02X (%s) -> %s | write #%lu, "
                  "group-A pin config %s",
             (unsigned)B220_AW9106_ADDR, (unsigned)B220_AW9106_REG_GPIO_CFG_A, (unsigned)value,
             (why != nullptr) ? why : "?", esp_err_to_name(err), (unsigned long)n,
             (value == B220_AW9106_GPIO_CFG_A_GPIO) ? "= 0x00 (all group-A pins are outputs)"
                                                    : "!= 0x00 (a group-A pin is not an output)");
    return err;
}

/*
 * THE single writer of AW9106B register 0x02.  Everything that programs the
 * audio PA (InitializeBacklight(), b220_pa_enable_one_pulse(), the ZL38063
 * accessor below) funnels through here, so the write count and the rising-edge
 * count cannot drift away from what the chip actually saw.  After the PA enable
 * it also refuses a change (b220_aw9106_frozen_refuses() above), so a later writer
 * cannot add a second SHDN edge and move the AW8737S out of mode 1.
 *
 * The caller passes the device handle explicitly rather than reading
 * s_board_aw9106_dev, because InitializeBacklight() must audit its own 0x02
 * write before it has published the handle.
 */
static esp_err_t b220_aw9106_write_reg02(i2c_master_dev_handle_t dev, uint8_t value,
                                         const char* why)
{
    if (dev == nullptr) {
        /* No device at 0x5B: nothing was written, so nothing is counted. */
        return ESP_ERR_INVALID_STATE;
    }
    if (b220_aw9106_frozen_refuses(B220_AW9106_REG_OUT_A, value, s_aw9106_reg02_last_value, why)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t b[2] = {B220_LOOPBACK_PA_REG, value};
    const esp_err_t err = i2c_master_transmit(dev, b, sizeof(b), 100);

    const uint32_t n = ++s_aw9106_reg02_writes;
    const int bit1 = (value >> 1) & 0x1;
    if (err == ESP_OK) {
        if (s_aw9106_reg02_last_bit1 == 0 && bit1 == 1) {
            const uint32_t edges = ++s_aw9106_reg02_rising_edges;
            if (edges > B220_PA_SHDN_MAX_RISING_EDGES &&
                s_aw9106_reg02_warned_edges < edges) {
                s_aw9106_reg02_warned_edges = edges;
                ESP_LOGW(TAG, "pa: !!! AW8737S SHDN RISING EDGE #%lu (bit 1 of reg "
                              "0x%02X went 0 -> 1) - only %u edge is allowed, the "
                              "amplifier is now in a HIGHER mode (2 edges = mode 2 "
                              "1.0 W, 3 = mode 3 0.8 W, 4 = mode 4 NCN OFF = NO "
                              "OUTPUT); offending write #%lu by %s !!!",
                         (unsigned long)edges, (unsigned)B220_LOOPBACK_PA_REG,
                         (unsigned)B220_PA_SHDN_MAX_RISING_EDGES,
                         (unsigned long)n, (why != nullptr) ? why : "?");
            }
        }
        s_aw9106_reg02_last_bit1 = bit1;
        s_aw9106_reg02_last_value = value;
    }

    ESP_LOGI(TAG, "pa: AW9106B 0x%02X reg 0x%02X <- 0x%02X (%s) -> %s | write #%lu, "
                  "SHDN bit1=%d, rising edges so far=%lu",
             (unsigned)B220_AW9106_ADDR, (unsigned)B220_LOOPBACK_PA_REG,
             (unsigned)value, (why != nullptr) ? why : "?", esp_err_to_name(err),
             (unsigned long)n, bit1, (unsigned long)s_aw9106_reg02_rising_edges);
    return err;
}

/*
 * The one and only PA enable - five steps, in this exact order:
 *
 *   1. 0x12 (GPMD_A) <- 0x02: group A into the drive mode in which its outputs
 *      really drive the pin.  Without this the pulse below does nothing at all -
 *      proven on hardware: with GPMD_A = 0x08 the group-A register can be 0x00,
 *      0xFF or any other value and SHDN never moves (steps 1..9 of the walk were
 *      silent), and the 1 kHz tone became audible exactly at the steps that put
 *      GPMD_A back to 0x02;
 *   2. 0x04 (GPIO_CFG_A) <- 0x00: every group-A pin a plain output, the second
 *      half of the configuration that was audible in that same walk;
 *   3. 0x02 <- 0x00 and hold it there for B220_PA_SHDN_LOW_HOLD_US (2 ms).  This
 *      is the >= TOFF low period the datasheet requires BEFORE the pulse series,
 *      without which the chip has not registered the shutdown and does not count
 *      the edges that follow;
 *   4. 0x02 <- 0x02 - exactly one rising edge -> AW8737S mode 1 (1.2 W);
 *   5. latch: 0x12/0x04/0x02 are frozen for the rest of the boot, so nothing that
 *      runs later - the stock tail's 0x12 <- 0x08 (which used to switch the PA
 *      straight back off), a diagnostic, a bare re-assert of 0x02 - can undo the
 *      enable or add an edge.
 *
 * The singleton guard is the other half of the fix: the mode is selected by a
 * COUNT, so a second enable sequence anywhere in the boot (a retry path, a
 * diagnostic, the loopback start running twice) would silently move the
 * amplifier to mode 2 and beyond.  A repeat call therefore does NOT touch the
 * bus; it logs a warning and the boot stays at one edge.
 *
 * A failed step 1/2 does NOT abort the sequence: the error is logged, the pulse
 * is still attempted (it costs nothing but the single edge), and the final-state
 * line below then states which mode value really went over the wire.  A failed
 * step 3 DOES abort - without the low period the edge would not select a mode.
 *
 * The two log lines the boot is read from:
 *   pa: SHDN enable sequence: 0x12(GPMD_A)<-0x02, 0x04(GPIO_CFG_A)<-0x00,
 *       0x02<-0x00, hold 2 ms, 0x02<-0x02 (exactly 1 rising edge -> AW8737S mode
 *       1, 1.2 W)
 *   pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02 |
 *       0x02 writes=N, SHDN rising edges=1 (AW8737S mode 1)
 */
static esp_err_t b220_pa_enable_one_pulse(const char* tag)
{
    if (s_pa_enable_done) {
        ESP_LOGW(TAG, "pa: enable sequence already ran this boot (requested again by "
                      "%s) - IGNORED on purpose: a second 0 -> 1 on reg 0x%02X bit 1 "
                      "would be SHDN rising edge #%lu and would move the AW8737S out "
                      "of mode 1 (1.2 W)",
                 (tag != nullptr) ? tag : "?", (unsigned)B220_LOOPBACK_PA_REG,
                 (unsigned long)(s_aw9106_reg02_rising_edges + 1u));
        return s_pa_enable_result;
    }
    s_pa_enable_done = true;

    if (s_board_aw9106_dev == nullptr) {
        /* No AW9106B at 0x5B (InitializeBacklight() failed): nothing can be
         * written, so no edge is produced and no write is counted. */
        s_pa_enable_result = ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG, "pa: NO PA enable possible - the AW9106B at 0x%02X never came "
                      "up, so SHDN cannot be pulsed (the speaker stays silent)",
                 (unsigned)B220_AW9106_ADDR);
        return s_pa_enable_result;
    }

    ESP_LOGI(TAG, "pa: SHDN enable sequence: 0x%02X(GPMD_A)<-0x%02X, 0x%02X(GPIO_CFG_A)<-0x%02X, "
                  "0x%02X<-0x%02X, hold %u ms, 0x%02X<-0x%02X (exactly %u rising edge -> AW8737S "
                  "mode 1, 1.2 W)",
             (unsigned)B220_AW9106_REG_GPMD_A, (unsigned)B220_AW9106_GPMD_A_GPIO,
             (unsigned)B220_AW9106_REG_GPIO_CFG_A, (unsigned)B220_AW9106_GPIO_CFG_A_GPIO,
             (unsigned)B220_LOOPBACK_PA_REG, (unsigned)B220_LOOPBACK_PA_OFF,
             (unsigned)(B220_PA_SHDN_LOW_HOLD_US / 1000u),
             (unsigned)B220_LOOPBACK_PA_REG, (unsigned)B220_LOOPBACK_PA_ON,
             (unsigned)B220_PA_SHDN_MAX_RISING_EDGES);

    /* Step 1/5: the drive mode FIRST - see the register block at the top of this
     * file.  GPMD_A = 0x02 is what makes the step-4 pulse reach SHDN at all. */
    esp_err_t err = b220_aw9106_write_gpmd_a(s_board_aw9106_dev, B220_AW9106_GPMD_A_GPIO,
                                             "PA enable step 1/5: GPMD_A <- GPIO mode");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pa: step 1/5 (GPMD_A <- 0x%02X) FAILED (%s) - the pulse below is sent "
                      "anyway, but with GPMD_A != 0x%02X the group-A outputs do not drive SHDN "
                      "and the speaker stays silent",
                 (unsigned)B220_AW9106_GPMD_A_GPIO, esp_err_to_name(err),
                 (unsigned)B220_AW9106_GPMD_A_GPIO);
    }

    /* Step 2/5: all group-A pins as plain outputs. */
    const esp_err_t err_cfg = b220_aw9106_write_gpio_cfg_a(
        s_board_aw9106_dev, B220_AW9106_GPIO_CFG_A_GPIO, "PA enable step 2/5: GPIO_CFG_A <- 0x00");
    if (err_cfg != ESP_OK) {
        ESP_LOGE(TAG, "pa: step 2/5 (GPIO_CFG_A <- 0x%02X) FAILED (%s) - carrying on; the "
                      "final-state line below states what really got written",
                 (unsigned)B220_AW9106_GPIO_CFG_A_GPIO, esp_err_to_name(err_cfg));
        if (err == ESP_OK) {
            err = err_cfg;
        }
    }

    /* Step 3/5: shutdown registered (SHDN low, >= TOFF).  This one aborts. */
    const esp_err_t err_low =
        b220_aw9106_write_reg02(s_board_aw9106_dev, B220_LOOPBACK_PA_OFF,
                                "PA enable step 3/5: low hold (shutdown registered)");
    if (err_low != ESP_OK) {
        ESP_LOGE(TAG, "pa: step 3/5 (0x%02X <- 0x%02X) FAILED (%s) - the enable pulse is NOT "
                      "sent, because a rising edge without the low period would not select a mode",
                 (unsigned)B220_LOOPBACK_PA_REG, (unsigned)B220_LOOPBACK_PA_OFF,
                 esp_err_to_name(err_low));
        if (err == ESP_OK) {
            err = err_low;
        }
    } else {
        /* A busy-wait, not vTaskDelay(): the low period must really be >= 2 ms, and
         * vTaskDelay(1) can only express whole 1 ms ticks (1 ms would be under the
         * margin).  2 ms once per boot is free. */
        esp_rom_delay_us(B220_PA_SHDN_LOW_HOLD_US);

        /* Step 4/5: the single rising edge. */
        const esp_err_t err_edge = b220_aw9106_write_reg02(
            s_board_aw9106_dev, B220_LOOPBACK_PA_ON, "PA enable step 4/5: the one rising edge");

        ESP_LOGI(TAG, "pa: enable sequence done: %lu write(s) to reg 0x%02X this boot, "
                      "%lu rising edge(s) on bit 1 (AW8737S mode %s), last result %s",
                 (unsigned long)s_aw9106_reg02_writes, (unsigned)B220_LOOPBACK_PA_REG,
                 (unsigned long)s_aw9106_reg02_rising_edges,
                 (s_aw9106_reg02_rising_edges == B220_PA_SHDN_MAX_RISING_EDGES)
                     ? "1 = 1.2 W"
                     : "NOT 1 - check the log above",
                 esp_err_to_name(err_edge));
        if (err == ESP_OK) {
            err = err_edge;
        }
    }

    /* Step 5/5: freeze the three registers - only if the whole sequence really
     * succeeded.  A failed enable leaves them writable on purpose: there is no PA
     * to protect then, and the final-state line below says what went wrong. */
    if (err == ESP_OK) {
        s_aw9106_pa_locked = true;
        ESP_LOGI(TAG, "pa: 0x%02X(GPMD_A)/0x%02X(GPIO_CFG_A)/0x%02X are now LATCHED - any later "
                      "change is refused by b220_aw9106_frozen_refuses()",
                 (unsigned)B220_AW9106_REG_GPMD_A, (unsigned)B220_AW9106_REG_GPIO_CFG_A,
                 (unsigned)B220_AW9106_REG_OUT_A);
    } else {
        ESP_LOGW(TAG, "pa: the enable sequence did not complete cleanly (%s), so 0x%02X/0x%02X/"
                      "0x%02X stay UNLATCHED",
                 esp_err_to_name(err), (unsigned)B220_AW9106_REG_GPMD_A,
                 (unsigned)B220_AW9106_REG_GPIO_CFG_A, (unsigned)B220_AW9106_REG_OUT_A);
    }

    b220_pa_log_final_state();
    s_pa_enable_result = err;
    return err;
}

/* ==================== production codec bridge (Phase 4) ====================
 * Everything below is additive: the five accessors are the whole interface
 * between this board file and the production audio codec
 * (zl38063_audio_codec.cc/.h, this directory), and none of them is on a proven
 * DSP or PA code path - they only read state that already exists and forward
 * one call to the latched PA enable above.
 *
 * Why an interface at all: the codec must NOT create an I2S channel.  I2S0 is
 * created exactly once, by B220_I2sUpBeforeDspBoot() above and BEFORE the
 * ZL38063 reset - the DSP is an I2S slave that takes its clock from this port's
 * BCLK and answers nothing over HBI until BCLK/WS run - and i2s_new_channel()
 * cannot succeed twice on one port.  The codec therefore borrows the handles
 * through these accessors and never re-creates, disables or deletes them.
 * Their declarations live in b220_audio_share.h, which this file and the codec
 * both include, so a signature drift fails the build instead of the link.
 * ========================================================================= */

/*
 * Is the ZL38063 application really up?  Read ONCE per boot, right after the
 * DSP bring-up, with a READ-ONLY HBI access.
 *
 * Why reg 0x0014 and not reg 0x0030: this product's application never updates
 * 0x0030, so "app status" says nothing here (zl38063_dsp_is_running() reads
 * exactly that register and is therefore not used).  0x0014 is the indicator
 * the stock firmware's own volume/route routine gates its route writes on - it
 * proceeds only when 0x0014 reads 0x200 (see the disassembly quoted at
 * zl38063_dsp.c:3405-3413) - and the port's own downlink experiment already
 * reads it after GO, so a read here is a proven-safe, side-effect-free access.
 * A read that does not answer (the HBI device was never attached) is reported
 * as "not proven", never as a failure of the audio path: the codec only warns.
 */
static bool s_b220_dsp_ready = false;

static bool B220_AudioProbeDspReady(void) {
    unsigned short clk_status = 0u;
    if (VprocTwolfHbiRead(0x0014u, 1, &clk_status) != VPROC_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "codec: reg 0x0014 did not answer over HBI (the HBI device may "
                      "never have been attached) - the ZL38063 readiness is "
                      "UNPROVEN");
        return false;
    }
    const bool ready = (clk_status == 0x0200u);
    ESP_LOGI(TAG, "codec: reg 0x0014 = 0x%04X -> the ZL38063 application is %s "
                  "(0x0200 is the value the stock volume/route routine gates its "
                  "route writes on; reg 0x0030 means nothing on this product)",
             (unsigned)clk_status, ready ? "READY" : "NOT ready");
    return ready;
}

extern "C" i2s_chan_handle_t b220_audio_i2s_tx(void) {
#if B220_I2S_BEFORE_DSP
    return s_i2s_tx;
#else
    /* B220_I2S_BEFORE_DSP is 0: no port is ever created, so the codec gets NULL
     * and reports silence instead of touching a channel that does not exist. */
    return nullptr;
#endif
}

extern "C" i2s_chan_handle_t b220_audio_i2s_rx(void) {
#if B220_I2S_BEFORE_DSP
    return s_i2s_rx;
#else
    return nullptr;
#endif
}

extern "C" esp_err_t b220_audio_pa_enable_once(const char* tag) {
    /* Straight through to the one latched enable sequence: a second caller gets
     * the logged "already ran this boot" warning plus the first call's result. */
    return b220_pa_enable_one_pulse(tag);
}

extern "C" bool b220_audio_dsp_ready(void) {
    return s_b220_dsp_ready;
}

/* =================== Phase 3-20: AW9106B SHDN bit finder ===================
 * B220_PA_BIT_FINDER (config.h) turns the boot into a meter-driven search for
 * the AW9106B output that really drives the AW8737S SHDN pin.
 *
 * Why it exists: the amplifier is measurably powered down (a voltmeter on SHDN
 * reads LOW with this firmware running) while the DSP's DAC1_P/M carry the 1 kHz
 * tone and the whole I2S/upload/release path is verified.  The one documented
 * answer - the stock audio_pa_on() writing reg 0x02 (GPIO_OUTPUT_A) bit 1 - does
 * NOT move SHDN on this board.  So either the bit, the register group or the
 * output DRIVE MODE is wrong, and none of the three can be settled from this
 * side of the wire: the instrument that answers it is the meter on SHDN.  The
 * walk therefore applies ONE candidate setting at a time, holds it long enough
 * to read, and logs exactly what it applied and what came back.
 *
 * The 22 steps, each held B220_FIND_HOLD_MS = 4 s (the restore step 20 s):
 *   1..8   reg 0x02 <- 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, one
 *          group-A output bit at a time (step 2 is the stock PA bit);
 *   9      reg 0x02 <- 0xFF, all group-A outputs high;
 *   10..12 reg 0x12 (GPMD_A) <- 0x02 plus reg 0x04 (GPIO_CFG_A) <- 0x00 / 0x0F /
 *          0xF0 while reg 0x02 <- 0xFF - the drive-mode variants, in case the bit
 *          is right but the pin is not in GPIO mode.  THIS is the step that
 *          settled the PA: run against the old build (whose InitializeBacklight()
 *          replayed the stock tail 0x11(CTL)<-0x1B and 0x12(GPMD_A)<-0x08, so the
 *          walk started with GPMD_A = 0x08, not the GPIO mode the stock
 *          audio_pa_on() assumed) steps 1..9 were silent and the 1 kHz tone became
 *          audible exactly here, with 0x12 = 0x02 and 0x04 = 0x00.  The stock tail
 *          is gone now (that overwrite WAS the bug) and the enable sequence
 *          already latches GPMD_A = 0x02 before the walk starts, so these steps
 *          only re-assert it - unless the walk below lifts the freeze on purpose;
 *   13..21 reg 0x13 (GPMD_B) <- 0x02 puts group B in GPIO mode, then reg 0x03
 *          (GPIO_OUTPUT_B) <- 0x01, 0x02, ..., 0x80 and 0xFF, one bit at a time
 *          - in case SHDN hangs off the second output group;
 *   22     restore the stock-ish configuration (GPMD_A=0x02, GPIO_CFG_A=0x00,
 *          GPMD_B=0x00, OUT_B=0x00) and reg 0x02 <- 0xFF, so whichever group-A
 *          bit is SHDN is left HIGH for a last confirmation.
 *
 * The walk is a DIAGNOSTIC and must be able to write 0x12/0x04/0x02, so it lifts
 * the post-enable freeze first (b220_aw9106_pa_unlock(), logged as a warning) and
 * then sends every one of its writes through the same audited writers as the
 * enable sequence - including 0x12/0x04, which it is the only caller to vary.
 * The reg-0x02 write counter and the SHDN rising-edge counter therefore stay
 * truthful, and both are logged at every step.  The AW8737S selects its operating
 * mode from the NUMBER of rising edges on SHDN, so this walk leaves it in mode
 * 2/3/4 (mode 4 = NCN function OFF = no output at all).  That is EXPECTED and
 * harmless HERE: the question is "which bit moves SHDN at all", not "does the amp
 * still play", and a power cycle restores mode 1.
 *
 * The walk runs in its OWN task rather than inside the loopback task: a 4 s hold
 * there would stop i2s_channel_write() and with it the 1 kHz tone, and "the tone
 * keeps running" is half of what this experiment needs.  The loopback task is
 * therefore never blocked, and a step that both raises SHDN and leaves the amp
 * in a playing mode is audible immediately.
 * ======================================================================== */
#if B220_PA_BIT_FINDER
/* The registers this walk touches.  0x11 (CTL) is only ever read back. */
#define B220_FIND_REG_OUT_A B220_LOOPBACK_PA_REG /* 0x02 = GPIO_OUTPUT_A */
#define B220_FIND_REG_OUT_B 0x03                 /* GPIO_OUTPUT_B        */
#define B220_FIND_REG_GPIO_CFG_A 0x04            /* GPIO_CFG_A           */
#define B220_FIND_REG_CTL 0x11                   /* CTL (read-back only) */
#define B220_FIND_REG_GPMD_A 0x12                /* GPMD_A               */
#define B220_FIND_REG_GPMD_B 0x13                /* GPMD_B               */
#define B220_FIND_START_DELAY_MS 6000u           /* after loopback start */
#define B220_FIND_HOLD_MS 4000u                  /* per step             */
#define B220_FIND_RESTORE_MS 20000u              /* the restore step     */
#define B220_FIND_TASK_STACK 5120                /* bytes                */
#define B220_FIND_TASK_PRIO 3                    /* below the loopback   */

/* The whole chip state in one line, so every step is interpretable alone. */
static const uint8_t s_pa_find_readback_regs[] = {
    B220_FIND_REG_OUT_A, B220_FIND_REG_OUT_B,  B220_FIND_REG_GPIO_CFG_A,
    B220_FIND_REG_CTL,   B220_FIND_REG_GPMD_A, B220_FIND_REG_GPMD_B,
};

static TaskHandle_t s_pa_find_task = nullptr;
static bool s_pa_find_rb_warned = false;

/* A step: up to four pre-writes (drive mode / configuration), then the write the
 * step is about.  A pre_reg slot of 0x00 means "unused" - this board never
 * writes register 0x00, so it is a safe sentinel. */
typedef struct {
    const char* label;
    uint8_t pre_reg[4];
    uint8_t pre_val[4];
    uint8_t reg;
    uint8_t value;
    uint16_t hold_ms;
} b220_pa_find_step_t;

/* Table shorthands (undefined right after the table, they are local to it). */
#define B220_FIND_NO_PRE {0, 0, 0, 0}, {0, 0, 0, 0}
#define B220_FIND_CFG_A(cfg) \
    {B220_FIND_REG_GPMD_A, B220_FIND_REG_GPIO_CFG_A, 0, 0}, {0x02, (cfg), 0, 0}
#define B220_FIND_GPMD_B_GPIO {B220_FIND_REG_GPMD_B, 0, 0, 0}, {0x02, 0, 0, 0}

static const b220_pa_find_step_t s_pa_find_steps[] = {
    /* 1..8: group A (reg 0x02, GPIO_OUTPUT_A) one output bit at a time.  Exactly
     * one bit is high per step, so the meter sees exactly one candidate line and
     * every other candidate is LOW for the whole 4 s hold.  Step 2 is the stock
     * audio_pa_on() bit. */
    {"group A bit0", B220_FIND_NO_PRE, B220_FIND_REG_OUT_A, 0x01, B220_FIND_HOLD_MS},
    {"group A bit1 = the stock PA bit", B220_FIND_NO_PRE, B220_FIND_REG_OUT_A, 0x02,
     B220_FIND_HOLD_MS},
    {"group A bit2", B220_FIND_NO_PRE, B220_FIND_REG_OUT_A, 0x04, B220_FIND_HOLD_MS},
    {"group A bit3", B220_FIND_NO_PRE, B220_FIND_REG_OUT_A, 0x08, B220_FIND_HOLD_MS},
    {"group A bit4", B220_FIND_NO_PRE, B220_FIND_REG_OUT_A, 0x10, B220_FIND_HOLD_MS},
    {"group A bit5", B220_FIND_NO_PRE, B220_FIND_REG_OUT_A, 0x20, B220_FIND_HOLD_MS},
    {"group A bit6", B220_FIND_NO_PRE, B220_FIND_REG_OUT_A, 0x40, B220_FIND_HOLD_MS},
    {"group A bit7", B220_FIND_NO_PRE, B220_FIND_REG_OUT_A, 0x80, B220_FIND_HOLD_MS},
    /* 9: every group-A output high at once. */
    {"group A ALL bits high", B220_FIND_NO_PRE, B220_FIND_REG_OUT_A, 0xFF, B220_FIND_HOLD_MS},
    /* 10..12: the drive-mode variants - GPMD_A back to the GPIO mode the stock
     * init used, with the three GPIO_CFG_A values, and all group-A outputs high.
     * These are the steps that produced sound on hardware (see the block above);
     * the enable sequence already latches 0x12 = 0x02, so the first one is a
     * no-op re-assert and 11/12 vary only 0x04. */
    {"GPMD_A=0x02 + GPIO_CFG_A=0x00 (STOCK INIT) + all A high", B220_FIND_CFG_A(0x00),
     B220_FIND_REG_OUT_A, 0xFF, B220_FIND_HOLD_MS},
    {"GPMD_A=0x02 + GPIO_CFG_A=0x0F + all A high", B220_FIND_CFG_A(0x0F), B220_FIND_REG_OUT_A, 0xFF,
     B220_FIND_HOLD_MS},
    {"GPMD_A=0x02 + GPIO_CFG_A=0xF0 + all A high", B220_FIND_CFG_A(0xF0), B220_FIND_REG_OUT_A, 0xFF,
     B220_FIND_HOLD_MS},
    /* 13..21: group B - GPMD_B as GPIO (step 13), then GPIO_OUTPUT_B one bit at a
     * time and finally all high, in case SHDN is on the second output group. */
    {"group B bit0 (GPMD_B=0x02)", B220_FIND_GPMD_B_GPIO, B220_FIND_REG_OUT_B, 0x01,
     B220_FIND_HOLD_MS},
    {"group B bit1 (GPMD_B=0x02)", B220_FIND_NO_PRE, B220_FIND_REG_OUT_B, 0x02, B220_FIND_HOLD_MS},
    {"group B bit2 (GPMD_B=0x02)", B220_FIND_NO_PRE, B220_FIND_REG_OUT_B, 0x04, B220_FIND_HOLD_MS},
    {"group B bit3 (GPMD_B=0x02)", B220_FIND_NO_PRE, B220_FIND_REG_OUT_B, 0x08, B220_FIND_HOLD_MS},
    {"group B bit4 (GPMD_B=0x02)", B220_FIND_NO_PRE, B220_FIND_REG_OUT_B, 0x10, B220_FIND_HOLD_MS},
    {"group B bit5 (GPMD_B=0x02)", B220_FIND_NO_PRE, B220_FIND_REG_OUT_B, 0x20, B220_FIND_HOLD_MS},
    {"group B bit6 (GPMD_B=0x02)", B220_FIND_NO_PRE, B220_FIND_REG_OUT_B, 0x40, B220_FIND_HOLD_MS},
    {"group B bit7 (GPMD_B=0x02)", B220_FIND_NO_PRE, B220_FIND_REG_OUT_B, 0x80, B220_FIND_HOLD_MS},
    {"group B ALL bits high", B220_FIND_NO_PRE, B220_FIND_REG_OUT_B, 0xFF, B220_FIND_HOLD_MS},
    /* 22: the restore.  Group B back to the reset default (0x03<-0x00, GPMD_B
     * 0x00), GPMD_A back to the GPIO mode with the stock GPIO_CFG_A, and all
     * group-A outputs high so a group-A SHDN bit stays HIGH for the final check. */
    {"RESTORE: GPMD_A=0x02, GPIO_CFG_A=0x00, GPMD_B=0x00, OUT_B=0x00, OUT_A high",
     {B220_FIND_REG_GPMD_A, B220_FIND_REG_GPIO_CFG_A, B220_FIND_REG_GPMD_B, B220_FIND_REG_OUT_B},
     {0x02, 0x00, 0x00, 0x00},
     B220_FIND_REG_OUT_A,
     0xFF,
     B220_FIND_RESTORE_MS},
};

#undef B220_FIND_NO_PRE
#undef B220_FIND_CFG_A
#undef B220_FIND_GPMD_B_GPIO

/* A plain, logged register write for everything the walk touches EXCEPT the three
 * registers that decide whether the PA can work (see b220_pa_find_write() below).
 * The read-back of every touched register is done by
 * b220_pa_find_readback_line(), which shows what the chip really holds rather than
 * what we think we sent. */
static esp_err_t b220_aw9106_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value,
                                       const char* why) {
    if (dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[2] = {reg, value};
    const esp_err_t err = i2c_master_transmit(dev, b, sizeof(b), 100);
    ESP_LOGI(TAG, "pa-find: AW9106B 0x%02X reg 0x%02X <- 0x%02X (%s) -> %s",
             (unsigned)B220_AW9106_ADDR, (unsigned)reg, (unsigned)value,
             (why != nullptr) ? why : "?", esp_err_to_name(err));
    return err;
}

static esp_err_t b220_aw9106_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* out) {
    if ((dev == nullptr) || (out == nullptr)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t v = 0;
    const esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 100);
    if (err == ESP_OK) {
        *out = v;
    }
    return err;
}

/* The finder's single write entry point: the three registers that the PA enable
 * owns - 0x02 (OUT_A), 0x12 (GPMD_A) and 0x04 (GPIO_CFG_A) - are routed into the
 * audited writers above, so the write counters, the SHDN rising-edge counter and
 * the tracked final state keep counting exactly what the chip saw even while a
 * 22-step walk hammers the registers.  Every other register goes through the plain
 * writer. */
static esp_err_t b220_pa_find_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value,
                                    const char* why) {
    if (reg == B220_FIND_REG_OUT_A) {
        return b220_aw9106_write_reg02(dev, value, why);
    }
    if (reg == B220_AW9106_REG_GPMD_A) {
        return b220_aw9106_write_gpmd_a(dev, value, why);
    }
    if (reg == B220_AW9106_REG_GPIO_CFG_A) {
        return b220_aw9106_write_gpio_cfg_a(dev, value, why);
    }
    return b220_aw9106_write_reg(dev, reg, value, why);
}

/* "0x02=0x00 0x03=0x00 ... " - a trailing '!' marks a register whose read-back
 * failed (some AW9106B bus wirings do not answer reads); the writes are
 * unaffected and the meter on SHDN is the real answer. */
static void b220_pa_find_readback_line(i2c_master_dev_handle_t dev, char* out, size_t out_len) {
    size_t off = 0;
    out[0] = '\0';
    for (size_t i = 0; i < (sizeof(s_pa_find_readback_regs) / sizeof(s_pa_find_readback_regs[0]));
         ++i) {
        const uint8_t reg = s_pa_find_readback_regs[i];
        uint8_t v = 0;
        const esp_err_t err = b220_aw9106_read_reg(dev, reg, &v);
        if ((err != ESP_OK) && !s_pa_find_rb_warned) {
            s_pa_find_rb_warned = true;
            ESP_LOGW(TAG,
                     "pa-find: read-back of reg 0x%02X failed (%s) - the AW9106B may not answer "
                     "register reads on this bus; the WRITES are unaffected and the voltmeter on "
                     "SHDN is the real result",
                     (unsigned)reg, esp_err_to_name(err));
        }
        if (off >= out_len) {
            break;
        }
        const int w = snprintf(out + off, out_len - off, "0x%02X=0x%02X%c ", (unsigned)reg,
                               (unsigned)v, (err == ESP_OK) ? ' ' : '!');
        if (w < 0) {
            break;
        }
        off += (size_t)w;
    }
}

static void B220_PaBitFinderTask(void* arg) {
    (void)arg;
    const unsigned total = (unsigned)(sizeof(s_pa_find_steps) / sizeof(s_pa_find_steps[0]));
    i2c_master_dev_handle_t dev = s_board_aw9106_dev;

    ESP_LOGW(
        TAG,
        "pa-find: ================ AW9106B SHDN BIT FINDER (B220_PA_BIT_FINDER=1) ============");
    ESP_LOGW(
        TAG,
        "pa-find: put the meter on the AW8737S SHDN pin (red = SHDN, black = GND) and watch it "
        "for the whole walk; the 1 kHz tone keeps playing throughout");
    ESP_LOGW(TAG,
             "pa-find: SHDN should go HIGH in exactly one step's window and drop again at the next "
             "step - the step number whose window shows it HIGH names the register and bit");
    ESP_LOGW(
        TAG,
        "pa-find: EXPECTED: the AW8737S counts rising edges on SHDN, so this walk will leave it "
        "in mode 2/3/4 (mode 4 = NCN function OFF = NO output at all) - that is fine here, this "
        "diagnostic only asks WHICH BIT MOVES SHDN; a power cycle restores mode 1");
    ESP_LOGW(
        TAG,
        "pa-find: step table: 1-8 = reg 0x%02X <- 0x01..0x80 (group A, one bit each; step 2 = the "
        "stock PA bit), 9 = reg 0x%02X <- 0xFF",
        (unsigned)B220_FIND_REG_OUT_A, (unsigned)B220_FIND_REG_OUT_A);
    ESP_LOGW(TAG,
             "pa-find: 10-12 = 0x%02X(GPMD_A)<-0x02 + 0x%02X(GPIO_CFG_A)<-0x00/0x0F/0xF0 with "
             "0x%02X<-0xFF - the combination that produced sound on hardware (0x00 = the stock "
             "init combination, which is 0x12<-0x02, 0x04<-0x00)",
             (unsigned)B220_FIND_REG_GPMD_A, (unsigned)B220_FIND_REG_GPIO_CFG_A,
             (unsigned)B220_FIND_REG_OUT_A);
    ESP_LOGW(
        TAG,
        "pa-find: 13-21 = 0x%02X(GPMD_B)<-0x02 then reg 0x%02X <- 0x01..0x80, 0xFF (group B, one "
        "bit each)",
        (unsigned)B220_FIND_REG_GPMD_B, (unsigned)B220_FIND_REG_OUT_B);
    ESP_LOGW(TAG,
             "pa-find: 22 = restore (GPMD_A=0x02, GPIO_CFG_A=0x00, GPMD_B=0x00, OUT_B=0x00, all "
             "group-A outputs high)");

    if (dev == nullptr) {
        ESP_LOGE(TAG,
                 "pa-find: NO AW9106B handle - the chip at 0x%02X never came up in "
                 "InitializeBacklight(), so the walk cannot move SHDN (nothing is written)",
                 (unsigned)B220_AW9106_ADDR);
        vTaskDelete(nullptr);
        return;
    }

    /* The enable sequence has already latched 0x12 = 0x02, 0x04 = 0x00 and 0x02 =
     * 0x02 and refuses any change from here on; this walk exists to change them, so
     * it lifts that freeze explicitly and loudly before its first write. */
    b220_aw9106_pa_unlock("the B220_PA_BIT_FINDER walk");

    ESP_LOGI(
        TAG,
        "pa-find: first step in %u s; %u steps x %u ms + a %u ms restore = about %u s of "
        "measuring",
        (unsigned)(B220_FIND_START_DELAY_MS / 1000u), total, (unsigned)B220_FIND_HOLD_MS,
        (unsigned)B220_FIND_RESTORE_MS,
        (unsigned)(((total - 1u) * (uint32_t)B220_FIND_HOLD_MS + B220_FIND_RESTORE_MS) / 1000u));

    vTaskDelay(pdMS_TO_TICKS(B220_FIND_START_DELAY_MS / 2u));
    ESP_LOGI(TAG, "pa-find: step 1 in %u s", (unsigned)(B220_FIND_START_DELAY_MS / 2000u));
    vTaskDelay(pdMS_TO_TICKS(B220_FIND_START_DELAY_MS / 2u));

    char rb[128];
    b220_pa_find_readback_line(dev, rb, sizeof(rb));
    ESP_LOGW(TAG, "pa-find: BASELINE before any write: %s", rb);
    ESP_LOGW(TAG,
             "pa-find: GPMD_A is expected to read 0x02 here (the enable sequence latched it, and "
             "with 0x08 the group-A outputs drive nothing - that overwrite was the bug the earlier "
             "walk found).  Steps 10-12 re-assert 0x02/0x00 and vary only 0x04");

    for (unsigned i = 0; i < total; ++i) {
        const b220_pa_find_step_t* st = &s_pa_find_steps[i];
        esp_err_t err = ESP_OK;

        for (unsigned k = 0; k < 4u; ++k) {
            if (st->pre_reg[k] == 0x00) {
                continue;
            }
            const esp_err_t e =
                b220_pa_find_write(dev, st->pre_reg[k], st->pre_val[k], "pa-find pre-write");
            if (e != ESP_OK) {
                err = e;
            }
        }
        if (st->reg != 0x00) {
            const esp_err_t e = b220_pa_find_write(dev, st->reg, st->value, "pa-find step write");
            if (e != ESP_OK) {
                err = e;
            }
        }

        b220_pa_find_readback_line(dev, rb, sizeof(rb));
        const unsigned hold_s = (unsigned)((st->hold_ms + 500u) / 1000u);
        /* The line the operator greps for while holding the probes. */
        ESP_LOGI(TAG, "pa-find step %u/%u: reg 0x%02X <- 0x%02X (%s) -> %s; check SHDN now", i + 1u,
                 total, (unsigned)st->reg, (unsigned)st->value, st->label,
                 (err == ESP_OK) ? "ok" : esp_err_to_name(err));
        ESP_LOGI(TAG,
                 "pa-find step %u/%u: readback %s| 0x%02X writes=#%lu, SHDN bit1 rising edges=%lu "
                 "(the walk may leave the amp in mode 2/3/4 - expected) | hold %u s",
                 i + 1u, total, rb, (unsigned)B220_FIND_REG_OUT_A,
                 (unsigned long)s_aw9106_reg02_writes, (unsigned long)s_aw9106_reg02_rising_edges,
                 hold_s);

        const uint32_t half_ms = (uint32_t)st->hold_ms / 2u;
        vTaskDelay(pdMS_TO_TICKS(half_ms));
        ESP_LOGI(TAG,
                 "pa-find step %u/%u: %u s left of %u s (reg 0x%02X = 0x%02X still applied) - read "
                 "SHDN NOW",
                 i + 1u, total, (unsigned)((st->hold_ms - half_ms + 500u) / 1000u), hold_s,
                 (unsigned)st->reg, (unsigned)st->value);
        vTaskDelay(pdMS_TO_TICKS((uint32_t)st->hold_ms - half_ms));
    }

    b220_pa_find_readback_line(dev, rb, sizeof(rb));
    ESP_LOGW(TAG, "pa-find: final state: %s", rb);
    ESP_LOGW(
        TAG,
        "pa-find: the meter should read SHDN HIGH now (step 22 leaves all group-A outputs high, "
        "including bit 1): if it does, the bit that moved SHDN is in group A and stays high; if "
        "it does not, the bit was in group B and step 22 has released it");
    ESP_LOGI(TAG, "pa-find: done");
    vTaskDelete(nullptr);
}

/* Started from B220_AudioLoopbackStart(), i.e. from the board constructor: the
 * loopback task owns the 1 kHz tone and is already running when this task's
 * 6 s delay expires. */
static void B220_PaBitFinderStart() {
    if (s_pa_find_task != nullptr) {
        ESP_LOGW(TAG, "pa-find: already started - only one walk per boot");
        return;
    }
    if (xTaskCreate(B220_PaBitFinderTask, "b220_pa_find", B220_FIND_TASK_STACK, nullptr,
                    B220_FIND_TASK_PRIO, &s_pa_find_task) != pdPASS) {
        s_pa_find_task = nullptr;
        ESP_LOGE(TAG, "pa-find: xTaskCreate failed (stack %d bytes, prio %d) - no walk",
                 (int)B220_FIND_TASK_STACK, (int)B220_FIND_TASK_PRIO);
    }
}
#endif /* B220_PA_BIT_FINDER */

/* ===================== the B220_AUDIO_LOOPBACK diagnostic ==================
 * From here to the matching #endif at the end of this region: the Phase
 * 3-14/3-17/3-19 diagnostic only.  The production build (B220_AUDIO_LOOPBACK 0)
 * compiles it out completely - the real codec (zl38063_audio_codec.cc) owns
 * I2S0 RX/TX there, and the two must never be in the same image.
 * ========================================================================= */
#if B220_AUDIO_LOOPBACK && B220_I2S_BEFORE_DSP

/* .bss in internal SRAM is DMA-capable and 16-bit aligned - PSRAM is avoided for
 * the same reason s_i2s_silence above avoids it. */
static int16_t s_b220_loopback_buf[B220_LOOPBACK_FRAMES * 2];

/*
 * Phase 3-17 : the downlink test tone generator (B220_AUDIO_TEST_MODE 1/2/3).
 *
 * Static, precomputed and allocation-free by construction: one 48-sample cycle of
 * a full-scale sine (48 kHz / 48 = 1 kHz) is built ONCE, before the task runs
 * (B220_ToneTableInit(), called from B220_AudioLoopbackStart()), and every mode
 * above then only reads it.  The entries are plain int16 LSB values, i.e. exactly
 * what goes into the I2S buffer, so no fixed-point rescaling can go wrong between
 * the table and the wire.  Mode 2's higher frequencies - which are not integer
 * divisors of the 48-sample cycle - are produced by a 32-bit phase accumulator:
 * the phase is scaled to the table position ((ph * CYCLE_LEN) >> 32) and the bits
 * just below are the interpolation fraction, giving a linear interpolation
 * between neighbouring entries.
 *
 * The task body then does a handful of integer operations per sample and nothing
 * else: no floating point, no malloc, no division, no libm call (interpolation is
 * one multiply and one shift, the amplitude ramp another, and the phase advance an
 * add).
 *
 * The table is a *picture* of a sine, not a measured one - it is written to drive
 * the downlink, so only its peak matters: 32767 * (1 - cos(pi/48)) / 2 = 32760 is
 * the largest exactly centred value that keeps the sampled peak at or below full
 * scale, and B220_ToneTableInit() logs the DC mean and the peak it actually got.
 *
 * Mode 3's beep uses the same table with its own phase accumulator and its OWN
 * ramp counter, restarted at the beginning of every 0.5 s burst: a burst must not
 * begin with a step from digital silence to full scale (that is the click).
 */
#define B220_TONE_HZ         1000u                     /* mode 1 and the beep */
#define B220_TONE_SWEEP_STEPS 8u                       /* mode 2: 1..8 kHz */
#define B220_TONE_SWEEP_STEP_US 1000000                /* one step per second */
/* The table IS one cycle: B220_TONE_CYCLE_LEN = 48 entries, one per sample of the
 * cycle at 48 kHz (48 entries = 1 kHz).  It is deliberately NOT padded to a power of
 * two so that a mask can be used: 48 is not a power of two, `& (48-1)` = `& 47` =
 * 0b101111 clears bits 4 and 5 of the index and folds the waveform onto itself, and a
 * fixed-field shift cannot express a 48-entry cycle at all (see B220_TonePhaseStep -
 * that mistake cost hours).  The index is `(ph * CYCLE_LEN) >> 32` (an exact scaling)
 * and the fraction is taken from the phase bits just below it. */
#define B220_TONE_CYCLE_LEN  48u                       /* 48 kHz / 48 = 1 kHz */
#define B220_TONE_TABLE_LEN  B220_TONE_CYCLE_LEN
/* Bits of the phase accumulator used as the interpolation FRACTION. */
#define B220_TONE_FRAC_BITS  8u
#define B220_TONE_FULL_SCALE 32760                     /* peak of one cycle */
/* ~20 ms at 48 kHz: mode 1 ramps up once at start, mode 3 once per burst. */
#define B220_TONE_RAMP_FRAMES ((B220_I2S_SAMPLE_RATE_HZ * 20u) / 1000u)
/* i2s_channel_write() blocks until the DMA has the block (nothing is generated on
 * the stack), but a mid-block timeout would still show up in write_err. */
#define B220_TONE_WRITE_TIMEOUT_MS 200
/* Mode 3: 500 ms of 1 kHz tone, then 1500 ms of echo, repeating. */
#define B220_BEEP_BURST_US   500000
#define B220_BEEP_PERIOD_US  2000000

/* One fixed-point increment for `hz`: 2^32 is one waveform CYCLE, so one sample
 * advances the phase by `2^32 * hz / sample_rate` = 89 478 485 at 1 kHz.
 *
 * The renderer turns that phase into a table position with
 * `(uint64_t)ph * CYCLE_LEN >> 32`, i.e. it SCALES the top bits to the cycle length
 * instead of shifting a fixed field out of them.  That distinction is the whole
 * trick: shifting a fixed field (say `ph >> 24`) makes the index advance by
 * 2^32-step / 2^24 entries and walk the cycle in 2^24/step samples - at 1 kHz that
 * is a 256-sample cycle, i.e. 187 Hz or an alias of it, not 1 kHz.  Verified
 * numerically: this step with the scaling index gives exactly 1000 positive-going
 * crossings per second at hz = 1000, and the right count at every sweep step. */
static uint32_t B220_TonePhaseStep(unsigned hz) {
    return (uint32_t)(((uint64_t)hz * (1ull << 32)) / B220_I2S_SAMPLE_RATE_HZ);
}

/* Plain LSB values (int16), i.e. exactly what goes into the DMA buffer. */
static int16_t s_b220_tone_table[B220_TONE_TABLE_LEN];

/* One-off table build.  Reads nothing but constants and writes the static table
 * above, so it is safe to call from the start path before the task exists. */
static void B220_ToneTableInit() {
    if (s_b220_tone_table[0] == B220_TONE_FULL_SCALE) {
        return; /* already built (the start path is guarded, this is belt and braces) */
    }
    for (unsigned j = 0; j < B220_TONE_TABLE_LEN; j++) {
        /* One 1 kHz cycle: entry j is the sample at j / CYCLE_LEN of the cycle.  The
         * phase of entry j is j / cycle_len (not (j+0.5)/cycle_len): the renderer's
         * index is the truncated phase position, so entry j stands for that position. */
        const double frac = (double)j / (double)B220_TONE_CYCLE_LEN;
        const double v =
            (double)B220_TONE_FULL_SCALE * cos(2.0 * 3.14159265358979323846 * frac);
        s_b220_tone_table[j] = (int16_t)((v >= 0.0) ? (int)(v + 0.5) : -(int)((-v) + 0.5));
    }
    static_assert((1u << B220_TONE_FRAC_BITS) <= (1u << 16),
                  "B220_TONE_FRAC_BITS must leave the index scaling room in the phase");
    static_assert(B220_TONE_CYCLE_LEN > 0u && B220_TONE_CYCLE_LEN <= 0x10000u,
                  "the cycle length must fit the 16 bits the fraction is taken below");
    /* One cycle must sum to zero (pure AC) and its peak must be the full scale:
     * the proof that the table really is a centred, full-scale sine and not an
     * accidental DC step into the speaker. */
    int64_t sum = 0;
    int peak = 0;
    for (unsigned i = 0; i < B220_TONE_CYCLE_LEN; i++) {
        const int s = s_b220_tone_table[i];
        sum += s;
        if ((s < 0 ? -s : s) > peak) {
            peak = (s < 0 ? -s : s);
        }
    }
    ESP_LOGI(TAG, "tone: 1 kHz cycle table built, %u entries of int16 LSB (one %u-sample "
                  "cycle), one-cycle dc_mean=%ld (of 255 = digital zero), peak=%d; "
                  "phase_step(1 kHz)=%u, %u index step(s) per sample",
             (unsigned)B220_TONE_TABLE_LEN, (unsigned)B220_TONE_CYCLE_LEN,
             (long)(sum / (int64_t)B220_TONE_CYCLE_LEN), peak,
             (unsigned)B220_TonePhaseStep(B220_TONE_HZ),
             (unsigned)(((uint64_t)B220_TonePhaseStep(B220_TONE_HZ) *
                         B220_TONE_CYCLE_LEN) >> 32));
    /* The rate line reports a PEAK, not a mean: for a sine the mean |sample| is
     * 2/pi = 0.63662 of the peak (a full-scale cycle therefore logs mean 20854 for
     * peak 32760).  Printed once, so a reader of the log knows the ratio instead of
     * wondering whether -4 dB means something is wrong. */
    ESP_LOGI(TAG, "tone: expected mean = peak * 2/pi = 0.63662 * peak (peak %d -> mean %d)",
             peak, (int)((peak * 7958) / 12500));
}

/* Render B220_LOOPBACK_FRAMES stereo frames of a sine at phase_step into buf and
 * return the highest absolute sample ACTUALLY written (i.e. after the ramp) - that
 * is the number the rate line reports.
 *
 * The table index is the phase SCALED to the cycle length, `(ph * CYCLE_LEN) >> 32`,
 * and the fraction is taken from the phase bits just below the ones the scaling
 * consumed.  The phase step (see B220_TonePhaseStep) is one full 2^32 turn per cycle,
 * so at 1 kHz the index advances by 48 * 1000 / 48000 = exactly one entry per sample
 * and a 48-sample (1 kHz) cycle comes out.
 *
 * The 20 ms amplitude ramp is a real multiply rather than a shift, so it has more
 * than the 17 values a shift could produce: ramp counts frames 0..960 and the
 * product is divided by 960 again.
 *
 * Phase 3-19: `mono` makes the block ONE 16-bit sample per frame instead of two
 * (I2S_SLOT_MODE_MONO's DMA layout - i2s_std.h:232-235 and active_slot = 1 in
 * i2s_std.c:150).  The caller must then hand i2s_channel_write() half the bytes:
 * B220_LOOPBACK_FRAMES * sizeof(int16_t).  The tone, its 1 kHz rate and the 5 ms
 * the block covers are unchanged either way. */
static unsigned B220_ToneRenderBlock(uint32_t phase_step, uint32_t* phase,
                                     unsigned ramp_from, int16_t* buf, bool mono) {
    unsigned peak = 0;
    uint32_t ph = *phase;
    for (unsigned f = 0; f < B220_LOOPBACK_FRAMES; f++) {
        /* Table position, scaled from the top of the phase to the cycle: this is the
         * step that a fixed-field shift cannot express, because the cycle length (48)
         * is not a power of two (see B220_TonePhaseStep). */
        const unsigned idx = (unsigned)(((uint64_t)ph * B220_TONE_CYCLE_LEN) >> 32);
        if (idx >= B220_TONE_CYCLE_LEN) {
            /* (ph * cycle_len) >> 32 is < cycle_len for ph < 2^32, so this is only a
             * guard against a future edit; it also documents the invariant. */
            break;
        }
        /* Fraction: the bits just below the ones the index consumed. */
        const unsigned frac = (unsigned)(ph >> 16) & ((1u << B220_TONE_FRAC_BITS) - 1u);
        const int a = (int)s_b220_tone_table[idx];
        const int b = (int)s_b220_tone_table[(idx + 1u) % B220_TONE_CYCLE_LEN];
        const int interp = a + (int)(((int32_t)(b - a) * (int32_t)frac) >>
                                     B220_TONE_FRAC_BITS);
        /* Amplitude ramp: 0..B220_TONE_RAMP_FRAMES (0 = silence, full = full scale)
         * over B220_TONE_RAMP_FRAMES frames, full scale from then on.  ramp_from is
         * the frame offset into the ramp, 0 at the start of a burst and
         * B220_TONE_RAMP_FRAMES once it has been spent (modes 1/2). */
        unsigned ramp_frame = ramp_from + f;
        if (ramp_frame > B220_TONE_RAMP_FRAMES) {
            ramp_frame = B220_TONE_RAMP_FRAMES;
        }
        const int32_t s = ((int32_t)interp * (int32_t)ramp_frame) /
                          (int32_t)B220_TONE_RAMP_FRAMES;
        if (mono) {
            buf[f] = (int16_t)s;       /* one 16-bit word per frame (mono) */
        } else {
            buf[2u * f] = (int16_t)s;      /* left  */
            buf[2u * f + 1u] = (int16_t)s; /* right - the same sample, no channel swap */
        }
        const unsigned av = (unsigned)(s < 0 ? -s : s);
        if (av > peak) {
            peak = av;
        }
        ph += phase_step;
    }
    *phase = ph;
    return peak;
}

/* Non-null once the task exists, which makes "created only once" a check rather
 * than a hope. */
static TaskHandle_t s_b220_loopback_task = nullptr;

#if B220_I2S_TX_VARIANT_SWEEP
/* ------------------------------------------------------------------------- *
 * Phase 3-19 : the I2S TX signal-level variant table and its stepper.
 *
 * One row per step, and a row holds the COMPLETE target state rather than a
 * delta, so every step is applied against the same baseline and a failed step
 * cannot leave a half-changed configuration behind for the next one.  Only the
 * six sweepable fields are in the row; the rest of the slot/clock/gpio config
 * comes from s_b220_tx_variant_cfg (what was last successfully written), and
 * data_bit_width is pinned to 16 bits in the applier because it is the DMA word
 * size, not a wire parameter.
 * ------------------------------------------------------------------------- */
#define B220_VARIANT_START_US  6000000 /* step 1 lands ~6 s after the task starts */
#define B220_VARIANT_HOLD_US   3000000 /* every step is held ~3 s */
#define B220_VARIANT_STATS_US  2000000 /* and gets its stats line ~2 s in */
#define B220_VARIANT_COUNT     9

typedef struct {
    const char* desc;      /* what the step calls itself in the log */
    bool        mono;      /* slot_mode: false = STEREO, true = MONO */
    bool        left;      /* MONO only: true = data in the LEFT slot */
    bool        slot32;    /* slot_bit_width: false = 16 bit, true = 32 bit */
    bool        bit_shift; /* true = Philips 1-bit delay (the baseline) */
    bool        bclk_inv;
    bool        ws_inv;
} B220_I2sTxVariant_t;

static const B220_I2sTxVariant_t s_b220_i2s_tx_variants[B220_VARIANT_COUNT] = {
    /* 1 */ {"baseline: 16-bit slot, STEREO, slot_mask=BOTH, bit_shift=1, no invert "
             "(the current configuration - nothing is reconfigured)", false, false, false, true, false, false},
    /* 2 */ {"invert_flags.bclk_inv=1: BCLK inverted (still 1.536 MHz, phase flipped)",
             false, false, false, true, true, false},
    /* 3 */ {"invert_flags.ws_inv=1: WS inverted (48 kHz, phase flipped)",
             false, false, false, true, false, true},
    /* 4 */ {"invert_flags.bclk_inv=1 + ws_inv=1: BOTH inverted",
             false, false, false, true, true, true},
    /* 5 */ {"slot_bit_width=32BIT with 16-bit data MSB-aligned: BCLK 3.072 MHz, WS "
             "still 48 kHz, 64 BCLK/frame", false, false, true, true, false, false},
    /* 6 */ {"slot_mode=MONO, data in the LEFT slot (slot_mask=I2S_STD_SLOT_LEFT); the "
             "DMA frame is ONE 16-bit word, so the block written is 240x16-bit",
             true, true, false, true, false, false},
    /* 7 */ {"slot_mode=MONO, data in the RIGHT slot (slot_mask=I2S_STD_SLOT_RIGHT); the "
             "DMA frame is ONE 16-bit word, so the block written is 240x16-bit",
             true, false, false, true, false, false},
    /* 8 */ {"bit_shift=0: MSB format, first bit on the first BCLK after the WS edge "
             "(no Philips 1-bit delay)", false, false, false, false, false, false},
    /* 9 */ {"back to baseline: 16-bit slot, STEREO, BOTH, bit_shift=1, no invert",
             false, false, false, true, false, false},
};

/* The state that is on the wire right now, fields in the same terms as a row.
 * Seeded with the baseline, which is what B220_I2sUpBeforeDspBoot() left. */
static B220_I2sTxVariant_t s_b220_variant_cur = {
    "baseline", false, false, false, true, false, false};

static unsigned      s_b220_variant_step = 0;   /* 0 = not started, 1..9 = applied */
static int64_t       s_b220_variant_next_us = 0;      /* when the NEXT step is due */
static int64_t       s_b220_variant_stats_us = 0;     /* when THIS step logs its stats */
static bool          s_b220_variant_stats_done = true;
static unsigned long s_b220_variant_tx_frames = 0;    /* TX frames in the current step */
static unsigned      s_b220_variant_tx_errs = 0;      /* write_err in the current step */

static bool B220_I2sTxVariantSame(const B220_I2sTxVariant_t* a,
                                  const B220_I2sTxVariant_t* b) {
    return (a->mono == b->mono) && (a->left == b->left) && (a->slot32 == b->slot32) &&
           (a->bit_shift == b->bit_shift) && (a->bclk_inv == b->bclk_inv) &&
           (a->ws_inv == b->ws_inv);
}

/* The once-per-boot "what is the baseline" line: the resolved slot/gpio
 * configuration that B220_I2sUpBeforeDspBoot() actually handed to
 * i2s_channel_init_std_mode(), plus the two clock rates the sweep will produce.
 * Printed so a reader of the log knows what step 1 means without the source. */
static void B220_I2sTxVariantLogBaseline() {
    const i2s_std_slot_config_t* s = &s_b220_tx_variant_cfg.slot_cfg;
    const i2s_std_gpio_config_t* g = &s_b220_tx_variant_cfg.gpio_cfg;
    ESP_LOGI(TAG, "i2s-tx sweep: B220_I2S_TX_VARIANT_SWEEP=1, %u steps, step 1 at +%u s, "
                  "each held ~%u s, stats line ~%u s into each step; the 1 kHz tone runs "
                  "throughout - watch DAC1_P/DAC1_M",
             (unsigned)B220_VARIANT_COUNT, (unsigned)(B220_VARIANT_START_US / 1000000),
             (unsigned)(B220_VARIANT_HOLD_US / 1000000),
             (unsigned)(B220_VARIANT_STATS_US / 1000000));
    ESP_LOGI(TAG, "i2s-tx sweep: baseline resolved config = data_bit_width=%d "
                  "(I2S_DATA_BIT_WIDTH_16BIT) slot_bit_width=%d slot_mode=%s slot_mask=0x%X "
                  "(%s) ws_width=%u ws_pol=%d bit_shift=%d msb_right=%d | invert mclk=%d "
                  "bclk=%d ws=%d | sample_rate=%u Hz",
             (int)s->data_bit_width, (int)s->slot_bit_width,
             (s->slot_mode == I2S_SLOT_MODE_MONO) ? "MONO" : "STEREO",
             (unsigned)s->slot_mask,
             (s->slot_mask == I2S_STD_SLOT_BOTH)
                 ? "BOTH"
                 : ((s->slot_mask == I2S_STD_SLOT_LEFT) ? "LEFT" : "RIGHT"),
             (unsigned)s->ws_width, (int)s->ws_pol, (int)s->bit_shift, (int)s->msb_right,
             (int)g->invert_flags.mclk_inv, (int)g->invert_flags.bclk_inv,
             (int)g->invert_flags.ws_inv, (unsigned)B220_I2S_SAMPLE_RATE_HZ);
    ESP_LOGI(TAG, "i2s-tx sweep: baseline BCLK = %u Hz (2 x 16 bit slots), step 5's BCLK = "
                  "%u Hz (2 x 32 bit slots), WS = %u Hz in both",
             (unsigned)(B220_I2S_SAMPLE_RATE_HZ * 2u * 16u),
             (unsigned)(B220_I2S_SAMPLE_RATE_HZ * 2u * 32u),
             (unsigned)B220_I2S_SAMPLE_RATE_HZ);
}

/* Apply one row.  disable -> reconfig slot (+ gpio when an invert flag really
 * changes) -> enable, which is the sequence i2s_std.h mandates ("Only allowed to
 * be called when the channel state is READY ... `i2s_channel_disable` should be
 * called before calling this function if I2S has started").
 *
 * The tone is NOT stopped: this runs on the loopback task between two blocks, so
 * the interruption is the handful of milliseconds the channel is disabled (and,
 * because on ESP32 HW v1 the RX module loops BCLK/WS back from the TX module -
 * i2s_ll_share_bck_ws() -> conf.sig_loopback -, that also stops the clock for the
 * DSP for the same moment; a deliberate, documented glitch). */
static esp_err_t B220_I2sTxApplyVariant(const B220_I2sTxVariant_t* v) {
    if (s_i2s_tx == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Step 1 (and any repeat of the current state) must not touch the hardware:
     * the baseline is what is already playing. */
    if (B220_I2sTxVariantSame(v, &s_b220_variant_cur)) {
        return ESP_OK;
    }

    i2s_std_slot_config_t slot = s_b220_tx_variant_cfg.slot_cfg;
    i2s_std_gpio_config_t gpio = s_b220_tx_variant_cfg.gpio_cfg;
    const bool invert_changes =
        ((gpio.invert_flags.bclk_inv != 0) != v->bclk_inv) ||
        ((gpio.invert_flags.ws_inv != 0) != v->ws_inv);

    slot.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT; /* the DMA word size - never swept */
    slot.slot_bit_width = v->slot32 ? I2S_SLOT_BIT_WIDTH_32BIT : I2S_SLOT_BIT_WIDTH_16BIT;
    slot.slot_mode = v->mono ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    slot.slot_mask = v->mono ? (v->left ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT)
                             : I2S_STD_SLOT_BOTH;
    slot.bit_shift = v->bit_shift;
    /* ws_width, ws_pol and msb_right keep whatever the baseline copy above holds
     * and are deliberately NOT swept: on ESP32 HW v1 the LL reads ws_width as a
     * boolean only - i2s_ll_tx_set_ws_width() writes conf.tx_short_sync =
     * (width == 1) and ignores the number (esp32/include/hal/i2s_ll.h) - so it
     * cannot shape the WS duty cycle anyway, and msb_right is the alignment knob
     * inside the FIFO word, not a wire-level slot parameter. */
    gpio.invert_flags.bclk_inv = v->bclk_inv ? 1u : 0u;
    gpio.invert_flags.ws_inv = v->ws_inv ? 1u : 0u;

    esp_err_t err = i2s_channel_disable(s_i2s_tx);
    if (err == ESP_ERR_INVALID_STATE) {
        /* An earlier step failed in a way that left the channel disabled (its own
         * recovery enable did not take either): enabling it is then the only way
         * back to a state this function can reconfigure from. */
        const esp_err_t up = i2s_channel_enable(s_i2s_tx);
        if (up == ESP_OK) {
            err = i2s_channel_disable(s_i2s_tx);
        }
    }
    if (err != ESP_OK) {
        return err; /* the channel is not RUNNING - nothing was changed */
    }
    err = i2s_channel_reconfig_std_slot(s_i2s_tx, &slot);
    if (err == ESP_OK) {
        s_b220_tx_variant_cfg.slot_cfg = slot;
        /* The DMA frame layout follows the SLOT config, so this is the moment the
         * tone path must switch between 960-byte stereo and 480-byte mono blocks -
         * before the channel is enabled again. */
        s_b220_tx_mono = v->mono;
    }
    if ((err == ESP_OK) && invert_changes) {
        err = i2s_channel_reconfig_std_gpio(s_i2s_tx, &gpio);
        if (err == ESP_OK) {
            s_b220_tx_variant_cfg.gpio_cfg = gpio;
        }
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_i2s_tx);
        if (err == ESP_OK) {
            s_b220_variant_cur = *v;
        }
    }
    if (err != ESP_OK) {
        /* The channel is disabled at this point: put it back on the air with
         * whatever configuration it now holds, so one failed step costs a glitch
         * and not the rest of the sweep. */
        const esp_err_t back = i2s_channel_enable(s_i2s_tx);
        if (back != ESP_OK) {
            ESP_LOGE(TAG, "i2s-tx variant: TX could not be re-enabled after the failed "
                          "reconfig (%s) - the tone is silent from here",
                     esp_err_to_name(back));
        }
    }
    return err;
}

/* One tick per loopback iteration (~5 ms).  It starts the sweep ~6 s in, applies
 * the next step every ~3 s and logs each step's stats line ~2 s into its hold.
 * `last_block_peak` is the peak of the block that was just written. */
static void B220_I2sTxVariantTick(int64_t started_us, int64_t now_us,
                                  unsigned last_block_peak) {
    if (s_b220_variant_step == 0) {
        if ((now_us - started_us) < B220_VARIANT_START_US) {
            return; /* the untouched baseline gets the first ~6 s */
        }
    } else if (now_us < s_b220_variant_next_us) {
        if (!s_b220_variant_stats_done && (now_us >= s_b220_variant_stats_us)) {
            s_b220_variant_stats_done = true;
            ESP_LOGI(TAG, "i2s-tx variant %u/%u: tx_peak=%u tx_frames=%lu write_err=%u",
                     s_b220_variant_step, (unsigned)B220_VARIANT_COUNT, last_block_peak,
                     s_b220_variant_tx_frames, s_b220_variant_tx_errs);
        }
        return;
    }

    if (s_b220_variant_step >= B220_VARIANT_COUNT) {
        return; /* step 9 is the baseline again and holds for the rest of the boot */
    }
    const unsigned n = s_b220_variant_step + 1u; /* 1-based step number */
    const B220_I2sTxVariant_t* v = &s_b220_i2s_tx_variants[n - 1u];
    const esp_err_t err = B220_I2sTxApplyVariant(v);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "i2s-tx variant %u/%u: %s -> ok", n, (unsigned)B220_VARIANT_COUNT,
                 v->desc);
    } else {
        ESP_LOGE(TAG, "i2s-tx variant %u/%u: %s -> FAILED (%s)", n,
                 (unsigned)B220_VARIANT_COUNT, v->desc, esp_err_to_name(err));
    }
    s_b220_variant_step = n;
    s_b220_variant_next_us = now_us + B220_VARIANT_HOLD_US;
    s_b220_variant_stats_us = now_us + B220_VARIANT_STATS_US;
    s_b220_variant_stats_done = false;
    s_b220_variant_tx_frames = 0;
    s_b220_variant_tx_errs = 0;
}
#endif /* B220_I2S_TX_VARIANT_SWEEP */

/* Peak and mean of |sample| over one block: the "silence or signal" answer that
 * has to be in the log before anyone listens to a speaker. */
static void B220_LoopbackBlockStats(const int16_t* samples, size_t count,
                                    unsigned* peak, unsigned* mean) {
    unsigned p = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        const int v = samples[i];
        const unsigned a = (unsigned)(v < 0 ? -v : v);
        if (a > p) {
            p = a;
        }
        sum += a;
    }
    *peak = p;
    *mean = count ? (unsigned)(sum / count) : 0u;
}

/* Phase 3-17: the mean |sample| of one block on its own, for the TX-only modes -
 * the tone's peak is known before the ramp (B220_ToneRenderBlock() returns it),
 * so only the mean is missing from their rate line. */
static unsigned B220_LoopbackBlockMean(const int16_t* samples, size_t count) {
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        const int v = samples[i];
        sum += (unsigned)(v < 0 ? -v : v);
    }
    return count ? (unsigned)(sum / count) : 0u;
}

/*
 * Phase 3-15 : the raw samples of a block, in hex, as one log line.
 *
 * peak/mean are the coarse answer ("all zeros?"); this is the fine one, because
 * a DSP that drives the bus with a small but real signal (a few LSBs, dither, a
 * quiet tone) is exactly the case peak/mean rounding would hide while it still
 * reads as "the DSP is talking".  The samples are printed as their raw 16-bit
 * pattern, L/R interleaved, so the ESP32-side DMA/byte-order question is visible
 * too (all-zero = the DSP drives DIN low; repeating pairs = one channel).
 *
 * 16 words x "%04X " = at most 16*5 = 80 chars plus the NUL, on the caller's
 * 3072-byte task stack.
 */
#define B220_LOOPBACK_HEX_SAMPLES 16u
#define B220_LOOPBACK_HEX_BYTES   (B220_LOOPBACK_HEX_SAMPLES * 5u + 1u)

static void B220_LoopbackLogSamples(const char* why, const int16_t* samples,
                                    size_t count) {
    char line[B220_LOOPBACK_HEX_BYTES];
    size_t off = 0;
    const size_t n = (count < B220_LOOPBACK_HEX_SAMPLES) ? count
                                                        : B220_LOOPBACK_HEX_SAMPLES;
    line[0] = '\0';
    for (size_t i = 0; i < n; ++i) {
        const int w = snprintf(&line[off], sizeof(line) - off,
                               (i == 0) ? "%04X" : " %04X",
                               (unsigned)(uint16_t)samples[i]);
        if ((w < 0) || ((size_t)w >= (sizeof(line) - off))) {
            break;
        }
        off += (size_t)w;
    }
    ESP_LOGI(TAG, "loopback: %s - first %u of %u sample(s), hex, L/R interleaved: %s",
             why, (unsigned)n, (unsigned)count, line);
}

static void B220_AudioLoopbackTask(void* arg) {
    (void)arg;
#if B220_I2S_TX_VARIANT_SWEEP
    /* Phase 3-19: the variant sweep needs a steady, known carrier, so with the
     * switch on the sample source is ALWAYS the fixed 1 kHz tone (mode 1) whatever
     * B220_AUDIO_TEST_MODE says - the swept I2S parameters are the only variable. */
    const int test_mode = B220_TEST_TONE;
#else
    const int test_mode = B220_AUDIO_TEST_MODE;
#endif
    unsigned long frames_in = 0;  /* stereo frames read from the DSP */
    unsigned long frames_out = 0; /* stereo frames written back to it */
    unsigned read_err = 0;        /* i2s_channel_read failures */
    unsigned write_err = 0;       /* i2s_channel_write failures */
    unsigned empty_reads = 0;     /* read returned OK but produced no bytes */
    unsigned blocks = 0;          /* blocks echoed since the last rate line */
    unsigned last_frames = 0;     /* size of the most recent non-empty block */
    unsigned last_peak = 0;       /* and its peak / mean |sample| */
    unsigned last_mean = 0;
    /* Phase 3-15: the raw-sample line is emitted once for the very first
     * non-empty block after start, and then again whenever the peak the rate
     * line reports changes from 0 to non-zero - i.e. when the DSP starts sending
     * data after looking silent.  reported_peak is the peak of the last rate
     * line that had data, so the second line can only fire on a real transition
     * (at most one extra line per ~2 s window, never a per-block flood).
     *
     * Phase 3-17: modes 1/2 hand the rate line the TX peak, so reported_peak
     * tracks what the reader actually sees on that line in every mode. */
    bool hex_logged = false;
    unsigned reported_peak = 0;
    const int64_t started_us = esp_timer_get_time();
    int64_t next_log_us = started_us + B220_LOOPBACK_LOG_PERIOD_US;
    /* Phase 3-17 tone state: one phase accumulator per generator (the sweep and
     * the beep never run in the same mode, but they must not share a phase),
     * one ramp counter per generator, and the mode-2 / mode-3 timers. */
    uint32_t tone_phase = 0;
    uint32_t beep_phase = 0;
    unsigned tone_ramp = B220_TONE_RAMP_FRAMES;    /* mode 1/2: ramp already done */
    unsigned beep_ramp = B220_TONE_RAMP_FRAMES;    /* mode 3: 0 at each burst start */
    unsigned tone_hz = B220_TONE_HZ;
    unsigned long tone_frames = 0;                 /* TX frames in this window */
    unsigned tone_blocks = 0;                      /* TX blocks in this window */
    unsigned tone_last_frames = B220_LOOPBACK_FRAMES;
    unsigned tone_peak = 0;                        /* last block's pre-ramp peak */
    /* Mode 3 (and mode 0) keep the DSP side of the window apart from the TX side:
     * "rx_peak > 0 and tx_peak > 0" is the uplink and the beep in one line, which
     * is exactly the "no sound at all" vs "sound but wrong" question. */
    unsigned rx_peak = 0;
    unsigned rx_blocks = 0;
    unsigned long rx_mean_sum = 0;                 /* to average rx_mean over blocks */
    unsigned long tone_sweep_index = 0;
    int64_t next_sweep_us = started_us;
    int64_t beep_cycle_us = started_us;
    bool beep_on = false;
#if B220_DSP_DOWNLINK_TRIES
    /* Phase 3-18: the downlink experiment (B220_DSP_DOWNLINK_TRIES, config.h) is
     * paced from HERE - this task is what keeps the 1 kHz TX tone running, so
     * issuing one candidate per window is what lets the DAC output be watched on
     * a scope while the DSP is reconfigured.  The first try is deliberately one
     * window late, so the tone's own startup lines are in the log before any
     * device write happens.  zl38063_dsp_downlink_tries_step() returns -1 once
     * the table is exhausted and then costs one comparison per window. */
    int64_t next_dl_us = started_us + B220_LOOPBACK_LOG_PERIOD_US;
#endif

    /* The PA is enabled by B220_AudioLoopbackStart() before this task exists; the
     * line is repeated here so the task's own first line says what it echoes. */
    ESP_LOGI(TAG, "loopback: B220_AUDIO_TEST_MODE=%d -> %s (48 kHz, 16-bit, stereo, "
                  "%d-frame blocks = %d bytes, prio %d)",
             test_mode,
             (test_mode == B220_TEST_ECHO)
                 ? "PLAIN ECHO RX -> TX (unchanged)"
                 : ((test_mode == B220_TEST_TONE)
                        ? "1 kHz FULL-SCALE SINE ON TX ONLY - RX is never read"
                        : ((test_mode == B220_TEST_SWEEP)
                               ? "FREQUENCY SWEEP ON TX, 1 kHz -> 8 kHz in 1 s steps, "
                                 "RX is never read"
                               : "ECHO RX -> TX PLUS A 0.5 s 1 kHz BEEP EVERY 2 s")),
             B220_LOOPBACK_FRAMES, (int)B220_LOOPBACK_BLOCK_BYTES,
             B220_LOOPBACK_TASK_PRIO);

#if B220_I2S_TX_VARIANT_SWEEP
    /* Phase 3-19: the baseline is documented before step 1 can touch anything. */
    B220_I2sTxVariantLogBaseline();
#endif

    for (;;) {
        size_t got = 0;
        esp_err_t err = ESP_OK;

        /* Phase 3-17: modes 1 and 2 do not read RX at all - that is the point of
         * the test ("the tone comes only from the ESP32, so if a human hears it
         * the whole ESP32 -> DSP -> PA -> speaker leg works").  Only modes 0 and 3
         * touch the RX channel. */
        if ((test_mode == B220_TEST_ECHO) || (test_mode == B220_TEST_ECHO_BEEP)) {
            err = i2s_channel_read(s_i2s_rx, s_b220_loopback_buf,
                                   B220_LOOPBACK_BLOCK_BYTES, &got,
                                   B220_LOOPBACK_READ_TIMEOUT_MS);
            if (err != ESP_OK) {
                ++read_err;
                if (read_err == 1) {
                    /* Two completely different root causes, so name both: ESP_ERR_TIMEOUT
                     * = the RX DMA got no data (DSP silent / not driving DIN),
                     * ESP_ERR_INVALID_STATE = the RX channel never came up (the RX
                     * enable in B220_I2sUpBeforeDspBoot() is non-fatal). */
                    ESP_LOGW(TAG, "loopback: i2s_channel_read failed: %s "
                                  "(TIMEOUT = no data from the DSP; INVALID_STATE = "
                                  "the RX channel is not enabled)",
                             esp_err_to_name(err));
                }
                got = 0;
            } else if (got == 0) {
                ++empty_reads;
            }
        }

        if ((test_mode == B220_TEST_TONE) || (test_mode == B220_TEST_SWEEP)) {
            /* ---- Phase 3-17 modes 1 + 2: TX only, no RX read ------------------ */
            if (test_mode == B220_TEST_SWEEP) {
                if (esp_timer_get_time() >= next_sweep_us) {
                    next_sweep_us += B220_TONE_SWEEP_STEP_US;
                    ++tone_sweep_index;
                    if (tone_sweep_index >= B220_TONE_SWEEP_STEPS) {
                        tone_sweep_index = 0;
                    }
                    tone_hz = (unsigned)(tone_sweep_index + 1u) * B220_TONE_HZ;
                    ESP_LOGI(TAG, "tone: sweep step %lu/%u, %u Hz",
                             tone_sweep_index + 1ul, B220_TONE_SWEEP_STEPS, tone_hz);
                }
            } else {
                tone_hz = B220_TONE_HZ;
            }
            /* tone_ramp starts at B220_TONE_RAMP_FRAMES (fully ramped): the 20 ms
             * fade-in is deliberately spent once, before the first block, so the
             * very first thing the speaker gets is already the steady tone rather
             * than a fade that the "is anything coming out at all?" listener
             * cannot hear. */
            /* Phase 3-19: the block LAYOUT follows the SWEPT slot mode.  In mono
             * the DMA frame is one 16-bit word (i2s_std.c:150 active_slot = 1,
             * i2s_common.c:624 bytes_per_frame = bytes_per_sample * active_slot),
             * so the same 240 frames are 480 bytes there and 960 bytes in stereo -
             * writing the stereo size in mono would double the sample rate. */
            const bool tx_mono = B220_TX_MONO_NOW();
            const size_t tx_block_bytes = tx_mono ? (B220_LOOPBACK_FRAMES * sizeof(int16_t))
                                                  : B220_LOOPBACK_BLOCK_BYTES;
            const size_t tx_frame_bytes = tx_mono ? sizeof(int16_t)
                                                  : B220_LOOPBACK_FRAME_BYTES;
            tone_peak = B220_ToneRenderBlock(B220_TonePhaseStep(tone_hz), &tone_phase,
                                             tone_ramp, s_b220_loopback_buf, tx_mono);
            const unsigned block_frames = B220_LOOPBACK_FRAMES;
            tone_blocks += 1u;
            last_frames = block_frames;
            last_peak = tone_peak;
            /* No mean is averaged here on purpose: the tone is a sine, so its mean
             * |sample| is exactly 2/pi = 0.6366 of the peak (logged once at table
             * build), and a full pass over the block per 5 ms would buy a number
             * that the peak already determines.  The rate line reports the peak as
             * "tx_mean~" and the reader divides.  (blocks is NOT bumped here: it
             * counts the DSP-side blocks of the window, and modes 1/2 have none.) */
            last_mean = (unsigned)(((unsigned long long)tone_peak * 7958ull) / 12500ull);
            size_t wrote = 0;
            err = i2s_channel_write(s_i2s_tx, s_b220_loopback_buf, tx_block_bytes, &wrote,
                                    B220_TONE_WRITE_TIMEOUT_MS);
            if (err != ESP_OK) {
                ++write_err;
                if (write_err == 1) {
                    ESP_LOGW(TAG, "tone: i2s_channel_write failed: %s",
                             esp_err_to_name(err));
                }
            }
            frames_out += wrote / tx_frame_bytes;
            tone_frames += wrote / tx_frame_bytes;
            tone_last_frames = (unsigned)(wrote / tx_frame_bytes);
#if B220_I2S_TX_VARIANT_SWEEP
            /* Phase 3-19: this step's own window, reported by its stats line. */
            s_b220_variant_tx_frames += tone_last_frames;
            if (err != ESP_OK) {
                ++s_b220_variant_tx_errs;
            }
#endif
        }

        if (((test_mode == B220_TEST_ECHO) || (test_mode == B220_TEST_ECHO_BEEP)) &&
            (got > 0)) {
            /* ---- modes 0 + 3, echo part: the frames the DSP sent to us ------ */
            const size_t frames = got / B220_LOOPBACK_FRAME_BYTES;
            frames_in += frames;
            ++blocks;
            last_frames = (unsigned)frames;
            B220_LoopbackBlockStats(s_b220_loopback_buf, got / sizeof(int16_t),
                                    &last_peak, &last_mean);
            /* The DSP side of this window, kept apart from the TX side above. */
            rx_peak = last_peak;
            ++rx_blocks;
            rx_mean_sum += last_mean;
            /* Phase 3-15: the raw first 16 samples of the first non-empty block,
             * in hex, as soon as it exists - the "is the DSP really sending
             * nothing, or only something small?" answer that peak/mean alone
             * cannot give.  reported_peak is seeded from it, so the 0 -> non-zero
             * line below is a genuine later transition, not a repeat of this.
             * (Phase 3-17: in mode 3 the buffer is overwritten by the beep during
             * a burst, so this line only reflects RX while the burst is off; the
             * hex line at each burst start is the TX-side counterpart.) */
            if (!hex_logged) {
                B220_LoopbackLogSamples("first non-empty block after start",
                                        s_b220_loopback_buf, got / sizeof(int16_t));
                hex_logged = true;
                reported_peak = last_peak;
            }
            if (test_mode == B220_TEST_ECHO_BEEP) {
                /* 0.5 s of 1 kHz every 2 s.  The burst's phase and ramp are its
                 * own and are restarted at the beginning of every burst, so the
                 * amplitude ramps in over the first ~20 ms of the burst (never a
                 * click) and the tone resumes in phase every time. */
                const int64_t now_beep_us = esp_timer_get_time();
                if (!beep_on && ((now_beep_us - beep_cycle_us) >= B220_BEEP_PERIOD_US)) {
                    beep_cycle_us += B220_BEEP_PERIOD_US;
                    beep_on = true;
                    beep_phase = 0;
                    beep_ramp = 0;
                    /* The RX block of this very iteration is still in the buffer:
                     * dump it as the last DSP-side sample before the burst covers
                     * it, beside the TX side the burst is about to produce. */
                    B220_LoopbackLogSamples("beep burst START - DSP (RX) block it "
                                            "replaces",
                                            s_b220_loopback_buf, got / sizeof(int16_t));
                    ESP_LOGI(TAG, "beep: burst START (1 kHz, 0.5 s on / 1.5 s off, "
                                  "ramped in over ~20 ms)");
                }
                if (beep_on && ((now_beep_us - beep_cycle_us) >= B220_BEEP_BURST_US)) {
                    beep_on = false;
                    ESP_LOGI(TAG, "beep: burst END (the echo resumes)");
                }
            }
            if (beep_on) {
                /* The beep replaces the echo for the length of the burst (the two
                 * would have to be summed, and the point of this mode is that the
                 * listener hears a clean, unambiguous tone). */
                const unsigned beep_peak = B220_ToneRenderBlock(
                    B220_TonePhaseStep(B220_TONE_HZ), &beep_phase, beep_ramp,
                    s_b220_loopback_buf, false);
                if (beep_ramp < B220_TONE_RAMP_FRAMES) {
                    beep_ramp += B220_LOOPBACK_FRAMES;
                    if (beep_ramp > B220_TONE_RAMP_FRAMES) {
                        beep_ramp = B220_TONE_RAMP_FRAMES;
                    }
                }
                last_peak = beep_peak;
                last_mean = B220_LoopbackBlockMean(s_b220_loopback_buf,
                                                   B220_LOOPBACK_FRAMES * 2u);
                size_t wrote = 0;
                err = i2s_channel_write(s_i2s_tx, s_b220_loopback_buf,
                                        B220_LOOPBACK_BLOCK_BYTES, &wrote,
                                        B220_TONE_WRITE_TIMEOUT_MS);
                if (err != ESP_OK) {
                    ++write_err;
                    if (write_err == 1) {
                        ESP_LOGW(TAG, "beep: i2s_channel_write failed: %s",
                                 esp_err_to_name(err));
                    }
                }
                frames_out += wrote / B220_LOOPBACK_FRAME_BYTES;
                tone_frames += wrote / B220_LOOPBACK_FRAME_BYTES;
                tone_blocks += 1u;
                tone_last_frames = (unsigned)(wrote / B220_LOOPBACK_FRAME_BYTES);
            } else {
                /* Plain echo (mode 0 and mode 3 outside a burst): the very same
                 * bytes back out, no gain, no channel swap. */
                size_t wrote = 0;
                err = i2s_channel_write(s_i2s_tx, s_b220_loopback_buf, got, &wrote,
                                        B220_LOOPBACK_WRITE_TIMEOUT_MS);
                if (err != ESP_OK) {
                    ++write_err;
                    if (write_err == 1) {
                        ESP_LOGW(TAG, "loopback: i2s_channel_write failed: %s",
                                 esp_err_to_name(err));
                    }
                } else {
                    frames_out += wrote / B220_LOOPBACK_FRAME_BYTES;
                }
            }
        }

        const int64_t now_us = esp_timer_get_time();
#if B220_I2S_TX_VARIANT_SWEEP
        /* Phase 3-19: the variant sweep is driven from here - the tone above has
         * just been written, so a step change never interrupts a render, and the
         * peak this reports is the block that is going out now. */
        B220_I2sTxVariantTick(started_us, now_us, tone_peak);
#endif
        if (now_us >= next_log_us) {
            next_log_us = now_us + B220_LOOPBACK_LOG_PERIOD_US;
            const unsigned t_s = (unsigned)((now_us - started_us) / 1000000);
            /* Phase 3-17: one line per ~2 s whose fields name the mode they come
             * from.  Modes 1/2 have no RX side at all, so their line reports the
             * TX peak (pre-ramp, returned by the renderer) / mean / frames of the
             * window; mode 3 reports BOTH peaks of the same window, which is what
             * tells "no sound at all" from "sound but wrong".  Every field is
             * computed for the mode that prints it, so a tone build never prints
             * a frame count it did not produce and vice versa - while all
             * counters stay cumulative (frames_in / frames_out / read_err /
             * write_err / empty_reads). */
            if (test_mode == B220_TEST_TONE) {
                ESP_LOGI(TAG, "tone   t=+%us: mode=1 (1 kHz full-scale sine, TX only, "
                              "RX never read) tx_frames=%lu tx_blocks=%u tx_last=%u "
                              "frames tx_peak=%u tx_mean~%u write_err=%u",
                         t_s, tone_frames, tone_blocks, tone_last_frames, tone_peak,
                         last_mean, write_err);
                tone_frames = 0;
                tone_blocks = 0;
            } else if (test_mode == B220_TEST_SWEEP) {
                ESP_LOGI(TAG, "sweep  t=+%us: mode=2 (1 kHz -> 8 kHz, 1 s/step, TX "
                              "only) step=%lu/%u hz=%u tx_frames=%lu tx_blocks=%u "
                              "tx_last=%u frames tx_peak=%u tx_mean~%u write_err=%u",
                         t_s, tone_sweep_index + 1ul, B220_TONE_SWEEP_STEPS, tone_hz,
                         tone_frames, tone_blocks, tone_last_frames, tone_peak,
                         last_mean, write_err);
                tone_frames = 0;
                tone_blocks = 0;
            } else if (test_mode == B220_TEST_ECHO_BEEP) {
                ESP_LOGI(TAG, "echo+beep t=+%us: mode=3 (echo + 0.5 s 1 kHz beep every "
                              "2 s; beep is %s) rx_frames=%lu rx_blocks=%u rx_last=%u "
                              "frames rx_peak=%u rx_mean=%lu tx_frames=%lu "
                              "tx_blocks=%u tx_peak=%u write_err=%u read_err=%u "
                              "empty_reads=%u",
                         t_s, beep_on ? "ON" : "off", frames_in, rx_blocks,
                         last_frames, rx_peak,
                         (unsigned long)(rx_blocks ? (rx_mean_sum / rx_blocks) : 0ul),
                         tone_frames, tone_blocks, tone_peak, write_err, read_err,
                         empty_reads);
                tone_frames = 0;
                tone_blocks = 0;
                tone_peak = 0;
                rx_blocks = 0;
                rx_mean_sum = 0;
                rx_peak = 0;
            } else {
                /* Mode 0: exactly the pre-3-17 line, so an old log and a new one
                 * can be compared field by field. */
                ESP_LOGI(TAG,
                         "loopback t=+%us: frames_in=%lu frames_out=%lu read_err=%u "
                         "write_err=%u empty_reads=%u blocks=%u last_block=%u frames "
                         "peak=%u mean=%u",
                         t_s, frames_in, frames_out, read_err, write_err, empty_reads,
                         blocks, last_frames, last_peak, last_mean);
                rx_blocks = 0;
                rx_mean_sum = 0;
            }
            /* Phase 3-15: this window's peak was 0 in the last window that had
             * data and is non-zero now - dump the block that changed it, so the
             * transition is verifiable at sample level and not just as a number.
             * It reads the most recent non-empty block, which with blocks > 0
             * happened inside this window (the buffer is not touched by an empty
             * read).  (Mode 0 only: in mode 3 that buffer holds the beep, whose
             * own hex line is emitted at each burst start.) */
            if ((test_mode == B220_TEST_ECHO) && (blocks > 0u) && hex_logged &&
                (last_peak != 0u) && (reported_peak == 0u)) {
                B220_LoopbackLogSamples("peak rose from 0 to non-zero",
                                        s_b220_loopback_buf,
                                        (size_t)last_frames * 2u);
                reported_peak = last_peak;
            }
            blocks = 0;
        }

        if (got == 0) {
            vTaskDelay(1); /* never spin when there is nothing to echo */
        }

#if B220_DSP_DOWNLINK_TRIES
        /* Phase 3-18: the downlink experiment's clock.  One candidate per ~2 s
         * window, applied from this task so the 1 kHz TX tone above never stops:
         * "downlink try N: reg 0x.... <- 0x.... -> ok|FAILED" lines land in the
         * log between the tone lines while the DSP is reconfigured, and the try's
         * own readback line says whether the write took.  Nothing happens here
         * while the loopback is silent (mode 1/2 never read RX, so got stays 0
         * and the delay above still runs a few times per window). */
        {
            const int64_t dl_now_us = esp_timer_get_time();
            if (dl_now_us >= next_dl_us) {
                next_dl_us = dl_now_us + B220_LOOPBACK_LOG_PERIOD_US;
                (void)zl38063_dsp_downlink_tries_step();
            }
        }
#endif
    }
}

/*
 * Start the one and only loopback task.  Called from the board constructor after
 * InitializeAudioDsp(), so it runs whatever that attempt's verdict was.
 */
static void B220_AudioLoopbackStart() {
    if (s_b220_loopback_task != nullptr) {
        ESP_LOGW(TAG, "loopback: already started - the task is created only once");
        return;
    }

    /* Phase 3-17: build the 1 kHz cycle table BEFORE the task can use it.  It is
     * built unconditionally (a 384-entry, 768-byte .bss table plus ~400 uses of
     * cos() once per boot) because the switch is a build-time constant, so mode 0
     * pays for 768 bytes of RAM and nothing else - and it keeps the audio path
     * free of both an init-time branch and any lazy-init race.  Modes 0 and 3's
     * echo path simply never read it. */
    B220_ToneTableInit();

#if B220_AUDIO_TEST_MODE == B220_TEST_ECHO
    ESP_LOGI(TAG, "loopback: DOWNLINK TEST MODE 0/3 = plain echo RX -> TX; the 1 kHz "
                  "table is built but unused (set B220_AUDIO_TEST_MODE to 1 in "
                  "config.h for the pure 1 kHz TX tone, 2 for the sweep, 3 for "
                  "echo + a 0.5 s beep every 2 s)");
#elif B220_AUDIO_TEST_MODE == B220_TEST_TONE
    ESP_LOGI(TAG, "loopback: DOWNLINK TEST MODE 1/3 = 1 kHz FULL-SCALE SINE ON TX "
                  "ONLY, RX is never read; everything the speaker can carry comes "
                  "from the ESP32, so hearing it clears the whole ESP32 -> DSP -> "
                  "PA -> speaker leg (set B220_AUDIO_TEST_MODE 0 for echo, 2 for the "
                  "sweep, 3 for echo + beep)");
#elif B220_AUDIO_TEST_MODE == B220_TEST_SWEEP
    ESP_LOGI(TAG, "loopback: DOWNLINK TEST MODE 2/3 = SWEEP 1 kHz -> 8 kHz, 1 s per "
                  "step, full scale, on TX only (RX is never read); a partly working "
                  "downlink is audible on some steps only (set B220_AUDIO_TEST_MODE 0 "
                  "for echo, 1 for the fixed 1 kHz tone, 3 for echo + beep)");
#else
    ESP_LOGI(TAG, "loopback: DOWNLINK TEST MODE 3/3 = ECHO RX -> TX PLUS A 0.5 s "
                  "1 kHz BEEP EVERY 2 s (beep = ESP32-generated, echo = DSP audio), "
                  "so \"no sound at all\" and \"sound but wrong\" are told apart by "
                  "ear and by the two peaks in the echo+beep line (set "
                  "B220_AUDIO_TEST_MODE 0 for echo only, 1 for the pure tone, 2 for "
                  "the sweep)");
#endif

#if B220_I2S_TX_VARIANT_SWEEP
    /* Phase 3-19: say it out loud, because this switch OVERRIDES the mode above:
     * the sample source is the fixed 1 kHz tone and the I2S TX signal level is the
     * variable.  The step table and the timing are logged by the task itself
     * (B220_I2sTxVariantLogBaseline()), together with the resolved baseline. */
    ESP_LOGI(TAG, "loopback: B220_I2S_TX_VARIANT_SWEEP=1 -> sample source is the FIXED 1 kHz "
                  "tone (B220_AUDIO_TEST_MODE is overridden to 1) and the I2S TX signal-level "
                  "parameters are swept from +%u s: %d steps, ~%u s each (baseline, bclk_inv, "
                  "ws_inv, both, 32-bit slot, mono-left, mono-right, bit_shift=0, baseline) - "
                  "watch DAC1_P/DAC1_M",
             (unsigned)(B220_VARIANT_START_US / 1000000), (int)B220_VARIANT_COUNT,
             (unsigned)(B220_VARIANT_HOLD_US / 1000000));
#endif

#if B220_DSP_ADF_RELEASE_TX
    /* Phase 3-15 : one line that says which release this loopback measures.  The
     * text comes from the DSP layer, which only claims the ADF order once the
     * sequence has really run - a boot whose upload failed, or which skipped it,
     * reports that here instead. */
    ESP_LOGI(TAG, "loopback: release = %s", zl38063_dsp_release_name());
#else
    ESP_LOGI(TAG, "loopback: release = the normal host-boot path "
                  "(B220_DSP_ADF_RELEASE_TX is 0: reg 0x12C + reg 0x32 <- 0x000D + "
                  "GO reg 0x32 <- 0x0008 from the upload, config record after it)");
#endif

    /* These are the handles B220_I2sUpBeforeDspBoot() left behind.  With either of
     * them null there is nothing to read from or write to, and creating a channel
     * of our own is not an option (one I2S0 port, one duplex pair). */
    if (s_i2s_tx == nullptr || s_i2s_rx == nullptr) {
        ESP_LOGE(TAG, "loopback: the I2S0 handles are not up (s_i2s_tx=%p "
                      "s_i2s_rx=%p; B220_I2sUpBeforeDspBoot() failed or never ran) "
                      "- loopback NOT started",
                 (void*)s_i2s_tx, (void*)s_i2s_rx);
        return;
    }

    /* The PA enable is the AW8737S SHDN pulse sequence (0x02 <- 0x00, 2 ms,
     * 0x02 <- 0x02), NOT a bare 0x02 <- 0x02: the chip selects its operating
     * mode from the number of rising edges, so the low period is part of the
     * command and the singleton inside the helper makes this the only edge of
     * the boot.  See the pulse block at the top of this file. */
    const esp_err_t pa = b220_pa_enable_one_pulse("loopback start");
    if (pa != ESP_OK) {
        /* Not fatal: the echo (and the peak/mean line) still proves the I2S path
         * even when the PA never answered - it is only the "can a human hear it"
         * half that is then missing. */
        ESP_LOGW(TAG, "loopback: the PA could not be enabled (%s); the echo still "
                      "runs but stays inaudible (reg 0x%02X = 0x%02X = muted)",
                 esp_err_to_name(pa), (unsigned)B220_LOOPBACK_PA_REG,
                 (unsigned)B220_LOOPBACK_PA_OFF);
    }

    if (xTaskCreate(B220_AudioLoopbackTask, "b220_loopback",
                    B220_LOOPBACK_TASK_STACK, nullptr, B220_LOOPBACK_TASK_PRIO,
                    &s_b220_loopback_task) != pdPASS) {
        s_b220_loopback_task = nullptr;
        ESP_LOGE(TAG, "loopback: xTaskCreate failed (stack %d bytes, prio %d)",
                 B220_LOOPBACK_TASK_STACK, B220_LOOPBACK_TASK_PRIO);
        return;
    }
    ESP_LOGI(TAG, "loopback: started (B220_AUDIO_LOOPBACK=1, B220_AUDIO_TEST_MODE=%d, "
                  "one rate line every ~%d s); set B220_AUDIO_LOOPBACK to 0 in "
                  "config.h for the normal path",
             B220_AUDIO_TEST_MODE, B220_LOOPBACK_LOG_PERIOD_US / 1000000);

#if B220_PA_BIT_FINDER
    /* Phase 3-20: the SHDN bit finder walks the AW9106B ~6 s after this point,
     * while the task above keeps the 1 kHz tone running.  It is a task of its own
     * because a 4 s hold in the loopback task would stop i2s_channel_write() and
     * silence the tone.  See the finder block at the top of this file. */
    B220_PaBitFinderStart();
#endif
}
#endif  /* B220_AUDIO_LOOPBACK && B220_I2S_BEFORE_DSP */

/* =========================================================================== *
 * Phase 5 : AUTO LISTEN / CONTINUOUS CONVERSATION (B220_AUTO_LISTEN, config.h).
 *
 * The board has no way to start a conversation: esp-sr WakeNet needs an
 * ESP32-S3 and this toy is a classic ESP32-D0WD, and there is no readable button
 * (the power key is a baseboard soft-latch, GPIO0 is the boot strap / ZL38063
 * chip-select and BOOT_BUTTON_GPIO was deliberately dropped from config.h).  So
 * nothing ever calls Application::StartListening() and the device reaches idle
 * and stays there forever.
 *
 * This block is that missing "button": a board-level watcher, not a core change.
 * It never touches application.cc / device_state_machine.* and it never changes a
 * state itself - it only observes Application::GetDeviceState() (application.h:71)
 * and then uses the SAME entry point a button or a wake word uses,
 * Application::StartListening() (application.h:100-103), which only sets
 * MAIN_EVENT_START_LISTENING (application.cc:776).  That is what makes it safe
 * from a non-main task: the note on the API says so ("Start listening
 * (event-based, thread-safe) ... Sends MAIN_EVENT_START_LISTENING to be handled in
 * Run()"), and the application's main loop turns that bit into the real
 * transition through SetListeningMode() -> SetDeviceState(kDeviceStateListening)
 * (application.cc:844-878), i.e. through the state machine like every other path.
 *
 * Anti-busy-loop rules (all three are needed):
 *   1. an arm is only prepared on a TRANSITION into idle (the previous poll saw a
 *      state that was not idle), never on every poll;
 *   2. the arm is delayed B220_AUTO_LISTEN_DELAY_MS and re-checked against the
 *      live state at the end of the delay;
 *   3. after B220_AUTO_LISTEN_MAX_MISSES consecutive arms that did not move the
 *      device out of idle the watcher goes dormant until the state leaves idle
 *      again (a device with no protocol/audio channel cannot be re-tried forever).
 * The task itself sleeps B220_AUTO_LISTEN_POLL_MS between one state read.
 *
 * Way out: B220_AUTO_LISTEN 0 compiles this whole block (task included) out.  At
 * runtime there is nothing to press on this board, so the loop's only stop is
 * power; the guards above keep it from becoming a busy loop, and the watcher
 * stops for the rest of an idle episode as soon as five arms in a row fail to
 * move the device (no protocol / no audio channel).
 * ========================================================================== */
#if B220_AUTO_LISTEN

/* The task only sleeps in vTaskDelay() and formats two short log lines, so it
 * needs no audio-sized stack; the priority is below the audio tasks (4..8) and
 * below the application's main task (10) - it must never delay audio or the
 * state machine. */
#define B220_AUTOLISTEN_TASK_STACK 3072 /* bytes */
#define B220_AUTOLISTEN_TASK_PRIO  2

static TaskHandle_t s_b220_autolisten_task = nullptr;
static esp_timer_handle_t s_b220_autolisten_power_timer = nullptr;
static bool s_b220_autolisten_seen_idle = false; /* last polled state was idle       */
static bool s_b220_autolisten_pending = false;   /* countdown to an arm is running   */
static bool s_b220_autolisten_dormant = false;   /* too many misses in a row         */
static bool s_b220_autolisten_path_fixed = false; /* output re-enabled this idle run  */
static unsigned s_b220_autolisten_arms = 0;      /* the "arm #N" counter             */
static unsigned s_b220_autolisten_misses = 0;    /* arms that did not leave idle     */
static unsigned s_b220_autolisten_wait_ticks = 0;/* polls left before the arm        */

/*
 * The audio service's own power policy disables the codec after
 * AUDIO_POWER_TIMEOUT_MS (audio_service.h:48, 15 s) without input/output, and
 * while the device sits in idle the input is off anyway (the idle handler calls
 * EnableVoiceProcessing(false) -> EnableInput(false)), so this board is in the
 * "duplex but input disabled" case of CheckAndUpdateAudioPowerState()
 * (audio_service.cc:844-848) and the OUTPUT gets disabled too.  That must not be
 * mistaken for a user mute: it is simply an idle audio path.  If listening were
 * armed that way the AudioService would re-enable the input on the first read
 * (audio_service.cc:196-200) while the output stayed off, so the reply would be
 * silent.  Re-enable it once per idle episode, exactly the way PlaySound() does it
 * (start the power timer, then EnableOutput(true)); on this codec that is a
 * latched no-op for the AW8737S PA, so it can never add a SHDN edge.
 */

static void B220_AutoListenEnsureAudioPath() {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if ((codec == nullptr) || codec->output_enabled() || s_b220_autolisten_path_fixed ||
        (s_b220_autolisten_power_timer == nullptr)) {
        return;
    }
    s_b220_autolisten_path_fixed = true;
    ESP_LOGI(TAG, "auto-listen: the audio service powered the codec output down while "
                  "idle (AUDIO_POWER_TIMEOUT_MS); re-enabling it so the reply can be "
                  "heard (once per idle episode)");
    esp_timer_stop(s_b220_autolisten_power_timer);
    esp_timer_start_periodic(s_b220_autolisten_power_timer,
                             AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
    codec->EnableOutput(true);
}

/*
 * Ask the application to listen.  Runs in this task, so everything it touches is
 * either a documented thread-safe entry point (StartListening) or a plain state
 * read (GetDeviceState / the codec's enabled flags).
 *
 * The codec's INPUT is deliberately not inspected: while the device waits in idle
 * it is off by design (the idle handler calls EnableVoiceProcessing(false) ->
 * EnableInput(false)) and the service re-enables it by itself on the first read
 * once listening starts (audio_service.cc:196-200), so "input disabled" is the
 * normal idle state here, not a mute.
 */
static void B220_AutoListenArm() {
    Application& app = Application::GetInstance();

    if (app.GetDeviceState() != kDeviceStateIdle) {
        /* The delay elapsed and the user (or the server) already moved on. */
        return;
    }

    /* The OUTPUT, by contrast, must be live or the reply would be silent. */
    B220_AutoListenEnsureAudioPath();

    const DeviceState before = app.GetDeviceState();
    s_b220_autolisten_arms++;
    ESP_LOGI(TAG, "auto-listen: arm #%u (state %s -> listening)",
             s_b220_autolisten_arms, DeviceStateMachine::GetStateName(before));
    app.StartListening();

    if (app.GetDeviceState() == before) {
        /* The request did not move the device (no audio channel and therefore no
         * protocol, most likely).  Count it and give up after too many in a row. */
        s_b220_autolisten_misses++;
        if (s_b220_autolisten_misses >= B220_AUTO_LISTEN_MAX_MISSES) {
            s_b220_autolisten_dormant = true;
            ESP_LOGE(TAG, "auto-listen: %u consecutive arms did not move the device out of "
                          "idle (no audio channel / no protocol?) - dormant until the "
                          "state leaves idle",
                     s_b220_autolisten_misses);
        }
    } else {
        s_b220_autolisten_misses = 0;
    }
}

/*
 * The watcher.  One Application::GetDeviceState() read per
 * B220_AUTO_LISTEN_POLL_MS sleep; an arm only after a transition into idle plus
 * the configured delay.
 */
static void B220_AutoListenSupervisorTask(void* arg) {
    (void)arg;
    Application& app = Application::GetInstance();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(B220_AUTO_LISTEN_POLL_MS));

        const DeviceState state = app.GetDeviceState();

        if (state != kDeviceStateIdle) {
            /* Left idle: everything is re-armed, including a watcher that had gone
             * dormant on misses or had repaired the audio path. */
            s_b220_autolisten_seen_idle = false;
            s_b220_autolisten_pending = false;
            s_b220_autolisten_dormant = false;
            s_b220_autolisten_path_fixed = false;
            s_b220_autolisten_misses = 0;
            continue;
        }

        if (!s_b220_autolisten_seen_idle) {
            /* A transition INTO idle (the previous poll did not see idle): this is
             * the only place an arm is scheduled. */
            s_b220_autolisten_seen_idle = true;
            s_b220_autolisten_pending = true;
            s_b220_autolisten_wait_ticks = (unsigned)(B220_AUTO_LISTEN_DELAY_MS /
                                                      B220_AUTO_LISTEN_POLL_MS);
            if (s_b220_autolisten_wait_ticks == 0u) {
                s_b220_autolisten_wait_ticks = 1u;
            }
            continue;
        }

        if (s_b220_autolisten_pending) {
            if (--s_b220_autolisten_wait_ticks == 0u) {
                s_b220_autolisten_pending = false;
                B220_AutoListenArm();
            }
        }
    }
}

static void B220_AutoListenStart() {
    if (s_b220_autolisten_task != nullptr) {
        ESP_LOGW(TAG, "auto-listen: already started - the watcher is created once per boot");
        return;
    }

    /* The repair helper restarts the audio service's power-check timer the same way
     * AudioService/PlaySound does, so it needs its own handle. */
    const esp_timer_create_args_t timer_args = {
        .callback = nullptr,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "b220_autolisten",
        .skip_unhandled_events = true,
    };
    const esp_err_t terr = esp_timer_create(&timer_args, &s_b220_autolisten_power_timer);
    if (terr != ESP_OK) {
        s_b220_autolisten_power_timer = nullptr;
        ESP_LOGW(TAG, "auto-listen: esp_timer_create failed (%s); an idle-disabled codec "
                      "output will not be re-enabled",
                 esp_err_to_name(terr));
    }

    if (xTaskCreate(B220_AutoListenSupervisorTask, "b220_autolisten",
                    B220_AUTOLISTEN_TASK_STACK, nullptr, B220_AUTOLISTEN_TASK_PRIO,
                    &s_b220_autolisten_task) != pdPASS) {
        s_b220_autolisten_task = nullptr;
        ESP_LOGE(TAG, "auto-listen: xTaskCreate failed (stack %d bytes, prio %d); this "
                      "board has no wake word and no button, so nothing will start a "
                      "conversation",
                 B220_AUTOLISTEN_TASK_STACK, B220_AUTOLISTEN_TASK_PRIO);
        return;
    }
    ESP_LOGI(TAG, "auto-listen: ENABLED (no wake word, no button on this board)");
}

#else  /* !B220_AUTO_LISTEN */

static void B220_AutoListenStart() {
    ESP_LOGI(TAG, "auto-listen: DISABLED (B220_AUTO_LISTEN=0 in config.h): the esp-sr WakeNet9 "
                  "wake word (CONFIG_USE_ESP_WAKE_WORD) starts conversations now");
}

#endif  /* B220_AUTO_LISTEN */

#if !B220_BUTTON_DISCOVERY
/*
 * The three physical keys, which are read THROUGH THE ZL38063 (see
 * b220_buttons.h and BUTTON-HARDWARE-REVERSE.md): the stock firmware's buttonTask
 * polls HBI 0x0000 (code) and HBI 0x0002 (status) over the same SPI link this
 * port uses for the DSP, and a press is a short pulse on both.
 *
 * The handler runs on the poller's task, never on the main event loop, so it
 * only calls Application's event-based thread-safe entry points
 * (application.h:93-109: ToggleChatState / StartListening / StopListening) and
 * the codec's volume setter.
 *
 * There is no long-press branch: this hardware cannot report how long a key was
 * held (b220_buttons.h), so every press is one action.
 *
 * The volume keys are honest about a hardware limit: Zl38063AudioCodec::
 * SetOutputVolume() stores the value (NVS + the UI's volume indicator) but does
 * NOT change the output level, because the real gain lives inside the DSP
 * (registers 0x0238/0x023A) and this port has no verified run-time write for it
 * (PART 3 §1.6).  So the keys work as far as the firmware can take them, and the
 * log says so instead of pretending.
 */
/*
 * ---------------------------------------------------------------------------
 * Volume keys: SINGLE press = one step, DOUBLE press = jump to the end of the
 * range.
 *
 * Why double press and not long press: this front-end cannot report how long a
 * key was held (b220_buttons.h).  A double press, on the other hand, is pure
 * sequencing, which the interface does report reliably - one 0x0003 per press,
 * and no second event while a key is held (measured).
 *
 * The step is therefore DEFERRED by B220_BUTTON_DOUBLE_CLICK_MS: if a second
 * press of the SAME key arrives inside that window the jump replaces the step,
 * otherwise the step is applied when the window closes.  The cost is that a
 * single volume step lands B220_BUTTON_DOUBLE_CLICK_MS after the press; the
 * benefit is that a double press never first moves by one step and then jumps.
 * The record key is NOT deferred - it opens/closes a conversation immediately.
 * ---------------------------------------------------------------------------
 */
static void B220_ButtonSetVolume(bool up, bool jump) {
    auto* codec = static_cast<Zl38063AudioCodec*>(Board::GetInstance().GetAudioCodec());
    if (codec == nullptr) {
        ESP_LOGW(TAG, "key %s: no codec to set", up ? "VOLUME_UP" : "VOLUME_DOWN");
        return;
    }

    const int before = codec->output_volume();
    int volume;
    if (jump) {
        volume = up ? B220_BUTTON_VOLUME_MAX : B220_BUTTON_VOLUME_MIN;
    } else {
        volume = before + (up ? B220_BUTTON_VOLUME_STEP : -B220_BUTTON_VOLUME_STEP);
    }
    if (volume < B220_BUTTON_VOLUME_MIN) {
        volume = B220_BUTTON_VOLUME_MIN;
    }
    if (volume > B220_BUTTON_VOLUME_MAX) {
        volume = B220_BUTTON_VOLUME_MAX;
    }

    /* This stores the value AND writes the DSP's real output gain; see
     * Zl38063AudioCodec::SetOutputVolume(). */
    codec->SetOutputVolume(volume);
    ESP_LOGI(TAG, "key %s (%s): volume %d -> %d",
             up ? "VOLUME_UP" : "VOLUME_DOWN", jump ? "double press" : "single press",
             before, volume);
}

/*
 * Double press: the FIRST press acts immediately (one step, exactly like any other
 * press), and a SECOND press of the same key within B220_BUTTON_DOUBLE_CLICK_MS is
 * taken as "go to the end of the range".
 *
 * The first version deferred the step by the whole double-click window, so that a
 * double press would never move one step before jumping.  That was the wrong
 * trade-off for this hardware: it put a ~400 ms lag on every ordinary volume press,
 * which is what a user actually does most of the time.  Acting immediately and
 * upgrading the second press costs one step of overshoot on a double press, which
 * the jump to the limit absorbs anyway, and removes the lag from the common path.
 *
 * The state is two variables rather than a timer, so nothing has to be created,
 * cancelled or cleaned up: s_last_volume_us records the previous press.
 */
static bool s_last_volume_up = false;
static int64_t s_last_volume_us = 0;

/*
 * Say WHICH key was accepted, not merely that one was: the indicator is an RGB
 * light (six parallel LEDs on the AW9106B's OUT0/OUT1/OUT2 - see b220_led.h), so
 * the colour can carry the meaning.  All of these are non-blocking: b220_led.c
 * services the effect on its own task, which is why calling this from the key
 * poller's task is safe.
 */
static void B220_KeyFeedback(b220_button_action_t action) {
    switch (action) {
    case B220_BTN_ACTION_RECORD:
        /* White: the talk key, which is also what a wake word does. */
        (void)b220_led_pulse(B220_LED_WHITE, 140, B220_LED_OFF);
        break;
    case B220_BTN_ACTION_VOLUME_UP:
        (void)b220_led_pulse(B220_LED_GREEN, 120, B220_LED_OFF);
        break;
    case B220_BTN_ACTION_VOLUME_DOWN:
        (void)b220_led_pulse(B220_LED_RED, 120, B220_LED_OFF);
        /*
         * THE FALLBACK FOR THE FALLBACK.  The log is readable over TCP on port 3333,
         * which is how it will be read once the baseboard covers the serial connector
         * - but that needs a working network, and "the network will not come up" is
         * itself one of the things worth debugging.  So a volume-down press also dumps
         * the recent log to the SERIAL port (see b220_tcp_log.h).  It is a deliberate
         * second job for a key that already has one: a debug aid must not depend on
         * the thing being debugged.  Set B220_TCP_LOG_DUMP_ON_VOLUME_DOWN to 0 in
         * config.h to retire it.
         */
#if B220_TCP_LOG_DUMP_ON_VOLUME_DOWN
        b220_tcp_log_dump();
#endif
        break;
    default:
        (void)b220_led_pulse(B220_LED_BLUE, 120, B220_LED_OFF);
        break;
    }
}

static void B220_ButtonAction(b220_button_action_t action) {
    Application& app = Application::GetInstance();

    /* Colour-coded acknowledgement on the RGB indicator. */
    B220_KeyFeedback(action);

    switch (action) {
    case B220_BTN_ACTION_RECORD:
        /* The record/talk key: the same entry point a wake word uses.  Acted on
         * immediately - deferring a conversation toggle would feel broken. */
        app.ToggleChatState();
        ESP_LOGI(TAG, "key RECORD: ToggleChatState() requested (state %s)",
                 DeviceStateMachine::GetStateName(app.GetDeviceState()));
        break;

    case B220_BTN_ACTION_VOLUME_UP:
    case B220_BTN_ACTION_VOLUME_DOWN: {
        const bool up = (action == B220_BTN_ACTION_VOLUME_UP);
        const int64_t now_us = esp_timer_get_time();

        /* A press of the same key inside the window is the second half of a double
         * press: jump to the end of the range.  The step the first press already
         * applied is absorbed by the jump. */
        const bool double_press =
            (up == s_last_volume_up) && (s_last_volume_us != 0) &&
            ((now_us - s_last_volume_us) < (int64_t)B220_BUTTON_DOUBLE_CLICK_MS * 1000);

        s_last_volume_up = up;
        s_last_volume_us = now_us;

        if (double_press) {
            ESP_LOGI(TAG, "key %s: double press -> jump to the %s of the range",
                     up ? "VOLUME_UP" : "VOLUME_DOWN", up ? "top" : "bottom");
            B220_ButtonSetVolume(up, /*jump=*/true);
        } else {
            /* One press, one step, applied NOW - no waiting to see whether a second
             * press follows. */
            B220_ButtonSetVolume(up, /*jump=*/false);
        }
        break;
    }

    default:
        break;
    }
}

static void B220_ButtonsStart() {
    const esp_err_t err = b220_buttons_start(B220_ButtonAction);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "the ZL38063 key poller did not start (%s); the three physical keys "
                      "will do nothing", esp_err_to_name(err));
    }
#if !B220_BUTTON_KEY_1 || !B220_BUTTON_KEY_2 || !B220_BUTTON_KEY_3
    ESP_LOGW(TAG, "the key->function mapping is NOT configured "
                  "(B220_BUTTON_KEY_1/2/3 = 0 in config.h): each press is logged with its raw "
                  "key value but no action is taken - see docs/BUTTON-DISCOVERY.md.");
#endif
}
#endif  /* !B220_BUTTON_DISCOVERY */

/*
 * Factory ILI9341 initialisation table, extracted from the stock firmware image
 * at DROM 0x3f4bdbf2 (18-byte records: command, 16 data bytes, "len | flags").
 * It is replayed verbatim because the panel is an unbranded module; guessing a
 * stock init sequence would risk wrong colours/gamma on a clone controller.
 *
 * Passing this table through ili9341_vendor_config_t *replaces* the driver's
 * default sequence, so sleep-out (0x11) and display-on (0x29) are included here.
 * Note the table itself issues 0x21 (INVON) and MADCTL 0x36 = 0x00, therefore
 * this board must not call esp_lcd_panel_invert_color().
 */
static const ili9341_lcd_init_cmd_t b220_lcd_init_cmds[] = {
    {0xC0, (uint8_t[]){0x00}, 1, 0},
    {0xC2, (uint8_t[]){0x01, 0xFF}, 2, 0},
    {0xC3, (uint8_t[]){0x13}, 1, 0},
    {0xC4, (uint8_t[]){0x20}, 1, 0},
    {0xC6, (uint8_t[]){0x0F}, 1, 0},
    {0xCA, (uint8_t[]){0x0F}, 1, 0},
    {0xC8, (uint8_t[]){0x08}, 1, 0},
    {0x55, (uint8_t[]){0xB0}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0xB1, (uint8_t[]){0x40, 0x02, 0x14}, 3, 0},
    {0x26, (uint8_t[]){0x01}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4, 0},
    {0x2C, (uint8_t[]){0x00}, 1, 0},
    {0xE0, (uint8_t[]){0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43,
                       0x47, 0x09, 0x15, 0x12, 0x16, 0x19}, 14, 0},
    {0xE1, (uint8_t[]){0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44,
                       0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19}, 14, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},  // sleep out (the stock table had no delay)
    {0x29, (uint8_t[]){0x00}, 0, 20},   // display on
};

#if B220_DIAGNOSTIC
/*
 * Bring-up diagnostics.  The stock firmware never touches a backlight pin (no
 * GPIO or I2C write exists for one in its image), so the panel light has to
 * come from something we have not identified yet.  These sweeps drive the
 * candidates one at a time so a human can watch the screen.
 */
static const int kBacklightProbePins[] = {2, 4, 5, 12, 13, 14, 15, 20, 24, 25, 26, 28, 29, 30, 31};

static void ProbePanelId(esp_lcd_panel_io_handle_t io) {
    uint8_t id[4] = {};
    esp_err_t err = esp_lcd_panel_io_rx_param(io, 0x04, id, sizeof(id));  // RDDID
    ESP_LOGW(TAG, "PROBE RDDID(0x04): err=%d -> %02X %02X %02X %02X (ILI9341 expects xx 00 93 41)",
             err, id[0], id[1], id[2], id[3]);
    uint8_t pm[3] = {};
    err = esp_lcd_panel_io_rx_param(io, 0x0A, pm, sizeof(pm));           // RDDPM
    ESP_LOGW(TAG, "PROBE RDDPM(0x0A): err=%d -> %02X %02X %02X", err, pm[0], pm[1], pm[2]);
}

static void ProbeBacklightPins() {
    ESP_LOGW(TAG, "PROBE >>> GPIO sweep starts: watch the screen for the next ~35s");
    for (int pin : kBacklightProbePins) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << pin;
        cfg.mode = GPIO_MODE_OUTPUT;
        if (gpio_config(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "PROBE GPIO%d: config failed", pin);
            continue;
        }
        ESP_LOGW(TAG, "PROBE GPIO%d = HIGH", pin);
        gpio_set_level((gpio_num_t)pin, 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        gpio_set_level((gpio_num_t)pin, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    ESP_LOGW(TAG, "PROBE <<< GPIO sweep done");
}

static void ProbeAw9106() {
    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = GPIO_NUM_18;
    bus_cfg.scl_io_num = GPIO_NUM_23;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "PROBE AW9106: i2c bus init failed");
        return;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = 0x5B;
    dev_cfg.scl_speed_hz = 100000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "PROBE AW9106: no device at 0x5B");
        i2c_del_master_bus(bus);
        return;
    }
    /* No {0x02, ...} entry: this probe runs from the board constructor AFTER the
     * PA enable sequence, so writing reg 0x02 here - even with the same value the
     * sequence already left - could only ever be a 0 -> 1 transition on the
     * AW8737S SHDN line, i.e. a second mode selection.  The group-A output level
     * is therefore left exactly as the enable sequence set it.  (The target
     * register of this sweep is DIM0..DIM5, 0x20..0x25, not group A.)
     *
     * WARNING for whoever re-enables B220_DIAGNOSTIC/B220_BACKLIGHT_SWEEP: the
     * {0x7F, 0x00} software reset in this sequence resets the whole chip, so the
     * GPMD_A = 0x02 and GPIO_CFG_A = 0x00 that the enable sequence latched are
     * gone afterwards.  The two writes below re-assert them (through the audited
     * writers, which is why they can be seen in the log), but the PA itself is NOT
     * re-enabled - doing that from here would be a second SHDN rising edge (mode 2
     * at best).  Re-run the PA enable sequence AFTER this probe if this switch is
     * ever turned back on. */
    const uint8_t init_seq[][2] = {
        {0x7F, 0x00}, {0x11, 0x10}, {0x12, 0x02}, {0x04, 0x00}, {0x13, 0x00},
    };
    for (auto& kv : init_seq) {
        esp_err_t e;
        if (kv[0] == B220_AW9106_REG_GPMD_A) {
            e = b220_aw9106_write_gpmd_a(dev, kv[1], "PROBE AW9106: re-assert GPMD_A");
        } else if (kv[0] == B220_AW9106_REG_GPIO_CFG_A) {
            e = b220_aw9106_write_gpio_cfg_a(dev, kv[1], "PROBE AW9106: re-assert GPIO_CFG_A");
        } else {
            uint8_t b[2] = {kv[0], kv[1]};
            e = i2c_master_transmit(dev, b, 2, 100);
        }
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "PROBE AW9106: write 0x%02X failed (device missing?)", kv[0]);
            i2c_master_bus_rm_device(dev);
            i2c_del_master_bus(bus);
            return;
        }
    }
    ESP_LOGW(TAG, "PROBE >>> AW9106 sweep starts: watch the screen for the next ~15s");
    for (int ch = 0; ch < 6; ++ch) {
        uint8_t on[2] = {(uint8_t)(0x20 + ch), 0xFF};
        ESP_LOGW(TAG, "PROBE AW9106 DIM%d = 0xFF", ch);
        i2c_master_transmit(dev, on, 2, 100);
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint8_t off[2] = {(uint8_t)(0x20 + ch), 0x00};
        i2c_master_transmit(dev, off, 2, 100);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    ESP_LOGW(TAG, "PROBE <<< AW9106 sweep done");
    i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);
}
#endif  // B220_DIAGNOSTIC

/*
 * The LCD backlight, as the framework's Backlight interface wants it.
 *
 * The panel's light is AW9106B DIM3/DIM4 (registers 0x23/0x24) - measured, and the
 * reason b220_led.c never drives those two.  Implementing this interface rather
 * than inventing a settings layer is what makes the brightness both adjustable AND
 * persistent: Backlight::SetBrightness(percent, permanent=true) writes
 * "display"/"brightness" to NVS, and Backlight::RestoreBrightness() reads it back,
 * so the board's only job is the register conversion.
 *
 * Percent (0..100) -> DIMx (0..255), rounded so that 50% is 128 rather than 127.
 * Nothing is written when the AW9106B never came up: the panel is dark either way
 * and a failing I2C write on every brightness change would only add noise.
 */
class B220_Backlight : public Backlight {
public:
    B220_Backlight() = default;

protected:
    /*
     * Apply the saved brightness at START-UP, directly, and make sure the base
     * class does not then override it with its own default.
     *
     * Two separate problems are solved here, and the first version of this code
     * only solved one of them (measured: the panel came up at 100% and was then
     * visibly dragged down to 75%, one step per 5 ms):
     *
     * 1. NO FADE AT START-UP.  Backlight::SetBrightness() only stores a TARGET and
     *    the register writes happen one step per 5 ms from the CURRENT value, which
     *    at boot is 0.  Restoring through it would push a hundred near-black values
     *    into DIM3/DIM4 - and a low value there is measured to switch this panel OFF
     *    - so the screen would go dark and crawl back up on every boot.  Writing the
     *    hardware once and priming brightness_ makes RestoreBrightness() take the
     *    base class's "value unchanged" early return, so no timer starts at all.  A
     *    fade is right for a change the user makes; it is wrong for a restore.
     *
     * 2. WHICH DEFAULT WINS.  The base class's RestoreBrightness() falls back to 75%
     *    when nothing is stored - so on a device that has never had its brightness
     *    changed, its default fought this board's (B220_LED_BACKLIGHT_DEFAULT_
     *    PERCENT, 100%): the board applied 100%, then the base class read "nothing
     *    stored -> 75" and faded down to it.  The fix is to persist this board's
     *    default once, so that from then on "nothing stored" cannot happen.
     *
     * Settings has no "does this key exist" call, hence the sentinel: a value that is
     * not a valid percentage can only mean "never stored".  (Erasing the key later,
     * e.g. a factory reset, brings the board default back rather than 75%.)
     */
    static constexpr int kNeverStored = -1;

public:
    void ApplySavedBrightnessNow() {
        Settings settings("display", true);   /* read_write: the persist below needs it */
        const int stored = settings.GetInt("brightness", kNeverStored);

        int percent;
        if (stored == kNeverStored || stored < 0 || stored > B220_LED_BACKLIGHT_MAX_PERCENT) {
            percent = B220_LED_BACKLIGHT_DEFAULT_PERCENT;
            settings.SetInt("brightness", percent);
            ESP_LOGI(TAG, "backlight: no saved brightness (or an invalid one: %d) - applying this "
                          "board's default %d%% and storing it",
                     stored, percent);
        } else {
            percent = stored;
            ESP_LOGI(TAG, "backlight: applying the saved brightness %d%%", percent);
        }

        brightness_ = (uint8_t)percent;
        target_brightness_ = brightness_;
        step_ = 1;
        SetBrightnessImpl(brightness_);
    }

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        if (s_board_aw9106_dev == nullptr) {
            ESP_LOGW(TAG, "backlight %u%%: the AW9106B is not attached, nothing written",
                     (unsigned)brightness);
            return;
        }
        const uint8_t dim = (uint8_t)(((uint16_t)brightness * 255u + 50u) / 100u);
        const uint8_t regs[2] = {0x23, 0x24};   /* DIM3 / DIM4 = the panel light */
        for (int i = 0; i < 2; i++) {
            const uint8_t b[2] = {regs[i], dim};
            const esp_err_t err = i2c_master_transmit(s_board_aw9106_dev, b, sizeof(b), 100);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "backlight %u%% (DIM=0x%02X): reg 0x%02X write failed (%s)",
                         (unsigned)brightness, (unsigned)dim, (unsigned)regs[i],
                         esp_err_to_name(err));
                return;
            }
        }
        ESP_LOGI(TAG, "backlight %u%% -> AW9106B DIM3/DIM4 (reg 0x23/0x24) = 0x%02X",
                 (unsigned)brightness, (unsigned)dim);
    }
};

class XiaoMoDouB220Board : public WifiBoard {
private:
    LcdDisplay* display_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;

protected:
    /*
     * The board's backlight object.  Created lazily by GetBacklight() so that the
     * Backlight base constructor (which creates a fade timer) runs inside a normal
     * task, not during the board's static construction.
     */
    B220_Backlight* backlight_ = nullptr;

    /*
     * The panel backlight is NOT a GPIO on this board: the stock firmware never
     * drives one.  It brings up the AW9106B LED driver on I2C0 (0x5B, SDA 18 /
     * SCL 23 @100 kHz) and sets DIM0/DIM1/DIM2 to 0x3F (max) - those are the
     * only persistent non-zero channel writes in the whole stock image.
     * Without this the panel shows content but stays dark.
     * Replay: chip_aw9110b_init (stock 0x402078dc) -> mode-3 setup (0x40207a6c)
     * -> DIM0/1/2 = max (0x40207b90), then DIM3/4 = 0x80 (stock 0x400d622e).
     * The replay is NOT verbatim any more: the stock tail's {0x12, 0x08} entry is
     * dropped (0x08 is the group-A mode in which the outputs do not drive the AW8737S
     * SHDN pin, so it killed the audio PA) and the 0x12/0x04 writes go through the
     * audited writers.  The DIM writes that light the panel are untouched.
     */
    void InitializeBacklight() {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = I2C_NUM_0;
        bus_cfg.sda_io_num = B220_AW9106_I2C_SDA;
        bus_cfg.scl_io_num = B220_AW9106_I2C_SCL;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = 1;
        esp_err_t err = i2c_new_master_bus(&bus_cfg, &i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AW9106B: i2c bus init failed (%s); backlight stays off", esp_err_to_name(err));
            return;
        }
        /* Publish the bus for b220_led_i2c_bus() and
         * b220_button_discovery_i2c_bus().  Done here, and not after the AW9106B
         * device is registered, so the borrowers can see the bus even if the LED
         * driver itself does not answer. */
        s_board_i2c_bus = i2c_bus_;

        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = B220_AW9106_ADDR;
        dev_cfg.scl_speed_hz = 100000;
        i2c_master_dev_handle_t dev = nullptr;
        err = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AW9106B: cannot register device 0x%02X (%s)", B220_AW9106_ADDR, esp_err_to_name(err));
            return;
        }

        static const uint8_t init_seq[][2] = {
            {0x7F, 0x00},  // software reset
            {0x11, 0x10},  // CTL
            {0x12, 0x0A},  // GPMD_A = GPIO bit | LED-driver bit: SHDN can move AND the
                           // six common-anode RGB LEDs on OUT0/1/2 can be driven (0x08
                           // alone drives the LEDs but was measured to silence the PA)
            {0x04, 0x00},  // GPIO_CFG_A = all group-A pins outputs
            /* NO {0x02, ...} HERE - the audio PA's SHDN line is reg 0x02 bit 1 and
             * the AW8737S counts 0 -> 1 transitions as MODE SELECTIONS (see the
             * pulse block at the top of this file).  Register 0x02 is therefore
             * programmed exactly once per boot, by the audited write below, so the
             * write count and the edge count cannot drift: the stock sequence's
             * 0x02 <- 0x00 is folded into that single call. */
            {0x13, 0x00},  // GPMD_B
            {0x20, 0x00}, {0x21, 0x00}, {0x22, 0x00},
            {0x23, 0x00}, {0x24, 0x00}, {0x25, 0x00},
            {0x14, 0x07},  // breathing/fade setup
            {0x05, 0x07},  // GPIO_CFG_B
            {0x17, 0x01}, {0x18, 0x01}, {0x19, 0x01},
            {0x11, 0x1B},  // CTL (stock value)
            /* The stock tail replays {0x12, 0x08} (GPMD_A) right here.  THAT ENTRY
             * IS DELETED ON PURPOSE: 0x08 is the value with which the group-A
             * outputs drive nothing at all, so it switched the audio PA back off
             * behind the enable sequence - it ran AFTER the 0x12 <- 0x02 above and
             * is the difference between no sound and sound on this board (proven
             * with the B220_PA_BIT_FINDER walk).  GPMD_A must stay 0x02 from here
             * on; the enable sequence re-asserts it, and the post-enable freeze
             * refuses any later change.  The 0x11 <- 0x1B CTL value is kept - it
             * is not one of the PA's registers.
             *
             * FLIP SIDE: if the panel ever needs the group-A DIM/PWM engine back,
             * that engine is what 0x08 selects, and the only way to have both is a
             * GPMD_A value that keeps bit 1 in the GPIO/output mode AND the DIM
             * pins in theirs (e.g. 0x0A).  That combination has never been tested
             * on this hardware, so it is NOT used here: 0x02 is the value that was
             * audible, and the DIM registers below (0x20..0x25) are the ones that
             * actually set the backlight level. */
            /* DIM0/1/2 = the six parallel common-anode RGB LEDs on OUT0/1/2, which
             * b220_led.c drives.  They start OFF, and that is what the module's idle
             * state means.  (This port used to park them at 0x3F, believing they were
             * the backlight: at 25% of an 8-bit range nothing was visible, which is
             * what made the RGB LEDs look unconnected.) */
            {0x20, 0x00},  // DIM0 = OUT0 = RED   - driven by b220_led.c
            {0x21, 0x00},  // DIM1 = OUT1 = GREEN - driven by b220_led.c
            {0x22, 0x00},  // DIM2 = OUT2 = BLUE  - driven by b220_led.c
            /* DIM3/DIM4 are the LCD backlight on this unit: measured, and the reason
             * this module never drives them below 0x80 (a lower level switched the
             * panel off). */
            /* The backlight could go brighter (measured: 0xFF is the brightest, the
             * panel steps up at every value), but 0x80 is the FACTORY value and this
             * port keeps it: brightness is a power/heat trade-off, not a correctness
             * issue.  Must stay >= 0x80 - a lower value switched the panel off. */
            {0x23, 0x80},  // DIM3 = LCD backlight (factory value; see config.h)
            {0x24, 0x80},  // DIM4 = LCD backlight-related
            /* The output-driver configuration WITHOUT which the RGB channels stay dark
             * even at DIMx = 0xFF.  Both values are taken from the independent port of
             * this toy, which lights the same LEDs; neither register was ever written
             * by this port before.  Measured afterwards on this board: with these two
             * writes plus DIMx, OUT0/1/2 light red/green/blue/white. */
            {0x15, 0x1B},  // output driver config (never written before; see above)
            {0x16, 0x08},  // output driver config (never written before; see above)
            {0x14, 0x00},  // steady output: no breathing/pulse override of the DIM values
        };
        for (auto& kv : init_seq) {
            /* The two DRIVE-MODE registers (0x12 GPMD_A, 0x04 GPIO_CFG_A) go through
             * the audited writers, so their writes are logged, counted and tracked
             * for the final-state line exactly like the 0x02 writes; everything
             * else in this stock replay is a plain write. */
            if (kv[0] == B220_AW9106_REG_GPMD_A) {
                err = b220_aw9106_write_gpmd_a(dev, kv[1], "backlight init: stock init 0x12");
            } else if (kv[0] == B220_AW9106_REG_GPIO_CFG_A) {
                err = b220_aw9106_write_gpio_cfg_a(dev, kv[1], "backlight init: stock init 0x04");
            } else {
                uint8_t b[2] = {kv[0], kv[1]};
                err = i2c_master_transmit(dev, b, 2, 100);
            }
            if (err != ESP_OK) {
                // A missing/unpowered AW9106B must not abort the boot.
                ESP_LOGW(TAG, "AW9106B: write 0x%02X=0x%02X failed (%s)", kv[0], kv[1], esp_err_to_name(err));
                break;
            }
        }
        /* The one and only reg-0x02 write of the boot: SHDN low, which is also
         * what the stock init sequence programmed (the AW9106B's own reset
         * default is 0x00 as well, so this is not an edge - bit 1 was already 0
         * and the audit line proves it with `bit1=0, rising edges so far=0`).
         * The enable pulse later re-writes this same value as the low hold of the
         * sequence, so the boot still has exactly one 0 -> 1 transition. */
        err = b220_aw9106_write_reg02(dev, B220_LOOPBACK_PA_OFF,
                                      "backlight init: SHDN low (no PA enable)");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AW9106B: reg 0x%02X <- 0x%02X failed (%s); the PA enable "
                          "sequence will still try (and will report the failure)",
                     (unsigned)B220_LOOPBACK_PA_REG, (unsigned)B220_LOOPBACK_PA_OFF,
                     esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "AW9106B backlight on (DIM0/1/2 = 0x3F)");
        /* Keep the handle: the ZL38063 diagnostic and the audio-loopback PA
         * enable both write group A through it. */
        s_board_aw9106_dev = dev;
    }

    /*
     * SPI2 is shared by the LCD (CS 27) and, on the stock hardware, the ZL38063
     * HBI port (CS 0 / GPIO0, 10 MHz).  Phase 2 only brings up the LCD device.
     */
    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_SPI_MISO_PIN;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        // Keep the bus DMA buffer modest; LVGL batches flushes itself.
        buscfg.max_transfer_sz = 16 * 1024;
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

#if B220_ORIENT_CARD
    /*
     * One-shot bring-up aid: 8 colour bars (R G B White blacK Yellow Cyan
     * Magenta) plus white 12x12 blocks in the top-left and bottom-right corners.
     * A human can then report orientation, mirroring, offsets and colour order
     * in a single pass.
     *
     * The card is drawn with a RAW esp_lcd_panel_draw_bitmap(), i.e. in PANEL
     * coordinates, so it uses the glass size (B220_DISPLAY_PANEL_*) and not the
     * LVGL logical size.  With B220_DISPLAY_ROTATE_90 = 1 the panel has
     * swap_xy set, so what appears on screen is this card rotated by 90 deg:
     * the 8 bars come out as horizontal bands from one edge, and "left" in the
     * buffer is "top" on the glass.  Read it like that:
     *
     *   portrait  (rotate90 = 0): bars are VERTICAL stripes from the left edge,
     *                             white corners at top-left + bottom-right;
     *   landscape (rotate90 = 1): bars are HORIZONTAL bands from the top edge,
     *                             white corners at top-right + bottom-left
     *                             (or top-left + bottom-right if the rotation
     *                             went the other way).
     *
     * Either way, exactly ONE white corner must be in the top row and one in the
     * bottom row, and the bar order must read R G B W K Y C M.  If the corners
     * are in the same column there is no rotation; if the colours read
     * R B G ... the byte order is wrong (a raw write must hand the panel the
     * high byte first, hence the __builtin_bswap16() below - the LVGL path does
     * this itself via swap_bytes).
     */
    void DrawOrientationCard(esp_lcd_panel_handle_t panel) {
        const int w = B220_DISPLAY_PANEL_WIDTH, h = B220_DISPLAY_PANEL_HEIGHT;
        auto* fb = (uint16_t*)heap_caps_malloc(w * h * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!fb) {
            ESP_LOGW(TAG, "orientation card: no DMA-capable memory");
            return;
        }
        const uint16_t bars[8] = {0xF800, 0x07E0, 0x001F, 0xFFFF,
                                  0x0000, 0xFFE0, 0x07FF, 0xF81F};
        for (int x = 0; x < w; ++x) {
            const uint16_t c = __builtin_bswap16(bars[(x * 8) / w]);
            for (int y = 0; y < h; ++y) {
                fb[y * w + x] = c;
            }
        }
        for (int y = 0; y < 12; ++y) {
            for (int x = 0; x < 12; ++x) {
                fb[y * w + x] = 0xFFFF;                      // top-left marker
                fb[(h - 1 - y) * w + (w - 1 - x)] = 0xFFFF;  // bottom-right marker
            }
        }
        ESP_LOGW(TAG, "ORIENT CARD panel %dx%d (LVGL logical %dx%d, rotate90=%d): bars R G B W K Y C M "
                      "in panel order, white corners at panel TL+BR",
                 w, h, DISPLAY_WIDTH, DISPLAY_HEIGHT, (int)B220_DISPLAY_ROTATE_90);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, w, h, fb));
        free(fb);
    }
#endif

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 10 * 1000 * 1000;  // the stock firmware runs this bus at 10 MHz
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        // Reset is done manually below with the stock timing, so the panel
        // driver must not drive the pin itself.
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;

        ili9341_vendor_config_t vendor_config = {
            .init_cmds = b220_lcd_init_cmds,
            .init_cmds_size = sizeof(b220_lcd_init_cmds) / sizeof(b220_lcd_init_cmds[0]),
        };
        panel_config.vendor_config = &vendor_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

        // The stock firmware asserts reset (active low, cfg[16] = 0) for 100 ms,
        // releases it and waits another 100 ms before sending commands.
        // esp_lcd's built-in reset only waits ~10 ms, which some ILI9341 clones
        // do not tolerate, so we replay the stock timing ourselves.
        gpio_config_t rst_cfg = {};
        rst_cfg.pin_bit_mask = 1ULL << DISPLAY_RST_PIN;
        rst_cfg.mode = GPIO_MODE_OUTPUT;
        ESP_ERROR_CHECK(gpio_config(&rst_cfg));
        gpio_set_level(DISPLAY_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(DISPLAY_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
#if B220_DIAGNOSTIC
        ProbePanelId(panel_io);
#endif
        // No esp_lcd_panel_invert_color() / disp_on_off() here on purpose:
        // the factory table already sends 0x21 and 0x29.
        //
        // The 90 deg rotation (B220_DISPLAY_ROTATE_90 in config.h) is applied
        // HERE, as a real MADCTL write, and not through lv_display_set_rotation():
        // this port never calls set_rotation(), so lvgl_port_disp.c's
        // lvgl_port_disp_rotation_update() stays at LV_DISPLAY_ROTATION_0 and
        // re-asserts exactly the transform we pass to SpiLcdDisplay() below.
        // The two arguments are the same DISPLAY_SWAP_XY / DISPLAY_MIRROR_*
        // macros, so the panel and the LVGL resolution can never disagree.
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        /* A 90 deg rotation is "logical X runs along the panel's Y".  The numbers
         * to check at the bench are these three: the glass is physically
         * 128 x 160 (B220_DISPLAY_PANEL_*), the LVGL screen must be the other way
         * round, and the panel must have been told exactly the transform we then
         * hand to SpiLcdDisplay().  If the picture comes out rotated the wrong
         * way, flip DISPLAY_MIRROR_X together with DISPLAY_MIRROR_Y in config.h
         * (that reverses the direction of the rotation and nothing else). */
        ESP_LOGI(TAG, "LCD geometry: glass %dx%d -> LVGL logical %dx%d "
                      "(swap_xy=%d mirror_x=%d mirror_y=%d, rotate90=%d, MADCTL written per driver cache)",
                 B220_DISPLAY_PANEL_WIDTH, B220_DISPLAY_PANEL_HEIGHT,
                 DISPLAY_WIDTH, DISPLAY_HEIGHT,
                 (int)DISPLAY_SWAP_XY, (int)DISPLAY_MIRROR_X, (int)DISPLAY_MIRROR_Y,
                 (int)B220_DISPLAY_ROTATE_90);
#if !B220_DISPLAY_LOGICAL_IS_VALID()
#error "DISPLAY_WIDTH/HEIGHT do not match the panel size for the selected rotation (see config.h)"
#endif

#if B220_ORIENT_CARD
        DrawOrientationCard(panel);
        vTaskDelay(pdMS_TO_TICKS(5000));  // let a human read the card
#endif

        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

#if B220_AUDIO_BRINGUP
#if B220_DSP_HANDS_OFF
    /*
     * Phase 3-⑨ : hands-off mode (B220_DSP_HANDS_OFF in config.h).
     *
     * A 60 s boot log of the STOCK firmware on this board (baseboard unplugged,
     * sound works) contains only {AT+Pon} spam after startup: it never prints
     * "1- Firmware boot loading started ....", "prgmBase 0x…", "execAddr 0x…" or
     * "2- Loading the image to RAM....done".  The stock firmware therefore never
     * host-boots the ZL38063 - the DSP auto-boots from its own slave SPI flash
     * and the stock only configures it and talks to it over I2S.
     *
     * Our host-boot path does the opposite (GPIO19 reset pulse, reg 0x14 <- 1 =
     * forced boot-ROM mode, 3699 record writes, entry point, GO), which may be
     * destroying a perfectly running DSP application - and would explain why the
     * upload "succeeds" while reg 0x030 never reports a running app.
     *
     * So this path does the absolute minimum: I2S up (unchanged call), attach the
     * HBI device, write the single HBI configuration register that the stock
     * driver itself writes, and then only READ the DSP for ~20 s to see whether
     * the auto-booted application reports RUNNING on its own.
     *
     * GPIO19 - the DSP reset - is never configured, driven or read here, so the
     * reset net is left exactly as the board powers it up.  Everything that
     * writes to the DSP (reset, boot prepare, upload, matrix, mailbox probe,
     * verify-upload window writes, config record, volume) and the diagnostics
     * that reconfigure pins (zl38063_dsp_gpio_forensics(), which drives GPIO19;
     * zl38063_dsp_hbi_sweep(), which rebinds CS) are deliberately not called.
     */
    void InitializeAudioDspHandsOff() {
#if B220_I2S_BEFORE_DSP
        /* The unchanged I2S-before-DSP call: the ZL38063 keeps its BCLK.  It
         * touches the ESP32 I2S peripheral only - no DSP access at all. */
        esp_err_t i2s_err = B220_I2sUpBeforeDspBoot();
        if (i2s_err != ESP_OK) {
            ESP_LOGE(TAG, "hands-off: I2S bring-up failed (%s); continuing "
                          "without a DSP clock", esp_err_to_name(i2s_err));
        }
#else
        ESP_LOGW(TAG, "hands-off: B220_I2S_BEFORE_DSP is 0, so no I2S is brought "
                      "up before the HBI attach (the DSP may miss its BCLK)");
#endif
        /* Attach the HBI device + write the HBI config register, nothing else. */
        esp_err_t err = zl38063_dsp_hands_off_attach();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "hands-off: HBI attach/config failed (%s) - no further "
                          "DSP access is possible", esp_err_to_name(err));
            return;
        }

        int first_running_s = -1;   /* first poll whose masked status was non-zero */
        int last_status = -1;       /* last masked status read, -1 = never answered */
        for (int s = 1; s <= B220_DSP_HANDS_OFF_POLLS; s++) {
            vTaskDelay(pdMS_TO_TICKS(1000));    /* one poll per second */
            int status = zl38063_dsp_status_line((unsigned)s);
            if (status < 0) {
                continue;               /* reg 0x030 did not answer this second */
            }
            last_status = status;
            if (status != ZL38063_APP_STATUS_NOT_RUNNING && first_running_s < 0) {
                first_running_s = s;
                ESP_LOGW(TAG, "hands-off: the DSP application status (reg 0x0030 "
                              "& 0x0F) first became non-zero (0x%02X) at t=+%ds - "
                              "the DSP is running its own application",
                         (unsigned)status, s);
            }
        }

        if (first_running_s > 0) {
            ESP_LOGI(TAG, "hands-off verdict: DSP application reported RUNNING "
                          "after %d s", first_running_s);
        } else {
            ESP_LOGW(TAG, "hands-off verdict: DSP application never reported "
                          "running within %d s", B220_DSP_HANDS_OFF_POLLS);
            if (last_status < 0) {
                ESP_LOGW(TAG, "hands-off: reg 0x0030 never answered during the "
                              "polls (HBI dead, DSP not selected, or no BCLK)");
            } else {
                ESP_LOGW(TAG, "hands-off: reg 0x0030 & 0x0F stayed 0x%02X for "
                              "every poll", (unsigned)last_status);
            }
        }
    }
#endif  // B220_DSP_HANDS_OFF

    /*
     * Phase 3-1: prove the HBI link to the ZL38063 and report what it says.
     * Two possible states:
     *   (a) the DSP already runs its firmware from its own slave flash - then we
     *       only push our configuration record and set the volume;
     *   (b) it sits in its boot ROM waiting for a host-boot image (register 0x34
     *       reads 0xD3D3 in that state) - the stock firmware appears to provide
     *       that image, and we do not own a copy, so this is reported loudly.
     */
    void InitializeAudioDsp() {
#if B220_DSP_HANDS_OFF
        /* Phase 3-⑨ : hands-off.  Do NOT reset the DSP and do NOT host-boot it -
         * the evidence is in InitializeAudioDspHandsOff() above.  Set
         * B220_DSP_HANDS_OFF to 0 in config.h (one line) to get the host-boot
         * path in the #else branch back, byte for byte as it is today. */
        InitializeAudioDspHandsOff();
#else
        /*
         * Phase 3-⑥ : ESP32 I2S first, DSP reset second.
         *
         * The ZL38063's master clock is this port's BCLK, so the clock has to be
         * present in the window between the GPIO19 reset release and the HBI
         * boot handshake - that is the order the stock firmware uses and the one
         * thing our previous build (no I2S at all) was missing.
         * B220_I2S_BEFORE_DSP = 0 in config.h puts the old single-shot order back.
         */
        esp_err_t err = ESP_OK;
#if B220_DSP_VERIFY_BEFORE_GO && !B220_DSP_ADF_RELEASE_TX
        /* Phase 3-11 : attach the pre-GO observer BEFORE the first reset + host
         * boot of this boot.  Every zl38063_dsp_begin() below - the first one and
         * each retry - is a full upload, and the hook runs once per upload at the
         * instant the last record has been written and reg 0x12C / reg 0x32 <-
         * 0x000D have NOT been sent yet (see zl38063_dsp_set_pre_go_hook()).
         * That is what separates "the transfer never put the image in RAM" from
         * "the image was there and the start/crash later cleared it" - the
         * post-GO verify further down this function covers the other side of the
         * same boot.  Registration is unconditional here; whether the hook is ever
         * called is decided by B220_DSP_VERIFY_BEFORE_GO inside the upload path.
         *
         * It stays registered while B220_DSP_RELEASE_MATRIX is 1 as well, and both
         * go through the one shared read-back (zl38063_dsp_sample_records_readback(),
         * zl38063_dsp.c): the matrix needs the same pre-GO numbers, taken at the
         * same instant, to compare with its post-release ones - that comparison is
         * the whole experiment.
         *
         * Phase 3-15: NOT registered on the ADF-order path
         * (B220_DSP_ADF_RELEASE_TX): that experiment measures the device after a
         * release that no read-back disturbed, so the pre-GO observer (and its
         * page-255 window reads) is deliberately left out of it. */
        zl38063_dsp_set_pre_go_hook(B220_PreGoVerify);
#endif
#if B220_I2S_BEFORE_DSP
        esp_err_t i2s_err = B220_I2sUpBeforeDspBoot();
        if (i2s_err != ESP_OK) {
            // Deliberately not fatal for the diagnostics: the DSP bring-up is
            // still worth running and reporting when the clock cannot be set up.
            ESP_LOGE(TAG, "ZL38063: I2S bring-up before the DSP boot failed (%s)",
                     esp_err_to_name(i2s_err));
        }
#if !B220_DSP_ADF_RELEASE_TX
        /*
         * Phase 3-16 (3/3): this inner branch has NO ADF call site any more.
         *
         * It used to call zl38063_dsp_release_adf_tx() itself while the
         * B220_DSP_ADF_RELEASE_TX block below (after the #endif of
         * B220_I2S_BEFORE_DSP) called it too, so with
         * B220_I2S_BEFORE_DSP=1 (config.h) BOTH call sites compiled into one
         * boot and the function was entered twice - which is why its rejection
         * line appeared twice in the log (it was one call site too many, not one
         * call site entered twice).  The single call site is the one below; on
         * this path (ADF off) the normal reset + host boot with its retry loop
         * stays exactly as it was.
         */
        err = zl38063_dsp_begin();
#if !B220_DSP_RELEASE_MATRIX
        /* Retry the whole reset + boot sequence while the DSP still reports no
         * application: if the missing clock was the blocker, one of these
         * attempts answers once BCLK has been running for a moment.  The stock
         * firmware retries this path 6 times.
         *
         * Phase 3-12: NOT done while the hook-release matrix is compiled in.  The
         * upload is already proven correct there (the pre-GO observer reads all 9
         * sampled records back identical before any release command), so five more
         * identical full uploads would only repeat that proof while adding ~25 s of
         * boot; the matrix's own first variant is that same fresh attempt.  The
         * default path (matrix off) keeps the 6-attempt retry loop unchanged. */
        int attempt = 1;
        while (err == ESP_OK && !zl38063_dsp_is_running() && attempt < B220_DSP_BOOT_ATTEMPTS) {
            attempt++;
            ESP_LOGW(TAG,
                     "ZL38063: no application after attempt %d/%d - retrying "
                     "the reset + host boot (I2S clock is up)",
                     attempt - 1, B220_DSP_BOOT_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(100));
            err = zl38063_dsp_begin();
        }
        ESP_LOGI(TAG, "ZL38063: %d boot attempt(s), responding=%s, last err=%s", attempt,
                 zl38063_dsp_is_running() ? "yes" : "no", esp_err_to_name(err));
#else
        ESP_LOGI(TAG, "ZL38063: single reset + host boot (B220_DSP_RELEASE_MATRIX is 1, "
                      "so the 6-attempt retry loop is skipped and the release matrix "
                      "below runs one fresh attempt per variant instead); responding=%s, "
                      "last err=%s", zl38063_dsp_is_running() ? "yes" : "no",
                 esp_err_to_name(err));
#endif
#endif  /* !B220_DSP_ADF_RELEASE_TX: the normal reset + host boot, with its retry
         * loop; on the ADF-order path the single call site below runs that same
         * reset + upload inside zl38063_dsp_release_adf_tx() itself (step 1/5) */
#else   /* !B220_I2S_BEFORE_DSP: there is no I2S bring-up above, so the normal
         * path boots the DSP here.  The ADF-order path can never reach this
         * branch - line 49 fails the build for B220_DSP_ADF_RELEASE_TX=1 with
         * B220_I2S_BEFORE_DSP=0 - so no second #if is needed around the call. */
        err = zl38063_dsp_begin();
#endif
#if B220_DSP_ADF_RELEASE_TX
        /*
         * Phase 3-16 : THE ADF-order call site - the only one in this file, and
         * the only one per boot.  It sits outside the #if B220_I2S_BEFORE_DSP
         * branch (and outside that switch's #else too), so it always runs after
         * B220_I2sUpBeforeDspBoot() has created the I2S0 handles and started
         * BCLK/WS: the DSP takes its clock from that port, and this switch
         * combination is the only supported one (line 49 fails the build for
         * B220_DSP_ADF_RELEASE_TX=1 with B220_I2S_BEFORE_DSP=0).
         *
         * The earlier attempt at this fix only moved the call here and left the
         * second call site inside the I2S branch: both compiled, the function
         * was entered twice per boot, and BOTH entries were then rejected by its
         * old s_hbi_ready entry guard (the message the log showed twice).  Phase
         * 3-16 (2/3) removed that guard - the function's own step 1/5 attaches
         * the bus, so the entry check could never pass - and (3/3) removed the
         * duplicate call site above.
         *
         * This call IS the DSP bring-up on this path: it opens with
         * zl38063_dsp_begin() (GPIO19 reset, HBI attach, 0xFD80, the 3699-record
         * upload, reg 0x12C), so no zl38063_dsp_begin() is issued on its own
         * here.  Nothing follows it on this path either, so the whole ADF order
         * reaches the device before the loopback reads the first sample. */
        err = zl38063_dsp_release_adf_tx();
        ESP_LOGI(TAG, "ZL38063: ADF-order release path (B220_DSP_ADF_RELEASE_TX=1): "
                      "one reset + one full upload + the ADF release sequence "
                      "(0x12C, 0x000D, config record, GO, soft reset), no retry "
                      "loop, no diagnostic - responding=%s, last err=%s",
                 zl38063_dsp_is_running() ? "yes" : "no", esp_err_to_name(err));
#endif  /* B220_DSP_ADF_RELEASE_TX */

#if B220_ZL_DIAG && !B220_DSP_ADF_RELEASE_TX
        // Phase 3-2 diagnostic: zl38063_dsp_begin() has attached the HBI device
        // to SPI2 by now (even when the host boot below failed), so dump the
        // interesting registers in both byte orders and read the pin levels.
        // Read-only on the bus; placed before the early return on purpose, so
        // it also runs in the "DSP does not answer" case we are chasing.
        zl38063_dsp_hbi_diag();
        // Phase 3-3 diagnostic: GPIO19 ownership matrix + AW9106 group-A probe.
        // Runs after the HBI diag on purpose (same read-only HBI usage), and in
        // the failure case that the diag above is chasing.  It restores GPIO19
        // and the AW9106 register before it returns.
        zl38063_dsp_gpio_forensics();
        // Phase 3-4 diagnostic: brute-force the HBI transport - every plausibly
        // free CS pin x SPI mode x clock, one read-only 1-word read of register
        // 0x0034 each, then the normal device (CS 0 / mode 0 / 10 MHz) is
        // restored.  This is the last open variable: which GPIO is really the
        // DSP chip-select.  Read-only on the bus and on the device.
        zl38063_dsp_hbi_sweep();
        // Phase 3-5 diagnostic: which GPIO carries the baseboard UART?  Kept for
        // the later baseboard/motor work, but deliberately NOT run for audio:
        // the stock firmware plays sound with the baseboard unplugged, so the
        // baseboard handshake cannot be what brings the DSP up - and this probe
        // costs ~3.5 s of boot and briefly floats GPIO19 (the DSP reset).
        // b220_baseboard_uart_sweep();
#endif
#if B220_DSP_RELEASE_MATRIX && !B220_DSP_ADF_RELEASE_TX
        /*
         * Phase 3-12 : the hook-release matrix (B220_DSP_RELEASE_MATRIX).
         *
         * The upload is PROVEN correct by now - the pre-GO observer just read
         * its 9 sampled records back identical, before any host command was sent,
         * while reg 0x0034 still held 0xD3D3 - and yet the release sequence this
         * path normally ends with (reg 0x12C <- execAddr, reg 0x32 <- 0x000D,
         * VprocTwolfFirmwareStart() -> reg 0x32 <- 0x0008) leaves reg 0x0030
         * reporting "not running" and the image area reading zeros.  The open
         * question is which of those two host commands actually releases the boot
         * ROM: the early RE marked the vendor's VprocTwolfFirmwareStart
         * (0x40126210) as a delay stub (ets_delay_us), so stock may never write
         * 0x0008 at all.
         *
         * The matrix answers it with one FULL fresh attempt per variant (GPIO19
         * reset pulse, HBI attach, the whole 3699-record upload) and that variant's
         * release writes only, then ~1 s settle and the four status registers plus
         * the same 9 sampled records read back.  It stops at the first variant
         * whose masked app status (reg 0x0030 & 0x0F) is non-zero.
         *
         * It replaces the boot-address matrix below (which is compiled out by
         * B220_DSP_ADDR_MATRIX 0 in config.h while this runs: two full-attempt
         * matrices in one boot would interleave two sets of app-status lines).
         * When the matrix reports a running application, err is left ESP_OK and
         * the config-record / volume path below runs against it; when it does
         * not, err is set to ESP_FAIL so the early return below skips the mailbox
         * probe and the read-back verify (the matrix has already issued the
         * release writes and read the image back five times).
         */
        if (err == ESP_OK && !zl38063_dsp_is_running()) {
            const bool matrix_running = (zl38063_dsp_release_matrix() == ESP_OK);
            ESP_LOGI(TAG, "ZL38063: release matrix -> app running: %s",
                     zl38063_dsp_release_matrix_app_running() ? "yes" : "no");
            if (!matrix_running) {
                err = ESP_FAIL;
            }
        }
#endif

#if B220_DSP_ADDR_MATRIX && !B220_DSP_ADF_RELEASE_TX
        // Phase 3-7: the image transfer now completes (reg 0x34 gate passed, all
        // 3699 records sent, reg 0x12C <- execAddr, reg 0x32 <- 0x000D), but the
        // DSP still reports "not running" (reg 0x030 & 0x0F == 0).  Our two RE
        // passes disagree about which of 0x00080200 / 0x00083040 is the RAM
        // entry point, so try both pairings once, in order, with a full reset +
        // host boot each: today's values (0x00080200 / 0x00083040) first, then
        // the swap.  ~4-5 s of boot per attempt; it refuses to run at all when
        // an application is already running, so a working boot is untouched and
        // the normal case costs nothing.  When a pairing does come up running,
        // err is cleared so the config-record path below still runs against it.
        //
        // Phase 3-12: not used together with B220_DSP_RELEASE_MATRIX above (see
        // that block); config.h keeps them mutually exclusive.
        if (zl38063_dsp_boot_addr_matrix() == ESP_OK) {
            err = ESP_OK;
        }
#endif
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ZL38063: HBI bring-up failed (%s)", esp_err_to_name(err));
            return;
        }
        if (!zl38063_dsp_is_running()) {
            /* Phase 3-12: skipped when the release matrix is on - that matrix
             * already issued the release writes itself (including, in variant 4,
             * exactly the pair the probe below sends), so re-running it here would
             * put a second 0x000D/0x0008 into the same boot and confuse the log
             * this experiment is read from.  The probe is destructive by design
             * (it drops the device into boot ROM); the default path is unchanged.
             *
             * Phase 3-15: skipped on the ADF-order path for the same reason and a
             * stronger one - zl38063_dsp_mailbox_probe() writes reg 0x014 <- 1,
             * which forces the device back into its boot ROM and would destroy
             * exactly the state the ADF release just created (and the loopback is
             * supposed to measure).  It is the one destructive call this path must
             * never make, so the guard is on the switch, not on the diagnostics. */
#if !B220_DSP_RELEASE_MATRIX && !B220_DSP_ADF_RELEASE_TX
            zl38063_dsp_mailbox_probe();
#endif
        }
#if !B220_DSP_RELEASE_MATRIX && !B220_DSP_ADF_RELEASE_TX
        if (!zl38063_dsp_is_running()) {
            /* Phase 3-10 : calibrate the page-255 window addressing BEFORE the
             * read-back verify below, because that verify reads the uploaded image
             * through that window and the read side does not exist in Microsemi's
             * driver at all - it is our inference.  The calibration writes a known
             * 16-word pattern with the upload's own write path and reads it back
             * under every plausible encoding, logging one line per combination and
             * a verdict the verify then uses.  ~8 small HBI round trips, one-shot,
             * and it refuses to run on a live application.  Enabled by
             * B220_DSP_PAGE255_CALIB in config.h.
             *
             * Phase 3-12: with the release matrix on this is a no-op anyway - the
             * matrix already ran this one-shot before its variants (and set the
             * same flag, via B220_ReleaseMatrixCalibrate()) - so the flag test is
             * all that is needed and the encoding it measured is reused. */
#if B220_DSP_PAGE255_CALIB
            if (!s_dsp_page255_calibrated) {
                s_dsp_page255_calibrated = true;
                zl38063_dsp_page255_calibrate();
            }
#endif
            /* Phase 3-⑧ : before giving up, read the uploaded image back out of
             * DSP RAM and compare it with the image we sent - the open
             * hypothesis is that the transfer silently wrote the wrong bytes.
             * Runs ONCE per boot: it is read-only on the device (only the
             * page-255 window base the upload also writes) but costs ~0.4-0.7 s,
             * and the answer cannot change between the retries above.  Enabled
             * by B220_DSP_VERIFY_UPLOAD in config.h. */
#if B220_DSP_VERIFY_UPLOAD
            if (!s_dsp_upload_verified) {
                s_dsp_upload_verified = true;
                zl38063_dsp_verify_uploaded_image();
            }
#endif
            ESP_LOGW(TAG, "ZL38063: no application running - audio path will stay silent");
            return;
        }
#endif  /* !B220_DSP_RELEASE_MATRIX && !B220_DSP_ADF_RELEASE_TX: the
         * calibration+verify diagnostics above */
#if B220_DSP_ADF_RELEASE_TX
        /* Phase 3-15 : nothing follows the ADF release on this path - and that is
         * the point.  The 260-register config record is step 3 of the release
         * inside zl38063_dsp_release_adf_tx() and the soft reset after the GO is
         * the step that "makes the configuration take effect" (tw_spi_access.c:
         * 111 -> :130), so re-loading the same config here and then writing a
         * volume gain would be two MORE host commands emitted after the release
         * this experiment measures, into a device that has just been reset to
         * make the previous config live.  The clean path therefore goes straight
         * to the audio loopback; the read-only status line taken 500 ms after the
         * release (inside that function) is the last device access before it. */
        ESP_LOGI(TAG, "ZL38063: ADF release path: skipping the post-release config "
                      "record + volume writes (the config record is step 3 of the "
                      "release and the soft reset already made it take effect); the "
                      "audio loopback starts next");
#else
        err = zl38063_dsp_load_config();
        ESP_LOGI(TAG, "ZL38063: config record load -> %s", esp_err_to_name(err));
        err = zl38063_dsp_set_volume(60);
        ESP_LOGI(TAG, "ZL38063: volume set -> %s", esp_err_to_name(err));
#endif
#endif  // B220_DSP_HANDS_OFF
    }
#endif

    void InitializeButtons() {
        /*
         * The board DOES have three keys (record / volume+ / volume-), but none
         * of them is wired to an ESP32 GPIO, so nothing can be registered here:
         * the stock firmware reads them from two ZL38063 HBI registers over the
         * same SPI link this port uses for the DSP (reg 0x0002 = status,
         * reg 0x0000 = key code), which is confirmed on hardware.
         *
         * The keys are therefore driven by b220_buttons.c, started at the END of
         * the constructor - i.e. after the DSP host-boot - because that poller
         * cannot work until the HBI link exists.  Starting it here would just
         * spend the whole bring-up logging read failures.
         *
         * What is genuinely absent on this board: a power key the ESP32 can read
         * (it is a baseboard soft latch) and GPIO0 as a BOOT button (it is the
         * boot strapping pin and the ZL38063 chip select).  WiFi provisioning
         * therefore happens automatically when no credentials are stored.
         */
    }

public:
    /*
     * Handed to Application for the display-brightness APIs and the MCP tool.
     * Created on first use, and the saved brightness is restored by the constructor
     * below (not here), so that the restore happens after the AW9106B is attached.
     */
    Backlight* GetBacklight() override {
        if (backlight_ == nullptr) {
            backlight_ = new B220_Backlight();
        }
        return backlight_;
    }

    XiaoMoDouB220Board() {
        InitializeBacklight();
        /*
         * Apply the user's saved brightness.  It has to be AFTER
         * InitializeBacklight(): it writes AW9106B reg 0x23/0x24 and needs the device
         * handle created there.
         *
         * ApplySavedBrightnessNow() does the whole job - read NVS, fall back to this
         * board's default and STORE it, then write the hardware once and prime the
         * base class's state - which is why Backlight::RestoreBrightness() is NOT
         * called here.  Calling it was the bug the bench caught: it reads the same
         * key but falls back to its own 75% default, so it silently pulled the panel
         * down from this board's 100% with a visible fade.  The init sequence above
         * still parks the panel at a known-good value first, so a settings read that
         * fails can never leave the screen dark.
         */
        static_cast<B220_Backlight*>(GetBacklight())->ApplySavedBrightnessNow();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
#if B220_AUDIO_BRINGUP
        InitializeAudioDsp();
#endif
        /*
         * Phase 4 (production): the two things the audio codec needs from the
         * boot, both AFTER the DSP bring-up above and BEFORE anything can play.
         *
         * 1. The proof that the ZL38063 application is up.  reg 0x0014 == 0x0200
         *    is the real ready indicator on this product (reg 0x0030, which
         *    zl38063_dsp_is_running() reads, is never updated by its
         *    application).  Read-only and once per boot; the codec logs it and
         *    warns when it is false, and never refuses to run because of it.
         *
         * 2. The ONE AW8737S SHDN enable sequence of this boot.  It used to be
         *    issued by B220_AudioLoopbackStart() below, which the production
         *    build compiles out (B220_AUDIO_LOOPBACK 0) - so without this call
         *    the amplifier would stay shut down and the speaker silent.  The
         *    sequence is latched (s_pa_enable_done), so the codec's later
         *    EnableOutput(true) is a logged no-op with this call's result, and
         *    no second 0 -> 1 edge can ever reach the AW9106B.
         */
        s_b220_dsp_ready = B220_AudioProbeDspReady();
        {
            const esp_err_t pa_err = b220_pa_enable_one_pulse("board init (production codec)");
            if (pa_err != ESP_OK) {
                ESP_LOGW(TAG, "codec: the boot-time PA enable did not complete cleanly "
                              "(%s) - the speaker may stay silent; the DSP/I2S link is "
                              "unaffected", esp_err_to_name(pa_err));
            }
        }
#if B220_AUDIO_LOOPBACK
        /* Phase 3-14: the standalone audio loopback starts HERE - after the DSP
         * bring-up attempt above has run, and regardless of its verdict (the
         * early returns inside InitializeAudioDsp() do not skip this), because
         * the audio path is the test now, not reg 0x0030.  The call is outside
         * the B220_AUDIO_BRINGUP guard on purpose: it only needs the I2S handles,
         * which B220_I2sUpBeforeDspBoot() either left open or did not, and it
         * says so loudly in the log when they are missing.  It creates exactly
         * one task per boot.  B220_AUDIO_LOOPBACK 0 in config.h removes this
         * call, the task and its buffers completely. */
#if B220_I2S_BEFORE_DSP
        B220_AudioLoopbackStart();
#else
        ESP_LOGW(TAG, "loopback: B220_AUDIO_LOOPBACK is 1 but B220_I2S_BEFORE_DSP "
                      "is 0, so no I2S0 handles exist and the loopback cannot run "
                      "(it never creates a second I2S channel)");
#endif
#endif  // B220_AUDIO_LOOPBACK
        /*
         * Phase 5 (B220_AUTO_LISTEN): start the board's own "button".  It is
         * created last in the constructor so the audio codec, the DSP bring-up and
         * the PA enable above are all done before the first arm can happen, and it
         * does not touch anything itself until the application has reached idle -
         * Application::GetInstance() is safe here (the application's event group is
         * created in its constructor, see application.cc:24-25).  With
         * B220_AUTO_LISTEN 0 this call and the whole watcher are compiled out (the
         * stub only logs why nothing will start a conversation).
         */
        B220_AutoListenStart();
#if B220_BUTTON_DISCOVERY
        /* Find out which chip actually samples the toy's three keys.  The task
         * only READS (see b220_button_discovery.h); it is started last so the
         * audio bring-up, the PA enable and the panel are already in their final
         * state and the probe cannot be blamed for changing any of them. */
        {
            const esp_err_t btn_err = b220_button_discovery_start();
            if (btn_err != ESP_OK) {
                ESP_LOGW(TAG, "button discovery: could not start (%s)", esp_err_to_name(btn_err));
            }
        }
#endif
#if !B220_BUTTON_DISCOVERY
        ESP_LOGI(TAG, "board init: starting the ZL38063 key poller");
        /* The three physical keys, polled through the ZL38063 over HBI.  Started
         * last so the DSP host-boot, the PA enable and the panel are already in
         * their final state - the poller only reads two DSP registers, but this
         * order keeps "who touched what" unambiguous in the log. */
        B220_ButtonsStart();
        ESP_LOGI(TAG, "board init: key poller started (task created)");
#endif
        /*
         * Board indicator lights (AW9106B 0x23/0x24) - see b220_led.h.  This runs
         * AFTER the PA enable on purpose: it attaches its own device to the same
         * I2C bus and only ever writes 0x20..0x25, never the SHDN register 0x02,
         * so the amplifier's single rising edge is untouched.
         */
        ESP_LOGI(TAG, "board init: starting the indicator-light module");
        {
            const esp_err_t led_err = b220_led_start();
            if (led_err != ESP_OK) {
                ESP_LOGW(TAG, "indicator lights: b220_led_start() -> %s",
                         esp_err_to_name(led_err));
            }
        }
        ESP_LOGI(TAG, "board init: indicator-light module returned");
        /*
         * No boot blink: the indicator is dark at rest (see b220_led.c), and the
         * panel plus the audio bring-up already show the device is alive.  The
         * RGB light is exercised by the key feedback instead.
         */
        ESP_LOGI(TAG, "board init: RGB indicator left DARK at rest (no boot blink)");
        /*
         * Baseboard link (motors, head/tail lights) on UART0 - the same wire as
         * the console.  Only the "AT+Pon" handshake is sent automatically; motion
         * frames are sent on demand through b220_base_uart.h.
         */
        ESP_LOGI(TAG, "board init: starting the baseboard handshake");
#if B220_BASE_UART_SELFTEST
        /* Bench check: proves UART0 itself works, baseboard or not.  See config.h. */
        (void)b220_base_uart_selftest();
#endif
#if B220_BASE_UART_RX_REPORT
        /* Print whatever the baseboard says, so "no reply" is a fact rather than an
         * assumption. */
        b220_base_uart_start_rx_report();
#endif
        /* Motors and lights: limits, heartbeat and the timeout guard live in there. */
        (void)b220_baseboard_start();
        {
            const esp_err_t base_err = b220_base_uart_start_handshake();
            if (base_err != ESP_OK) {
                ESP_LOGW(TAG, "baseboard: handshake could not start (%s)",
                         esp_err_to_name(base_err));
            }
        }

        /*
         * Board constructor finished.  This line exists because a boot that
         * stalled inside the constructor (the key poller busy-looping on the
         * GPIO38 edge flag) looked exactly like "the DSP is slow to come up" in
         * the log: everything after this point simply never ran.  With this
         * marker, a stall is obvious and its location is between two lines.
         */
        /*
         * The light policy goes last: it samples the device state and drives the RGB
         * indicator (red = no cloud connection, white breathing = listening/speaking,
         * dark = ready and idle).  See b220_led_policy.c.
         */
        {
            const esp_err_t pol_err = b220_led_policy_start();
            if (pol_err != ESP_OK) {
                ESP_LOGW(TAG, "board init: the RGB light policy did not start (%s)",
                         esp_err_to_name(pol_err));
            }
        }

        /*
         * THE BASEBOARD'S ENTRY POINTS.
         *
         * Until these existed, b220_base_motion()/stop()/lights() had no callers at all -
         * the motors and lights could not be commanded by anything.  Exposing them as MCP
         * tools is what lets a spoken request reach them ("往前走一点", "停", "把灯变蓝"),
         * and it is the layer that fits this device: the assistant is the interface.
         *
         * The tools are thin on purpose.  The limits, the heartbeat that keeps a move
         * smooth and the timeout that stops the wheels if anything goes wrong all live in
         * b220_baseboard.c, so they apply no matter who calls it - including a future
         * button mapping - and cannot be forgotten at one call site.
         */
        {
            auto& mcp = McpServer::GetInstance();
            mcp.AddTool(
                "self.base.move",
                "Drive the toy's base. Use it when the user asks the device to go "
                "somewhere, turn, or back up.\n"
                "Args:\n"
                "  `direction`: \"forward\" or \"backward\" to drive; \"left\" or \"right\"\n"
                "    to turn while moving (an arc); \"spin_left\" or \"spin_right\" to turn\n"
                "    ON THE SPOT - use these only when the user says to turn in place,\n"
                "    otherwise prefer left/right.\n"
                "  `power`: the drive speed, 10 to 255. Omit for 255 (full).\n"
                "    raise it when the movement needs to be quicker.\n"
                "  `seconds`: how long to drive, 0.2 to 10.0. Omit for 2.0.\n"
                "The device stops by itself when the time is up, so prefer short moves and "
                "repeat them rather than asking for one long one.",
                PropertyList({Property("direction", kPropertyTypeString),
                              Property("power", kPropertyTypeInteger, 255, 10, 255),
                              Property("seconds", kPropertyTypeString, "2.0")}),
                [](const PropertyList& properties) -> ToolResult {
                    const std::string direction = properties["direction"].value<std::string>();
                    const int power = properties["power"].value<int>();
                    /* seconds arrives as a string so "0.8" is expressible; the Property
                     * class only has integer support. */
                    double seconds = 2.0;
                    const std::string secs = properties["seconds"].value<std::string>();
                    if (!secs.empty()) {
                        seconds = strtod(secs.c_str(), nullptr);
                    }
                    int ms = (int)(seconds * 1000.0);
                    const esp_err_t err = b220_baseboard_move(direction.c_str(), power, ms);
                    if (err != ESP_OK) {
                        return std::unexpected(std::string("move failed: ") +
                                               esp_err_to_name(err));
                    }
                    return std::string("driving ") + direction + " at " + std::to_string(power) +
                           "% for " + std::to_string(ms) + " ms";
                });

            mcp.AddTool(
                "self.base.stop",
                "Stop the toy's base immediately. Use it whenever the user says stop, and "
                "when a move should not continue.",
                PropertyList(),
                [](const PropertyList&) -> ToolResult {
                    const esp_err_t err = b220_baseboard_stop();
                    if (err != ESP_OK) {
                        return std::unexpected(std::string("stop failed: ") +
                                               esp_err_to_name(err));
                    }
                    return std::string("stopped");
                });

            mcp.AddTool(
                "self.base.lights",
                "Set the base's lights. The FRONT lights are RGB and take a colour; the "
                "REAR light is a single red LED and only understands red or off, so it is "
                "controlled separately - the lights stay on until changed.\n"
                "Args:\n"
                "  `color`: \"white\", \"red\", \"yellow\", \"green\", \"blue\", "
                "\"deepblue\", \"purple\" for the front; \"rear_red\" for the rear; "
                "\"front_off\" / \"rear_off\" to switch one group off; \"off\" for both.",
                PropertyList({Property("color", kPropertyTypeString)}),
                [](const PropertyList& properties) -> ToolResult {
                    const std::string color = properties["color"].value<std::string>();
                    const esp_err_t err = b220_baseboard_lights(color.c_str());
                    if (err != ESP_OK) {
                        return std::unexpected("unknown colour: " + color);
                    }
                    return std::string("lights ") + color;
                });

            ESP_LOGI(TAG, "board init: baseboard MCP tools registered (self.base.move / "
                          "self.base.stop / self.base.lights)");
        }

        ESP_LOGI(TAG, "board init complete: display, keys, indicator lights and the baseboard "
                      "link are up (application start follows)");
#if B220_BACKLIGHT_SWEEP
        /* BRING-UP PROBE ONLY.  This pair runs LAST in the constructor, i.e. AFTER
         * the PA enable sequence, and ProbeAw9106() still issues a 0x7F software
         * reset (followed by CTL/GPMD_A/GPIO_CFG_A/GPMD_B) to the AW9106B.  That
         * reset clears the group-A output register 0x02 AND the latched GPMD_A =
         * 0x02: the probe re-asserts 0x12/0x04 through the audited writers (so both
         * show up in the log), but the PA enable itself is NOT repeated - a second
         * SHDN rising edge would be mode 2 at best, mode 4 = no output at worst.
         * The switch is 0 in every working build; if it is ever needed again,
         * re-run the PA enable AFTER this probe (or drop the 0x7F reset), do not
         * just flip the switch. */
        ProbeBacklightPins();
        ProbeAw9106();
#endif
    }

    /*
     * Phase 4 (production audio): the real codec is the ZL38063 AcuEdge DSP on
     * the one full-duplex I2S0 link (bck 5 / ws 25 / dout 26 / din 35, 48 kHz,
     * 16-bit, stereo; reset GPIO19, HBI on SPI2 with CS GPIO0).
     *
     * Zl38063AudioCodec does NOT create the I2S channel - it borrows the pair
     * B220_I2sUpBeforeDspBoot() created before the DSP reset, which is also what
     * keeps BCLK/WS running for the DSP (a second i2s_new_channel() on I2S0
     * would fail).  That pair exists once per boot, so this is a singleton in
     * the same way every other board's codec is.
     *
     * The sample rates come from config.h and must be the rate the port really
     * runs at (B220_I2S_SAMPLE_RATE_HZ, 48 kHz): the audio service resamples
     * 48 kHz <-> 16 kHz only when the codec declares something other than
     * 16 kHz, so declaring 16 kHz for a 48 kHz port would play and capture at
     * three times the pitch.
     */
    virtual AudioCodec* GetAudioCodec() override {
        static Zl38063AudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

/* The audio PA is bit 1 of group A: 0x02 = PA on, 0x00 = mute/off.
 *
 * This is the ZL38063 diagnostic's entry point into reg 0x02 (zl38063_dsp.c step
 * 3 of zl38063_dsp_gpio_forensics(), whose pattern is 0x3D / 0x00 and so never
 * moves bit 1).  Nothing else calls it any more: the loopback's PA enable is
 * b220_pa_enable_one_pulse(), because a bare 0x02 <- 0x02 after the enable
 * sequence would be a SECOND rising edge on the AW8737S SHDN line and would move
 * the amplifier from mode 1 to mode 2.  Any call that does arrive still goes
 * through the audited writer - so it is logged, counted, and, once the PA enable
 * has latched 0x02/0x12/0x04, REFUSED outright by b220_aw9106_frozen_refuses()
 * (a diagnostic that wants to move these registers has to lift that freeze first,
 * as the B220_PA_BIT_FINDER walk does).
 *
 * It is defined unconditionally and stays weak: the ZL38063 side declares it weak
 * so a diagnostic-free build degrades to "step skipped", and defining it here in
 * every configuration keeps every switch combination linking. */
extern "C" esp_err_t b220_aw9106_set_gpio_output_a(uint8_t value);
extern "C" __attribute__((weak)) esp_err_t b220_aw9106_set_gpio_output_a(uint8_t value) {
    i2c_master_dev_handle_t dev = s_board_aw9106_dev;
    if (dev == nullptr) {
        // No device at 0x5B (InitializeBacklight() failed) - nothing was written.
        return ESP_ERR_INVALID_STATE;
    }
    return b220_aw9106_write_reg02(dev, value, "b220_aw9106_set_gpio_output_a()");
}

/* b220_led.c borrows the board's I2C0 bus to attach its own READ/WRITE device
 * for the AW9106B (indicator channels 0x23/0x24).  It writes nothing else on that
 * chip - in particular never register 0x02, which the AW8737S SHDN line owns.
 * Compiled unconditionally: the indicator lights exist in every build. */
extern "C" i2c_master_bus_handle_t b220_led_i2c_bus(void);
extern "C" i2c_master_bus_handle_t b220_led_i2c_bus(void) {
    return s_board_i2c_bus;
}

#if B220_BUTTON_DISCOVERY
/* b220_button_discovery.c borrows the board's I2C0 bus to attach its own
 * READ-ONLY devices (the AW9106B at 0x5B for its GPIO input registers, and the
 * SC7A20 at 0x19 for its CLICK/INT1 registers).  It must not own that bus:
 * InitializeBacklight() created it and the AW8737S SHDN write path lives on it.
 *
 * All this hands out is the handle, from the file-scope mirror that
 * InitializeBacklight() fills in as soon as i2c_new_master_bus() succeeds - so
 * the hand-out is valid from the moment the bus exists and it stays NULL (and
 * the probe skips the I2C sources) when the backlight bring-up failed.  It is
 * compiled out together with the rest of the probe when B220_BUTTON_DISCOVERY
 * is 0, which is the production value. */
extern "C" i2c_master_bus_handle_t b220_button_discovery_i2c_bus(void);
extern "C" i2c_master_bus_handle_t b220_button_discovery_i2c_bus(void) {
    return s_board_i2c_bus;
}
#endif

DECLARE_BOARD(XiaoMoDouB220Board);
