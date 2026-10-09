/****************************************************************************
* zl38063_vproc.h - Voice Processor devices high level access module function
*                prototypes, variables
*
* Copyright Microsemi Inc, 2018. All rights reserved.
* Licensed under the MIT License. See LICENSE.txt in the project
* root for license information.
*
* Port of esp-adf release/v2.x components/audio_hal/driver/zl38063/
* (api_lib/vproc_common.h + api_lib/vproc_common.c + api_lib/vproc_data_types.h
* + api_lib/vprocTwolf_access.c/.h) flattened into two board-local files.
*
* Licence: see ZL38063-LICENSE.txt next to this file.
***************************************************************************/

#ifndef ZL38063_VPROC_H
#define ZL38063_VPROC_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * From api_lib/vproc_common.h
 *==========================================================================*/

#define DEBUG_LOGD ESP_LOGD
#define DEBUG_LOGE ESP_LOGE
#define DEBUG_LOGI ESP_LOGI

#define VPROC_TIMEOUT 500
#define TAG_SPI "SPI"

/* create a 16-bit word out of two bytes */
#define MAKE16(a, b) (unsigned short)(((unsigned short)(b) << 8) | \
                                      (unsigned short)(a))
/* create a 32-bit word out of 4 bytes */
#define MAKE32(a, b, c, d) (unsigned long)(((unsigned long)d << 24) | \
                                           ((unsigned long)c << 16) | \
                                           ((unsigned long)b << 8) |  \
                                           (unsigned long)(a))

/* device/access status codes */
typedef enum VprocStatusType {
    VPROC_STATUS_SUCCESS      =      0,
    VPROC_STATUS_FAILURE,
    VPROC_STATUS_INIT_FAILED,
    VPROC_STATUS_WR_FAILED,
    VPROC_STATUS_RD_FAILED,
    VPROC_STATUS_FW_LOAD_FAILED,
    VPROC_STATUS_CFG_LOAD_FAILED,
    VPROC_STATUS_CLOSE_FAILED,
    VPROC_STATUS_FW_SAVE_FAILED,
    VPROC_STATUS_GFG_SAVE_FAILED,
    VPROC_STATUS_MAU_NOT_READY,
    VPROC_STATUS_CHK_FAILED,
    VPROC_STATUS_FUNC_NOT_SUPPORTED,
    VPROC_STATUS_INVALID_ARG,
    VPROC_STATUS_ERR_VTD_CODE,
    VPROC_STATUS_ERR_VERIFY,
    VPROC_STATUS_DEVICE_BUSY,
    VPROC_STATUS_ERR_HBI,
    VPROC_STATUS_ERR_IMAGE,
    VPROC_STATUS_MAILBOX_BUSY,
    VPROC_STATUS_CMDREG_BUSY,
    VPROC_STATUS_IN_CRTCL_SECTN,
    VPROC_STATUS_BOOT_LOADING_MORE_DATA,
    VPROC_STATUS_BOOT_LOADING_CMP,
    VPROC_STATUS_DEV_NOT_INITIALIZED,
} VprocStatusType;

/* Device reset modes */
typedef enum VprocResetMode {
    VPROC_RST_HARDWARE_ROM  =  0,   /* reset + reload firmware from flash */
    VPROC_RST_HARDWARE_RAM  =  1,   /* reset + reload firmware from RAM */
    VPROC_RST_SOFTWARE      =  2,
    VPROC_RST_AEC           =  3,
    VPROC_RST_BOOT          =  4
} VprocResetMode;

/*============================================================================
 * From api_lib/vproc_data_types.h
 *
 * Only the types the ported API actually needs are kept.  These are
 * bit-identical to upstream (uint16 -> unsigned short, uint32 -> uint32_t).
 *==========================================================================*/

typedef signed char     int8;
typedef unsigned char   uint8;
typedef unsigned short  uint16;

/* These typedefs MUST stay bit-compatible with the fixed-width types the
 * ported code is passed (uint8_t/uint16_t/uint32_t); guard that assumption. */
#ifdef __cplusplus
static_assert(sizeof(uint16) == sizeof(uint16_t), "uint16 must be 16-bit");
static_assert(sizeof(uint8) == sizeof(uint8_t), "uint8 must be 8-bit");
#else
_Static_assert(sizeof(uint16) == sizeof(uint16_t), "uint16 must be 16-bit");
_Static_assert(sizeof(uint8) == sizeof(uint8_t), "uint8 must be 8-bit");
#endif

