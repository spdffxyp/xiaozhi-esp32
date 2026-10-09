/****************************************************************************
* zl38063_fwimg.c - weak placeholders for the ZL38063 firmware image symbols
*
* The real firmware image (Microsemi's zl38063_firmware.h / libfirmware.a) is
* licence-gated and is not present in this tree, but the host-side boot loader
* in zl38063_vproc.c has to keep compiling and linking without it.
*
* The five symbols below are DEFINED WEAK here, with an empty image.  Dropping
* in a real image (a strong definition of the same names, e.g. a generated
* zl38063/zl38063_firmware.h compiled with ZL38063_HAVE_FIRMWARE_IMAGE)
* overrides them with no duplicate-symbol error: the linker always prefers a
* strong definition over a weak one.
*
* VprocTwolfHbiBoot_alt() sees twFirmwareStreamLen == 0, calls
* Zl38063FwImageReportMissing() and returns VPROC_STATUS_ERR_IMAGE instead of
* silently pretending that it uploaded something.
*
* This file carries no Microsemi code; it is board glue (no licence header).
***************************************************************************/

#include "zl38063_vproc.h"

#include "esp_log.h"

#define TAG_FWIMG "ZL38063_FW"

/* Empty image: one all-zero record and a zero stream length, so the boot
 * loader sees "no image" instead of walking off the end of the array.
 * Casts keep C++ (and -Wmissing-braces) quiet about the nested struct. */
__attribute__((weak)) const twFwr st_twFirmware[1] = {
    { { 0 }, 0, 0, 0 },
};

__attribute__((weak)) const uint16_t firmwareStreamLen = 0;

__attribute__((weak)) const uint32_t executionAddress = 0;

__attribute__((weak)) const uint8_t haveProgramBaseAddress = 0;

__attribute__((weak)) const uint32_t programBaseAddress = 0;

/* Called by the ported boot path when a boot load is attempted without an
 * image, so the reason shows up in the log. */
void Zl38063FwImageReportMissing(void)
{
    ESP_LOGW(TAG_FWIMG, "no ZL38063 firmware image available: this driver "
                        "cannot host-boot the DSP. Either let the device "
                        "self-boot from its own slave flash, or add a "
                        "licence-gated image (see zl38063/zl38063_firmware.h).");
}
