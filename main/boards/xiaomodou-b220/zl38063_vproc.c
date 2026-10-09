/****************************************************************************
* zl38063_vproc.c - Voice Processor devices high level access module function
*                definitions
*
* NOTE: The registers of the device are 16-bit wide. A 32-bit access
*       is not required. However, the 32-bit access functions are provided
*       only if the host wants to access two consecutives 16-bit registers
*       in one single access.
*
****************************************************************************
* Copyright Microsemi Inc, 2018. All rights reserved.
* Licensed under the MIT License. See LICENSE.txt in the project
* root for license information.
*
* Port of esp-adf release/v2.x components/audio_hal/driver/zl38063/
* (api_lib/vproc_common.h + api_lib/vproc_common.c + api_lib/vproc_data_types.h
* + api_lib/vprocTwolf_access.c/.h) flattened into two board-local files.
*
* The VprocHAL* platform hooks now call the SPI transport in zl38063_hbi.c
* instead of driving the SPI peripheral from this file, so VprocTwolfHbi*()
* keeps its upstream behaviour without owning the (LCD-shared) SPI bus.
*
* Licence: see ZL38063-LICENSE.txt next to this file.
***************************************************************************/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(ESP_PLATFORM) || defined(__XTENSA__) || defined(__riscv)
#include "esp_rom_sys.h"
#define VPROC_ROM_DELAY_US(us) esp_rom_delay_us(us)
#else
#define VPROC_ROM_DELAY_US(us) ((void)(us))
#endif

/*
 * config.h (the board's) is needed ONLY by the pre-GO hook below: it defines
 * B220_DSP_VERIFY_BEFORE_GO, which decides whether the hook call sites are
 * compiled in at all.  With the switch at 0 the two "#if" blocks below disappear
 * and this file compiles exactly as it did before the hook existed.
 *
 * ORDER MATTERS: config.h has to be included BEFORE zl38063_hbi.h.  Both define
 * ZL38063_HBI_CS_GPIO - config.h as GPIO_NUM_0, the header as a plain 0 fallback -
 * and a macro redefined with a *different token sequence* is a warning that this
 * build turns into an error (-Werror), even though both expand to 0.  With
 * config.h first, zl38063_hbi.h's "#ifndef" sees the board's definition and skips
 * the fallback: exactly the include order zl38063_dsp.c uses for the same reason.
 */
#include "config.h"

#include "zl38063_vproc.h"
#include "zl38063_hbi.h"

/*------------------------------------------------------*/
/* TWOLF REGisters */
#define HOST_CMD_REG             0x0032  /* Host Command register */
#define HOST_CMD_IDLE            0x0000  /* idle/ operation complete */
#define HOST_CMD_FWR_GO          0x0008  /* start/restart firmware (GO) */
#define HOST_CMD_HOST_LOAD_CMP   0x000D  /* host application load complete */
#define HOST_CMD_FWR_STOP        0x8000  /* stop firmware */

#define HOST_SW_FLAGS_REG        0x0006
#define HOST_SW_FLAGS_CMD        0x0001

#define CLK_STATUS_REG           0x0014  /* clock status register */
#define PAGE_255_BASE_HI_REG     0x000C  /* page 255 window base, high half;
                                          * write 2 words here (HI then LO) */

#define HBI_CONFIG_REG           0xFD00
#define HBI_CONFIG_ENDIANNESS    0x0000
#define HBI_CONFIG_DIVEMODE      0x0000
#define HBI_CONFIG_WAKE          0x0080
#define HBI_CONFIG_VAL (HBI_CONFIG_ENDIANNESS | \
                        HBI_CONFIG_DIVEMODE | HBI_CONFIG_WAKE)

#define HOST_CMD_PARAM_RESULT_REG 0x0034 /* Host Command Param/Result register */

/* Suppression bits for the two release writes the hook-release matrix
 * (B220_DSP_RELEASE_MATRIX) controls at their call sites.  Defined in
 * zl38063_vproc.h as well, because zl38063_dsp.c composes the mask. */
#ifndef B220_RELEASE_CMD_HOST_LOAD_CMP
#define B220_RELEASE_CMD_HOST_LOAD_CMP   0x0001u  /* reg 0x32 <- 0x000D */
#endif
#ifndef B220_RELEASE_CMD_FIRMWARE_START
#define B220_RELEASE_CMD_FIRMWARE_START  0x0002u  /* reg 0x32 <- 0x0008 */
#endif

/* The boot ROM identifies itself with this magic in the param/result reg. */
#define TWOLF_BOOTROM_MAGIC      0xD3D3

#define TWOLF_MAILBOX_SPINWAIT  1000

/* PAGE_255_WRITE: page 255 is the "window" used to push firmware data; the
 * low byte of the target address becomes the window offset.  Both the mask that
 * produces that low byte and the /2 that zl38063_hbi_write() applies to it on the
 * way out live in zl38063_hbi_page255_window_offset() (zl38063_hbi.c/.h). */
#define PAGE_255_WRITE_CMD(page, offset) \
    ((uint16_t)(((uint16_t)(page) << 8) | (uint16_t)(offset)))

/* The ported API uses `unsigned short` for 16-bit device words and this file
 * reinterprets those buffers as uint16_t for zl38063_hbi_*(); guard it. */
