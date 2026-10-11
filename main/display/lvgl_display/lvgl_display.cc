#include <esp_err.h>
#include <esp_log.h>
#include <material_symbols.h>
#include <cstdlib>
#include <cstring>
#include <string>

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "dynamic_glyph_cache.h"
#include "jpg/image_to_jpeg.h"
#include "lvgl_display.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "Display"

/* --------------------------------------------------------------- indicators --
 * Shared by the volume ring below.
 *
 *   STATUS_TICK_MS       how often the ring watches the codec volume and how often
 *                        the battery is re-read.  Short enough that a key press
 *                        feels immediate, long enough that an idle device does
 *                        almost nothing: the tick is one comparison unless
 *                        something actually changed.
 *   VOLUME_OSD_HOLD_MS   how long the ring stays up after the last volume change.
 *   STATUS_BATTERY_TICKS how many ticks between battery refreshes (1 s at 100 ms).
 */
#define STATUS_TICK_MS 100
#define VOLUME_OSD_HOLD_MS 2000
#define STATUS_BATTERY_TICKS 10

LvglDisplay::LvglDisplay() {
    dynamic_glyph_cache_ = std::make_unique<DynamicGlyphCache>();
    // Notification timer
    esp_timer_create_args_t notification_timer_args = {
        .callback =
            [](void* arg) {
                LvglDisplay* display = static_cast<LvglDisplay*>(arg);
                DisplayLockGuard lock(display);
                lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

bool LvglDisplay::SetTextFont(std::shared_ptr<LvglFont> text_font) {
    if (text_font == nullptr || text_font->font() == nullptr) {
        return false;
    }

    DisplayLockGuard lock(this);
    auto& theme_manager = LvglThemeManager::GetInstance();
    auto light_theme = theme_manager.GetTheme("light");
    auto dark_theme = theme_manager.GetTheme("dark");
    if (light_theme == nullptr && dark_theme == nullptr) {
        return false;
    }

    // LVGL styles keep raw lv_font_t pointers. Keep the previous font owners alive until the
    // current theme has rebound every style to the new font.
    auto previous_light_font = light_theme != nullptr ? light_theme->text_font() : nullptr;
    auto previous_dark_font = dark_theme != nullptr ? dark_theme->text_font() : nullptr;
    if (light_theme != nullptr) {
        light_theme->set_text_font(text_font);
    }
    if (dark_theme != nullptr) {
        dark_theme->set_text_font(text_font);
    }
    if (current_theme_ != nullptr) {
        SetTheme(current_theme_);
    }
    previous_light_font.reset();
    previous_dark_font.reset();
    return true;
}

bool LvglDisplay::AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) {
    if (dynamic_glyph_cache_ == nullptr) {
        return false;
    }
    if (glyphs.empty()) {
        if (!TextGlyphStorageUsesPsram()) {
            ClearTextGlyphs();
        }
        return false;
    }
    if (bpp != 1 && bpp != 4) {
        return false;
    }

    DisplayLockGuard lock(this);
    if (!lock || current_theme_ == nullptr) {
        return false;
    }
    auto text_font = current_theme_->GetTextFont();
    if (text_font == nullptr) {
        return false;
    }

    auto fallback = dynamic_glyph_cache_->EnsureFont(text_font->font(), bpp);
    if (fallback == nullptr) {
        return false;
    }
    text_font->SetFallback(fallback);
    return dynamic_glyph_cache_->AddGlyphs(glyphs);
}

void LvglDisplay::ClearTextGlyphs() {
    if (dynamic_glyph_cache_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    dynamic_glyph_cache_->Clear();
}

LvglDisplay::~LvglDisplay() {
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if (low_battery_popup_ != nullptr) {
        lv_obj_del(low_battery_popup_);
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void LvglDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetStatus('%s') called before SetupUI() - message will be lost!", status);
    }
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetStatus('%s') failed: status_label_ is nullptr (SetupUI() was called but "
                     "label not created)",
                     status);
        }
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "ShowNotification('%s') called before SetupUI() - message will be lost!",
                 notification);
    }
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "ShowNotification('%s') failed: notification_label_ is nullptr (SetupUI() was "
                     "called but label not created)",
                     notification);
        }
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

