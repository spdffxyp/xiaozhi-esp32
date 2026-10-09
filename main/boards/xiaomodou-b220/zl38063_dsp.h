/****************************************************************************
* zl38063_dsp.h - board-facing bring-up API for the ZL38063 AcuEdge DSP
*
* The ZL38063 on this board does NOT self-boot.  The factory firmware host-boots
* it over the HBI on every power-up, with the image it carries in the ESP32
* application image; the DSP's own slave flash is never relied upon.  The host
* therefore has to
*
*   1. pulse the hardware reset (GPIO19 low 10 ms, high, 500 ms pause),
*   2. get on the HBI (SPI2, CS GPIO0) and configure the HBI port,
*   3. host-boot the recovered image
*      (zl38063_dsp_upload_firmware(), done automatically by
*       zl38063_dsp_begin() when no application is running), and
*   4. push the board's configuration record (*.cr2) into the DSP RAM.
*      (use zl38063_config_record.h + zl38063_dsp_load_config())
*
* The image itself lives in zl38063/zl38063_firmware_image.h and is linked in by
* zl38063_fwimg_data.c, which overrides the weak empty symbols in
* zl38063_fwimg.c.  See zl38063/zl38063_firmware_image.md for the evidence.
*
* Ported/derived from esp-adf release/v2.x
* components/audio_hal/driver/zl38063/ (zl38063.c and api_lib/); the pieces
* that carry the Microsemi MIT licence keep their headers, see
* ZL38063-LICENSE.txt next to this file.
***************************************************************************/

#ifndef ZL38063_DSP_H
#define ZL38063_DSP_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* App-status values returned by VprocTwolfGetAppStatus() (register 0x030,
 * masked with 0x0F upstream). */
#define ZL38063_APP_STATUS_NOT_RUNNING 0x0
#define ZL38063_APP_STATUS_RUNNING     0x1
#define ZL38063_APP_STATUS_MUTE        0x2

/* The boot ROM identifies itself with this magic in register 0x034 once it is
 * waiting for a host (HBI) firmware image. */
#define ZL38063_BOOTROM_MAGIC 0xD3D3

/**
 * @brief Bring up the ZL38063: pulse reset, attach to SPI2, init HBI, host-boot.
 *
 * Sequence (reset timings and register order are the ones the stock firmware
 * uses - see zl38063/zl38063_firmware_image.md):
 *   1. GPIO19 as output, driven low for 10 ms, then released high;
 *   2. wait 500 ms before the first HBI access;
 *   3. zl38063_hbi_init()   - attach a device to the EXISTING SPI2 bus;
 *   4. VprocTwolfHbiInit()  - write the HBI configuration register;
 *   5. read and log the application status (register 0x030);
 *   6. if no application is running, host-boot the linked-in image via
 *      zl38063_dsp_upload_firmware(); if one IS running, log that and skip the
 *      upload (an upload would reset a working DSP);
 *   7. read and log the status again.
 *
 * It does NOT load the configuration record - call zl38063_dsp_load_config()
 * after this, once the application is running.
 *
 * Idempotent: a second call re-runs the reset pulse, and only re-uploads if the
 * device reports no running application.
 *
 * @note The caller must have initialised SPI2 before calling this (the LCD
 *       code does it).
 *
 * @return
 *   - ESP_OK   on success (including "an application was already running" and
 *              "the device did not answer but the HBI attach itself worked",
 *              which only produce warnings)
 *   - ESP_FAIL if the device cannot be attached to the bus, the HBI cannot be
 *              configured, or the host-boot upload fails
 *   - other esp_err_t values from the SPI driver
 */
esp_err_t zl38063_dsp_begin(void);

/**
 * @brief Host-boot the ZL38063 from the firmware image linked into this build.
 *
 * This is the stock power-up flow, not a self-boot: the DSP is reset into its
 * boot ROM and the image is pushed over HBI.  Register order, all inside the
 * ported VprocTwolf* API:
 *
 *   reg 0x14 <- 1  (force boot ROM mode)      VprocTwolfHbiBootPrepare()
 *   300 ms                                    (called by ...HbiBoot_alt)
 *   reg 0x34 must read 0xD3D3
 *   per record: reg 0x0C <- target (2 words)  HbiSrecBoot_alt()
 *               reg 0xFF <- data (numWords)   (called by ...HbiBoot_alt)
 *   reg 0x12C <- execAddr, default 0x00083040 (2 words)
 *                 (zl38063_dsp_set_boot_addresses() can select the other
 *                  recovered pairing at run time)
 *   reg 0x32 <- 0x000D                        VprocTwolfcmdRegWr()
 *   poll reg 0x06 / verify result             VprocTwolfCheckCmdResult()
 *   reg 0x32 <- 0x0008 ("GO")                 VprocTwolfFirmwareStart()
 *   1000 ms, then read reg 0x030
 *
 * There is no separate prepare call here on purpose: VprocTwolfHbiBoot_alt()
 * already performs VprocTwolfHbiBootPrepare() internally, exactly like
 * Microsemi's single-shot loader.
 *
 * The image symbols come from zl38063_fwimg_data.c /
 * zl38063/zl38063_firmware_image.h (3699 records).  This function does not load
 * the configuration record; call zl38063_dsp_load_config() afterwards.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before zl38063_dsp_begin(),
 *         ESP_ERR_NOT_FOUND if no image is linked in (firmwareStreamLen == 0),
 *         ESP_FAIL if the boot ROM handshake or an HBI access fails.
 */
esp_err_t zl38063_dsp_upload_firmware(void);

/**
 * @brief Detach the ZL38063 from the bus (does not free the shared bus).
 */
esp_err_t zl38063_dsp_end(void);

/**
 * @brief Probe whether the DSP is sitting in its boot ROM.
 *
 * Replays the stock firmware's boot-mode handshake exactly:
 *   VprocTwolfHbiWrite(0x014, 1, {1})   -> stop the running firmware and reset
 *                                          the device into boot ROM mode
 *   300 ms wait
 *   VprocTwolfHbiRead(0x034, 1, &v)     -> boot ROM answers 0xD3D3 when it is
 *                                          waiting to be host-loaded
 *
 * The raw value read is logged.  0xD3D3 means "host-boot me" - call
 * zl38063_dsp_upload_firmware() to do that; 0x0000 (or a read failure) means
 * the device is either not running, not listening, or already has an
 * application.
 *
 * This probe is destructive (it resets the DSP into its boot ROM), so it
 * returns ESP_OK immediately, without touching the device, when
 * zl38063_dsp_is_running() is true.  Nothing else is touched.
 *
 * @return ESP_OK if the read succeeded (whatever the value) or was skipped,
 *         ESP_ERR_INVALID_STATE before zl38063_dsp_begin(), ESP_FAIL if an HBI
 *         access failed.
 */