#ifdef __cplusplus
static_assert(sizeof(unsigned short) == 2, "VPROC API assumes 16-bit unsigned short");
#else
_Static_assert(sizeof(unsigned short) == 2, "VPROC API assumes 16-bit unsigned short");
#endif

/*============================================================================
 * Platform hooks - api_lib/vproc_common.c
 *==========================================================================*/

int VprocHALInit(void)
{
    /*
     * The SPI bus itself is brought up by the LCD code and the device is
     * attached by zl38063_hbi_init(); there is nothing platform specific left
     * to do here, exactly as the upstream comment says.
     */
    return 0;
}

void VprocHALcleanup(void)
{
    /* Keep the device attached: the LCD keeps using the bus. */
}

void Vproc_msDelay(unsigned short time)
{
    /* Upstream used a busy-wait; yield instead so the LCD/audio tasks run. */
    vTaskDelay(pdMS_TO_TICKS((uint32_t)time));
}

void VprocWait(unsigned long int time)
{
    /* Upstream comment says 125 us resolution but the code used us*1000, i.e.
     * milliseconds.  Kept byte-for-byte compatible (milliseconds). */
    VPROC_ROM_DELAY_US(time * 1000UL);
}

int VprocHALWrite(unsigned short val)
{
    return zl38063_hbi_wr16_cmd((uint16_t)val);
}

int VprocHALRead(unsigned short *pVal)
{
    if (pVal == NULL) {
        return -1;
    }
    return zl38063_hbi_rd16((uint16_t *)pVal);
}

/*============================================================================
 * Static helpers - api_lib/vprocTwolf_access.c
 *==========================================================================*/

/* VprocTwolfMailboxAcquire(): check for the availability of the mailbox.
 * 0 = success, else fail. */
static VprocStatusType VprocTwolfMailboxAcquire(uint16 flag, uint16 timeout)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;
    uint16 i = 0;
    uint16 temp = 0x0BAD;

    for (i = 0; i < timeout; i++) {
        status = VprocTwolfHbiRead(HOST_SW_FLAGS_REG, 1, &temp);
        if (status != VPROC_STATUS_SUCCESS) {
            DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
            return status;
        }
        if (!(temp & flag)) {
            break;
        }
        Vproc_msDelay(10);
    }
    DEBUG_LOGD(TAG_SPI, "timeout count = %u: ", (unsigned)i);
    if ((i >= timeout) && (temp & flag)) {
        return VPROC_STATUS_MAILBOX_BUSY;
    }
    return VPROC_STATUS_SUCCESS;
}

/* VprocTwolfcmdRegAcquire(): wait until the last command completed. */
static VprocStatusType VprocTwolfcmdRegAcquire(uint16 flag, uint16 timeout)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;
    uint16 i = 0;
    uint16 temp = 0x0BAD;

    for (i = 0; i < timeout; i++) {
        status = VprocTwolfHbiRead(HOST_CMD_REG, 1, &temp);
        if (status != VPROC_STATUS_SUCCESS) {
            DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
            return status;
        }
        if (temp == flag) {
            break;
        }
        Vproc_msDelay(10);
    }
    DEBUG_LOGD(TAG_SPI, "timeout count = %u, cmdReg = 0x%04x: ",
               (unsigned)i, (unsigned)temp);
    if ((i >= timeout) && (temp != flag)) {
        return VPROC_STATUS_CMDREG_BUSY;
    }
    return VPROC_STATUS_SUCCESS;
}

/* VprocTwolfcmdRegWr(): access the host command register. */
static VprocStatusType VprocTwolfcmdRegWr(unsigned short cmd)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;
    unsigned short flag = 0x0BAD;

    /* Check whether the host owns the command register. */
    status = VprocTwolfMailboxAcquire(HOST_SW_FLAGS_CMD, TWOLF_MAILBOX_SPINWAIT);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
        return status;
    }
    /* Write the command into the Host Command register. */
    status = VprocTwolfHbiWrite(HOST_CMD_REG, 1, &cmd);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
        return status;
    }
    /* Release the command reg. */
    flag = HOST_SW_FLAGS_CMD;
    status = VprocTwolfHbiWrite(HOST_SW_FLAGS_REG, 1, &flag);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
        return status;
    }
    /* Wait for the command to complete. */
    status = VprocTwolfcmdRegAcquire(HOST_CMD_IDLE, TWOLF_MAILBOX_SPINWAIT);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: CMD_REG - Operation is not complete",
                   (int)status);
        return status;
    }
    return VPROC_STATUS_SUCCESS;
}

