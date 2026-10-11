/*
 * b220_lcd_display.h - the B220's screen.
 *
 * THIS CLASS CURRENTLY ADDS NOTHING.  It is kept as a name, and as the place the status
 * readouts will go back to, because the implementation that lived here has been REMOVED after
 * it was found to corrupt LVGL's internal state.
 *
 * WHAT WAS HERE, AND WHY IT IS NOT
 * --------------------------------
 * A battery percentage was drawn in the status bar beside the framework's battery icon, and a
 * volume ring was driven from a periodic tick added to LvglDisplay.  Both worked on the bench
 * for a while and then the device began panicking inside LVGL's own layout passes, in two
 * different places from one firmware image:
 *
 *     lv_font_get_glyph_dsc <- lv_label_event <- ... <- lv_display_refr_timer
 *     get_prop_core (NULL style) <- calc_content_height <- ... <- lv_display_refr_timer
 *
 * Three attempts to make it safe each traded one fault for another:
 *
 *   1. guarding the cached label pointer and recreating it -> still faulted
 *   2. a SetTheme() override that called UpdateBatteryStatus() -> RECURSED, because SetTheme()
 *      runs on the app_main path (Application::Initialize -> UpdateStatusBar), so the device
 *      hung until the task watchdog fired, with app_main never returning
 *   3. the same override deleting the label with lv_obj_del() -> faulted inside LVGL's own
 *      teardown (lv_event_mark_deleted <- lv_obj_destructor), because a theme swap is not a
 *      place to add or remove objects
 *
 * The pattern: the additions reached into LVGL from OUTSIDE the LVGL task - the periodic tick
 * runs on the esp_timer task and the theme swap runs on app_main - and DisplayLockGuard does
 * not save that, because it is a NON-BLOCKING try-lock that silently gives up.  Work that
 * touches LVGL on a timer has to be marshalled onto the LVGL task, not locked and hoped for.
 *
 * The full implementation is parked, unbuilt, in docs/parked/ (the .volume-ui files).  It is a
 * starting point for whoever picks this up, not something to restore as-is.
 *
 * WHAT REMAINS
 * ------------
 * The battery gauge itself (b220_battery.cc) is untouched and still running: plain ADC plus a
 * discharge table, no LVGL objects, feeding Board::GetBatteryLevel() so the framework's own
 * battery icon still reflects the charge.
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
};

#endif  // B220_LCD_DISPLAY_H