esp_err_t zl38063_dsp_mailbox_probe(void);

/**
 * @brief Push the board configuration record into the DSP RAM.
 *
 * Writes all ZL38063_CONFIG_RECORD_COUNT (260) {register, value} pairs from
 * zl38063_config_record.h one word at a time, exactly as upstream
 * VprocTwolfLoadConfig() does.  There is NO gate and no early exit: a
 * protocol-level RE of the stock loader proved its loop is unconditional, so
 * every one of the 260 registers is written.  Only valid after the firmware is
 * running, i.e. after zl38063_dsp_begin() has host-booted it.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if begin() was not called,
 *         ESP_FAIL if a register write failed.
 */
esp_err_t zl38063_dsp_load_config(void);

/**
 * @brief Set the DSP output volume.
 *
 * @param vol 0..100, where 0 mutes.  The DSP register value is -90 dB + vol,
 *            i.e. the raw gain byte written to registers 0x238/0x23A.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before begin(),
 *         ESP_ERR_INVALID_ARG if vol > 100, ESP_FAIL on a write error.
 */
esp_err_t zl38063_dsp_set_volume(uint8_t vol);

/**
 * @brief Read the application status from the device.
 *
 * @return true if the DSP firmware is running (status 0x1 or 0x2).
 */
bool zl38063_dsp_is_running(void);

/**
 * @brief Select the prgmBase / execAddr pair the next host boot uses.
 *
 * The recovered stock image declares prgmBase 0x00080200 and execAddr
 * 0x00083040 (zl38063_fwimg_data.c), and those are exactly the values used as
 * long as this setter has never been called - i.e. today's behaviour.  The two
 * reverse-engineering passes on that image disagree about which of the two
 * values is the RAM entry point written to register 0x12C
 * (zl38063/PROTOCOL-REPORT.md vs zl38063_firmware_image.md), so this hook makes
 * the pairing selectable at run time: no rebuild is needed to try the swap.
 *
 * @param prgm_base value for twFirmware.prgmBase.  Purely informational in this
 *                  port: HbiSrecBoot_alt() re-writes the page-255 base register
 *                  from every record's own targetAddr, so this only reaches the
 *                  log.
 * @param exec_addr value for twFirmware.execAddr, i.e. the 32-bit value
 *                  HbiSrecBoot_alt() writes to register 0x12C and the address
 *                  the boot ROM jumps to after reg 0x32 <- 0x0008.
 *
 * Sticky until called again (there is deliberately no "back to the image
 * default" call); every call is logged under tag "ZL38063".  It does not touch
 * the device - the value only takes effect in the next
 * zl38063_dsp_upload_firmware() call.
 */
void zl38063_dsp_set_boot_addresses(uint32_t prgm_base, uint32_t exec_addr);

/**
 * @brief Pre-GO hook: called by the upload path immediately AFTER the last image
 *        record has been written and BEFORE the host commands that release the
 *        boot ROM (B220_DSP_VERIFY_BEFORE_GO).
 *
 * Why it exists: the page-255 window write path is proven correct (a known
 * 16-word pattern written through it reads back 16/16 with enc=b), yet the
 * upload's own image words are NOT in DSP RAM afterwards (target 0x00080200
 * reads 0x0000 although the record sent 0x1C0F 0x0010 0x0041 ...), and reg 0x0034
 * goes 0xD3D3 -> 0x0000 while reg 0x0030 never reports a running app.  That
 * leaves two explanations - "the image IS in RAM and started, but the DSP
 * crashes/resets afterwards and clears it" vs. "the transfer never writes the
 * image records at all" - and a post-GO-only log cannot tell them apart.
 *
 * The hook is the observation point that can: it runs at the last instant at
 * which the image is supposed to be in RAM and NO host command has yet been sent
 * (register 0x12C and register 0x32 <- 0x000D come after it), so a matching
 * read-back there proves the transfer worked.
 *
 * Contract for @p fn (see B220_PreGoVerify() in xiaomodou_b220.cc for the one
 * implementation):
 *   - it runs INSIDE the host-boot transfer, on the caller's task, with the
 *     device in boot ROM mode, so it must use the HBI layer only (no reset, no
 *     other register writes than the page-255 window base 0x0C that the upload
 *     itself writes for every record, no delay longer than a few ms);
 *   - it must not return anything: the upload path cannot fail on it.  A hook
 *     that gives up early only loses log lines;
 *   - it is called from VprocTwolfHbiBoot_alt() (HbiSrecBoot_alt(), after the
 *     record loop) and from VprocTwolfHbiBootConclude() (the s-record streaming
 *     loader), i.e. once per upload, before either of the two commands that can
 *     start the image.
 *
 * The hook is stored in a plain function pointer and is never called from
 * anywhere else; passing NULL clears it.  This setter exists unconditionally, so
 * a build with B220_DSP_VERIFY_BEFORE_GO == 0 links and accepts a registration -
 * it just never calls the hook (both call sites are preprocessor-guarded), i.e.
 * the default behaviour is unchanged.
 *
 * @param fn function to call at the pre-GO point, or NULL to clear it.
 */
void zl38063_dsp_set_pre_go_hook(void (*fn)(void));

