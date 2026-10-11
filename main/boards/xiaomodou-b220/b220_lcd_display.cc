/*
 * b220_lcd_display.cc - see b220_lcd_display.h for what this class is (currently: a name) and
 * for the record of what was removed from it and why.
 */

#include "b220_lcd_display.h"

/* No overrides: the status-bar battery percentage and the volume ring were removed because
 * they corrupted LVGL state from outside the LVGL task.  The parked implementation is in
 * docs/parked/. */