/* config record entry: binary-identical to one row of
 * zl38063_config_record[][2] */
typedef struct {
    uint16 reg;
    uint16 value;
} dataArr;

/* Firmware image blocks, needed by VprocTwolfHbiBoot_alt().  Layout is
 * identical to upstream vproc_data_types.h. */
typedef struct {
    uint16 buf[16];      /* the firmware data block to send to the device */
    uint16 numWords;     /* number of valid words in buf[]; 0 = address-only */
    uint32_t targetAddr; /* base address written to the page 255 base regs */
    uint8 useTargetAddr; /* 1 => targetAddr must be written to the device */
} twFwr;

typedef struct {
    twFwr   *st_Fwr;
    uint32_t byteCount;            /* total firmware bytes - NOT USED */
    uint8    havePrgmBase;
    uint32_t prgmBase;
    uint32_t execAddr;             /* execution start address in RAM */
    uint16   twFirmwareStreamLen;  /* number of blocks within the firmware */
} twFirmware;

/*----------------------------------------------------------------------------
 * Firmware image symbols needed by VprocTwolfHbiBoot_alt().
 *
 * These are DECLARED here and DEFINED WEAK in zl38063_fwimg.c (empty image),
 * so the tree links with or without a licence-gated image.  Supplying a real
 * image (a strong definition, e.g. a generated zl38063/zl38063_firmware.h with
 * ZL38063_HAVE_FIRMWARE_IMAGE) overrides the weak ones without duplicate
 * symbol errors.
 *--------------------------------------------------------------------------*/
extern const twFwr st_twFirmware[];           /* converted s-record blocks  */
extern const uint16_t firmwareStreamLen;      /* number of blocks to walk   */
extern const uint32_t executionAddress;       /* program entry point        */
/* Some board builds (B220_DSP_VERIFY_UPLOAD only reads it for a log line)
 * never actually use this flag; the attribute keeps -Wunused-const-variable
 * quiet there instead of turning an unrelated diagnostic into a build error. */
#if defined(__GNUC__)
extern const uint8_t haveProgramBaseAddress __attribute__((unused)); /* 0/1 */
#else
extern const uint8_t haveProgramBaseAddress;  /* 0/1                        */
#endif
extern const uint32_t programBaseAddress;     /* base address (informational) */

/* Provided by zl38063_fwimg.c; logs why a boot load cannot proceed. */
void Zl38063FwImageReportMissing(void);

#define TWOLF_MAILBOX_SPINWAIT 1000 /* at least 1000 to avoid mailbox busy */

/*============================================================================
 * Platform hooks (implemented in zl38063_vproc.c on top of zl38063_hbi_*)
 *
 * Upstream these live in vproc_common.c and talked to SPI directly.  Here the
 * SPI framing lives in zl38063_hbi.c and these are thin adapters, so the
 * signature of the ported VPROC API can stay untouched.
 *==========================================================================*/
extern int  VprocHALInit(void);                          /* 0 = ok */
extern void VprocHALcleanup(void);
extern int  VprocHALWrite(unsigned short val);           /* 0 = ok */
extern int  VprocHALRead(unsigned short *pVal);          /* 0 = ok */
extern void Vproc_msDelay(unsigned short time);          /* milliseconds */
extern void VprocWait(unsigned long int time);           /* milliseconds */

/*============================================================================
 * Ported public API (api_lib/vprocTwolf_access.h)
 *==========================================================================*/

/* Initialise the HBI bus and configure the device HBI pin config register. */
VprocStatusType VprocTwolfHbiInit(void);

/* Close the communication path to the device. */
VprocStatusType VprocTwolfHbiCleanup(void);

/* Read up to 254 words starting at device register @cmd. */
VprocStatusType VprocTwolfHbiRead(
    unsigned short cmd,       /* the 16-bit register to read from */
    unsigned char numwords,   /* number of 16-bit words to read */
    unsigned short *pData);   /* pointer to the read data buffer */

/* Write up to 126 words starting at device register @cmd. */
VprocStatusType VprocTwolfHbiWrite(
    unsigned short cmd,       /* the 16-bit register to write to */
    unsigned char numwords,   /* number of 16-bit words to write */
    unsigned short *pData);   /* the words to write */

/* Write a config record (one register per HBI write, as upstream does).
 *
 * zl38063_config_record.h (this directory) is layout-identical to dataArr[]. */
VprocStatusType VprocTwolfLoadConfig(dataArr *pCr2Buf, unsigned short numElements);