/*============================================================================
 * Host-boot release-write gates (B220_DSP_RELEASE_MATRIX / B220_DSP_ADF_RELEASE_TX)
 *
 * The hook-release matrix and the single clean ADF-order release
 * (zl38063_dsp_release_adf_tx()) both have to be able to emit or suppress each of
 * the two writes that can release the boot ROM:
 *
 *   reg 0x32 <- 0x000D   "host application load complete", issued by
 *                        VprocTwolfHbiBoot_alt() through VprocTwolfcmdRegWr();
 *   reg 0x32 <- 0x0008   "GO", issued by VprocTwolfFirmwareStart() - the
 *                        function the early RE marked as a delay stub in the
 *                        vendor firmware, i.e. the write the stock may never
 *                        make.
 *
 * Both are suppressed at their CALL SITE, never inside VprocTwolfcmdRegWr() or
 * VprocTwolfFirmwareStart() themselves: those keep their exact upstream
 * behaviour for every other caller (FirmwareStop, the streaming loader,
 * VprocTwolfHbiBootConclude()).  The only state involved is the file-static
 * below, which is 0 unless zl38063_dsp_release_matrix() or
 * zl38063_dsp_release_adf_tx() (zl38063_dsp.c) sets it through these setters -
 * so the default boot path, every byte on the wire, is unchanged.
 *
 * Nothing else in the release sequence is gated: boot prepare (reg 0x14 <- 1 +
 * the 300 ms wait + the 0xD3D3 gate), the record transfer and the reg 0x12C
 * entry-point write inside HbiSrecBoot_alt() always happen exactly as before.
 *
 * The two setters are file-static helpers + small externals rather than macros
 * so the naming and the bit meanings live in one place; they are declared in
 * zl38063_vproc.h.
 *==========================================================================*/

/* Values of the suppression mask.  Named in the header too (that is the
 * contract the release matrix compiles against); repeated here only as the
 * implementation's own reference. */
static unsigned int s_suppress_release_cmds = 0u;

void zl38063_vproc_release_cmd_host_load_complete_from_host_boot(bool suppress)
{
    if (suppress) {
        s_suppress_release_cmds |= B220_RELEASE_CMD_HOST_LOAD_CMP;
    } else {
        s_suppress_release_cmds &= ~B220_RELEASE_CMD_HOST_LOAD_CMP;
    }
}

bool zl38063_vproc_release_cmd_host_load_complete_is_suppressed(void)
{
    return (s_suppress_release_cmds & B220_RELEASE_CMD_HOST_LOAD_CMP) != 0u;
}

/* Same for the GO write: the matrix issues reg 0x32 <- 0x0008 itself (or not at
 * all, in variants 1, 2 and 5).  VprocTwolfFirmwareStart() is left byte-for-byte
 * as upstream; only this one call site in the upload path consults the flag. */
void zl38063_vproc_firmware_start_from_host_boot(bool suppress)
{
    if (suppress) {
        s_suppress_release_cmds |= B220_RELEASE_CMD_FIRMWARE_START;
    } else {
        s_suppress_release_cmds &= ~B220_RELEASE_CMD_FIRMWARE_START;
    }
}

bool zl38063_vproc_firmware_start_is_suppressed(void)
{
    return (s_suppress_release_cmds & B220_RELEASE_CMD_FIRMWARE_START) != 0u;
}

/*============================================================================
 * Public API - api_lib/vprocTwolf_access.c
 *==========================================================================*/

/* VprocTwolfHostCmdWr(): issue one host command through the loader's own
 * path (VprocTwolfcmdRegWr() = mailbox acquire, reg 0x32 <- cmd, reg 0x0006 <-
 * 1, poll reg 0x32 back to 0), for the one caller that must emit the release
 * commands itself: the hook-release matrix (B220_DSP_RELEASE_MATRIX).  The
 * release variants 0x000D / 0x0008 go out through exactly the same framing as
 * VprocTwolfHbiBoot_alt() / VprocTwolfFirmwareStart() use, so "the matrix sent
 * it" and "the default path sent it" are the same bytes on the wire. */
VprocStatusType VprocTwolfHostCmdWr(unsigned short cmd)
{
    return VprocTwolfcmdRegWr(cmd);
}

/* Defined further down, next to TwolfHbiPage255Write(). */
static VprocStatusType TwolfHbiPage255Read(
    uint32_t targetAddr,
    unsigned char numWords,
    unsigned short *pDdata,
    unsigned int offset_encoding);

VprocStatusType VprocTwolfHbiInit(void)
{
    unsigned short buf = HBI_CONFIG_REG | HBI_CONFIG_VAL;

    if (VprocHALInit() != 0) {
        return VPROC_STATUS_INIT_FAILED;
    }
    /* 0xFD80 = HBI configure command; this is what turns the HBI port on. */
    if (VprocHALWrite(buf) != 0) {
        DEBUG_LOGE(TAG_SPI, "HBI configure (0x%04x) failed", (unsigned)buf);
        return VPROC_STATUS_ERR_HBI;
    }
    return VPROC_STATUS_SUCCESS;
}

VprocStatusType VprocTwolfHbiCleanup(void)
{
    VprocHALcleanup();
    return VPROC_STATUS_SUCCESS;
}

VprocStatusType VprocTwolfHbiRead(
    unsigned short cmd,     /* register to read from */
    unsigned char numwords,
    unsigned short *pData)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;

    if ((pData == NULL) || (numwords == 0)) {
        return VPROC_STATUS_INVALID_ARG;
    }
    if (zl38063_hbi_read((uint16_t)cmd, (uint8_t)numwords, (uint16_t *)pData) != 0) {
        DEBUG_LOGE(TAG_SPI, "ERROR: VPROC_STATUS_RD_FAILED,CMD:0x%04x", (unsigned)cmd);
        return VPROC_STATUS_RD_FAILED;
    }
    return status;
}