#if B220_DSP_RELEASE_MATRIX
/**
 * @brief Hook-release matrix: which host command actually releases the boot ROM
 *        (B220_DSP_RELEASE_MATRIX).
 *
 * Compiled in only when B220_DSP_RELEASE_MATRIX is 1 (config.h).  Call it from
 * the "the DSP still reports no application" branch, in place of the normal
 * post-upload sequence and in place of zl38063_dsp_boot_addr_matrix().
 *
 * State of the bring-up when this was added (hardware-proven, do not
 * re-investigate):
 *   - the UPLOAD is correct: the pre-GO hook verifies 9 records spanning the
 *     whole image BEFORE any release command is sent and finds
 *     mismatching_words=0 with the read-back identical to what was sent, while
 *     reg 0x0034 still reads 0xD3D3 (boot ROM still waiting);
 *   - after the release sequence (reg 0x12C <- 0x00083040, reg 0x32 <- 0x000D,
 *     then VprocTwolfFirmwareStart() -> reg 0x32 <- 0x0008) the DSP leaves the
 *     boot ROM (0x0034 -> 0x0000) but reg 0x0030 never reports a running
 *     application and a later read of the image area returns zeros: the DSP
 *     appears to reset/crash at or after the release.
 *   - strong suspicion (early RE): the vendor's VprocTwolfFirmwareStart
 *     (0x40126210) is a delay stub (ets_delay_us), i.e. stock may never write
 *     0x0008 and the GO may be implied by 0x000D alone - while this port always
 *     writes both.
 *
 * For every variant it runs a FULL fresh attempt (GPIO19 reset pulse -> HBI
 * attach -> HBI config -> the complete 3699-record host-boot upload), then issues
 * exactly that variant's release writes, waits ~1 s and logs reg 0x0030 / 0x0032
 * / 0x0034 / 0x0006 plus the read-back of the SAME 9 sampled image records
 * (records 0 and 1, then every 1/9th of the image, then the last one) through the
 * page-255 window with the calibrated encoding - the same read code, the same
 * encoding and therefore the same numbers as the pre-GO hook's, only taken after
 * the release.
 *
 * Variant list, in the order they RUN (the reference-shaped ones first, because
 * 1..4 are already known failures on this hardware; the matrix stops at the first
 * variant whose masked app status is non-zero):
 *   5.  reg 0x12C + 0x000D + the 260-entry config record + 0x0008
 *                                                     (ADF order, config first);
 *   6.  as 5 plus VprocTwolfReset(VPROC_RST_SOFTWARE) = reg 0x0006 <- 0x0002
 *                                                     (the complete ADF order);
 *   7.  reg 0x12C + 0x000D + 0x0008 + the soft reset    (no config load);
 *   8.  as 6 but with the config record AFTER the GO  (isolates that order);
 *   9.  as 6 with the soft reset sent TWICE, 500 ms apart - the remedy
 *       esp-adf's example_apps/tw_hal_verify.c:126-134 names for "the reset did
 *       not take effect because the I2S master was not stable yet";
 *  10.  as 6 with reg 0x32 <- 0x000B (HOST_CMD_HOST_FLASH_INIT) between 0x000D and
 *       the config record - the one command of ADF's VprocTwolfSaveImgToFlash()
 *       (vprocTwolf_access.c:950-956, called at tw_spi_access.c:90) that precedes
 *       the GO in the reference order;
 *   1.  reg 0x12C only;
 *   2.  reg 0x12C + 0x000D                              (no 0x0008 anywhere);
 *   3.  reg 0x12C + 0x0008                              (no 0x000D);
 *   4.  reg 0x12C + 0x000D + 0x0008                     (today's behaviour).
 *
 * Suppression mechanics: the upload path's own two writes are gated at their
 * call sites by zl38063_vproc_release_cmd_host_load_complete_from_host_boot() and
 * zl38063_vproc_firmware_start_from_host_boot() (zl38063_vproc.c/.h), and the
 * matrix re-emits the ones its variant wants through VprocTwolfHostCmdWr(), which
 * is the loader's own framing.  In variants 1 and 2 no 0x0008 is ever written,
 * in variant 3 no 0x000D is ever written, and nothing else in the transfer moves.
 * The config record goes through VprocTwolfLoadConfig() and the soft reset through
 * VprocTwolfReset(VPROC_RST_SOFTWARE) - the same ported entry points the
 * production path uses, never a raw register write.
 *
 * It stops at the first variant whose masked application status (reg 0x0030 &
 * 0x0F) is non-zero and leaves the device in that state, so the caller can go on
 * into zl38063_dsp_load_config(); zl38063_dsp_release_matrix_app_running() then
 * reports whether that happened, which is what the caller tests before running
 * the diagnostics and the read-back verify.
 *
 * Cost: one reset + full 3699-record upload per variant (~4-5 s each) plus ~1 s
 * settle and the 9-record read-back; ~50 s if all ten run, less because the
 * reference-shaped six run first and it stops at the first success.  It refuses to
 * run while an application is already running.  With the switch at 0 none of this
 * is compiled in and the default path is untouched.
 *
 * @return ESP_OK  an application was already running, or one of the variants
 *                 produced a non-zero masked app status,
 *         ESP_FAIL  every variant left reg 0x0030 reporting "not running",
 *         ESP_ERR_INVALID_STATE before zl38063_dsp_begin().
 */
esp_err_t zl38063_dsp_release_matrix(void);

/**
 * @brief Did zl38063_dsp_release_matrix() leave a running application behind?
 *
 * Reported by the matrix itself (it polls reg 0x0030 after each variant); the
 * caller uses it to decide whether to continue into the config record / volume
 * path instead of the diagnostics.  False before the matrix has run.
 */
bool zl38063_dsp_release_matrix_app_running(void);
#endif /* B220_DSP_RELEASE_MATRIX */

#if B220_DSP_ADF_RELEASE_TX
/**
 * @brief The single clean path: ONE reset + ONE full upload, then the esp-adf
 *        release order, then (500 ms later, read-only) the status registers
 *        (B220_DSP_ADF_RELEASE_TX).
 *
 * The reference order for this chip (esp-adf example_apps/tw_spi_access.c:43-134,
 * mirrored under zl38063/adf_ref/) is
 *     0x000D  ->  config record  ->  0x0008 (GO)  ->  VprocTwolfReset(SOFTWARE)
 * with reg 0x12C written by the record transfer itself.  This call emits exactly
 * that - the same five entry points, in the same order, as variant 6 of
 * zl38063_dsp_release_matrix() - and nothing else:
 *   - one zl38063_dsp_begin() (GPIO19 reset pulse, HBI attach, 0xFD80, the
 *     complete 3699-record transfer, reg 0x12C) with the upload path's own
 *     reg 0x32 <- 0x000D and reg 0x32 <- 0x0008 suppressed at their call sites,
 *     so the four release writes below are the complete set;
 *   - reg 0x32 <- 0x000D (VprocTwolfHostCmdWr), the 260-register config record
 *     (VprocTwolfLoadConfig), reg 0x32 <- 0x0008 (VprocTwolfHostCmdWr),
 *     VprocTwolfReset(VPROC_RST_SOFTWARE) = reg 0x0006 <- 0x0002;
 *   - then ~500 ms and ONE read-only line with reg 0x0030 / 0x0032 / 0x0034 /
 *     0x0006, so the caller can start the audio loopback immediately after.
 *
 * Deliberately absent: zl38063_dsp_mailbox_probe() (destructive - reg 0x014 <- 1
 * forces the device back into its boot ROM), the HBI diagnostic / GPIO forensics
 * / HBI sweep, the page-255 calibration, the pre-GO and post-GO read-backs and
 * both matrices.  Those are compiled out by their own switches in config.h, and
 * the board file gates their call sites on !B220_DSP_ADF_RELEASE_TX as well, so
 * this one switch selects the clean path on its own.
 *
 * Compiled in only when B220_DSP_ADF_RELEASE_TX is 1 (config.h); it needs
 * B220_I2S_BEFORE_DSP 1 because the loopback runs on the I2S0 handles that call
 * creates (the board file fails the build otherwise).
 *
 * @return ESP_OK             the upload and all five steps ran (a release step
 *                            that reported a device-side failure is logged, and
 *                            the read-only status line after it is the
 *                            measurement),
 *         ESP_ERR_INVALID_STATE before zl38063_dsp_begin(),
 *         ESP_FAIL / ESP_ERR_NOT_FOUND  the reset + upload itself failed.
 */
