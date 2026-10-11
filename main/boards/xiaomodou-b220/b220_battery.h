/*
 * b220_battery.h - the B220's battery gauge (placeholder; see b220_battery.cc).
 */
#ifndef B220_BATTERY_H
#define B220_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ready;     /* at least one accepted sample has been published */
    bool charging;  /* always false on this hardware - see the .cc notes */
    int raw_adc;    /* filtered raw ADC reading */
    int voltage_mv; /* converted to mV the way the stock firmware converts it */
    int percent;    /* 0..100, or -1 when no sample is available */
    int sample_count;
} b220_battery_state_t;

/** Start the sampling task.  Safe to call more than once; returns the first error. */
esp_err_t b220_battery_start(void);

/** Latest sample.  `ready` is false until the first accepted reading. */
void b220_battery_get_state(b220_battery_state_t* state);

/** Convert a filtered raw reading to battery mV, using the stock firmware's own maths. */
int b220_battery_raw_to_mv(int raw_adc);

/** Convert battery mV to a percentage using the stock firmware's discharge curve. */
int b220_battery_voltage_to_percent(int voltage_mv);

#ifdef __cplusplus
}
#endif

#endif /* B220_BATTERY_H */