VprocStatusType VprocTwolfHbiWrite(
    unsigned short cmd,     /* register to write to */
    unsigned char numwords,
    unsigned short *pData)
{
    if ((pData == NULL) || (numwords == 0) || (numwords > 126)) {
        DEBUG_LOGE(TAG_SPI, "number of words is out of range. Maximum is 126");
        return VPROC_STATUS_INVALID_ARG;
    }
    /*
     * 16-bit SPI access mode - each word goes out in its own CS assertion,
     * the HBI command word carries the word count.  This matches upstream
     * spis_tw_hbi_wr16_cmd() + spis_tw_hbi_wr16_data().
     */
    if (zl38063_hbi_write((uint16_t)cmd, (uint8_t)numwords, (uint16_t *)pData) != 0) {
        DEBUG_LOGE(TAG_SPI, "ERROR: VPROC_STATUS_WR_FAILED,CMD:0x%04x", (unsigned)cmd);
        return VPROC_STATUS_WR_FAILED;
    }
    return VPROC_STATUS_SUCCESS;
}

/* Public wrapper for the page-255 window read (framing defined further down,
 * next to TwolfHbiPage255Write()).  Additive: no existing path calls it.
 *
 * @targetAddr: DSP RAM address as the image records carry it; the window offset
 *              field is derived from it by
 *              zl38063_hbi_page255_window_offset() (zl38063_hbi.c), the single
 *              place that arithmetic lives.
 * @offset_encoding: ZL38063_PAGE255_OFF_BYTE or ZL38063_PAGE255_OFF_WORD, i.e.
 *              whether the target's low byte goes into command bits 14..8 as-is
 *              or divided by two.  The upload's own write path divides
 *              (hbi_send_cmd()), so OFF_WORD is the encoding that mirrors it. */
VprocStatusType VprocTwolfHbiPage255Read(
    uint32_t targetAddr,
    unsigned char numWords,
    unsigned short *pDdata,
    unsigned int offset_encoding)
{
    return TwolfHbiPage255Read(targetAddr, numWords, pDdata, offset_encoding);
}

VprocStatusType VprocTwolfLoadConfig(dataArr *pCr2Buf, unsigned short numElements)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;
    unsigned short i;
    unsigned short buf;

    if (pCr2Buf == NULL) {
        return VPROC_STATUS_INVALID_ARG;
    }
    /* Send the config to the device RAM, one register at a time. */
    for (i = 0; i < numElements; i++) {
        buf = pCr2Buf[i].value;
        status = VprocTwolfHbiWrite(pCr2Buf[i].reg, 1, &buf);
        if (status != VPROC_STATUS_SUCCESS) {
            DEBUG_LOGE(TAG_SPI, "config write failed at element %u (reg 0x%04x)",
                       (unsigned)i, (unsigned)pCr2Buf[i].reg);
            return VPROC_STATUS_ERR_HBI;
        }
    }
    return status;
}

/*============================================================================
 * Firmware boot loading (api_lib/vprocTwolf_access.c)
 *
 * NOT deleted, unlike the first cut of this port: the stock B220 firmware
 * really does host-boot the DSP.  VprocTwolfHbiBoot() walks 3699 records with
 * a 44-byte stride (0x40125cfd..0x40125d8f in the stock image), writing the
 * target address to HBI register 0x0C and the data words through the page 255
 * window, exactly like HbiSrecBoot_alt() below; the functions here are therefore
 * always compiled.  The recovered image is linked in by zl38063_fwimg_data.c, so
 * these paths now load the DSP for real; without it st_twFirmware[] and friends
 * stay zero (weak definitions in zl38063_fwimg.c) and a boot load reports
 * VPROC_STATUS_ERR_IMAGE instead of guessing.
 *==========================================================================*/

/* VprocTwolfCheckCmdResult(): 0 in the param/result register means the last
 * host command completed successfully. */
VprocStatusType VprocTwolfCheckCmdResult(void)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;
    unsigned short buf = 0;

    status = VprocTwolfHbiRead(HOST_CMD_PARAM_RESULT_REG, 1, &buf);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
        return status;
    }
    if (buf != 0) {
        DEBUG_LOGE(TAG_SPI, "Command failed...Resultcode = 0x%04x", (unsigned)buf);
        return VPROC_STATUS_ERR_VERIFY;
    }
    return VPROC_STATUS_SUCCESS;
}

/* TwolfHbiPage255Write(): push one block of firmware words through the page
 * 255 window at @offset. */
static VprocStatusType TwolfHbiPage255Write(
    unsigned char page,
    unsigned char offset,
    unsigned char numWords,
    unsigned short *pDdata)
{
    uint16 cmdWrd = (uint16)PAGE_255_WRITE_CMD(page, offset);

    if (VprocTwolfHbiWrite(cmdWrd, numWords, pDdata) != VPROC_STATUS_SUCCESS) {
        return VPROC_STATUS_ERR_HBI;
    }
    return VPROC_STATUS_SUCCESS;
}

/*============================================================================
 * Page-255 window READ side (additive; used by the upload-verify probe and by
 * the window calibration)
 *
 * This mirrors TwolfHbiPage255Write() above word for word and is deliberately
 * NOT routed through VprocTwolfHbiRead(): that entry point re-decodes a 16-bit
 * *register address* (page = addr >> 8, offset = (addr & 0xFF) / 2), whereas a
 * page-255 access is a data WINDOW whose command word is built from the target
 * address' low byte, HBI_SELECT_PAGE(0xFF) then HBI_PAGED_READ(offset, n-1)
 * (vprocTwolf_access.c).  VprocTwolfHbiWrite() happens to arrive at exactly that
 * framing for page 255 - the /2 in its decoder and the /2 in the window are the
 * same operation - so "write cmd 0xFF00|lowbyte" and "read cmd
 * HBI_PAGED_READ(lowbyte/2)" address the same window position.  That is the
 * whole point of the fix: the read must put lowbyte/2 in bits 14..8, because
 * that is what zl38063_hbi_write()/hbi_send_cmd() put there for the upload's own
 * window write.  Both numbers come from zl38063_hbi_page255_window_offset()
 * (zl38063_hbi.c), which is the only place the mask/scale is written down.
 *==========================================================================*/

