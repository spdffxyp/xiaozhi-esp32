/*
 * b220_gsensor.h - the B220's accelerometer (SC7A20H), read on demand.
 *
 * WHAT IT IS
 *   A three-axis accelerometer on the board's I2C0 bus - SDA GPIO18, SCL GPIO23, the same
 *   two wires the AW9106B sits on - at address 0x19.  The reference port of this toy names
 *   it (modou_gsensor.h: address 0x19, WHO_AM_I 0x0f expected 0x11) as an SC7A20H, which is
 *   register-compatible with the LIS3DH.  This port had also already touched it: the button
 *   discovery probe (b220_button_discovery.c) reads its CLICK/INT1 status registers, so the
 *   address and the bus access pattern are not new information.
 *
 * WHAT THIS DOES, AND WHAT IT DELIBERATELY DOES NOT
 *   Reads only, and only when asked.  There is no sampling task here.
 *
 *   That is a deliberate choice rather than an unfinished one: this chip shares I2C0 with the
 *   AW9106B, which owns the LCD backlight and the audio PA's SHDN line - the one path in this
 *   firmware where a stray write has audible and visible consequences (see the audited PA
 *   sequence in xiaomodou_b220.cc).  A 50-100 Hz polling loop would sit on that bus
 *   continuously for the sake of a feature nobody has asked for yet.  A read costs one
 *   transaction and happens when a caller wants a value.
 *
 *   When a feature does need continuous motion - shake detection, tilt control, tap - the
 *   likely shape is the chip's own CLICK/motion interrupt on its INT pin, not a poll loop, and
 *   which GPIO that is has not been identified yet.  That work starts here.
 *
 * UNITS
 *   The raw registers are 10-bit left-justified in 16 bits at the +-2g range the reference
 *   configures, which makes `raw >> 6` equal to MILLI-g.  The struct carries both: the raw
 *   value for anyone comparing against the reference implementation's numbers, and mg for
 *   anyone reasoning about orientation.
 */

#ifndef B220_GSENSOR_H
#define B220_GSENSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ready;       /* the chip was found and configured */
    uint8_t who_am_i; /* WHO_AM_I (0x0f); 0x11 identifies an SC7A20/LIS3DH */
    /* Raw 16-bit register values, as the reference implementation uses them. */
    int16_t raw_x;
    int16_t raw_y;
    int16_t raw_z;
    /* Same values in milli-g, scaled so the largest axis of a resting reading is 1 g.  At rest
     * exactly one axis therefore sits near +-1000. */
    int mg_x;
    int mg_y;
    int mg_z;
    /* The scale that produced those, i.e. the raw value of that 1 g axis.  It is DERIVED from
     * the sample rather than taken from a datasheet - see b220_gsensor.c - so reporting it is
     * what makes a reading auditable. */
    unsigned counts_per_g;
    /* Which way gravity points, as a short word ("flat", "tilt-left", ...).  Points to a
     * string constant; never NULL once the chip is ready. */
    const char* orientation;
} b220_gsensor_state_t;

/*
 * Find and configure the chip.  Idempotent and cheap after the first call.  Returns
 * ESP_ERR_NOT_FOUND when nothing answers at 0x19, which is a normal outcome on a unit whose
 * accelerometer is absent or unpopulated - callers should treat it as "no motion data"
 * rather than as a failure.
 */
esp_err_t b220_gsensor_init(void);

/* Read all three axes now.  Returns ESP_ERR_INVALID_STATE when the chip is not there. */
esp_err_t b220_gsensor_read(b220_gsensor_state_t* state);

/* One-line description for a log or a tool result. */
void b220_gsensor_describe(const b220_gsensor_state_t* state, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* B220_GSENSOR_H */