esp_err_t zl38063_dsp_release_adf_tx(void);

/**
 * @brief Which release zl38063_dsp_release_adf_tx() last ran, as one log line.
 *
 * Returns a static string, never NULL.  It reports "did NOT run" until the
 * sequence has really run once, so a boot in which it was skipped or whose
 * upload failed cannot claim the ADF order - the audio loopback logs this line
 * to say which release it is measuring.
 */
const char *zl38063_dsp_release_name(void);
#endif /* B220_DSP_ADF_RELEASE_TX */

#if B220_DSP_ADDR_MATRIX
/**
 * @brief One-shot prgmBase/execAddr boot-attempt matrix (B220_DSP_ADDR_MATRIX).
 *
 * Compiled in only when B220_DSP_ADDR_MATRIX is 1 (config.h).  Call it at boot
 * once, after the normal reset + host-boot attempt (the
 * B220_DSP_BOOT_ATTEMPTS loop) and after the B220_ZL_DIAG probes; it refuses to
 * do anything while an application is already running, so a working boot is
 * never disturbed.
 *
 * It re-tries the whole reset + host boot once per candidate pairing, with
 * zl38063_dsp_set_boot_addresses() selecting the pair and
 * zl38063_dsp_begin() doing the reset pulse, the HBI attach and the full 3699
 * record upload:
 *   a. prgmBase 0x00080200 / execAddr 0x00083040 - today's values;
 *   b. prgmBase 0x00083040 / execAddr 0x00080200 - the swap.
 * After each attempt it waits ~1 s, calls zl38063_dsp_status_dump() (when
 * B220_ZL_DIAG is on) and logs reg 0x030 & 0x0F.  It stops at the first pairing
 * that reports a running application and leaves that pairing selected, so the
 * caller can continue into zl38063_dsp_load_config().
 *
 * Cost: one full DSP boot per attempt (~4-5 s each, ~10 s worst case), zero
 * when the application is already running.
 *
 * @return ESP_OK  the DSP reports a running application (either it already did,
 *                 or one of the two pairings brought it up),
 *         ESP_FAIL when neither pairing produced a running application,
 *         ESP_ERR_INVALID_STATE before zl38063_dsp_begin().
 */
esp_err_t zl38063_dsp_boot_addr_matrix(void);
#endif /* B220_DSP_ADDR_MATRIX */

#if B220_ZL_DIAG
/**
 * @brief HBI link diagnostic: byte order vs. dead MISO vs. wrong CS/mode.
 *
 * Compiled in only when B220_ZL_DIAG is 1 (config.h); call it AFTER
 * zl38063_dsp_begin(), which is what attaches the HBI device to SPI2.  It is
 * read-only on the bus (no register writes), so it is safe to call at any point
 * after the attach, including when the host boot already failed.
 *
 * What it does, all logged with ESP_LOGI under tag "ZL38063":
 *   1. prints the current zl38063_hbi_get_swap() state;
 *   2. reads {0x0006, 0x0020, 0x0022, 0x0024, 0x0030, 0x0032, 0x0034, 0x012C}
 *      one word at a time WITH the swap enabled and logs each raw value;
 *   3. swaps to the raw wire order (zl38063_hbi_set_swap(false)) and reads the
 *      same eight registers again, in a clearly labelled block;
 *   4. logs the ESP32 input levels of SCLK GPIO32 / MOSI GPIO33 / MISO GPIO34 /
 *      CS GPIO0 / DSP reset GPIO19 (read only - no pin is reconfigured);
 *   5. restores the swap to enabled (the default).
 *
 * A read that returns a vproc error logs the error code and the probe carries
 * on with the next register; it never aborts.
 *
 * How to read the output:
 *   - the same "swapped" values as a correct dump, "raw" values byte-mirrored
 *     (0x00D3 instead of 0xD3D3, 0x0100 instead of 0x0001, ...) -> H1: the
 *     byte order is the bug, fix ZL38063_HBI_BIG_ENDIAN/zl38063_hbi_set_swap();
 *   - both blocks all 0x0000 (or all 0xFFFF) and/or vproc read errors -> H2:
 *     nothing is driving MISO; the DSP is not answering (pins/CS/power/reset);
 *   - plausible values in one block but the wrong chip/register map, or the
 *     reset GPIO reading 0 -> H3: CS wiring, SPI mode or clock.
 */
void zl38063_dsp_hbi_diag(void);