/* One page-255 window read: select page 0xFF, send the paged READ command, then
 * clock out @numWords words - one HBI word transfer each, exactly the read path
 * VprocTwolfHbiRead() uses (spis_tw_hbi_rd16_cmd() + N x spis_tw_hbi_rd16()). */
static VprocStatusType TwolfHbiPage255Read(
    uint32_t targetAddr,
    unsigned char numWords,
    unsigned short *pDdata,
    unsigned int offset_encoding)
{
    uint16_t off;

    if ((pDdata == NULL) || (numWords == 0)) {
        return VPROC_STATUS_INVALID_ARG;
    }
    off = (uint16_t)zl38063_hbi_page255_window_offset(targetAddr, offset_encoding);

    if (zl38063_hbi_wr16_cmd(ZL38063_HBI_SELECT_PAGE(0xFF)) != 0) {
        return VPROC_STATUS_ERR_HBI;
    }
    if (zl38063_hbi_wr16_cmd(ZL38063_HBI_PAGED_READ(off, numWords - 1)) != 0) {
        return VPROC_STATUS_ERR_HBI;
    }
    for (unsigned char i = 0; i < numWords; i++) {
        if ((zl38063_hbi_rd16((uint16_t *)&pDdata[i])) != 0) {
            return VPROC_STATUS_ERR_HBI;
        }
    }
    return VPROC_STATUS_SUCCESS;
}

/*============================================================================
 * Pre-GO observation hook (B220_DSP_VERIFY_BEFORE_GO)
 *
 * The upload path is the only place that knows the last image record has just
 * been written and that no host command has been sent yet, so that is where an
 * observer has to be attached.  This is a plain function pointer: no hook means
 * no call (and with B220_DSP_VERIFY_BEFORE_GO == 0 the call sites below are
 * preprocessor-removed as well, so the default build is unchanged).
 *
 * The call sites are the two places that complete a host load and are ordered
 * BEFORE the commands that release the boot ROM:
 *   - HbiSrecBoot_alt(): right after the record loop and before reg 0x12C (the
 *     entry point) and reg 0x32 <- 0x000D (host load complete);
 *   - VprocTwolfHbiBootConclude(): before its reg 0x32 <- 0x000D write, i.e.
 *     before the same point for the s-record streaming loader.
 * Neither path has any other completion point, so the hook can never run after a
 * host command that could start the image.
 *
 * The setter and the pointer exist unconditionally so the API always links; only
 * the two call sites are switched by B220_DSP_VERIFY_BEFORE_GO, which means a
 * build with the switch at 0 can still register a hook that is simply never
 * called.
 *==========================================================================*/
/*
 * __attribute__((unused)) matters only for the B220_DSP_VERIFY_BEFORE_GO == 0
 * build, where nothing reads the pointer and a file-static that is never
 * referenced would be a -Wunused-variable (the build uses -Werror in places).
 * With the switch at 1 the attribute is inert.
 */
static void (*s_pre_go_hook)(void) __attribute__((unused)) = NULL;

void zl38063_dsp_set_pre_go_hook(void (*fn)(void))
{
    s_pre_go_hook = fn;
}

/* HbiSrecBoot_alt(): load an image that was already converted to the twFwr
 * array layout (Microsemi's tw_convert output). */
