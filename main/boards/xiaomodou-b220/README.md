# 小魔豆 B220 (XiaoMoDou B220)

ESP32 玩具机器人「小魔豆 B220」的小智（xiaozhi-esp32）移植板级。

## 硬件

| 项目 | 值 | 来源 |
| --- | --- | --- |
| 主控 | ESP32-D0WD rev1.0（双核 LX6，240 MHz） | esptool / 原厂引导日志 |
| Flash | 16 MB — GigaDevice GD25Q128（JEDEC `c8 6018`） | esptool `flash_id` |
| PSRAM | 8 MB SPI PSRAM @ 80 MHz | 原厂固件启用了 `spiram_psram.c` |
| 显示 | **128×160 SPI 面板，已旋转 90°（逻辑 160×128）**，SPI2 @ 10 MHz（原分析按初始化表的窗口误判为 ILI9341 240×320；实测物理是 128×160，几何/色序已实测确认） | 实测 |
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
| **闲置（可做按键输入）** | **20 / 36 / 37 / 38 / 39**（其中只有 20 能当输出） |

> ⚠️ GPIO0 在原机上兼作 ZL38063 的 HBI 片选，同时是 **ESP32 启动 strapping 脚**。
> 玩具自己的 USB 桥会在开机瞬间把它拉低，所以**插着 micro-usb 开机会直接进下载模式**；
> 要运行固件请拔掉 micro-usb（用电池供电）。

> ⚠️ 启动键是底座上的软开关（按一下关机/再按开机），**ESP32 读不到它**；配网靠"无凭据时
> 自动进配网模式"，交互靠唤醒词。录音/音量± 三个功能键的接线见下文「按键」。

## 屏幕旋转 90°

`config.h` 里一个开关决定方向：

```c
#define B220_DISPLAY_ROTATE_90 1     /* 1 = 橫向 160×128；0 = 原厂竖屏 128×160 */
```

- 物理玻璃固定 **128×160**（`B220_DISPLAY_PANEL_WIDTH/HEIGHT`）；`DISPLAY_WIDTH/HEIGHT`
  是 **LVGL 逻辑画布**，由开关自动推导（旋转时是 160×128），所以两者不可能对不上。
- 旋转是通过 **MADCTL（0x36）的真实 `swap_xy` + 镜像组合**实现的（`esp_lcd_panel_swap_xy` /
  `esp_lcd_panel_mirror`），不是 LVGL 软旋转，因此**不占额外 PSRAM、不掉帧**。
  lvgl_port 停留在 `LV_DISPLAY_ROTATION_0`，它在 flush 时直接使用面板坐标。
- **方向反了就同时翻转 `DISPLAY_MIRROR_X` 与 `DISPLAY_MIRROR_Y`**（只改这两个，`SWAP_XY` 不动），
  那是"反向旋转 90°"，不会引入镜像。
- 开机日志会打印几何自检：
  `LCD geometry: glass 128x160 -> LVGL logical 160x128 (swap_xy=1 mirror_x=0 mirror_y=1, rotate90=1)`。
- 想用方向测试卡确认，把 `B220_ORIENT_CARD` 置 1：旋转后那 8 条色带会变成**横向色带**。

## 按键（录音 / 音量+ / 音量−）：经 ZL38063 扩展（**已上机确认**）

这三个键**不接在 ESP32 的 GPIO 上**，也**不走 AW9106B**：原厂 `buttonTask`（`0x40124d4c`）
从 ZL38063 的两个 HBI 寄存器取值，走的正是本移植给 DSP 用的那条 SPI 链路。
第一次上机实测（诊断构建）完全对上：

| 寄存器 | 空闲 | 按下 | 抬起 | 含义 |
| --- | --- | --- | --- | --- |
| HBI `0x0000` | `0x0000` | **`0x0003`** | `0x0002` | **状态**：bit0 = 1 才是"按下" |
| HBI `0x0002` | 无意义 | `0x0008` / `0x000A` / `0x000B` | 无意义 | **按键码**：音量+ / 音量− / 录音 |

另有一根**前端中断线 GPIO38**（空闲高、按下时下降沿），中断只决定"什么时候去读"，
寄存器决定"读到了什么"；轮询也独立跑，所以中断不工作时按键照样正确。

**这是事件式接口，不是电平式**：一次物理按压会产生**两个事件**（按下 `0x0003`、抬起 `0x0002`），
`0x0002` / `0x0008-0x000B` **不能**当作"一直按着"来读。驱动只对"按下"事件动作，
抬起只用来关闭长按窗口 —— 这正是之前"按下和松开都触发功能"的根因。