/* Read @numWords words back out of the page-255 data WINDOW at the window
 * position of @targetAddr (a DSP RAM address as it appears in the image records,
 * e.g. 0x00080200).  Additive helper: the upstream driver has no window-read path
 * at all, only the write side TwolfHbiPage255Write(), so the framing here is that
 * write framing with HBI_PAGED_READ in place of HBI_PAGED_WRITE:
 * HBI_SELECT_PAGE(0xFF), then HBI_PAGED_READ(offset, n-1), then one HBI word read
 * per word.  It never writes a device register, so it cannot disturb whatever the
 * DSP is doing.
 *
 * The offset field is computed by zl38063_hbi_page255_window_offset()
 * (zl38063_hbi.c/.h), the single place that maps a target address onto the window
 * command word.
 *
 * @offset_encoding selects how @targetAddr's low byte is folded into the command
 * word:
 *   ZL38063_PAGE255_OFF_BYTE = (targetAddr & 0xFF), the raw byte offset - the
 *       value the loader hands to VprocTwolfHbiWrite();
 *   ZL38063_PAGE255_OFF_WORD = (targetAddr & 0xFF) / 2, which is what
 *       zl38063_hbi_write()/hbi_send_cmd() actually puts in bits 14..8 for the
 *       upload's own window write (ZL38063_PAGE255_OFF_UPLOAD).
 * The two agree only for targets whose low byte is 0, which is why the old
 * raw-byte assumption produced mismatching read-backs for every other record;
 * the empirical calibration (zl38063_dsp_page255_calibrate()) decides which one
 * a window read really has to use. */
VprocStatusType VprocTwolfHbiPage255Read(
    uint32_t targetAddr,
    unsigned char numWords,
    unsigned short *pDdata,
    unsigned int offset_encoding);

/*--------------------------------------------------------------------------
 * Host (HBI) firmware boot loading.
 *
 * This board's DSP does NOT self-boot: the stock B220 firmware host-boots it on
 * every power-up, and that is what these functions implement.  The recovered
 * stock image (3699 twFwr records, execAddr 0x00083040) is linked in by
 * zl38063_fwimg_data.c / zl38063/zl38063_firmware_image.h; the weak empty
 * placeholders in zl38063_fwimg.c only remain as the fallback when no image is
 * available, in which case the boot path reports VPROC_STATUS_ERR_IMAGE.
 *------------------------------------------------------------------------*/

/* Boot mode entry: stop the running firmware and put the device in its boot
 * ROM so that it can accept an image.  Returns VPROC_STATUS_SUCCESS once the
 * boot ROM answers 0xD3D3 at register 0x034. */
VprocStatusType VprocTwolfHbiBootPrepare(void);

/* Alternative single-shot load of a converted *.s3 image already in host RAM. */
VprocStatusType VprocTwolfHbiBoot_alt(twFirmware *st_firmware);

/* Streaming variant: one ascii s-record line per call.  Returns
 * TWOLF_STATUS_NEED_MORE_DATA (22), TWOLF_STATUS_BOOT_COMPLETE (23) or an
 * error code. */
#define ZL38063_TWOLF_STATUS_NEED_MORE_DATA 22
#define ZL38063_TWOLF_STATUS_BOOT_COMPLETE  23
VprocStatusType VprocTwolfHbiBootMoreData(char *dataBlock);

/* Close the host load: tell the device the transfer is complete and verify. */
VprocStatusType VprocTwolfHbiBootConclude(void);

/* Read the host command param/result register (0x034); 0 means the last
 * command succeeded. */
VprocStatusType VprocTwolfCheckCmdResult(void);

/* Reset the device.  NB: this board has no host-wired reset other than the
 * GPIO19 line driven by zl38063_dsp_begin(); this call is the register-level
 * reset the device understands. */
VprocStatusType VprocTwolfReset(VprocResetMode mode);

VprocStatusType VprocTwolfGetAppStatus(uint16 *status);

VprocStatusType VprocTwolfSetVolume(uint8 vol);
VprocStatusType VprocTwolfGetVolume(int8_t *vol);

/* Start/restart the firmware already in RAM. */
VprocStatusType VprocTwolfFirmwareStart(void);
/* Halt the running firmware (leaves the device in boot mode). */
VprocStatusType VprocTwolfFirmwareStop(void);

