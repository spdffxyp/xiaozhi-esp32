# 小魔豆 B220 (XiaoMoDou B220)

ESP32 玩具机器人「小魔豆 B220」的小智（xiaozhi-esp32）移植板级。

## 硬件

| 项目 | 值 | 来源 |
| --- | --- | --- |
| 主控 | ESP32-D0WD rev1.0（双核 LX6，240 MHz） | esptool / 原厂引导日志 |
| Flash | 16 MB — GigaDevice GD25Q128（JEDEC `c8 6018`） | esptool `flash_id` |
| PSRAM | 8 MB SPI PSRAM @ 80 MHz | 原厂固件启用了 `spiram_psram.c` |
| 显示 | **128×160 SPI 面板**，SPI2 @ 10 MHz（原分析按初始化表的窗口误判为 ILI9341 240×320；实测物理是 128×160，几何/色序已实测确认） | 实测 |
| 背光 | **AW9106B（I2C 0x5B）DIM0/DIM1/DIM2 = 0x3F**，不是 GPIO | 逆向 + 实测 |
| 音频 | **ZL38063 AcuEdge DSP**（3 麦阵列 → DSP → I2S → ESP32；放音反方向） | 原厂驱动 `vproc_common.c` / `zl38063` |
| I2C | AW9106B 灯驱 @0x5B、SC7A20 加速度计 @0x19（SDA 18 / SCL 23） | 原厂驱动 |
| TF 卡 | SDMMC slot 1（CLK 14 / CMD 15 / D0 2） | 原厂驱动 |
| 底座 | 电机 / 头尾 LED 在底座板，走 UART `{AT+...}` | 原厂 `UartControlService` |

## 引脚（全部由原厂固件逆向得到）

| 功能 | GPIO |
| --- | --- |
| LCD SCLK / MOSI / MISO | 32 / 33 / 34 |
| LCD CS / DC / RST | 27 / 21 / 22 |
| I2S BCLK / WS / DOUT / DIN（接 ZL38063） | 5 / 25 / 26 / 35 |
| ZL38063 RESET | 19 |
| ZL38063 HBI 片选（与 LCD 共用 SPI2） | **0** |
| I2C SDA / SCL | 18 / 23 |
| TF 卡 | 14 / 15 / 2 / 4 / 12 / 13 |

> ⚠️ GPIO0 在原机上兼作 ZL38063 的 HBI 片选，同时是 **ESP32 启动 strapping 脚**。
> 玩具自己的 USB 桥会在开机瞬间把它拉低，所以**插着 micro-usb 开机会直接进下载模式**；
> 要运行固件请拔掉 micro-usb（用电池供电）。

> ⚠️ 这块板子上**没有任何 ESP32 可见的按键**：启动键是底座上的软开关（按一下关机/再按开机），
> 固件读不到它。所以小智靠"无凭据时自动进配网模式"启动，后续靠唤醒词交互。

## 构建

```sh
. /path/to/esp-idf/export.sh          # 需要 ESP-IDF >= 6.0.1（本机 v6.1）
python scripts/build.py xiaomodou-b220 # 或：python scripts/build.py main/boards/xiaomodou-b220
```

`config.json` 覆盖了三处与参考面包板不同的设置：**16 MB flash**、
**`partitions/v2/16m.csv`**、**`CONFIG_SPIRAM=y`**。

## 当前状态

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| Phase 2 | 板级骨架：LCD 出图 + **背光** + WiFi 配网 | ✅ **已完成**（音频暂用 `DummyAudioCodec`） |
| Phase 3 | ZL38063 音频：复位/握手 + 灌入 1042 字节配置记录 + 全双工 I2S | 待做 |
| Phase 4 | 增值：底座 AT 协议（电机/头尾灯）、AW9106 指示通道、SC7A20、TF 卡 | 待做 |

**背光不是 GPIO**：原厂固件从不驱动任何背光引脚，而是初始化 I2C 上的 **AW9106B（0x5B）**
并把 **DIM0/DIM1/DIM2 写到 0x3F（最大）**——这是全镜像里唯一持续非零的 LED 通道写入。
本板级在 `InitializeBacklight()` 里原样回放该序列（DIM3/DIM4 = 0x80 作指示灯）。
（另：LEDC/PWM 在该固件里完全不存在，所以没有调光能力。）

## 已知待测项

- **调优（本轮已加）**：CPU `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`、PSRAM `CONFIG_SPIRAM_SPEED_80M`。
- **SPI 时钟**：10 MHz 与原厂一致，稳定后可上调（LCD 与 DSP 在同一总线可各自设时钟）。
- **按键**：无（见上文）。
- **原始 `draw_bitmap` 通路需要手动字节交换**：LVGL 那条路由 `SpiLcdDisplay` 的 `swap_bytes=1` 处理，
  但**裸 `esp_lcd_panel_draw_bitmap()` 的缓冲必须自己 `__builtin_bswap16()`**，否则红蓝互换。
  （自检用的方向测试卡就踩过这个坑，代码里已修正并注明。）