原厂节拍：`gpioclear` = 10 ms 轮询、`gpiolongpress` = 2000 ms 长按。

> ✅ **与同硬件开源项目逐条核对过**：本机实测的寄存器/键值映射，与
> [FlowerCat-chen/Flower_modou](https://github.com/FlowerCat-chen/Flower_modou)
> 的 `components/modou_drivers/modou_dsp_keys.c` **完全一致**
> （`0x0000`=status、`0x0002`=key、`0x08`=VOL+、`0x0a`=VOL−、`0x0b`=MIC、
> `status & 1` 才算按下、`0x0002` 抬起不作动作）。对照报告见
> [docs/KEY-VERIFICATION-vs-Flower_modou.md](docs/KEY-VERIFICATION-vs-Flower_modou.md)。

**正式驱动**：[b220_buttons.c](b220_buttons.c) / [b220_buttons.h](b220_buttons.h) —— 10 ms 轮询
`zl38063_hbi_read(0x0000/0x0002)`，短按/长按分类，录音键走 `ToggleChatState()`，音量键走
`SetOutputVolume()`。**只读**：不写 DSP `reg 0x0014`、不复位、不重灌固件。

**还差一步**：把"哪个码 = 哪个键"量出来。这**无法从日志推断**（三个码是同时看到的），
必须**一次只按一个键**。为此板级带一套只读定位固件（`B220_BUTTON_DISCOVERY = 1`，见
[docs/BUTTON-DISCOVERY.md](docs/BUTTON-DISCOVERY.md)），它还会同时采样 ESP32 空闲脚
（20/36/38/39）、AW9106B 输入寄存器、SC7A20 与整个 HBI 地址空间，用来做事后排除。

三个码填进 `config.h` 之后把 `B220_BUTTON_DISCOVERY` 置 0 即可（两者互斥）：

```c
#define B220_BUTTON_CODE_1  0x000B   /* 录音键（示例值，按实测填） */
#define B220_BUTTON_CODE_2  0x0008   /* 音量+ */
#define B220_BUTTON_CODE_3  0x000A   /* 音量− */
#define B220_BUTTON_POLL_MS        10
#define B220_BUTTON_LONGPRESS_MS   2000
```

> ⚠️ 映射没填（三个宏 = 0）时驱动**不会误动作**：每次按压只打印原始 key 值并提示。
>
> ⚠️ **音量键目前只是"存值"**：`Zl38063AudioCodec::SetOutputVolume()` 只写 NVS + UI 指示，
> 真正的增益在 DSP 内部（`reg 0x0238/0x023A`），本移植还没有验证过的运行期写法（PART 3 §1.6）。
> 要让音量真的变化需要单独确认 `zl38063_dsp_set_volume()` 的档位表 —— 与按键是两件事。
>
> ✅ 每次按键动作都会让**板载指示灯闪两下**（见下面「指示灯」一节），这是没有屏幕时唯一的反馈。

## 指示灯（AW9106B 0x5B）—— 本轮新增

板载指示灯不是 GPIO，是 AW9106B（I2C `0x5B`）的恒流通道。**只驱动 `0x23`/`0x24` 两路**
（工厂回放把它们留在 `0x80`，之前没有任何代码再碰过）；待机熄灭，按键动作闪两下。

```c
#include "b220_led.h"
b220_led_blink_once();                       /* 两下短闪，非阻塞 */
b220_led_set_indicator(B220_LED_INDICATOR_A, 0x3F);
b220_led_set_both(0x00);                     /* 全灭 */
b220_led_set_backlight(0x20);                /* 调背光（0x20/0x21/0x22） */
```

> ⚠️ `0x20/0x21/0x22` 在本机是**LCD 背光**（写 `0x3F` 屏才亮），不是彩灯 ——
> 开源参考项目把它们当顶部 RGB 灯用了（那是它的机器），**本移植没有照抄这个映射**。
> 本机是否另有顶部 RGB、以及 `0x23`/`0x24` 各是哪颗灯，需要上机点灯走查。
> 细节与"未照抄的三条初始化写法"见 [docs/B220-LED-AND-BASE-UART.md](docs/B220-LED-AND-BASE-UART.md) §1。

> ⚠️ **绝不动 `0x02/0x04/0x12`**：那是 AW8737S 功放 SHDN 的所有权寄存器，
> 一个 boot 只允许一个上升沿（PART 2 §9）。`b220_led.c` 只写 `0x20..0x25`。

## 底座通信（UART0：电机 / 头尾灯）—— 本轮新增

底座 MCU 挂在 **UART0（TX GPIO1 / RX GPIO3，115200 8N1）**，也就是**和日志、烧录口同一根线**。
帧格式 `{AT+...}\r\n`。开机延迟 3 s 自动发一次 `AT+Pon`（解锁电机）。

```c
#include "b220_base_uart.h"
b220_base_uart_send("AT+Ver", 120);                  /* 只发不等回包 */
b220_base_motion(3, 90, 40, 600);                    /* 前进：mode 3，直行 90，动力 40，600 ms */
b220_base_stop();                                    /* 两条停止帧 */
b220_base_lights(1, 1);                              /* 头/尾灯（色号等实测） */
```

> ⚠️ 因为共用一根线：我们发的 `{AT+...}` 会出现在日志里（正常）；我们的日志也会进底座，
> 底座会忽略不认识的文本。默认**只发不收**（不抢控制台输入）；要读回包用
> `b220_base_uart_init(true)` + `b220_base_uart_send_and_receive()`。
>
> 运动模式（`AT+CMD=(9,1,0,1),(mode,units,steer,power,0),...`）：`0/1` 原地左右转、
> `2/3` 后退/前进、`4/5` 右弯；`steer` 0..180（90 直行）、`units` 按 **200 ms** 向上取整、
> `power` 1..100。命令表与"仍需实测"清单见
> [docs/B220-LED-AND-BASE-UART.md](docs/B220-LED-AND-BASE-UART.md) §2。

## 构建

```sh
. /path/to/esp-idf/export.sh          # 需要 ESP-IDF >= 6.0.1（本机 v6.1）
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi
```

`config.json` 覆盖了与参考面包板不同的若干项：**16 MB flash**、**`partitions/v2/16m.csv`**、
**`CONFIG_SPIRAM=y`**、**240 MHz**、以及三行唤醒词配置（`CONFIG_USE_ESP_WAKE_WORD=y` /
`CONFIG_USE_AFE_WAKE_WORD=n` / `CONFIG_WAKE_WORD_DISABLED=n`）。

## 当前状态

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| Phase 2 | 板级骨架：LCD 出图 + **背光** + WiFi 配网 | ✅ 已完成 |
| Phase 3 | ZL38063 音频：I2S 先起 + 复位/握手 + 3699 条固件记录 + 1042 对配置 + release | ✅ 已完成 |
| Phase 4 | 生产 codec（48 kHz 借用 I2S0）+ 功放单沿使能 + 唤醒词 + 云端会话 | ✅ 已完成 |
| Phase 5 | **屏幕旋转 90°** | ✅ 已完成 |
| Phase 6 | **三个按键（经 ZL38063）**：定位完成、驱动已写好并接入 | 🔶 只差"哪个码 = 哪个键"（一次只按一个键即可量出） |

**背光不是 GPIO**：原厂固件从不驱动任何背光引脚，而是初始化 I2C 上的 **AW9106B（0x5B）**
并把 **DIM0/DIM1/DIM2 写到 0x3F（最大）**——这是全镜像里唯一持续非零的 LED 通道写入。
本板级在 `InitializeBacklight()` 里原样回放该序列（DIM3/DIM4 = 0x80 作指示灯）。
（另：LEDC/PWM 在该固件里完全不存在，所以没有调光能力。）

## 已知待测项

- **调优**：CPU `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`、PSRAM `CONFIG_SPIRAM_SPEED_80M` 均已生效。
- **SPI 时钟**：10 MHz 与原厂一致，稳定后可上调（LCD 与 DSP 在同一总线可各自设时钟）。
- **按键**：接线未知，先用 `B220_BUTTON_DISCOVERY=1` 定位（见上文）。
- **旋转方向**：当前实现为顺时针 90°；若上机后反了，同时翻转 `DISPLAY_MIRROR_X/_Y`。
- **原始 `draw_bitmap` 通路需要手动字节交换**：LVGL 那条路由 `SpiLcdDisplay` 的 `swap_bytes=1` 处理，
  但**裸 `esp_lcd_panel_draw_bitmap()` 的缓冲必须自己 `__builtin_bswap16()`**，否则红蓝互换。
  （自检用的方向测试卡就踩过这个坑，代码里已修正并注明；方向卡现在使用面板原生尺寸绘制。）
