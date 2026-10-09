/****************************************************************************
* zl38063_firmware.h - ZL38063 firmware image for host (HBI) boot loading
*
* TEMPLATE - the image itself is licence-gated and is NOT in this tree.
*
* Why this exists
* ---------------
* A protocol RE pass proved that the stock B220 firmware DOES host-boot the
* DSP: inside its VprocTwolfHbiBoot there is a loop of up to 3699 records with
* a 28-byte stride that writes 2 words at a time to HBI register 0x0C (the
* page 255 window), preceded by the mailbox sequence (reg 0x14 <- 1, 300 ms,
* read reg 0x34 must be 0xD3D3, write reg 0x12C = execAddr halves, write
* reg 0x32 <- 0x000D, then poll reg 0x06).  The ported helpers implement all
* of that; only the image bytes are missing.
*
* How to populate it
* ------------------
* 1. Obtain the ZL38063 image (Microsemi SDS account, or read it out of the
*    slave SPI flash of a working toy) and convert the *.s3 with Microsemi's
*    tw_convert tool into the twFwr array layout below - the same way esp-adf
*    generated its firmware/zl38063_firmware.h.
* 2. Put the converted array in a .c file as
*        const twFwr st_twFirmware[] = { ... };
*    together with the four scalars, and compile it with
*    -DZL38063_HAVE_FIRMWARE_IMAGE so the strong definitions replace the weak
*    empty ones in zl38063_fwimg.c.
*
* twFwr layout (see zl38063_vproc.h; identical to upstream vproc_data_types.h):
*     uint16 buf[16];       one s-record's worth of data words
*     uint16 numWords;      how many of buf[] are valid (0 = address-only)
*     uint32 targetAddr;    32-bit target address, split into two HBI words
*     uint8  useTargetAddr; 1 => write targetAddr to the page 255 base first
***************************************************************************/

#ifndef ZL38063_FIRMWARE_H
#define ZL38063_FIRMWARE_H

#include <stdint.h>

#include "zl38063_vproc.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The image symbols are declared in zl38063_vproc.h and defined WEAK (empty)
 * in zl38063_fwimg.c, so this template only has to document the contract.
 * Nothing includes this file: a real image is simply a strong definition of
 *
 *     const twFwr    st_twFirmware[];
 *     const uint16_t firmwareStreamLen;
 *     const uint32_t executionAddress;
 *     const uint8_t  haveProgramBaseAddress;
 *     const uint32_t programBaseAddress;
 *
 * which the linker prefers over the weak empty ones.  Defining
 * ZL38063_HAVE_FIRMWARE_IMAGE is only a marker for whoever adds the image.
 */

#ifdef __cplusplus
}
#endif

#endif /* ZL38063_FIRMWARE_H */