void LvglDisplay::ApplyVolumeOverlayTheme() {
  auto* theme = static_cast<LvglTheme*>(current_theme_);
  if (theme == nullptr || volume_arc_ == nullptr || volume_label_ == nullptr) {
    return;
  }
  /* The status bar sits on the theme background, but the ring floats over the
   * chat area, so it is drawn in the theme's TEXT colour for contrast. */
  const lv_color_t color = theme->text_color();
  lv_obj_set_style_arc_color(volume_arc_, color, LV_PART_MAIN);
  lv_obj_set_style_arc_color(volume_arc_, color, LV_PART_INDICATOR);
  lv_obj_set_style_text_color(volume_label_, color, 0);

  const lv_font_t* font = theme->text_font() != nullptr ? theme->text_font()->font() : nullptr;
  if (font != nullptr) {
    lv_obj_set_style_text_font(volume_label_, font, 0);
    /* A background is what keeps the digits readable over whatever the
     * application is showing behind the ring. */
    lv_obj_set_style_bg_opa(volume_label_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(volume_label_, theme->background_color(), 0);
    lv_obj_set_style_radius(volume_label_, (font->line_height + theme->spacing(2)) / 2, 0);
    lv_obj_set_style_pad_hor(volume_label_, theme->spacing(2), 0);
    lv_obj_set_style_pad_ver(volume_label_, 0, 0);
  }
}


/*
 * The ring lives on a SCREEN OF ITS OWN (lv_obj_create(nullptr) makes a screen that
 * is not loaded), never on the application's screen.  Three reasons:
 *
 *   1. SetupUI() may be called more than once (theme/asset reload), and a screen
 *      that is never loaded cannot be deleted with the widgets it holds.
 *   2. The app screen is rearranged by the layouts around it; an overlay drawn on
 *      top of the chat area would be clipped or scrolled by them.
 *   3. Hiding the ring is then one lv_screen_load() back to the app screen, with no
 *      z-order or hidden-flag bookkeeping, and the ring keeps its own widget tree
 *      across every screen the application shows.
 *
 * The screen is transparent and its background is fully click-through, so the only
 * pixels it owns are the ring and its label.  Nothing else in this class draws on
 * this screen, so the widgets are created once and only their values change.
 */
void LvglDisplay::BuildVolumeOverlay() {
  volume_osd_ = lv_obj_create(nullptr);
  if (volume_osd_ == nullptr) {
    return;
  }
  lv_obj_set_style_bg_opa(volume_osd_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(volume_osd_, 0, 0);
  lv_obj_set_style_pad_all(volume_osd_, 0, 0);
  lv_obj_remove_flag(volume_osd_, LV_OBJ_FLAG_SCROLLABLE);
  /* Non-touch hardware: the overlay must never swallow an input event. */
  lv_obj_remove_flag(volume_osd_, LV_OBJ_FLAG_CLICKABLE);

  /* A ring that fits the smaller dimension of the screen, with a stroke thick
   * enough to read at arm's length on a 160x128 panel. */
  const int size = LV_MIN(width_, height_) * 2 / 3;
  const int ring_width = LV_MAX(size / 12, 3);
  volume_arc_ = lv_arc_create(volume_osd_);
  lv_obj_set_size(volume_arc_, size, size);
  lv_obj_center(volume_arc_);
  lv_arc_set_rotation(volume_arc_, 0);
  lv_arc_set_bg_angles(volume_arc_, 0, 360);
  lv_arc_set_range(volume_arc_, 0, 100);
  lv_arc_set_value(volume_arc_, 0);
  /* The ring is HOLLOW: the base arc is the track (the "empty" part, drawn
   * dimmed) and the indicator is the filled part drawn on top of it, so what is
   * missing from the circle IS the missing volume.
   *
   * The value range is 0..1000 rather than 0..100 - i.e. a tenth of a percent per
   * step - because of an LVGL quirk at exactly 100%: a full circle arrives at the
   * renderer as start_angle == end_angle (lv_arc_set_angles() subtracts 360 from
   * an end angle of 360, and lv_draw_sw_arc() treats two equal angles as an empty
   * arc).  Value 996 maps to 358 degrees, which truncates to 358: a full-looking
   * ring with a two-degree notch that is under a pixel wide at this size.  The
   * alternative - no indicator at all at 100% - would look identical here but
   * would make "100%" a special case that never draws. */
  lv_arc_set_range(volume_arc_, 0, 1000);
  lv_arc_set_value(volume_arc_, 0);
  lv_obj_set_style_bg_opa(volume_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(volume_arc_, ring_width, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(volume_arc_, LV_OPA_40, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(volume_arc_, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(volume_arc_, ring_width, LV_PART_INDICATOR);
  /* No knob: this is a display, not a slider. */
  lv_obj_set_style_bg_opa(volume_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(volume_arc_, 0, LV_PART_KNOB);
  lv_obj_remove_flag(volume_arc_, LV_OBJ_FLAG_CLICKABLE);

  volume_label_ = lv_label_create(volume_osd_);
  lv_obj_set_style_text_align(volume_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(volume_label_, "");
  lv_obj_center(volume_label_);

  ApplyVolumeOverlayTheme();
}


void LvglDisplay::HideVolumeOverlay() {
  DisplayLockGuard lock(this);
  if (!lock) {
    return;
  }
  HideVolumeOverlayLocked();
}


void LvglDisplay::HideVolumeOverlayLocked() {
  if (volume_osd_shown_ < 0) {
    return;
  }
  /* Back to the screen the application is using.  lv_display_get_screen_active()
   * returns THIS display's screen, which is not necessarily lv_screen_active()'s
   * when more than one display is registered. */
  lv_display_t* disp = display_ != nullptr ? display_ : lv_display_get_default();
  lv_obj_t* active = lv_display_get_screen_active(disp);
  if (active != nullptr && active != volume_osd_) {
    lv_screen_load(active);
  }
  volume_osd_shown_ = -1;
  volume_osd_hide_deadline_us_ = 0;
}


/*
 * The periodic tick exists because the two indicators this class owns have no
 * event of their own to hang off:
 *
 *   - Volume reaches the codec by several routes (the board's own keys, the
 *     `self.audio_speaker.set_volume` MCP tool, a future touch surface).  Watching
 *     codec->output_volume() here catches all of them at once, instead of asking
 *     every one of those call sites to remember to update the UI.  The value is
 *     read WITHOUT the display lock: it is a plain int on the codec.
 *   - The battery is sampled by the board in the background, so it changes with no
 *     application event at all.  UpdateStatusBar() alone would leave a stale
 *     percentage on screen for minutes at a time.
 *
 * The lock is only taken when there is something to draw, so an idle device pays
 * one integer comparison per tick.
 */
void LvglDisplay::OnStatusTimer() {
  if (!setup_ui_called_) {
    return;
  }

  int volume = -1;
  auto codec = Board::GetInstance().GetAudioCodec();
  if (codec != nullptr) {
    volume = codec->output_volume();
  }

  const int64_t now_us = esp_timer_get_time();
  const bool volume_changed = (volume != volume_osd_reported_);
  const bool hide_due = (volume_osd_shown_ >= 0) && (now_us >= volume_osd_hide_deadline_us_);
  const bool battery_due = (++status_battery_tick_ >= STATUS_BATTERY_TICKS);

  if (!volume_changed && !hide_due && !battery_due) {
    return;
  }
  if (battery_due) {
    status_battery_tick_ = 0;
  }

  DisplayLockGuard lock(this);
  if (!lock) {
    return;
  }

  if (volume_changed) {
    volume_osd_reported_ = volume;
    if (volume >= 0) {
      UpdateVolumeOverlay(volume);
      volume_osd_hide_deadline_us_ = esp_timer_get_time() + (int64_t)VOLUME_OSD_HOLD_MS * 1000;
    }
  }
  if ((volume_osd_shown_ >= 0) && (esp_timer_get_time() >= volume_osd_hide_deadline_us_)) {
    HideVolumeOverlayLocked();
  }

  if (battery_due) {
    UpdateBatteryStatus();
  }
}


void LvglDisplay::ShowVolumeOverlay(int volume) {
  DisplayLockGuard lock(this);
  if (!lock || !setup_ui_called_) {
    return;
  }
  if (volume < 0) {
    HideVolumeOverlayLocked();
    return;
  }
  volume_osd_reported_ = volume;
  UpdateVolumeOverlay(volume);
  volume_osd_hide_deadline_us_ = esp_timer_get_time() + (int64_t)VOLUME_OSD_HOLD_MS * 1000;
}


void LvglDisplay::StartStatusTimer() {
  if (status_timer_ != nullptr) {
    return;
  }
  esp_timer_create_args_t args = {
      .callback = [](void* arg) { static_cast<LvglDisplay*>(arg)->OnStatusTimer(); },
      .arg = this,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "display_status",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&args, &status_timer_) != ESP_OK) {
    status_timer_ = nullptr;
    ESP_LOGW(TAG, "status timer unavailable: no volume ring, no periodic battery refresh");
    return;
  }
  ESP_ERROR_CHECK(esp_timer_start_periodic(status_timer_, STATUS_TICK_MS * 1000));
}


void LvglDisplay::UpdateBatteryStatus() {
  auto& app = Application::GetInstance();
  auto& board = Board::GetInstance();

  // Update battery icon
  int battery_level;
  bool charging, discharging;
  const char* icon = nullptr;
  if (board.GetBatteryLevel(battery_level, charging, discharging)) {
    if (charging) {
      icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT;
    } else {
      const char* levels[] = {
          MATERIAL_SYMBOLS_BATTERY_ANDROID_0,       MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
          MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
          MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
          MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL,
      };
      int level_index =
          battery_level <= 0 ? 0 : (battery_level >= 100 ? 7 : 1 + ((battery_level - 1) * 6 / 99));
      icon = levels[level_index];
    }
    DisplayLockGuard lock(this);
    if (battery_label_ != nullptr && battery_icon_ != icon) {
      battery_icon_ = icon;
      lv_label_set_text(battery_label_, battery_icon_);
    }

    // Check low battery popup only when clock tick event is triggered
    // Because when initializing, the battery level is not ready yet.
    if (low_battery_popup_ != nullptr && battery_allow_popup_) {
      if (strcmp(icon, MATERIAL_SYMBOLS_BATTERY_ANDROID_0) == 0 && discharging) {
        if (lv_obj_has_flag(low_battery_popup_,
                            LV_OBJ_FLAG_HIDDEN)) {  // Show if low battery popup is hidden
          lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
          app.Schedule([&app]() { app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY); });
        }
      } else {
        // Hide the low battery popup when the battery is not empty
        if (!lv_obj_has_flag(low_battery_popup_,
                             LV_OBJ_FLAG_HIDDEN)) {  // Hide if low battery popup is shown
          lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        }
      }
    }
  }
}


void LvglDisplay::UpdateVolumeOverlay(int volume) {
  if (volume_osd_ == nullptr) {
    BuildVolumeOverlay();
    if (volume_osd_ == nullptr) {
      return;
    }
  }
  if (volume < 0) {
    volume = 0;
  }
  if (volume > 100) {
    volume = 100;
  }

  char text[12];
  lv_snprintf(text, sizeof(text), "%d%%", volume);
  lv_label_set_text(volume_label_, text);
  /* The full ring is 100%: 996 of the arc's 0..1000 range spans 358 of the 360
   * degrees (see BuildVolumeOverlay() for why not 360), and the indicator grows
   * clockwise from the top because bg_angle_start is 0 and the arc's rotation is
   * 0. */
  lv_arc_set_value(volume_arc_, volume * 996 / 100);

  /* The label's size follows its text, so re-centre it on the ring. */
  lv_obj_center(volume_label_);
  lv_screen_load(volume_osd_);
  volume_osd_shown_ = volume;
}


void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    /*
     * First status refresh means SetupUI() has run, so LVGL is up and the periodic tick can
     * start.  Doing it here keeps the ring and the battery refresh working for every display
     * built on this class, OLED and LCD alike.
     */
    StartStatusTimer();
    if (!setup_ui_called_ || codec == nullptr) {
        return;
    }

    // Update mute icon
    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        // Update icon if mute state changes
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, MATERIAL_SYMBOLS_VOLUME_OFF);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // Update time — show HH:MM when idle; refresh immediately on state
    // transitions (update_all=true) and on every minute change thereafter.
    if (app.GetDeviceState() == kDeviceStateIdle) {
        time_t now = time(NULL);
        struct tm* tm_now = localtime(&now);
        if (tm_now->tm_year >= 2025 - 1900) {
            int cur_min = tm_now->tm_hour * 60 + tm_now->tm_min;
            if (update_all || cur_min != last_displayed_clock_min_) {
                last_displayed_clock_min_ = cur_min;
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M", tm_now);
                SetStatus(time_str);
            }
        } else {
            ESP_LOGW(TAG, "System time not set (tm_year=%d)", tm_now->tm_year);
        }
    } else {
        // Reset so the clock re-appears immediately when idle resumes.
        last_displayed_clock_min_ = -1;
    }

    esp_pm_lock_acquire(pm_lock_);
    /*
     * The low-battery popup is tied to the clock tick, not to every status refresh:
     * UpdateStatusBar() is called on state transitions too, and the battery level is not
     * ready during start-up.  The periodic tick that also calls UpdateBatteryStatus() must
     * never raise the popup, so the permission is only granted here.
     */
    battery_allow_popup_ = !update_all;
    UpdateBatteryStatus();
    battery_allow_popup_ = false;

    // Update network icon every 10 seconds
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // Don't read 4G network status during firmware upgrade to avoid occupying UART resources
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,      kDeviceStateStarting,   kDeviceStateWifiConfiguring,
            kDeviceStateListening, kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) !=
            allowed_states.end()) {
            const char* network_state_icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && network_state_icon != nullptr &&
                network_icon_ != network_state_icon) {
                DisplayLockGuard lock(this);
                network_icon_ = network_state_icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

    esp_pm_lock_release(pm_lock_);
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {}

void LvglDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to take snapshot, draw_buffer is nullptr");
        return false;
    }

    // swap bytes
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);
    }

    // Clear output string and use callback version to avoid pre-allocating large memory blocks
    jpeg_data.clear();

    // Use callback-based JPEG encoder to further save memory
    bool ret =
        image_to_jpeg_cb((uint8_t*)draw_buffer->data, draw_buffer->data_size, draw_buffer->header.w,
                         draw_buffer->header.h, V4L2_PIX_FMT_RGB565, quality,
                         [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                             std::string* output = static_cast<std::string*>(arg);
                             if (data && len > 0) {
                                 output->append(static_cast<const char*>(data), len);
                             }
                             return len;
                         },
                         &jpeg_data);
    if (!ret) {
        ESP_LOGE(TAG, "Failed to convert image to JPEG");
    }

    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    ESP_LOGE(TAG, "LV_USE_SNAPSHOT is not enabled");
    return false;
#endif
}