/**
 * @brief GPIO19 ownership forensics + AW9106 group-A probe (B220_ZL_DIAG only).
 *
 * Compiled in only when B220_ZL_DIAG is 1 (config.h); call it AFTER
 * zl38063_dsp_begin() (that is what attaches the HBI device to SPI2), next to
 * zl38063_dsp_hbi_diag().  It answers the question the HBI diag can only hint
 * at: after begin() drove GPIO19 - the DSP reset, active low - high, a plain
 * read still returns 0.  Is that net really held low by something external?
 *
 * WARNING: unlike the HBI diag this is NOT read-only on the GPIO side.  It
 * deliberately re-configures GPIO19, which our own driver owns, and drives it,
 * but it restores the pin (output, driven HIGH) before it returns.
 *
 * What it does, all logged with ESP_LOGI under tag "ZL38063":
 *   1. GPIO19 ownership matrix - each row configures the pin, waits 2 ms and
 *      reads the pad level back: output LOW, output HIGH, input with the
 *      internal pull-up, input with the internal pull-down;
 *   2. VprocTwolfGetAppStatus() again (register 0x030) - "does the DSP answer
 *      now?".  It is safe to call repeatedly: it only reads, it never writes a
 *      device register;
 *   3. AW9106 group A: reg 0x02 <- 0x3D (the five non-PA group-A outputs high,
 *      bit 1 held LOW), 10 ms, app status, then reg 0x02 <- 0x00 (the value
 *      InitializeBacklight() programs) and the app status once more.  The AW9106B
 *      sits on the board's I2C bus, so this is the only device write in the whole
 *      diagnostic and it is undone in the same function.  If the board's helper
 *      b220_aw9106_set_gpio_output_a() is not linked in, this step is skipped
 *      and says so.
 *
 *      Bit 1 is excluded because it is the AW8737S SHDN line (group-A OUT_A1) and
 *      that amplifier counts RISING EDGES on SHDN as MODE SELECTIONS (1 edge =
 *      mode 1, 1.2 W; 4 edges = mode 4, NCN OFF = no output).  Writing 0x3F here
 *      used to add exactly such an edge; 0x3D cannot.
 *
 * How to read the output:
 *   - output HIGH -> 1 and input pull-up -> 1: the ESP32 owns the net; the
 *     earlier read of 0 was a read-path artefact, look elsewhere (CS, SPI mode,
 *     power);
 *   - output HIGH -> 0 and/or input pull-up -> 0: something external holds the
 *     net low (another driver, a short, or GPIO19 is not wired to that net at
 *     all) -> the DSP stays in reset, which explains every all-zero HBI read;
 *   - "app status ... " value that differs between steps 2 and 3 -> the AW9106
 *     group-A outputs are involved in whatever gates the DSP.
 */
void zl38063_dsp_gpio_forensics(void);

/**
 * @brief Brute-force HBI transport sweep: CS x SPI mode x clock (B220_ZL_DIAG).
 *
 * Compiled in only when B220_ZL_DIAG is 1 (config.h); call it AFTER
 * zl38063_dsp_begin(), immediately after zl38063_dsp_gpio_forensics().  It
 * answers the last open question in the bring-up: "is the DSP even being
 * addressed?"  Everything else - HBI framing, byte order, the reset line, the
 * firmware image, the config record - is already proven, so the candidates left
 * are the chip-select pin, the SPI mode and the clock.
 *
 * For every (CS, mode, clock) combination it tears the HBI device off SPI2 and
 * re-adds a fresh one on the SAME bus via zl38063_hbi_rebind() (never
 * spi_bus_free()/spi_bus_initialize() - the LCD owns that bus), then performs a
 * single 1-word HBI read of register 0x0034, the boot-ROM magic register that
 * must answer 0xD3D3.  Because the sweep uses the normal zl38063_hbi_* layer,
 * the HBI command framing and the 16-bit word byte-order swap are bit-identical
 * to the production path.
 *
 * Register reads are the ONLY bus activity: the sweep never writes a device
 * register, so it cannot leave the DSP in a strange state, and it runs whether
 * or not the host boot succeeded.  When 0x0034 returns something other than
 * 0x0000/0xFFFF the value is treated as "the DSP answered" and register 0x0030
 * (application status) is read as well.
 *
 * Sweep space (from the board's pin map in config.h):
 *   - CS  : 0, 2, 4, 12, 13, 14, 15, 19, 20, 24, 27, 28, 29, 30, 31 - i.e. every
 *           plausibly free GPIO, plus GPIO27 (the LCD's own CS) as a control;
 *           pins owned by the LCD/HBI bus, I2C, I2S, the console and
 *           flash/PSRAM are excluded;
 *   - mode: 0, 3 (and 1, 2 when the cheap pass finds nothing);
 *   - clock: 10 MHz and 1 MHz;
 *   - byte order: NOT swept - the swap stays ENABLED (verified correct).
 *
 * A pin that fails to attach is logged and skipped, never fatal.  When the
 * sweep finishes, the normal device (CS 0, mode 0, 10 MHz, swap on) is restored
 * so the rest of the boot is unchanged.
 *
 * Output, all ESP_LOGI under tag "ZL38063":
 *   ===== ZL38063 HBI sweep (cs x mode x clock) =====
 *   sweep cs=0   mode0 10MHz -> reg0x34=0x0000 reg0x30=0x0000
 *   sweep cs=0   mode0  1MHz -> reg0x34=0xFFFF
 *   sweep cs=0   mode3 10MHz -> ATTACH FAILED
 *   ...
 *   sweep HIT: cs=27 mode0 10MHz reg0x34=0xD3D3 reg0x30=0x0001
 *   ===== sweep done: N combos, H hits =====
 * plus a summary line listing every CS that produced a non-0x0000/0xFFFF value.
 */
void zl38063_dsp_hbi_sweep(void);

/**
 * @brief Log every boot/status register the bring-up cares about (B220_ZL_DIAG).
 *
 * Compiled in only when B220_ZL_DIAG is 1 (config.h).  Call it after a boot
 * attempt - zl38063_dsp_begin(), zl38063_dsp_upload_firmware() or
 * zl38063_dsp_boot_addr_matrix().  All it does is READ: there is no HBI write
 * anywhere in it, so it is safe to call at any point after
 * zl38063_dsp_begin() attached the device, including when the host boot failed.
 *
 * It logs one ESP_LOGI line per register, all under tag "ZL38063":
 *   status dump reg 0x0030 (app status ...)          = 0x0000
 *   status dump reg 0x0032 (host command / status)   = 0x000D
 *   status dump reg 0x0034 (cmd result ...)          = 0xD3D3
 *   status dump reg 0x0006 (host flags / mailbox)    = 0x....
 *   status dump reg 0x012C (entry point, hi word)    = 0x0008
 *   status dump reg 0x0020 / 0x0022 / 0x0024 (boot ROM id block)
 * then the 0x012C/0x012D pair as one 32-bit entry point (the two candidate
 * addresses 0x00083040 / 0x00080200 differ only in the low word, so the single
 * 0x012C word cannot tell them apart) and a masked app-status summary line.
 *
 * A read that fails logs "<vproc error N>" in place of the value and the dump
 * carries on with the next register; it never aborts.
 */
void zl38063_dsp_status_dump(void);
#endif /* B220_ZL_DIAG */

