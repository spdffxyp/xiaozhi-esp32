/*
 * b220_lcd_display.cc - see b220_lcd_display.h for what this adds and why.
 */

#include "b220_lcd_display.h"

#include "b220_battery.h"

#include <lvgl.h>

#include <cstdio>
#include <cstring>

static const char* TAG = "b220-lcd";

/*
 * Shown until the first accepted ADC reading (about a second after boot), and for
 * good on a unit whose ADC never produces one.  Deliberately not "0%" and not
 * "100%": both are claims about the battery, and this label is supposed to say
 * "no reading" instead.
 */
#define B220_BATTERY_UNKNOWN_TEXT "--%"

/*
 * How far left of the status bar's right edge the percentage is parked.
 *
 * The status bar is a full-width, transparent overlay on top of the top bar, and
 * the two share the same corner: the framework's battery icon (and the mute icon)
 * sit at the right end of the TOP bar.  So "right edge of the status bar" and "the
 * battery icon" are the same place, and a label drawn there would cover the icon.
 *
 * This offset puts the digits immediately to the LEFT of that icon instead, so both
 * stay readable: the icon keeps carrying the level shape (and is where a charging
 * bolt would appear once the charge-detect line is identified), the digits carry the
 * number.  24 px is about one icon-font line (the font is 20 px) plus the
 * framework's own 2 px spacing between the two icons.
 */
#define B220_BATTERY_PERCENT_OFFSET_X (-24)

void B220LcdDisplay::UpdateBatteryStatus() {
    /*
     * Create the label on first use rather than in a SetupUI() override: the base
     * class owns SetupUI(), and its two variants (WeChat style / plain) build
     * different widget trees.  By the time this runs, status_bar_ exists in both,
     * and hanging the label off the status bar means it does not take part in the
     * top bar's space-between row - the row is already full (network icon on the
     * left, mute + battery icons on the right) and an extra flex child there would
     * push the icons off a 160 px screen.
     */
    if (battery_percent_label_ == nullptr) {
        if (status_bar_ == nullptr) {
            /* UpdateStatusBar() ran before SetupUI(): nothing to attach to yet, and
             * the next tick will find the bar there. */
            return;
        }
        battery_percent_label_ = lv_label_create(status_bar_);
        lv_label_set_text(battery_percent_label_, B220_BATTERY_UNKNOWN_TEXT);
        lv_obj_set_style_text_align(battery_percent_label_, LV_TEXT_ALIGN_RIGHT, 0);
        /* Absolutely positioned, so the bar's own layout (which centres the clock
         * and the notification on top of it) is not disturbed.  IGNORE_LAYOUT says
         * the same thing to LVGL explicitly, in case the bar ever gains a real
         * layout: this label must never push the clock off-centre. */
        lv_obj_add_flag(battery_percent_label_, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(battery_percent_label_, LV_ALIGN_TOP_RIGHT, B220_BATTERY_PERCENT_OFFSET_X, 0);
        ESP_LOGI(TAG, "battery percentage label added to the status bar");
    }

    b220_battery_state_t state = {};
    b220_battery_get_state(&state);

    /* be print-safe for any int the API could ever return - "%d%%" of a
     * full-range int is 12 bytes, and the compiler enforces that - while the
     * displayed value is the clamped 0..100 the gauge publishes. */
    char text[sizeof(battery_percent_text_)];
    if (state.ready && state.percent >= 0) {
        snprintf(text, sizeof(text), "%d%%", state.percent);
    } else {
        snprintf(text, sizeof(text), "%s", B220_BATTERY_UNKNOWN_TEXT);
    }

    if (strcmp(text, battery_percent_text_) == 0) {
        return; /* nothing changed: do not touch LVGL, do not redraw the bar */
    }
    snprintf(battery_percent_text_, sizeof(battery_percent_text_), "%s", text);
    lv_label_set_text(battery_percent_label_, text);
}