static VprocStatusType HbiSrecBoot_alt(twFirmware *st_firmware)
{
    uint16 index = 0;
    uint16 gTargetAddr[2] = {0, 0};
    VprocStatusType status = VPROC_STATUS_SUCCESS;

    if ((st_firmware == NULL) || (st_firmware->st_Fwr == NULL)) {
        return VPROC_STATUS_INVALID_ARG;
    }
    if (st_firmware->twFirmwareStreamLen == 0) {
        Zl38063FwImageReportMissing();
        return VPROC_STATUS_ERR_IMAGE;
    }

    while (index < st_firmware->twFirmwareStreamLen) {
        /* Put the address into our global target address. */
        gTargetAddr[0] = (uint16)((st_firmware->st_Fwr[index].targetAddr &
                                   0xFFFF0000UL) >> 16);
        gTargetAddr[1] = (uint16)(st_firmware->st_Fwr[index].targetAddr &
                                  0x0000FFFFUL);

        /* Write the data block to the device. */
        if (st_firmware->st_Fwr[index].numWords != 0) {
            if (st_firmware->st_Fwr[index].useTargetAddr) {
                status = VprocTwolfHbiWrite(PAGE_255_BASE_HI_REG, 2, gTargetAddr);
                if (status != VPROC_STATUS_SUCCESS) {
                    DEBUG_LOGE(TAG_SPI, "Unable to set target address 0x%04x:0x%04x",
                               (unsigned)gTargetAddr[0], (unsigned)gTargetAddr[1]);
                    return VPROC_STATUS_ERR_HBI;
                }
            }
            status = TwolfHbiPage255Write(0xFF,
                                          (uint8)zl38063_hbi_page255_window_offset(
                                              st_firmware->st_Fwr[index].targetAddr,
                                              ZL38063_PAGE255_OFF_BYTE),
                                          (unsigned char)st_firmware->st_Fwr[index].numWords,
                                          st_firmware->st_Fwr[index].buf);
            if (status != VPROC_STATUS_SUCCESS) {
                DEBUG_LOGE(TAG_SPI, "status = %d, numWords = %d",
                           (int)status, (int)st_firmware->st_Fwr[index].numWords);
                return status;
            }
        }
        index++;
    }

    /*
     * Pre-GO observation point (B220_DSP_VERIFY_BEFORE_GO): every image record
     * has just been written and NO host command has been sent yet - the entry
     * point (reg 0x12C) and the host-load-complete command (reg 0x32 <- 0x000D,
     * which VprocTwolfHbiBoot_alt() issues right after this function returns)
     * both come later.  That makes this the last instant at which a read-back of
     * the image can distinguish "the transfer never wrote the records" from "the
     * image was in RAM and something cleared it after the start".  The hook is
     * read-only on the device apart from the page-255 window base register the
     * upload itself writes per record; without a registered hook (the default)
     * this line disappears at compile time.
     */
#if B220_DSP_VERIFY_BEFORE_GO
    if (s_pre_go_hook != NULL) {
        s_pre_go_hook();
    }
#endif

    /* Program the program's execution start register (0x12C). */
    gTargetAddr[0] = (uint16)((st_firmware->execAddr & 0xFFFF0000UL) >> 16);
    gTargetAddr[1] = (uint16)(st_firmware->execAddr & 0x0000FFFFUL);
    status = VprocTwolfHbiWrite(0x12C, 2, gTargetAddr);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "unable to program page 1 execution address");
        return status;
    }

    DEBUG_LOGI(TAG_SPI, "prgmBase 0x%08x", (unsigned)st_firmware->prgmBase);
    DEBUG_LOGI(TAG_SPI, "execAddr 0x%08x", (unsigned)st_firmware->execAddr);
    return VPROC_STATUS_SUCCESS;
}

/* VprocTwolfHbiBootPrepare(): stop the current firmware, reset into boot ROM
 * and check that the boot ROM is listening (0xD3D3 at register 0x034). */
VprocStatusType VprocTwolfHbiBootPrepare(void)
{
    unsigned short buf[2] = {0, 0};
    VprocStatusType status = VPROC_STATUS_SUCCESS;

    /* Write 1 to register 0x14 to stop the running firmware and reset the
     * device into boot ROM mode. */
    buf[0] = 1;
    status = VprocTwolfHbiWrite(CLK_STATUS_REG, 1, buf);
    if (status != VPROC_STATUS_SUCCESS) {
        return VPROC_STATUS_ERR_HBI;
    }
    Vproc_msDelay(300);     /* wait for the reset to complete */

    buf[0] = buf[1] = 0;
    status = VprocTwolfHbiRead(HOST_CMD_PARAM_RESULT_REG, 1, buf);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
        return status;
    }
    /* Check that the device is accessible for commands. */
    if (buf[0] != TWOLF_BOOTROM_MAGIC) {
        DEBUG_LOGE(TAG_SPI, "ERROR: HBI is not accessible (0x%04x, want 0x%04x)",
                   (unsigned)buf[0], (unsigned)TWOLF_BOOTROM_MAGIC);
        return VPROC_STATUS_ERR_HBI;
    }
    return status;
}

/* VprocTwolfHbiBoot_alt(): single-shot boot load from host RAM. */
VprocStatusType VprocTwolfHbiBoot_alt(twFirmware *st_firmware)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;
    unsigned short buf = 0;

    status = VprocTwolfHbiBootPrepare();
    if (status != VPROC_STATUS_SUCCESS) {
        return status;
    }

    /* Transfer the image.  st_firmware->havePrgmBase is deliberately not used
     * here, exactly as upstream: HbiSrecBoot_alt() re-writes the page 255 base
     * register from every record whose useTargetAddr is set. */
    status = HbiSrecBoot_alt(st_firmware);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: firmware transfer failed", (int)status);
        return status;
    }

    /* Tell the device that the host load is complete - unless the release path
     * (B220_DSP_RELEASE_MATRIX / B220_DSP_ADF_RELEASE_TX) has asked this one host
     * command to be suppressed so it can issue the release writes itself, in its
     * own order and with its own logging.  s_suppress_release_cmds is 0 in every
     * other build and in every other code path, so this changes nothing there. */
    if ((s_suppress_release_cmds & B220_RELEASE_CMD_HOST_LOAD_CMP) == 0u) {
        buf = HOST_CMD_HOST_LOAD_CMP;
        status = VprocTwolfcmdRegWr(buf);
        if (status != VPROC_STATUS_SUCCESS) {
            DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
            return status;
        }
    } else {
        /* The release path issues (or deliberately skips) this command itself.
         * The boot ROM has just been told nothing, so reg 0x034 still carries the
         * 0xD3D3 "waiting for a host image" magic and VprocTwolfCheckCmdResult()
         * below would - correctly - report a failure.  That check is therefore
         * skipped together with the command it verifies, and the transfer is
         * reported as successful: the records and reg 0x12C are in place, which
         * is exactly the state the release sequence wants to release from.  This
         * branch is unreachable unless the mask is set, i.e. never in a default
         * build. */
        DEBUG_LOGI(TAG_SPI, "reg 0x32 <- 0x000D suppressed at the host-boot call "
                            "site (release path); the result check is skipped too");
    }

    /* Verify that the boot load succeeded - only when the command that it
     * verifies was actually sent (see the suppression branch above). */
    if ((s_suppress_release_cmds & B220_RELEASE_CMD_HOST_LOAD_CMP) == 0u) {
        if (VprocTwolfCheckCmdResult() != VPROC_STATUS_SUCCESS) {
            DEBUG_LOGE(TAG_SPI, "ERROR: failed to load the firmware");
            return VPROC_STATUS_FW_LOAD_FAILED;
        }
    }
    return VPROC_STATUS_SUCCESS;
}