#if B220_DSP_PAGE255_CALIB
/**
 * @brief Shared read-back of sampled image records through the page-255 window.
 *
 * One implementation of "are the records we sent actually in DSP RAM?", used by
 * BOTH read-back passes so their numbers are comparable:
 *   - the pre-GO hook (B220_DSP_VERIFY_BEFORE_GO, implemented in
 *     xiaomodou_b220.cc, registered with zl38063_dsp_set_pre_go_hook()), which
 *     runs inside the upload at the instant the last record has been written and
 *     no host command has been sent yet;
 *   - the hook-release matrix (B220_DSP_RELEASE_MATRIX,
 *     zl38063_dsp_release_matrix()), which reads the same records back after the
 *     variant's release sequence.
 *
 * It re-points the page-255 window base exactly the way HbiSrecBoot_alt() does
 * (reg 0x0C <- {hi16, lo16}, only for records with useTargetAddr set), reads each
 * record's words with VprocTwolfHbiPage255Read() using the encoding the page-255
 * calibration proved (zl38063_dsp_page255_calib_encoding(), enc=b on this
 * hardware), and logs one line per record plus a SUMMARY line, all prefixed with
 * @p tag, under tag "ZL38063":
 *
 *   <tag>: record <i> target=0x<addr> matched <m>/<n> (read XXXX XXXX XXXX XXXX
 *           expected XXXX XXXX XXXX XXXX)
 *   <tag>: SUMMARY records=<total> visited=<m> ok=<k> words=<w> matched=<w>
 *           mismatching_words=<x> read_errors=<e> window_enc=<a|b> calibrated=<0|1>
 *
 * The only device write is the 2-word window base register the upload itself
 * writes per record, so it is safe inside the upload as well as after it.  A
 * record whose data could not be read is counted in read_errors and the pass
 * carries on.
 *
 * @param tag    short label used as the log prefix ("pre-GO", "post-release-v4").
 * @param idx    record indices into st_twFirmware[]; values >= firmwareStreamLen
 *               are clamped to the last record.
 * @param nidx   number of entries in @p idx.
 * @param stats  optional output, filled with the counters behind the SUMMARY line.
 *
 * @return ESP_OK once the pass ran (whatever it found),
 *         ESP_ERR_INVALID_STATE before zl38063_dsp_begin(),
 *         ESP_ERR_NOT_FOUND when no image is linked in (firmwareStreamLen == 0).
 */
struct zl38063_sample_stats {
    unsigned int  records;       /* records actually read                */
    unsigned int  records_ok;    /* records whose every word matched     */
    unsigned int  words;         /* words compared                       */
    unsigned int  matched;       /* words identical to the image         */
    unsigned long mismatching;   /* words that differ                    */
    unsigned int  read_errors;   /* records whose window read failed     */
    bool          image_missing; /* firmwareStreamLen == 0               */
};

esp_err_t zl38063_dsp_sample_records_readback(
    const char *tag,
    const unsigned int *idx,
    unsigned int nidx,
    struct zl38063_sample_stats *stats);

/**
 * @brief The sample set both read-back passes use.
 *
 * Record 0, record 1 (when the image has one), @p n records evenly spaced across
 * the whole image (k/n of it for k = 1..n) and always the last record; duplicates
 * are dropped, so a short or small image yields fewer entries.  The same helper
 * builds the set for the pre-GO hook and for the release matrix, which is what
 * makes "9 records spanning the whole image" mean the same records in both.
 *
 * @param rec_cnt number of records in the linked image (firmwareStreamLen).
 * @param n       number of evenly spaced samples (0 is treated as 1).
 * @param idx     output array.
 * @param max     capacity of @p idx; at most this many entries are written.
 *
 * @return the number of entries written (0 when rec_cnt == 0, idx == NULL or
 *         max == 0).
 */
unsigned int zl38063_dsp_sample_index_set(unsigned int rec_cnt, unsigned int n,
                                          unsigned int *idx, unsigned int max);

/* Capacity a caller's index array needs for zl38063_dsp_sample_index_set():
 * record 0 + record 1 + n samples + the last record. */
#define ZL38063_DSP_SAMPLE_MAX 16u
#endif /* B220_DSP_PAGE255_CALIB */

#if B220_DSP_PAGE255_CALIB || B220_DSP_VERIFY_UPLOAD
/**
 * @brief The page-255 window read encoding the last calibration settled on.
 *
 * Returns ZL38063_PAGE255_OFF_WORD (enc=b, offset field = (targetAddr & 0xFF) / 2,
 * what the upload's own window write emits) until
 * zl38063_dsp_page255_calibrate() has run and chosen otherwise.  This is the
 * encoding zl38063_dsp_verify_uploaded_image() reads with, and the pre-GO observer
 * (B220_DSP_VERIFY_BEFORE_GO) has to read with it too: both compare the same
 * records through the same window, only at different moments, so a difference
 * between their results must not be an artefact of the read framing.
 *
 * Declared for the same reason as the two functions below (this header saw
 * B220_DSP_PAGE255_CALIB / B220_DSP_VERIFY_UPLOAD from config.h).
 */
unsigned int zl38063_dsp_page255_calib_encoding(void);

/**
 * @brief Whether zl38063_dsp_page255_calibrate() has run and produced a verdict.
 *
 * False means the encoding above is still the upload's own wire encoding, i.e. a
 * read-back taken now cannot be attributed to a measured framing.
 */
bool zl38063_dsp_page255_calibrated(void);
#endif /* B220_DSP_PAGE255_CALIB || B220_DSP_VERIFY_UPLOAD */

