/*
 * b220_lcd_display.h - the B220's screen: the framework's SPI LCD UI plus the two
 * status readouts this toy needs.
 *
 * What it adds on top of SpiLcdDisplay:
 *
 *   1. The battery as a PERCENTAGE in the top-right corner.  The generic status
 *      bar draws a battery ICON, and an icon cannot answer the question a user
 *      actually asks ("how much is left?").  The icon is left alone - it still
 *      carries the charging/level shape, and one of those is worth keeping - and
 *      the digits are drawn next to it.
 *
 *      With no data yet (the first ADC reading takes about a second, and a unit
 *      whose ADC fails never produces one) the label reads "--%" rather than a
 *      made-up 0% or 100%.
 *
 *   2. Nothing else.  The volume ring is NOT here: it lives in LvglDisplay, because
 *      every volume path in the firmware ends at AudioCodec::SetOutputVolume() and
 *      a per-board copy of the indicator would have to be repeated for every board
 *      that wants one.  See LvglDisplay::ShowVolumeOverlay().
 */

#ifndef B220_LCD_DISPLAY_H
#define B220_LCD_DISPLAY_H

#include "lcd_display.h"

class B220LcdDisplay : public SpiLcdDisplay {
public:
    B220LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    /**
     * Draw the battery percentage, next to the framework's battery icon.
     *
     * Called by the base class both from UpdateStatusBar() and from the display's
     * own 1 Hz tick, so the number stays current even when no application event
     * happens for minutes.  The label is created on the first call, i.e. after
     * SetupUI() has built the status bar.
     */
    void UpdateBatteryStatus() override;

private:
    lv_obj_t* battery_percent_label_ = nullptr;
    /* The last string actually drawn, so the once-a-second refresh does not touch
     * LVGL (and therefore does not invalidate the status bar) when nothing moved.
     * The level changes at most a few times an hour.
     *
     * 12 bytes, not 5: the compiler refuses a "%d%%" snprintf into a smaller buffer
     * (an int could in principle be 11 digits plus the sign), and being right about
     * that is cheaper than arguing with -Werror=format-truncation.  The value itself
     * is clamped to 0..100 by the gauge. */
    char battery_percent_text_[12] = {0};
};

#endif  // B220_LCD_DISPLAY_H
