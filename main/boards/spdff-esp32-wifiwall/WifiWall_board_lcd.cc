#include "dual_network_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include "sleep_timer.h"
#include "adc_battery_monitor.h"
#include <esp_sleep.h>
#include <driver/gpio.h>

#define TAG "ESP32-WifiWall"

LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_14_1);

class CompactWifiBoardLCD : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    Button asr_button_;

    LcdDisplay* display_;
    SleepTimer* sleep_timer_ = nullptr;
    AdcBatteryMonitor* adc_battery_monitor_ = nullptr;

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeBatteryMonitor() {
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, BATTERY_VOLTAGE_PIN, BATTERY_UPPER_R, BATTERY_LOWER_R, CHARGING_PIN);
        adc_battery_monitor_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                sleep_timer_->SetEnabled(false);
            } else {
                sleep_timer_->SetEnabled(true);
            }
        });
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    void InitializePowerSaveTimer() {
#if CONFIG_USE_ESP_WAKE_WORD
        sleep_timer_ = new SleepTimer(300);
#else
        sleep_timer_ = new SleepTimer(30);
#endif
        sleep_timer_->OnEnterLightSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            // Show the standby screen
            GetDisplay()->SetPowerSaveMode(true);
            // Enable sleep mode, and sleep in 1 second after DTR is set to high
            // modem_->SetSleepMode(true, 1);
            // Set the DTR pin to high to make the modem enter sleep mode
            // modem_->GetAtUart()->SetDtrPin(true);
            GetBacklight()->SetBrightness(10);
        });
        sleep_timer_->OnExitLightSleepMode([this]() {
            // Set the DTR pin to low to make the modem wake up
            // modem_->GetAtUart()->SetDtrPin(false);
            // Hide the standby screen
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        sleep_timer_->SetEnabled(true);
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 20 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
 

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }


 
    void InitializeButtons() {

        // 配置 GPIO
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << BUILTIN_LED_GPIO,  // 设置需要配置的 GPIO 引脚
            .mode = GPIO_MODE_OUTPUT,           // 设置为输出模式
            .pull_up_en = GPIO_PULLUP_DISABLE,  // 禁用上拉
            .pull_down_en = GPIO_PULLDOWN_DISABLE,  // 禁用下拉
            .intr_type = GPIO_INTR_DISABLE      // 禁用中断
        };
        gpio_config(&io_conf);  // 应用配置

        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                ESP_LOGI(TAG, "Boot button pressed, enter WiFi configuration mode");
                EnterWifiConfigMode();
                return;
            }
            gpio_set_level(BUILTIN_LED_GPIO, 1);
            app.ToggleChatState();
        });

        asr_button_.OnClick([this]() {
            std::string wake_word="你好小智";
            Application::GetInstance().WakeWordInvoke(wake_word);
        });

        touch_button_.OnPressDown([this]() {
            gpio_set_level(BUILTIN_LED_GPIO, 1);
            Application::GetInstance().StartListening();
        });

        touch_button_.OnPressUp([this]() {
            gpio_set_level(BUILTIN_LED_GPIO, 0);
            Application::GetInstance().StopListening();
        });

    }

    void InitializeWakeupSources() {
        ESP_LOGI(TAG, "Configuring GPIO wakeup sources...");

        // **非常重要**: 确保RTC外设域在睡眠期间保持供电，GPIO唤醒需要它
        ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));
    
        // 将所有可能用于唤醒的按钮引脚组合到一个 bitmask 中
        // const uint64_t wakeup_pin_mask = (1ULL << BOOT_BUTTON_GPIO) |
        //                                  (1ULL << TOUCH_BUTTON_GPIO) |
        //                                  (1ULL << ASR_BUTTON_GPIO);
        const uint64_t wakeup_pin_mask = (1ULL << ASR_BUTTON_GPIO);

        // 配置GPIO从低电平唤醒
        // 这假设您的按钮在按下时会产生一个低电平信号（例如，通过上拉电阻接到VCC，按下时接地）
        // 如果您的按钮逻辑相反（按下为高电平），请使用 ESP_GPIO_WAKEUP_HIGH
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(wakeup_pin_mask, ESP_EXT1_WAKEUP_ALL_LOW));

        ESP_LOGI(TAG, "GPIO wakeup enabled for pins: BOOT, TOUCH, ASR");
    }

public:
    CompactWifiBoardLCD() :
        boot_button_(BOOT_BUTTON_GPIO), touch_button_(TOUCH_BUTTON_GPIO), asr_button_(ASR_BUTTON_GPIO) {
        InitializeBatteryMonitor();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeWakeupSources();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
        
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        ESP_LOGI(TAG, "battery level is: %d", level);
        return true;
    }

    // virtual void SetPowerSaveMode(bool enabled) override {
    //     if (!enabled) {
    //         sleep_timer_->WakeUp();
    //     }
    // }
};

DECLARE_BOARD(CompactWifiBoardLCD);