#if B220_DSP_PAGE255_CALIB
/**
 * @brief Calibrate the page-255 window addressing empirically
 *        (B220_DSP_PAGE255_CALIB).
 *
 * Compiled in only when B220_DSP_PAGE255_CALIB is 1 (config.h).  Call it after a
 * boot attempt whose application is NOT running and BEFORE
 * zl38063_dsp_verify_uploaded_image(): the verify reads the uploaded image back
 * through the page-255 window, and that read side is an inference - Microsemi's
 * driver only ever WRITES the window (TwolfHbiPage255Write(), no read anywhere in
 * zl38063/adf_ref/) - so the verify is worthless until the read framing is known.
 *
 * Why it is needed at all: the first read-back verify logged
 * `record 0 target 0x00080200 -> page-255 window base low byte 0x200 (window
 * offset 0)` - 0x200 is not a low byte - and it put the raw low byte in the
 * command word where zl38063_hbi_write() actually puts low byte / 2.  Both are
 * now computed in exactly one place, zl38063_hbi_page255_window_offset()
 * (zl38063_hbi.c/.h).
 *
 * What it does:
 *   1. writes a distinctive 16-word pattern (0xC0DE ^ (0x1111 * i)) to a scratch
 *      DSP RAM target that no image record writes - 0x00080A24, the first word of
 *      the single documented hole 0x00080A24..0x00082FFF inside the image's own
 *      coverage 0x00080128..0x0009f4e6 (zl38063/zl38063_firmware_image.md §2);
 *   2. does that write with EXACTLY the upload's write path: reg 0x0C <-
 *      {hi16, lo16} (2 words), then VprocTwolfHbiWrite(0xFF00 | lowbyte, 16);
 *   3. reads it back under every plausible window read framing and logs one line
 *      per combination, under tag "ZL38063":
 *        calib enc=<a|b> preselect=<0|1> base=<hi_lo|lo_hi> -> matched m/16
 *          (read w0 w1 w2 w3, expected w0 w1 w2 w3)
 *      where enc=a is offset field = targetAddr & 0xFF, enc=b is
 *      (targetAddr & 0xFF) / 2, preselect=1 means SELECT_PAGE(0xFF) = 0xFEFF is
 *      sent first, and base=hi_lo is the loader's {hi16, lo16} order;
 *   4. when something matched, additionally probes whether the offset field steps
 *      by one WORD (reading target+2 under enc=b must return pattern[1..2]);
 *   5. concludes with the combination the verify should use, or - when none
 *      matched - sweeps all 128 window positions for the pattern and says whether
 *      the write path reached DSP RAM at all.
 *
 * Cost: 8 combinations x ~19 HBI transactions.  It WRITES 16 words of scratch RAM
 * (plus the 2-word window base register the upload already writes), so it refuses
 * to run while an application is running, exactly like the verify.  Idempotent;
 * the window base is left pointing at the scratch target and the verify re-points
 * it per record.  The verdict is remembered for the verify pass.
 *
 * @return ESP_OK once the calibration ran (whatever it found),
 *         ESP_ERR_INVALID_STATE before zl38063_dsp_begin(),
 *         ESP_FAIL if the pattern could not even be written.
 */
esp_err_t zl38063_dsp_page255_calibrate(void);
#endif /* B220_DSP_PAGE255_CALIB */

#if B220_DSP_VERIFY_UPLOAD
/**
 * @brief Read the uploaded firmware image back out of DSP RAM and compare it
 *        with the image we sent (B220_DSP_VERIFY_UPLOAD).
 *
 * Compiled in only when B220_DSP_VERIFY_UPLOAD is 1 (config.h).  Call it after a
 * boot attempt whose application is NOT running: the hypothesis it tests is that
 * the 3699-record host upload silently wrote the wrong bytes (bad record replay,
 * wrong write target, wrong word count), so the DSP executes garbage and never
 * sets reg 0x030 to "running".
 *
 * It walks the very same twFwr array the upload walked (st_twFirmware[] /
 * firmwareStreamLen, the records zl38063_dsp_upload_firmware() feeds to
 * VprocTwolfHbiBoot_alt()) and for every sampled record:
 *   1. re-points the page-255 window base at that record's targetAddr exactly
 *      the way HbiSrecBoot_alt() does - one HBI write of 2 words (hi16, lo16) to
 *      register 0x0C, skipped for records whose useTargetAddr is clear, which is
 *      also how the loader treats them;
 *   2. reads that record's numWords words back through the page-255 window:
 *      HBI_SELECT_PAGE(0xFF) + HBI_PAGED_READ(offset field, numWords-1) + one HBI
 *      word read per word (VprocTwolfHbiPage255Read() in zl38063_vproc.c/.h - the
 *      read mirror of the window write the upload uses).  The offset field is
 *      computed by zl38063_hbi_page255_window_offset() (zl38063_hbi.c/.h), the one
 *      place that mask/scale lives, and its encoding is the one
 *      zl38063_dsp_page255_calibrate() proved reproduces a known pattern - enc=b,
 *      (targetAddr & 0xFF) / 2, which is what hbi_send_cmd() itself puts in the
 *      command word for the upload's own 0xFF00 | lowbyte write.  Before the fix
 *      this probe fed the RAW low byte into the command word (and printed the low
 *      16 bits where a low byte belongs: "base low byte 0x200" for target
 *      0x00080200), so its 13780/14856 mismatching words were an artefact of the
 *      read side, not evidence about the upload;
 *   3. compares word for word and counts the mismatches.
 *
 * Sampling and cost: every 4th record, plus records 0, 1, 2 and the last two
 * (the two ends are exactly what mattered on hardware before, and the log says
 * in plain text that the pass is sampled).  ~920 records x ~16 words: roughly a
 * third of the upload's own ~3 s, i.e. ~0.4-0.7 s of extra boot time when the
 * DSP is not running.
 *
 * Read-only except for the window-base register writes described above - the
 * same writes the upload already performs - so the DSP is left in the state the
 * failed boot attempt produced.  It refuses to run (and says so) while an
 * application IS running, because a live application's own RAM writes would make
 * the comparison meaningless.
 *
 * Logged under tag "ZL38063" (all lines prefixed "upload verify: ", wrapped in
 * "----- ZL38063 host-upload read-back verify" markers):
 *   - records, payload words, record stride and the current app status;
 *   - the sampling rule and the exact page-255 read framing used;
 *   - one line per mismatch, up to 8: `mismatch: <rec> target=0x%08lX word %u:
 *     sent 0x%04X read 0x%04X` (the 8-mismatch cap is the requested format);
 *   - the summary: `SUMMARY records=%u verified=%u words=%u
 *     mismatching_words=%u (mismatching_records=%u, read_errors=%u)
 *     window_enc=<a|b> calibrated=<0|1>`, so the summary itself carries the
 *     addressing the numbers were produced with;
 *   - a hex dump of the first 16 words read back from record 0 (target
 *     0x00080200), so "is there anything sensible in DSP RAM at all?" is
 *     answerable by eye;
 *   - a conclusion.  When nothing matches at all it re-reads records 0 and 1
 *     with the OTHER page-255 offset encoding to separate "the DSP never received
 *     the data" from "this read command is wrong"; with the calibration compiled
 *     in and already run, that answer is interpreted against the calibrated
 *     encoding instead of being a second, competing guess.
 *
 * @return ESP_OK once the comparison ran (whatever it found),
 *         ESP_ERR_INVALID_STATE before zl38063_dsp_begin(),
 *         ESP_ERR_NOT_FOUND when no image is linked in (firmwareStreamLen == 0).
 */