/* AsciiHexToHex(): convert @len ascii hex characters to an integer. */
static unsigned int AsciiHexToHex(const char *str, unsigned char len)
{
    unsigned int val = 0;
    char c;
    unsigned char i = 0;

    for (i = 0; i < len; i++) {
        c = *str++;
        val <<= 4;
        if (c >= '0' && c <= '9') {
            val += (unsigned int)(c & 0x0F);
            continue;
        }
        c &= 0xDF;      /* upper-case */
        if (c >= 'A' && c <= 'F') {
            val += (unsigned int)((c & 0x07) + 9);
            continue;
        }
        return 0;
    }
    return val;
}

/* spisTwBootWrite(): feed one ascii s-record line to the device. */
static int spisTwBootWrite(char *blockOfFwrData)
{
    int status = 0;
    int rec_type = 0;
    int i = 0;
    int j = 0;
    uint8 numbytesPerLine = 0;
    uint8 numdataWordPerLine = 0;
    uint16 dataBuf[64];
    unsigned long address = 0;
    uint8 page255Offset = 0x00;
    uint16 cmd = 0;
    uint16 gTargetAddr[2] = {0, 0};

    if (blockOfFwrData == NULL) {
        return VPROC_STATUS_INVALID_ARG;
    }
    /* If this line is not an s-record, skip it. */
    if (blockOfFwrData[0] != 'S') {
        DEBUG_LOGE(TAG_SPI, "blockOfFwrData[0] = %c", blockOfFwrData[0]);
        return VPROC_STATUS_INVALID_ARG;
    }
    numbytesPerLine = (uint8)AsciiHexToHex(&blockOfFwrData[2], 2);
    if (numbytesPerLine < 5) {
        DEBUG_LOGE(TAG_SPI, "bad s-record byte count %u", (unsigned)numbytesPerLine);
        return VPROC_STATUS_INVALID_ARG;
    }
    numdataWordPerLine = (uint8)((numbytesPerLine - 5) / 2);
    if (numdataWordPerLine > (uint8)(sizeof(dataBuf) / sizeof(dataBuf[0]))) {
        return VPROC_STATUS_INVALID_ARG;
    }

    /* Get the s-record type. */
    rec_type = blockOfFwrData[1] - '0';

    /* Skip record types that do not carry an address we can use. */
    if ((rec_type == 4) || (rec_type == 5) || (rec_type == 6) || (rec_type == 0)) {
        return ZL38063_TWOLF_STATUS_NEED_MORE_DATA;
    }

    /* Get the address (s-records 1..3 carry 4 hex digits, 7..9 carry 8). */
    address = (rec_type <= 3) ? AsciiHexToHex(&blockOfFwrData[4], 4)
                              : AsciiHexToHex(&blockOfFwrData[4], 8);
    page255Offset = (uint8)zl38063_hbi_page255_window_offset(address,
                                                             ZL38063_PAGE255_OFF_BYTE);

    gTargetAddr[0] = (uint16)((address & 0xFFFF0000UL) >> 16);
    gTargetAddr[1] = (uint16)(address & 0x0000FFFFUL);

    /* Record types 7/8/9 carry the execution address. */
    if ((rec_type == 7) || (rec_type == 8) || (rec_type == 9)) {
        DEBUG_LOGI(TAG_SPI, "execAddr = 0x%08lx", address);
        status = VprocTwolfHbiWrite(0x12C, 2, gTargetAddr);
        if (status != 0) {
            DEBUG_LOGE(TAG_SPI, "unable to program page 1 execution address");
            return status;
        }
        return ZL38063_TWOLF_STATUS_BOOT_COMPLETE;
    }

    /* Program the page 255 window base for this block. */
    status = VprocTwolfHbiWrite(PAGE_255_BASE_HI_REG, 2, gTargetAddr);
    if (status != 0) {
        DEBUG_LOGE(TAG_SPI, "unable to set target address 0x%08lx", address);
        return VPROC_STATUS_FW_LOAD_FAILED;
    }

    /* Get the data bytes (they start after Sx + count + address). */
    j = (rec_type <= 3) ? 8 : 12;
    for (i = 0; i < numdataWordPerLine; i++) {
        dataBuf[i] = (uint16)AsciiHexToHex(&blockOfFwrData[j], 4);
        j += 4;
    }
    /* Write the data through the page 255 window. */
    cmd = (uint16)PAGE_255_WRITE_CMD(0xFF, page255Offset);
    status = VprocTwolfHbiWrite(cmd, numdataWordPerLine, dataBuf);
    if (status != 0) {
        return status;
    }
    return ZL38063_TWOLF_STATUS_NEED_MORE_DATA;
}