/*
 * Pre-GO observation hook (B220_DSP_VERIFY_BEFORE_GO in config.h).
 *
 * Registers @fn to be called at the last instant of a host load at which the
 * image is supposed to be in DSP RAM and NO host command has been sent yet:
 * HbiSrecBoot_alt() calls it right after its record loop and before it writes
 * reg 0x12C (the entry point), and VprocTwolfHbiBootConclude() calls it before
 * its reg 0x32 <- 0x000D (host load complete) write.  zl38063_dsp_upload_firmware()
 * then issues reg 0x32 <- 0x000D and, later, the GO - so a read-back taken here
 * says whether the RECORD TRANSFER put the image in RAM, independently of
 * anything the start sequence does afterwards.
 *
 * The setter exists in every build; the two call sites are compiled in only when
 * B220_DSP_VERIFY_BEFORE_GO == 1, so a hook registered against a build with the
 * switch at 0 is stored but never called.  @fn must be read-only on the device
 * apart from the page-255 window base register the upload itself writes, must
 * not reset or otherwise start the device, and returns nothing: the upload
 * cannot fail on its account.  NULL clears the hook.  Contract in full:
 * zl38063_dsp_set_pre_go_hook() in zl38063_dsp.h.
 */
void zl38063_dsp_set_pre_go_hook(void (*fn)(void));

/*--------------------------------------------------------------------------
 * Host-boot release-write gates (B220_DSP_RELEASE_MATRIX)
 *
 * The ZL38063 boot ROM is released by two host commands written to reg 0x0032:
 *
 *   reg 0x32 <- 0x000D  "host application load complete", which
 *                       VprocTwolfHbiBoot_alt() issues after the record transfer;
 *   reg 0x32 <- 0x0008  "GO", which VprocTwolfFirmwareStart() issues - and which
 *                       the early RE marked as a delay stub in the vendor
 *                       firmware, i.e. the write the stock may never make.
 *
 * Which of the two actually releases the boot ROM is the open hardware question,
 * so the hook-release matrix (zl38063_dsp_release_matrix(), B220_DSP_RELEASE_MATRIX)
 * has to run the upload with each of them emitted or suppressed independently.
 * These three entry points are the whole mechanism, and none of them changes any
 * wire byte unless a caller explicitly sets a suppression:
 *
 *   zl38063_vproc_release_cmd_host_load_complete_from_host_boot(true)
 *       -> the reg 0x32 <- 0x000D write inside VprocTwolfHbiBoot_alt() is
 *          SKIPPED, together with the VprocTwolfCheckCmdResult() that verifies
 *          it (with no command sent, reg 0x034 still holds the boot ROM's
 *          0xD3D3 magic, so that check would - correctly - fail).  Everything
 *          else in the call, including the 3699-record transfer and the reg
 *          0x12C entry-point write, happens exactly as before.
 *   zl38063_vproc_firmware_start_from_host_boot(true)
 *       -> the reg 0x32 <- 0x0008 write in zl38063_dsp_upload_firmware() is
 *          SKIPPED.  VprocTwolfFirmwareStart() itself is unchanged, so any other
 *          caller still gets the write.
 *   VprocTwolfHostCmdWr(cmd)
 *       -> issue one host command through the loader's own framing
 *          (VprocTwolfcmdRegWr(): mailbox acquire, reg 0x32 <- cmd, reg 0x0006
 *          <- 1, poll reg 0x32 back to 0), so the matrix's own 0x000D / 0x0008
 *          writes are byte-identical to the ones it suppresses.
 *
 * The suppression mask is file-static state in zl38063_vproc.c and starts at 0;
 * the two "..._is_suppressed()" queries exist so the caller can log what the
 * upload really emitted.  With the mask untouched, the default host-boot path
 * writes exactly the bytes it always wrote.
 *--------------------------------------------------------------------------*/
#define B220_RELEASE_CMD_HOST_LOAD_CMP   0x0001u  /* reg 0x32 <- 0x000D */
#define B220_RELEASE_CMD_FIRMWARE_START  0x0002u  /* reg 0x32 <- 0x0008 */

void zl38063_vproc_release_cmd_host_load_complete_from_host_boot(bool suppress);
bool zl38063_vproc_release_cmd_host_load_complete_is_suppressed(void);
void zl38063_vproc_firmware_start_from_host_boot(bool suppress);
bool zl38063_vproc_firmware_start_is_suppressed(void);

/* One host command through the loader's own framing (see above). */
VprocStatusType VprocTwolfHostCmdWr(unsigned short cmd);

#ifdef __cplusplus
}
#endif

#endif /* ZL38063_VPROC_H */
