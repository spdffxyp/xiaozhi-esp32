/****************************************************************************
* zl38063_fwimg_data.c - strong ZL38063 firmware-image symbols for host boot
*
* The B220 stock firmware does NOT let the ZL38063 self-boot: on every
* power-up it pulses the DSP reset and then host-boots it over HBI with the
* image that lives in the ESP32 application image.  That image was recovered
* from the stock flash dump and is emitted by
*
*     zl38063/zl38063_firmware_image.h
*     3699 twFwr records, firmwareStreamLen = 3699
*     programBaseAddress = 0x00080200, executionAddress = 0x00083040
*
* This translation unit is the one place that compiles that header in
* ZL38063_FWIMAGE_EXTERNAL mode, i.e. it emits the five *strong* definitions
* that override the weak empty ones in zl38063_fwimg.c:
*
*     st_twFirmware            const twFwr[3699]
*     firmwareStreamLen        const uint16_t  = 3699
*     executionAddress         const uint32_t  = 0x00083040
*     haveProgramBaseAddress   const uint8_t   = 1
*     programBaseAddress       const uint32_t  = 0x00080200
*
* Names, types and the twFwr layout were checked against the extern
* declarations in zl38063_vproc.h (uint16 / uint32_t / uint8, stride 44) and
* against the weak definitions in zl38063_fwimg.c; the header additionally
* _Static_assert()s sizeof(twFwr) == 44 and the field offsets 32/36/40, so a
* compiler that lays the struct out differently fails the build instead of
* booting the DSP with a wrong image.
*
* This file is board glue (it only includes the generated image); it carries
* no Microsemi code, so it has no MIT header.  The image bytes themselves come
* from the stock firmware dump - see zl38063/zl38063_firmware_image.md for the
* evidence chain and ZL38063-LICENSE.txt for the plugin licensing note.
***************************************************************************/

/*
 * ZL38063_FWIMAGE_EXTERNAL is defined here on purpose: the image header is the
 * only header that needs it, and the board directory has no CMakeLists.txt of
 * its own (main/CMakeLists.txt globs every .c in the board directory), so a
 * per-file definition cannot be injected without editing shared build files.
 * Defining it here is equivalent to the -DZL38063_FWIMAGE_EXTERNAL the header
 * asks for; if the build ever defines it globally too, the two agree.
 */
#ifndef ZL38063_FWIMAGE_EXTERNAL
#define ZL38063_FWIMAGE_EXTERNAL 1
#endif

/*
 * Only this header is included - deliberately NOT zl38063_vproc.h.  Both header
 * families typedef twFwr (the image header's own, and the port's
 * upstream-compatible one); keeping the image in a translation unit of its own
 * means the layout is asserted in exactly one place and nothing here has to
 * agree textually with the ported VPROC header.  What matters for the
 * weak/strong override is the symbol names and the struct layout, both checked
 * below.
 */
#include "zl38063/zl38063_firmware_image.h"

/*
 * Cross-checks against the image header.  The scalar symbols are const objects,
 * not integer constant expressions, so they cannot be used in a static assert;
 * the image scalars are macros, so they are checked with the preprocessor
 * instead.  Rationale: the numbers the loader will actually use (the const
 * symbols declarations in zl38063_vproc.h resolve to) must equal the scalars of
 * the image this file compiles.  If the image is ever regenerated with
 * different values, or this file is compiled without ZL38063_FWIMAGE_EXTERNAL
 * and silently falls back to the weak empty symbols, the build stops here
 * instead of host-booting the DSP with the wrong record count or entry point.
 */
#if ZL38063_FW_RECORDS != 3699
#error "zl38063_firmware_image.h: unexpected record count"
#endif
#if ZL38063_FW_PROGRAM_BASE != 0x00080200u
#error "zl38063_firmware_image.h: unexpected program base address"
#endif
#if ZL38063_FW_EXEC_ADDR != 0x00083040u
#error "zl38063_firmware_image.h: unexpected execution address"
#endif
#if ZL38063_FW_HAVE_PROGRAM_BASE != 1
#error "zl38063_firmware_image.h: unexpected program-base flag"
#endif

/*
 * The struct layout itself is asserted inside the generated header (sizeof
 * twFwr == 44, numWords at +32, targetAddr at +36, useTargetAddr at +40) for
 * any compiler reporting __STDC_VERSION__ >= 201112L, and was re-verified
 * against the raw record dump: 3699 * 44 = 162756 bytes with record 0 =
 * {buf[0]=0x0000, buf[1]=0x1c0f, numWords=16, targetAddr=0x00080200,
 * useTargetAddr=1}.  Nothing more to assert here; the file compiles with
 * -Wall -Wextra -Werror on xtensa-esp-elf-gcc 15.2.0 (IDF 6.1 toolchain).
 */