esp_err_t zl38063_dsp_verify_uploaded_image(void);
#endif /* B220_DSP_VERIFY_UPLOAD */

#if B220_DSP_HANDS_OFF
/**
 * @brief Hands-off HBI attach for a self-booting DSP (B220_DSP_HANDS_OFF).
 *
 * Compiled in only when B220_DSP_HANDS_OFF is 1 (config.h).  It is the whole
 * "touch the DSP as little as possible" bring-up: a stock-firmware boot log
 * captured on real hardware shows no host-boot traffic at all, i.e. the ZL38063
 * auto-boots from its own slave SPI flash and the stock firmware only configures
 * it.  Every register the current host-boot path writes (reg 0x14 <- 1, the
 * 3699 upload records, reg 0x12C, reg 0x32) therefore risks destroying a
 * running application.
 *
 * What it does, in this order:
 *   1. zl38063_hbi_init()  - attach one device on the EXISTING SPI2 bus
 *                            (CS GPIO0; GPIO0 is only put in floating mode);
 *   2. mark the HBI ready so the read helpers work;
 *   3. VprocTwolfHbiInit() - VprocHALInit() (a no-op in this port) plus ONE HBI
 *                            write of 0xFD80, the HBI port configuration
 *                            register.  That single write is the only device
 *                            write the stock driver makes.
 *
 * What it deliberately does NOT do (this is the point of the mode):
 *   - no GPIO19 access of any kind: no gpio_config(), no gpio_set_level(),
 *     no gpio_set_pull_mode(), no reset pulse.  The DSP's reset net stays
 *     exactly as the board powers it up;
 *   - VprocTwolfHbiWrite() is never called: no 0x014 (boot ROM mode), no
 *     0x0C/0xFF page-255 window, no 0x12C entry point, no 0x32 command;
 *   - no zl38063_dsp_upload_firmware(), no boot-address matrix, no mailbox
 *     probe, no config record, no volume write, no diagnostics.
 *
 * The caller must have initialised SPI2 (the LCD code does) and is expected to
 * bring I2S up first, exactly like the host-boot path does.
 *
 * @return ESP_OK once the device is attached and the HBI config register has
 *         been written,
 *         the esp_err_t from zl38063_hbi_init() when the attach fails,
 *         ESP_FAIL when the HBI configuration write fails (the device is then
 *         still attached and read-only access is still possible).
 */
esp_err_t zl38063_dsp_hands_off_attach(void);

/**
 * @brief One-line, read-only DSP status poll for the hands-off path.
 *
 * Compiled in only when B220_DSP_HANDS_OFF is 1 (config.h).  It reads exactly
 * the registers the hands-off experiment cares about - 0x0030 (app status),
 * 0x0032, 0x0034, 0x0006 and the boot ROM id block 0x0020 / 0x0022 / 0x0024 -
 * with one HBI read each (7 reads, no writes at all, so it can be called any
 * number of times on a live DSP), and logs them as a single ESP_LOGI line under
 * tag "ZL38063":
 *
 *   hands-off +3s: 0x0030=0x0000(app status: not running)
 *                  0x0032=0x0000(host command) 0x0034=0x0000(cmd result)
 *                  0x0006=0x0000(host flags) 0x0020=0x0101(id word 0)
 *                  0x0022=0x94AF(id word 1) 0x0024=0x1407(id word 2)
 *
 * This is the compact form of zl38063_dsp_status_dump() (which stays as it is,
 * B220_ZL_DIAG only): same register set, one line instead of ten, and usable
 * with B220_ZL_DIAG = 0.
 *
 * A register whose read fails is printed as <err N> and the poll carries on.
 *
 * @param elapsed_s seconds since the hands-off polling started, only used in the
 *                  log line.
 *
 * @return the masked application status (reg 0x0030 & 0x0F: 0 not running,
 *         1 running, 2 running+mute) when that read succeeded, or -1 when it
 *         failed or the HBI is not attached.
 */
int zl38063_dsp_status_line(unsigned elapsed_s);
#endif /* B220_DSP_HANDS_OFF */

/*
 * Downlink bring-up experiment (B220_DSP_DOWNLINK_TRIES in config.h).
 *
 * The ZL38063 application runs and the uplink works (I2S DIN / GPIO35 carries
 * real microphone audio), but the downlink is dead: a full-scale 1 kHz sine on
 * I2S TX (GPIO26) reaches the DSP's I2S_SDIA and the DSP's DAC output shows no
 * waveform.  The stepper below applies ONE candidate configuration change per
 * call while the caller's 1 kHz tone keeps playing, so the DAC output can be
 * watched on a scope.
 *
 * Try 2 and 3 are the ones the record dig motivates: the stock image's
 * configuration record is 1042 {reg,value} pairs (regs 0x0200..0x0A22) and the
 * stock loader writes ALL of them (its loop is `addx4 end = data +
 * 4*numElements` with numElements = the record's length field 0x0412 = 1042;
 * the esp-adf object zl38063_config.o agrees with configStreamLen = 1042), while
 * zl38063_config_record.h carries only the first 260 - so regs 0x0408..0x0A22
 * (782 entries, 157 non-zero) were never written.  See
 * zl38063/zl38063_config_record_full.h and zl38063_dsp.c for the full argument.
 *
 * Every device write is logged as
 *     downlink try N: reg 0x.... <- 0x.... -> ok|FAILED
 * followed by a read-only readback line and a per-try summary.  With
 * B220_DSP_DOWNLINK_TRIES 0 the stepper compiles to a no-op that immediately
 * reports "exhausted", and the 4168-byte record array is not linked in.
 *
 * The declarations exist in every build; only the behaviour differs, so a call
 * site guarded by the switch can never become a link error.
 */

/**
 * @brief Number of candidates the downlink experiment will try (0 when
 *        B220_DSP_DOWNLINK_TRIES is 0).
 */
unsigned zl38063_dsp_downlink_tries_count(void);

/**
 * @brief Rewind the stepper so the table runs again from try 1.
 *
 * Only useful when the caller wants to restart the sequence; the counters it
 * resets are plain file-static state owned by the audio loopback task.
 */
void zl38063_dsp_downlink_tries_reset(void);

/**
 * @brief Apply the next downlink candidate.  Call once per ~2 s audio window.
 *
 * @return the 0-based index of the try that was applied, or -1 when the table is
 *         exhausted (or the switch is 0).
 */
int zl38063_dsp_downlink_tries_step(void);

#ifdef __cplusplus
}
#endif

#endif /* ZL38063_DSP_H */
