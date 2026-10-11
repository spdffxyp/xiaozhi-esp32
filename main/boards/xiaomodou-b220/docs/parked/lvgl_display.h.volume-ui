#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <chrono>
#include <memory>
#include <string>

#include "display.h"
#include "lvgl_image.h"

class DynamicGlyphCache;
class LvglFont;

class LvglDisplay : public Display {
public:
    LvglDisplay();
    virtual ~LvglDisplay();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    /**
     * Refresh the battery part of the status bar.
     *
     * Split out of UpdateStatusBar() so a board can render the battery its own way
     * (a percentage instead of the generic icon, say) without having to reimplement
     * the clock, the mute icon or the network icon.  Called from UpdateStatusBar()
     * and from this class's own periodic tick.
     */
    virtual void UpdateBatteryStatus() override;
    virtual void SetPowerSaveMode(bool on);
    /**
     * Show the volume as a hollow ring (the full ring is 100%) for a moment.
     *
     * @param volume 0..100, clamped internally.  A negative value hides the ring at
     *               once.
     *
     * Safe to call from any task: the ring is drawn by the display's own update
     * timer, so this only records the request.
     */
    virtual void ShowVolumeOverlay(int volume) override;
    /** Hide the volume ring immediately. */
    virtual void HideVolumeOverlay() override;
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80) override;
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) override;
    virtual void ClearTextGlyphs() override;
    virtual bool SetTextFont(std::shared_ptr<LvglFont> text_font) override;
    virtual bool SupportsGuiOperations() const override { return true; }

protected:
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t* display_ = nullptr;

    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* mute_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;

    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;
    /* Only UpdateStatusBar(update_all == false) - the clock tick - may raise the
     * low-battery popup.  Set for the duration of its UpdateBatteryStatus() call. */
    bool battery_allow_popup_ = false;

    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;
    int last_displayed_clock_min_ = -1;  // -1 forces update on first idle tick
    std::unique_ptr<DynamicGlyphCache> dynamic_glyph_cache_;

    /* Periodic tick owned by this class: it drives the volume ring (which every
     * volume path reaches, including the MCP tool and the hardware keys) and keeps
     * the battery in the status bar fresh without waiting for a state transition.
     *
     * Started from the first UpdateStatusBar() call rather than from SetupUI():
     * UpdateStatusBar() is only ever reached after the UI exists, and it is reached
     * by every display built on this class, while the two SetupUI() implementations
     * live in the LCD and OLED subclasses.  Nothing is drawn before the first call,
     * so no indicator is missed. */
    esp_timer_handle_t status_timer_ = nullptr;

    /* --- volume ring ------------------------------------------------------- */
    lv_obj_t* volume_osd_ = nullptr;           // dedicated screen, hidden when idle
    lv_obj_t* volume_arc_ = nullptr;           // the hollow ring itself
    lv_obj_t* volume_label_ = nullptr;         // "NN%" in the middle of the ring
    int volume_osd_shown_ = -1;                // last drawn volume, -1 = hidden
    int volume_osd_reported_ = -1;             // last volume seen from the codec
    int64_t volume_osd_hide_deadline_us_ = 0;  // when to hide, 0 = no pending hide
    int status_battery_tick_ = 0;              // ticks left before the next battery refresh

    void StartStatusTimer();
    void OnStatusTimer();
    void BuildVolumeOverlay();
    void ApplyVolumeOverlayTheme();
    void UpdateVolumeOverlay(int volume);
    void HideVolumeOverlayLocked();

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

#endif