/* VprocTwolfHbiBootMoreData(): one ascii s-record line per call. */
VprocStatusType VprocTwolfHbiBootMoreData(char *dataBlock)
{
    return (VprocStatusType)spisTwBootWrite(dataBlock);
}

/* VprocTwolfHbiBootConclude(): finish a streaming host load and verify it. */
VprocStatusType VprocTwolfHbiBootConclude(void)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;
    unsigned short buf = HOST_CMD_HOST_LOAD_CMP;

#if B220_DSP_VERIFY_BEFORE_GO
    /*
     * The same pre-GO point as in HbiSrecBoot_alt(), for the s-record streaming
     * loader: the last line has been written and the host-load-complete command
     * below is the first thing that can release the boot ROM.  Kept in step with
     * that call site so an upload observer never sees a silent path.
     */
    if (s_pre_go_hook != NULL) {
        s_pre_go_hook();
    }
#endif

    status = VprocTwolfcmdRegWr(buf);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
        return status;
    }
    if (VprocTwolfCheckCmdResult() != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR: failed to load the firmware");
        return VPROC_STATUS_FW_LOAD_FAILED;
    }
    return status;
}

/* VprocTwolfReset(): reset the device.
 *
 * mode - VPROC_RST_HARDWARE_ROM, VPROC_RST_HARDWARE_RAM, VPROC_RST_SOFTWARE,
 *        VPROC_RST_AEC
 * Return: 0 = success, else fail. */
VprocStatusType VprocTwolfReset(VprocResetMode mode)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;
    unsigned short buf;

    if (mode == VPROC_RST_HARDWARE_RAM) {        /* hard reset, reload from RAM */
        buf = 0x05;
        status = VprocTwolfHbiWrite(0x014, 1, &buf);
    } else if (mode == VPROC_RST_HARDWARE_ROM) { /* power-on reset, reload from flash */
        buf = 0x09;
        status = VprocTwolfHbiWrite(0x014, 1, &buf);
    } else if (mode == VPROC_RST_AEC) {          /* AEC method */
        buf = 0x01;
        status = VprocTwolfHbiWrite(0x0300, 1, &buf);
    } else if (mode == VPROC_RST_SOFTWARE) {     /* soft reset */
        buf = 0x02;
        status = VprocTwolfHbiWrite(0x006, 1, &buf);
    } else {
        DEBUG_LOGE(TAG_SPI, "Invalid reset type");
        return VPROC_STATUS_INVALID_ARG;
    }

    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
        return status;
    }
    Vproc_msDelay(200);
    return VPROC_STATUS_SUCCESS;
}

VprocStatusType VprocTwolfSetVolume(uint8 vol)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;
    unsigned short buf = (unsigned short)vol;    /* Gain A */
    buf = (unsigned short)(buf + ((unsigned short)((unsigned short)vol << 8))); /* Gain B */
    int init = VprocHALInit();
    if (init != 0) {
        return VPROC_STATUS_INIT_FAILED;
    }
    /* NB: upstream reads back only the low byte (Gain A) in
     * VprocTwolfGetVolume(), so only register 0x238 is symmetric with it. */
    status = VprocTwolfHbiWrite(0x238, 1, &buf);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
        return status;
    }
    status = VprocTwolfHbiWrite(0x23A, 1, &buf);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
        return status;
    }
    return status;
}

VprocStatusType VprocTwolfGetVolume(int8_t *vol)
{
    VprocStatusType status = VPROC_STATUS_SUCCESS;
    unsigned short buf = 0;
    int init;

    if (vol == NULL) {
        return VPROC_STATUS_INVALID_ARG;
    }
    init = VprocHALInit();
    if (init != 0) {
        return VPROC_STATUS_INIT_FAILED;
    }
    status = VprocTwolfHbiRead(0x238, 1, &buf);
    if (status != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)status);
        return status;
    }
    *vol = (int8_t)buf;
    return status;
}

VprocStatusType VprocTwolfGetAppStatus(uint16 *status)
{
    VprocStatusType ret = VPROC_STATUS_SUCCESS;
    unsigned short buf = 0;
    int init;

    if (status == NULL) {
        return VPROC_STATUS_INVALID_ARG;
    }
    init = VprocHALInit();
    if (init != 0) {
        return VPROC_STATUS_INIT_FAILED;
    }
    ret = VprocTwolfHbiRead(0x030, 1, &buf);
    if (ret != VPROC_STATUS_SUCCESS) {
        DEBUG_LOGE(TAG_SPI, "ERROR %d: ", (int)ret);
        return ret;
    }
    *status = buf & 0x0F;
    return ret;
}

VprocStatusType VprocTwolfFirmwareStart(void)
{
    unsigned short buf = HOST_CMD_FWR_GO;
    return VprocTwolfcmdRegWr(buf);
}

VprocStatusType VprocTwolfFirmwareStop(void)
{
    unsigned short buf = HOST_CMD_FWR_STOP;
    return VprocTwolfcmdRegWr(buf);
}
