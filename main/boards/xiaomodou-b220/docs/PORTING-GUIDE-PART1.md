# 小魔豆 B220 移植小智（xiaozhi-esp32）实战手册 · PART 1

> **适用范围**：小魔豆 B220 AI 百变机器人（版本串 `B220_V01B97`，另有 `B220_V01B100`），主控 ESP32-D0WD rev1.0。
> **实测机**：MAC `b4:e6:2d:fc:4c:e5`（A 机），刷机口 `COM25`（3.3 V USB-TTL 接 ESP32 UART0）。
> **PART 1 覆盖**：成品效果 → 硬件全貌 → 工具与准备 → 刷机/运行纪律 → Phase 0 构建基线 → Phase 2 屏幕/背光/内存/主频。音频（ZL38063 主机启动、ADF 释放顺序、PA 使能、唤醒词与云端对话）见 PART 2。
> **证据标注**：**【实测】** 上机量到 / 日志打到；**【逆向】** 从原厂固件反汇编或数据结构读出；**【规格书】** 器件数据手册；**【已知未知】** 明确没有解开。

## 1. 成品效果与总体目标

### 1.1 做完之后这台玩具是什么样

一句话：**把原厂"图灵云 + 自家 App"的语音助手换成小智（xiaozhi-esp32）的"唤醒词 + 云对话"，而音频依然走原机的 3 麦阵列 → ZL38063 DSP → 喇叭 这条通路，不外加任何硬件。**

实测到的完整闭环（日志 `logs/wakeword.log`，A 机，2026-10-09）**【实测】**：

| 环节 | 现象 | 日志证据 |
| --- | --- | --- |
| 上电 | 屏亮、进 LVGL 界面 | `LcdDisplay: Turning display on` |
| 无凭据 | 自动进配网 AP | `WifiConfigurationAp: Access Point started with SSID Xiaozhi-4CE6` / `http://192.168.4.1` |
| 有凭据 | 直连家里的 AP 并激活 | `WifiStation: Got IP: 192.168.x.x` → `Application: Activation done` |
| 唤醒词 | 说"你好小智"即被识别 | `Application: Wake word detected: 你好小智 (state: 3)` |
| 云端 | MQTT 通道建立 | `MQTT: Connected to endpoint` / `MQTT: Session ID: 4c74af7f` |
| 对话 | 听 → 说 → 再听，多轮 | `StateMachine: State: listening -> speaking` → `speaking -> listening` |
| 工具调用 | 云端下发 MCP 工具 | `Application: << % get_weather...` / `<< % get_random_jokes...` |
| 结束 | 服务端 goodbye 后回 idle | `MQTT: Received goodbye message` → `StateMachine: State: listening -> idle` |

音频侧的关键一行（证明 DSP 真的起来了、而且是我们自己把它启动的）**【实测】**：

```
I (5834) XiaoMoDouB220: codec: reg 0x0014 = 0x0200 -> the ZL38063 application is READY
```

### 1.2 目标定义（必须守住的边界）

| 目标 | 说明 |
| --- | --- |
| ✅ **必须用原机 DSP 音频通路** | 明确**不接受**"加 INMP441 + MAX98357/NS4168 模块"的兜底方案。麦/喇叭必须继续走 ZL38063。 |
| ✅ **不加硬件** | 除调试期飞线的 3 根串口线（TX/RX/GND），成品不改板、不加模块。 |
| ✅ **可回滚** | 整片 16 MB 出厂备份必须存在且校验过，任何时候 `write_flash 0x0` 就能回原厂。 |
| ✅ **屏要亮** | 背光不是 GPIO，是 I2C 上的 AW9106B，必须单独把它点亮。 |

### 1.3 难度的真实分布

| 档位 | 内容 | 结论 |
| --- | --- | --- |
| ✅ 一定能做 | 刷机、ILI9341 出图、背光、WiFi 配网、唤醒词、云端全链路对话 | **已全部实测通过** |
| ⚠️ 要攻关 | 复用 ZL38063（复位 → HBI → 主机启动 → 配置记录 → ADF 释放 → PA 使能） | **已打通**，但排障链条很长（见 PART 2） |
| 🧱 **最硬的门槛** | **刷机通道**：本机 micro-USB 是**只读日志口**，PC→ESP32 无数据通路 | 必须飞线到 ESP32 模组 UART0（GPIO1/GPIO3/GND） |

## 2. 设备硬件全貌

### 2.1 总览表

| 项目 | 事实 | 来源 / 置信 |
| --- | --- | --- |
| 主控 | **ESP32-D0WD rev1.0**，双核 LX6，240 MHz | 【实测】`esptool flash_id` = `Chip is ESP32-D0WD (revision v1.0)`；引导日志 `Chip rev: v1.0` |
| Flash | **16 MB，GigaDevice GD25Q128**（JEDEC `c8 6018`） | 【实测】`Manufacturer: c8` / `Device: 6018` / `Detected flash size: 16MB` |
| Flash 模式 | DIO / 80 MHz | 【实测】引导横幅 `mode:DIO, clock div:1`；与原厂镜像头一致 |
| PSRAM | **8 MB SPI PSRAM @ 80 MHz，实际可映射 4 MB** | 【实测】`Found 8MB PSRAM device` / `Speed: 80MHz` / `Virtual address not enough for PSRAM … 4MB is mapped`（经典 ESP32 正常行为） |
| 显示 | **128×160 SPI 面板**（ILI9341 类克隆，SPI2 @ 10 MHz） | 【实测】；见 §6.1 分辨率之争 |
| 背光 | **AW9106B（I2C 0x5B）DIM0/DIM1/DIM2 = 0x3F**，**不是 GPIO** | 【逆向】+【实测】闭环，见 §2.6 / §6.3 |
| 麦克风 | **3 个**（原厂工厂测试项 `Mic 1: / Mic 2: / Mic 3:`） | 【逆向】 |
| 语音 DSP | **Microsemi/Microchip ZL38063 AcuEdge**：3 麦阵列前端 + AEC（尾音最长 256 ms）+ NR + 波束成形（最远 ≈4.9 m） | 【逆向】`MediaHal/DSP/zl38063/vproc_common.c`；【规格书】 |
| DSP ↔ ESP32 音频总线 | **一条全双工 I2S0**：BCLK **5** / WS **25** / DOUT **26** / DIN **35** | 【逆向】`i2s_pin_config_t` @ DROM `0x3f4317ac` = `{5,25,26,35}`；`i2s_config_t` @ DRAM `0x3ffbf398`：`mode=13`(MASTER\|TX\|RX)、16 bit、`use_apll=1` |
| DSP 控制总线 | **SPI2 上的 HBI**：片选 **GPIO0**，10 MHz，mode 0 | 【逆向】见 §2.5 |
| DSP 复位 | **GPIO19**（拉低 10 ms → 拉高） | 【逆向】函数 `0x40124418` |
| DSP 时钟 | **12.000 MHz 晶振**（规格书要求复位释放前已稳定） | 【规格书】ZL38063 datasheet Rev 4.0 |
| 功放 | **AW8737S**，由 AW9106B 的 **group-A 输出 A1（寄存器 0x02 bit1）** 驱动 SHDN | 【逆向】+【实测】见 §2.7 |
| 底座 | 独立 MCU，走 UART `{AT+...}` 协议（电机 / 头尾 LED） | 【逆向】 |
| 电源 | **底座上的软开关锁存**：按一下关机、再按一下开机，**ESP32 读不到** | 【实测】 |
| 音频资源分区 | `flashTone`，8 MB，74 个资源（69 MP3 + 5 个 Audacity XML） | 【逆向】见 §2.8 |
| 加速度计 | **SC7A20** @ I2C **0x19**（`WHO_AM_I`(0x0F) 必须 = `0x11`） | 【逆向】 |
| SD 卡 | **SDMMC slot 1**：CLK **14** / CMD **15** / D0 **2** / D1 **4** / D2 **12** / D3 **13** | 【逆向】；slot 0 的 GPIO6–11 被 Flash 占用，物理不可用 |

### 2.2 完整引脚表（全部由原厂固件逆向得到）

| 功能 | GPIO | 备注 |
| --- | --- | --- |
| LCD SCLK / MOSI / MISO | **32 / 33 / 34** | SPI2，10 MHz；MOSI/MISO 顺序判定见 §2.4 |
| LCD CS / DC / RST | **27 / 21 / 22** | RST 原厂 100 ms 低脉冲 |
| I2C0 SDA / SCL | **18 / 23** | 100 kHz；AW9106B @0x5B + SC7A20 @0x19 |
| I2S BCLK / WS / DOUT / DIN | **5 / 25 / 26 / 35** | 全双工 I2S0 主机；DOUT = ESP32→DSP，DIN = DSP→ESP32 |
| ZL38063 RESET | **19** | 输出，10 ms 低脉冲 |
| **ZL38063 HBI CS** | **0** | ★ 同时是启动 strapping 脚，见 §4.1 |
| SD 卡（SDMMC slot 1） | **14 / 15 / 2 / 4 / 12 / 13** | CLK / CMD / D0 / D1 / D2 / D3 |
| 控制台 UART0 | TX **1** / RX **3** | 飞线就在这里 |
| Flash / PSRAM | 6–11 / 16 / 17 | 不可用 |
| 原厂 UART 到"图灵 AI 模组" / 到底座 | **未取出** | 【已知未知】：固件里没有 `uart_set_pin` 字符串；`gpio_set_intr_type(38, …)` 痕迹**不能当引脚号用**（GPIO38 不是输出能力脚位，`SOC_GPIO_OUT_RANGE_MAX = 33`） |

**空闲 GPIO（技术上的 Plan B 余量，本项目未使用）**：20 / 24 / 28 / 29 / 30 / 31 / 36 / 37 / 38 / 39。注意 **24 与 28–31 未键合（not bonded out）**、**34–39 不能做输出**，所以真正能当输出的备用脚只有 20 等少数几个。

### 2.3 ESP32 直接驱动的外设 vs 底座驱动

先建立这个心智模型，否则会在"找按键/找电机 GPIO"上浪费一整晚：

- **ESP32 直接驱动**：LCD（SPI2）、ZL38063（I2S + SPI2-HBI + GPIO19）、AW9106B（I2C0）、SC7A20（I2C0）、SD 卡（SDMMC）。
- **底座板自己驱动**：左/右电机、头部 LED、尾部 LED、**电源软开关**。ESP32 只是往 UART 发 `{AT+LMY}` / `{AT+HLY}` / `{AT+MTO}` 这类命令。
- ⇒ **这台机器上没有任何 ESP32 可见的按键**。原厂工厂测试菜单里 "Head led test / Left motor test" 之类的处理函数最后落到的是 `AT+…` 字符串跳转表（表在 `0x3f4beb08`），**不是 GPIO 翻转**。

原厂 `{AT+...}` 命令表（从固件字符串提取）**【逆向】**：

| 命令 | 推测含义 |
| --- | --- |
| `AT+Pon` / `AT+PowerOn` | 上电握手（原厂日志里刷屏的就是它） |
| `AT+StartUp` / `AT+Run` / `AT+Stop` / `AT+Rst` / `AT+Reboot` | 启停 / 复位 |
| `AT+LMY` / `AT+LMN`、`AT+RMY` / `AT+RMN` | 左 / 右电机 开 / 关 |
| `AT+HLY` / `AT+HLN`、`AT+TLY` / `AT+TLN` | 头部 / 尾部 LED 开 / 关 |
| `AT+MTO` / `AT+CMD:MTOY` / `AT+CMD:MTON` | 动作 / 电机动作 |
| `AT+FTOID=` / `AT+OID=` | 下发对象 ID |
| `AT+Ver` | 版本查询 → `B220_V01B97` |
| `AT+CMD=(%d,%d,%d,%d),(%d,%d,%d,%d,%d)` | 参数化动作 |
| `AT+CMD:OK` / `AT+OK` | 应答 |

### 2.4 LCD 引脚顺序是怎么定下来的（可复现的推理）

原厂 LCD 配置结构体在 DROM `0x3f401654`，28 字节，逐字节读出来是：

```
0x3f401654: 02 00 00 00 | 22 21 20 1b | 15 16 00 00 | 80 96 98 00 | 00 00 00 00 | 01 00 00 00 | 01 00 00 00
            +0 = 2      +4  +5  +6  +7   +8  +9     +12 = 10 000 000 (=0x989680)  +20 = 1     +24 = 1
```

| 偏移 | 值 | 用途 | GPIO |
| --- | --- | --- | --- |
| +4 | 34 | `spi_bus_config_t` 第 2 个引脚（MISO） | **34** |
| +5 | 33 | 第 1 个引脚（MOSI） | **33** |
| +6 | 32 | 第 3 个引脚（SCLK） | **32** |
| +7 | 27 | SPI 设备片选 CS | **27** |
| +8 | 21 | 配置为输出（DC） | **21** |
| +9 | 22 | 输出 + 100 ms 复位脉冲（RESET） | **22** |
| +12 | 10 000 000 | SPI 时钟 10 MHz | — |
| +20 | 1 | SPI 主机号（SPI2） | — |

> **为什么确定 MOSI=33 / MISO=34**：ESP-IDF 的 `spi_bus_config_t` 里 MOSI 在前。若按 MISO 在前读，MOSI 会落到 **GPIO34 —— 而 GPIO34 是只输入脚**，物理上不可能做 MOSI。所以只剩一种解释。**【逆向 + 逻辑闭环】**

### 2.5 ZL38063 的 HBI 为什么在 SPI2 上、片选为什么是 GPIO0

这一条是"音频能不能通"的关键硬件事实：

1. `spi_lcd.c`（函数 `0x40203554`）在初始化完 LCD 自己的 SPI 设备之后，**又 `spi_bus_add_device` 了一次**，把句柄写进 DRAM 全局 `0x3ffc4c98`。**【逆向】**
2. `vproc_common.c` 的 HBI 读 / 写（`0x40125862` / `0x401258cf`）正是从这个全局取 `spi_device_handle_t`。**【逆向】**
3. ⇒ **DSP 的 HBI 与 LCD 共用 SPI2**：SCLK 32 / MOSI 33 / MISO 34；LCD 自己的 CS = 27，**DSP 的 CS = GPIO0**，**10 MHz**（`0x00989680`）、**mode 0**、`queue_size = 6`。
4. DSP 那个 `spi_device_interface_config_t` 被 `memset` 清零后**只写了时钟与队列深度**，`spics_io_num`(+16) **从未被写入 ⇒ 值为 0**；而且驱动确实把它当真实 GPIO 用：片选回调 `0x4020349c` 里 `l8ui a10, ifcfg, 0` → `gpio_set_level(pin = 0, level)`。**【逆向，高置信】**
5. 旁证：`spi_bus_add_device` 之后紧跟着直接 `gpio_set_level(0, …)`，与上游 `VprocHALInit` 里 `gpio_set_pull_mode(0, GPIO_FLOATING)` 一致。**【逆向】**

> ⚠️ 上游 esp-adf 参考实现里 `.clock_speed_hz = 1000000`（1 MHz）且注释声称 10 MHz；**B220 镜像里真的写的是 10 MHz**，即厂商改过这个值。**【逆向】**
>
> ⚠️ 两个后果：① **GPIO0 不能再当小智的 BOOT 按键**；② **GPIO0 同时是启动 strapping 脚**，玩具 USB 桥会在开机瞬间把它拉低 ⇒ 插着 micro-USB 开机必然进下载模式（§4.1）。

### 2.6 背光 = AW9106B，不是 GPIO（完整证据链）

| 项目 | 值 | 依据 |
| --- | --- | --- |
| 芯片 | **AW9106BTQR**（艾为 AWINIC，6 通道 LED 驱动 + GPIO 控制器，I²C）；固件里 tag `"aw9106"`，写 helper `0x40207770`、读 helper `0x40207808`、`chip_aw9110b_init` `0x402078dc` | 【实测】PCB 丝印 + 【逆向】字符串交叉引用 |
| 7 位地址 | **0x5B**（写 `0xB6` / 读 `0xB7`） | 【逆向】所有写操作第一实参 = 91 = 0x5B；与规格书 AD0/AD1 都拉高的地址一致 |
| 总线 / 引脚 | I2C0 主模式 **100 kHz**；**SDA 18 / SCL 23**，内部上拉使能 | 【逆向】`i2c_config_t` @ `0x3f402118`；两条独立路径互相印证（见下） |

**两条独立路径的互相印证（这就是"为什么敢直接写代码"）**：

- 路径 A：用 100 kHz 魔数扫数据段，命中结构体 `0x3f402118`：`+0=1(master) +4=18(sda) +8=1 +12=23(scl) +16=1 +20=100000`。
- 路径 B：从 IDF 驱动代码反推字段布局 —— `i2c_param_config`(`0x402343dc`) 里的 `i2c_set_pin` 调用点 `0x4023446f`：

```
40234464: l32i a14, a3, 16    ; scl_pullup_en = cfg+16
40234469: l32i.n a13, a3, 8   ; sda_pullup_en = cfg+8
4023446b: l32i a12, a3, 12    ; scl_io_num     = cfg+12
4023446d: l32i a11, a3, 4     ; sda_io_num     = cfg+4
4023446f: call8 i2c_set_pin
```

两条路推出**完全一致**的字段偏移与数值 ⇒ SDA=18 / SCL=23 / 100 kHz 可以确定。

**原厂初始化序列（`chip_aw9110b_init` @ `0x402078dc`，逐字节）**【逆向】**：

| 寄存器 | 值 | 名称 | 含义 |
| --- | --- | --- | --- |
| 0x7F | 0x00 | `RESET` | 软件复位（注意：**它清掉整颗芯片的所有配置**） |
| 0x11 | 0x10 | `CTL` | 电流大小设定 |
| 0x12 | 0x02 | `GPMD_A` | A 组模式（GPIO/输出模式） |
| 0x04 | 0x00 | `GPIO_CFG_A` | A 组配置为输出 |
| 0x02 | 0x00 | `GPIO_OUTPUT_A` | A 组输出值 0 |
| 0x13 | 0x00 | `GPMD_B` | B 组设为 LED 模式 |
| 0x20–0x25 | 0x00 | `DIM0`–`DIM5` | 6 路通道亮度全清零 |

之后还有一批呼吸灯配置：`0x14`(`EN_BRE`)、`0x11`(`CTL`)、`0x05`(`GPIO_CFG_B`)、`0x17`–`0x19`(`DLY0_BRE`–`DLY2_BRE`)、`0x15`(`FADE_TMR`)、`0x16`(`FULL_TMR`)。

**背光是怎么被确认的（实测闭环）**【实测】：LEDC/PWM 在原厂固件里**完全不存在**（`0x3FF59000-0x3FF5A000` 零常量），任何 GPIO 也没被当背光用（穷举了 `GPIO_OUT_W1TS/W1TC`、IOMUX、RTC_IO 的所有写入点）。而全镜像里**唯一持续非零**的 LED 通道写入就是 `DIM0/DIM1/DIM2 = 0x3F`。决定性一步：自检固件开机后**轮流点亮 AW9106 六路**（每路亮 2 秒、间隔 0.15 秒），**用户在约第 30–34 秒看到屏幕亮起、中间闪灭一次** —— 正好对上 DIM0/DIM1/DIM2 的点亮时序。**闭环完成。**

**同一条 I2C 上还有一颗 0x19 = SC7A20 加速度计**【逆向】**：固件在 `0x40123918` 一开始就读 `WHO_AM_I`(0x0F)，**期望值 `0x11`**，不等就直接退出。（LIS3DH / LIS2DH / LIS2DH12 是 `0x33`；**SC7A20 是 `0x11`**。）量产固件能工作 ⇒ 板上那颗必然返回 `0x11` ⇒ 它是 SC7A20 这一类。地址 0x19 说明 SA0/AD0 被拉高。写入的初始值：`0x21←0x91`、`0x22←0x40`、`0x23←0x88`、`0x24←0x80`、`0x30←0x2A`、`0x32←0x65`、`0x20←0x41`。中断由一个名为 `gsensor_block` 的任务处理。

### 2.7 AW8737S 功放：它不是"普通使能脚"

这一条在 PART 2 的调音阶段是**决定性的**，但属于硬件结构，先交代清楚：

- 扬声器功放 **AW8737S**，其 **SHDN 引脚（A4）低有效**，由 **AW9106B 的 group-A 输出 OUT_A1 = 寄存器 0x02 的 bit1** 驱动。**【逆向】**
- **SHDN 不是电平使能，而是"模式选择"**：SHDN 保持低超过 TOFF（规格书建议 1 ms）之后，芯片按**随后的上升沿个数**选工作模式：**【规格书】**

| 上升沿数 | 模式 | 输出功率 |
| --- | --- | --- |
| **1** | mode 1 | **1.2 W** ← 这块板要的 |
| 2 / 3 | mode 2 / mode 3 | 1.0 W / 0.8 W |
| **4** | mode 4 | **NCN 功能关闭 = 完全不出声** |
| >4 | 不允许 | — |

⇒ 所以**寄存器 0x02 的全生命周期里只能有恰好一次 0→1**。任何"顺手再写一次"都会把它推到 mode 2/3/4。这也是为什么板级代码要把它做成"唯一写入者 + 计数 + 锁存"（见 PART 2）。

### 2.8 原厂 Flash 分区与 `flashTone` 音频库

原厂整片 16 MB 的分区（**类型/子类型是厂商乱填的，只有名字有意义**）**【逆向】**：

| # | 分区 | 类型 | 偏移 | 大小 | 实际内容 |
| --- | --- | --- | --- | --- | --- |
| 0 | nvs | data/nvs | 0x009000 | 16 KB | WiFi 凭据、BLE 密钥、PHY 校准 |
| 1 | otadata | data/otadata | 0x00d000 | 8 KB | **全空** → 引导器默认启动 ota_0 |
| 2 | phy_init | data/phy | 0x00f000 | 4 KB | 全空，运行时生成 |
| 3 | **wifi** | app/ota_0 | **0x010000** | 3.75 MB | 主固件（**明文、未加密**） |
| 4–7 | Iram0 / Dram0 / Dram1 / profile | data/0x20 / 0x21 / 0x22 / 0xff | 0x3d0000 / 0x3f0000 / 0x410000 / 0x42c000 | 128 KB / 128 KB / 112 KB / 4 KB | Iram0 87% 空白；其余高熵不透明（熵 7.93–7.96） |
| 8 | **flashTone** | data/nvs_keys | **0x42d000** | **8 MB** | **74 个音频资源** |
| 9 | coredump | data/coredump | 0xc2d000 | 64 KB | 一次崩溃现场（非空） |
| 10 | wifi_1 | app/ota_1 | 0xc40000 | 3.75 MB | **全空 0xFF**，从未写过 |

**`flashTone` 归档格式（完整解出）**【逆向】**：分区开头是索引表，**每条 64 字节**：

| 偏移 | 类型 | 含义 |
| --- | --- | --- |
| +0 | u32 | magic，**只有第 0 条**是 `"SJ\0"`（`53 20 4a 00`），其余为 0 |
| +4 | u32 | 保留，恒 0 |
| +8 | u32 | **索引令牌 = `0x28 + (序号 << 8)`**（74/74 条吻合） |
| +12 / +16 | u32 | 资源在分区内的偏移 / **资源长度**（不是 CRC —— crc32 对 74/74 全部不匹配） |
| +20..+63 | 44 B | 恒 0 |

布局与账目（**分毫不差**）：

```
0x000000 .. 0x001280   索引表，74 × 64 B = 4736 B
0x001280 .. 0x001288   8 字节 0 填充
0x001288 .. 0x775120   资源载荷，7,814,720 B
0x775120 .. 0x800000   569,056 B 已擦除 0xFF（无条目、无 footer）
账目：4736 + 8 + 7814720 + 88(对齐填充) + 569056 = 8388608 = 分区大小 ✅
```

索引表步长的证明方法（可复现）：数"有多少条连续记录满足 `u32@+8 == 0x28 + 0x100*n`" —— 步长 0x20 → 1 命中，**步长 0x40 → 74 命中**，0x80 → 1，0x100 → 1。

内容构成：**69 个可解码 MP3 + 5 个 Audacity 2.3.2 工程 XML**（索引 041/044/045/046/047，是一套交通工具音效包 `42Police/45Truck/46Racing/47Fire_engine/48Helicopter`，22050 Hz）—— 连制作时的中间文件都一起烧进去了。**【逆向】**

> ⚠️ **不要把 `flashTone` 当资产直接用**：小智用的是自己的 `assets`(spiffs) 分区，格式完全不同。想把原机音效搬过来，必须按上面格式解包出 MP3，再按小智的 assets 打包方式重新打包。

### 2.9 两个单元的对比（A 机 / B 机）

| 项目 | A 机 | B 机 |
| --- | --- | --- |
| 整片备份 | `backup/esp32-16MB-full.bin`，MD5 `A6A27D2B4B7A8DE36636E9EF635BEA40` | `backup/esp32-16MB-unitB-stock.bin`，MD5 `9F9FABA6…A49D` |
| 应用分区 | 版本 A | **与原厂 A 机差约 20%**（App 行为/界面有差别） |
| **`flashTone` 8 MB 音频区** | 区域 MD5 `afde056c2d1b703a22dd4368c4a8b1f7` | **同 MD5 ⇒ 逐字节完全一致**（含 74 个载荷） |
| 互刷 | ❌ **不可互刷**，还原时各用各的备份 | |

【实测】两台原厂固件版本不同（App 分区 20% 差异），但**音频素材区 byte-identical**。这条对比有两个用处：① 证明音频区是"生产时统一烧的素材"，与 App 版本解耦；② 提醒**跨机恢复必须用对应机器的备份**，且会连 NVS 里的 `sta.mac` 一起搬过去（见 §4.6）。

## 3. 工具与准备

### 3.1 软件与硬件清单

| 工具 | 路径 / 规格 | 为什么是它 |
| --- | --- | --- |
| **ESP-IDF** | **v6.1**，`C:\esp\v6.1\esp-idf` | 该仓库要求 `idf: >=6.0.1`；本机只装了 5.2.5 / 5.3.2 / 5.4 ⇒ **必须先装 6.1** |
| IDF 环境入口 | `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` | ⚠️ **不要用** `C:\esp\v6.1\esp-idf\export.ps1` |
| **esptool** | **4.8.1**，跑在 `<idf-python>` 上 | 这个 python 环境已装好 esptool，不必另建虚拟环境 |
| **3.3 V USB-TTL 转接板** | **必须 3.3 V 档**（ESP32 的 IO 不耐 5 V） | 飞线到 UART0，唯一的数据通路 |
| 细漆包线 / 测试夹 + 万用表 | 通断档 + 直流电压档 | 3 根线：TX→GPIO3、RX→GPIO1、GND；验证共地、GPIO19 电平、追线 |
| **100 MHz 示波器** | 双通道更好 | 量 GPIO19 复位脉冲、I2S WS（48 kHz）、BCLK（1.536 MHz）、DAC 输出 |
| 串口抓取 | PowerShell `System.IO.Ports.SerialPort`，115200 | 见 §3.4（`capture-serial.ps1`） |

**为什么 `export.ps1` 会失败（值得记一笔）**：IDF 的 `export.ps1` 激活时会去调 `python`，本机 PATH 上先命中的是系统 Python 3.9，而 IDF 6.1 的工具脚本要求 ≥3.10，于是报错退出。EIM 生成的 profile（`Microsoft.v6.1.PowerShell_profile.ps1`）已经把 `IDF_PATH`、`IDF_TOOLS_PATH`、`PATH`、`PYTHON` 全部指向正确位置，所以**只用 profile，不用 export**。

标准开场：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py --version        # 应打印 ESP-IDF v6.1
cd <repo>
```

### 3.2 接线（就 3 根，VCC 不接）

| USB-TTL（3.3 V 档） | ESP32 模组 | 说明 |
| --- | --- | --- |
| **TX** | **GPIO3（RXD0，模组第 34 脚）** | ★ 决定性的一根：PC → ESP32 |
| RX | GPIO1（TXD0，第 35 脚） | 看日志 / 引导横幅 |
| GND | GND（第 1/15/38 脚或屏蔽罩） | **必须共地** |
| VCC | **不接** | 设备自己供电，避免两边电源打架 |
| DTR / RTS | **不接** | ⇒ esptool 一律 `--before no_reset` |

规则：**先接 GND 再接信号线**。接完先用万用表确认 TTL 的 GND ↔ 模组 GND 通断、TX 通到 GPIO3（第 34 脚）。

**为什么必须飞线（而不是直接用玩具的 micro-USB）**【实测，10 项证据】**：那条 micro-USB 属于**另一颗芯片**（USB CDC 虚拟串口 `\Device\VCP0`），它的 **TX 方向根本没接到 ESP32 的 UART0**，所以 PC 的命令永远到不了 ESP32。原厂固件里那套 `{AT+...}` 是 **ESP32 在向底座/AI 模组发**上电握手，不是它在等主机命令。⇒ 好消息：**不需要割线**，直接并上 TTL 就能刷。

### 3.3 esptool 公共参数（写死，别每次手打）

```powershell
$py = '<idf-python>'
cd <workspace>      # 仓库里的 esptool.cfg 会被自动加载（已针对本机调过复位等待）
$common = @('-m','esptool','--port','COM25','--before','no_reset','--after','no_reset','--baud','921600')
```

### 3.4 抓串口日志

本机 TTL 只接了 TX/RX/GND，**没有 DTR/RTS**，所以不能靠 esptool 复位；复位方式二选一：让 `flash-b220.ps1` 用 `--after watchdog_reset` 触发（推荐），或者手动拔插设备电源。

仓库脚本 `capture-serial.ps1`：

```powershell
.\capture-serial.ps1                 # 抓 30 秒到 logs\boot-<时间>.log
.\capture-serial.ps1 -Seconds 90     # 抓久一点（看配网/握手）
.\capture-serial.ps1 -Watch          # 持续监听，Ctrl+C 退出
```

核心实现（自己写一份也就这几行）：

```powershell
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.ReadTimeout = 500
$sp.DtrEnable   = $false     # ★ 千万别让 PowerShell 拉 DTR/RTS，否则会意外复位/进下载模式
$sp.RtsEnable   = $false
$sp.Open()
$sp.ReadLine()               # 循环读；TimeoutException 就继续等
```

⚠️ 抓日志前**先关掉占着 COM 口的其它工具**（串口助手、`idf.py monitor` 等），否则报"访问被拒绝"。

## 4. 刷机/运行的操作纪律（最容易踩坑的地方）

### 4.1 micro-USB / GPIO0 strapping：这台机器最反直觉的一条

> **本机的"进下载模式"不靠 DTR/RTS，而是靠"插着 micro-USB 上电"。**

| 目的 | 操作顺序 | 会看到什么 |
| --- | --- | --- |
| **进下载模式（刷机）** | ① 按一下启动键**关机** → ② **插上 micro-USB** → ③ 按一下启动键**开机** | `rst:0x1 (POWERON_RESET),boot:0x27 (DOWNLOAD_BOOT…)` + `waiting for download` |
| **退出下载模式（运行固件）** | ① **拔掉 micro-USB** → ② 按一下启动键关机 → ③ 再按一下开机 | `boot:0x3f (SPI_FAST_FLASH_BOOT)`，屏幕亮起 |

**原理**：玩具自己的 USB 桥在**开机瞬间把 GPIO0 拉低**；GPIO0 是 ESP32 的启动 strapping 脚，低电平 ⇒ ROM 必然进下载模式。所以 **插着 micro-USB 是没法正常运行的**。

补充事实：
- 启动键是**底座上的软开关**，ESP32 读不到它 ⇒ 固件里没有按键。
- 运行小智固件时**只靠 TTL 供电不够**（TTL 只接 TX/RX/GND），必须靠玩具电池。让芯片从下载模式回到运行：**拔掉 micro-USB 后重新上电**，或 `--after watchdog_reset`。

### 4.2 靠 `write_mem` 做软件复位（不需要 DTR/RTS）

```powershell
# 读一下 RTC_CNTL_OPTIONS0（bit31 = SW_SYS_RST）
& $py @common dump_mem 0x3FF48000 4 "$env:TEMP\rtc_opt0.bin"
# 触发一次软件复位 —— 会立刻复位芯片
& $py @common write_mem 0x3FF48000 0x80000000
```

> esptool 通常会报 `Packet content transfer stopped` —— **这是正常的**（芯片在命令执行中途复位了）。
> ⚠️ 若复位瞬间 GPIO0 仍是低电平（micro-USB 还插着），芯片**会再次落回下载模式**。

另外，esptool 4.8.1 的 `--after watchdog_reset` 走 RTC 看门狗，也**不需要 DTR/RTS**，本仓库的 `flash-b220.ps1` 用的就是它。但 **4.8.1 没有可用的 `hard_reset` 路径**（没接 DTR/RTS），所以 `--before` 永远是 `no_reset`。

### 4.3 ★ 两套分区布局**不兼容** —— 这是踩过的最大一个坑

| 区域 | **原厂布局**（备份） | **小智布局**（`partitions/v2/16m.csv`） |
| --- | --- | --- |
| bootloader / 分区表 / nvs / otadata / phy | 0x1000 / 0x8000 / 0x9000 / 0xd000 / 0xf000 | **同**（这 5 个偏移两套布局一致） |
| 应用 | `wifi`(ota_0) **0x10000**，3.75 MB | `ota_0` **0x20000**，3.9 MB |
| 备用应用 | `wifi_1`(ota_1) 0xc40000，空 | `ota_1` 0x410000，空 |
| 音频资源 | `flashTone` **0x42d000**，8 MB | `assets`(spiffs) **0x800000**，8 MB |
| 厂商私有 | `Iram0`/`Dram0`/`Dram1`/`profile` | 无 |

**这直接导致两条铁律**：

1. **写固件必须写全 5 个区域**（bootloader / 分区表 / otadata / app / assets），因为它们之间的**偏移关系全变了**。
2. **绝不能把 app 单独写到原厂布局上**。小智的 app 是按"自己的分区表和 bootloader"编译的，app 分区从 `0x20000` 开始、且 otadata 需要被初始化过；把它写到原厂的 `0x10000`，**引导器找不到可启动的分区，就会无限重启** —— 这不是"哈希算错"，是"给错了布局"。**这件事真的发生过。**

> 同族现象（**原厂固件**侧）：原厂 app 镜像尾部带 32 字节 SHA-256（覆盖 `app[0:0x37A1E0)`）。只改代码不回写哈希，bootloader 必然报
> `Checksum failed`（措辞是 Checksum，比的其实是 SHA-256）→ `No bootable app partitions in the partition table` → 无限重启。

⇒ 只要**切换布局方向**（原厂→小智 或 小智→原厂），**整片擦除 + 整片/全区域写回**是最省心的做法。小智固件的刷写参数必须与原厂镜像头一致：`--flash_mode dio --flash_freq 80m --flash_size 16MB`。

### 4.4 备份整片 16 MB（**动任何东西之前先做这一件事**）

```powershell
& $py @common read_flash 0 0x1000000 .\backup\esp32-16MB-full.bin
Get-FileHash .\backup\esp32-16MB-full.bin -Algorithm MD5
```

**判定标准**：末尾出现 `Staying in bootloader.`；文件**正好 16,777,216 字节**；本机 A 机出厂备份 MD5 = **`A6A27D2B4B7A8DE36636E9EF635BEA40`**（2026-10-06 采集）。

**耗时参考**【实测】：整片读回 **277.4 s / ≈484 kbit/s**（命令行 `--baud 921600`，实测吞吐受转接板限制）⇒ 16 MB 整片读写各约 **4.5–6 分钟**。

> ⚠️ **"重新读一遍，整片 MD5 和旧备份不一样"通常不是接线问题。** 设备只要运行过，**NVS 分区（0x9000–0xd000）就会变**（WiFi 状态、日志游标等），差异会落在 `0x000000` 那个 64 KB 块里的 NVS 区域，形态是"页轮转"（实测：`0xb000` 写入 1927 B、`0xc000` 擦除 2365 B）。**按 64 KB 分块比对**才能看清：本机实测 **255/256 个 64 KB 块逐字节一致** ⇒ **读取通路零误码**。

### 4.5 写入能力验证（**不破坏出厂固件**的一次性测试）

原厂分区表里 `wifi_1`（app/ota_1）位于 **0xc40000**、长 0x3c0000、**整片 0xFF 从未写过**，且 `otadata` 指向 `ota_0`，所以往这里写不影响启动、也不碰任何出厂数据：

```powershell
& $py -c "import os;open(r'.\backup\_wtest.bin','wb').write(os.urandom(65536))"
& $py @common write_flash 0xc40000 .\backup\_wtest.bin
& $py @common read_flash  0xc40000 0x10000 .\backup\_wtest-read.bin
Get-FileHash .\backup\_wtest.bin,.\backup\_wtest-read.bin -Algorithm MD5   # 必须一致
& $py @common erase_region 0xc40000 0x10000                               # 还原成空
```

**本机实测**【实测】：写入源与读回 MD5 都是 `84BA125300A596B5484DDCF62CBB4F41` ✅；`erase_region` 后读回全 `0xFF` ✅；出厂固件全程未被触碰（ota_0 / otadata 均未变）。

### 4.6 写小智固件（推荐用脚本，它会自动回读校验）

```powershell
cd <workspace>
.\flash-b220.ps1 -FullErase      # ★ 首次从原厂切到小智：整片擦除，避免 assets 分区残留原厂音频
.\flash-b220.ps1                 # 之后迭代：只写 5 个区域，保留 NVS/WiFi 凭据
.\flash-b220.ps1 -NoBoot         # 刷完不让它复位（排障用）
```

等价的手工命令：

```powershell
$B = '<repo>\build'
& $py @common erase_flash                      # 仅首次需要（约 1–2 分钟）
& $py @common write_flash `
    0x1000   "$B\bootloader\bootloader.bin" `
    0x8000   "$B\partition_table\partition-table.bin" `
    0xd000   "$B\ota_data_initial.bin" `
    0x20000  "$B\xiaozhi.bin" `
    0x800000 "$B\generated_assets.bin"
```

**判定标准（两条都要过）**：① `Hash of data verified.`（esptool 用片上 MD5 自校验，**唯一可信的成功标志**）；② `read_flash 0x20000 <app 长度>` 读回、与 `build\xiaozhi.bin` 比 MD5 必须一致。

> **跨机恢复的 MAC 陷阱**：把 A 机的整片备份写到 B 机会连 NVS 一起搬过去，其中包括 `nvs.net80211:sta.mac` —— WiFi 驱动会采用这个缓存值，于是**两台机器用同一个 MAC** 上同一网络，互相干扰。若确实要跨机恢复，写完清一下 NVS（eFuse 里的真实 MAC 不会被 flash 改变）：
>
> ```powershell
> & $py @common erase_region 0x9000 0x4000    # 清 NVS：MAC 回到出厂值，但需重新配网
> ```

### 4.7 恢复出厂（最终回滚路径）

```powershell
& $py @common write_flash 0x0 .\backup\esp32-16MB-full.bin
```

**必须看到 `Hash of data verified.`**。可选快速复查（只读前 64 KB，几秒）：

```powershell
& $py @common read_flash 0 0x10000 "$env:TEMP\b220-spot.bin"    # 只读前 64 KB
& $py -c "import sys;a=open(sys.argv[1],'rb').read(65536);b=open(sys.argv[2],'rb').read();print('identical:',a==b)" `
      .\backup\esp32-16MB-full.bin "$env:TEMP\b220-spot.bin"
```

写完后**拔掉 micro-USB** 才能开机运行。

### 4.8 波特率与错误信息对照表

| 现象 | 含义 / 处理 |
| --- | --- |
| **`Corrupt data, expected 0x1000 bytes but received 0xffd bytes`** | **串口链路误码**（读到一半丢字节）。**降波特率重试**：`921600 → 460800 → 115200`。不是芯片的问题。 |
| `No serial data received` | 没进下载模式（回 §4.1）；或 TX/RX 接反；或 GND 没共地 |
| `Invalid head of packet (0x2B/0xFE)` | 读到的是**应用日志**而不是 ROM 响应 ⇒ 芯片在跑固件，不在下载模式 |
| 端口打不开 / 拒绝访问 | 别的串口工具占着 COM25，先关掉 |
| 写一半失败 | 降速；缩短飞线；确认 TTL 是 3.3 V 档 |
| `Packet content transfer stopped` | 芯片在命令执行中途复位了（如 `write_mem 0x3FF48000 0x80000000`），**属预期** |
| 开机第一行 `flash read err, 1000` 然后 `rst:0x10 (RTCWDT_RTC_RESET)` | **本机已知的 Flash 冷启动裕量问题**（原厂固件同样现象），靠看门狗复位自愈，**不是刷坏了** |
| 刷完屏幕黑但能连上服务器 | 背光在 AW9106（I2C 0x5B）DIM0/1/2，先查这一处（§6.3） |
| 设备完全不启动 | 重新进下载模式，再整片恢复（§4.7） |

## 5. Phase 0：构建基线

### 5.1 板级选择链（加新板必须链条全通）

xiaozhi-esp32 一次构建只选**一块**板，选择链是**五环相扣**的：

```
main/boards/<dir>/config.json  →  scripts/build.py（读它，写 build/config/sdkconfig.json 并调用 idf.py）
        ↓
main/Kconfig.projbuild         →  choice BOARD_TYPE 里的一个 config BOARD_TYPE_xxx（带 depends on IDF_TARGET_*）
        ↓
main/CMakeLists.txt            →  elseif(CONFIG_BOARD_TYPE_xxx) set(BOARD_DIR "…")
        ↓
main/boards/<dir>/{config.h, config.json, *.cc}  →  里面有且只有一个 DECLARE_BOARD(...)
```

本板在这个链条里的落点（可作照抄模板）：

| 环节 | 本板内容 |
| --- | --- |
| `config.json` | `"type": "xiaomodou-b220"`、`"target": "esp32"`、`builds[0].name = "xiaomodou-b220"`、`sdkconfig_append`（见下） |
| `main/Kconfig.projbuild:167` | `config BOARD_TYPE_XIAOMODOU_B220` / `bool "XiaoMoDou B220 AI Robot (小魔豆 B220)"` / `depends on IDF_TARGET_ESP32` |
| `main/CMakeLists.txt:112` | `elseif(CONFIG_BOARD_TYPE_XIAOMODOU_B220)` → `set(BOARD_DIR "xiaomodou-b220")` |
| `main/boards/xiaomodou-b220/` | `config.h`（约 50 KB 带注释配置）、`xiaomodou_b220.cc`（板级实现）、`zl38063*`（音频，PART 2）、`README.md`、**一个 `DECLARE_BOARD`** |

`config.json` 里覆盖的 9 项 sdkconfig（**这就是"和参考面包板不同的地方"**）：

```json
"sdkconfig_append": [
    "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y",
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions/v2/16m.csv\"",
    "CONFIG_ESPTOOLPY_FLASHFREQ_80M=y",
    "CONFIG_SPIRAM=y",
    "CONFIG_SPIRAM_SPEED_80M=y",
    "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y",
    "CONFIG_USE_ESP_WAKE_WORD=y",
    "CONFIG_WAKE_WORD_DISABLED=n",
    "CONFIG_USE_AFE_WAKE_WORD=n"
]
```

> ⚠️ **仓库规则**：`CONFIG_SR_WN_*`（选唤醒词模型）**不能**写进板级 `config.json` —— `scripts/tests/test_build.py:172-184` 专门断言这一点，写了会让 host 测试挂掉。模型选择只有两个合法来源：① `sdkconfig.defaults.esp32:3` 的 `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y`（已存在）；② 命令行 `--wake-word nihaoxiaozhi`。

`partitions/v2/16m.csv`（小智侧布局）：

```
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,    0x4000,
otadata,  data, ota,     0xd000,    0x2000,
phy_init, data, phy,     0xf000,    0x1000,
ota_0,    app,  ota_0,   0x20000,   0x3f0000,
ota_1,    app,  ota_1,   ,          0x3f0000,
assets,   data, spiffs,  0x800000,  8M
```

### 5.2 规范构建命令

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd <repo>

# 先看看板子/变体名到底叫什么（避免拼错）
python scripts/build.py --list-boards

# Phase 2 基线构建
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220

# 之后（唤醒词阶段）加上官方开关
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi
```

`--wake-word nihaoxiaozhi` 是仓库的官方开关（`docs/custom-board.md:376-388`）：对 `esp32` 目标会展开成 `CONFIG_USE_ESP_WAKE_WORD=y` + `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y`（`scripts/build.py:338-385`；esp32 **不在** `_LITE_WAKE_WORD_TARGETS = {esp32c3, esp32c5, esp32c6}` 里，所以选的是 `wn9_*` 大模型而不是 `wn9s_*`）。

**构建日志里应该看到（证明唤醒词模型真的打进 assets 了）**：

```
  wakenet models: wn9_nihaoxiaozhi_tts (will be packaged)
Generated: .../srmodels/srmodels.bin
```

不去刷机也能自检：

```powershell
$B = '<repo>\build'
$txt = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$B\generated_assets.bin"))
"assets 含 srmodels: " + $txt.Contains('srmodels')     # 必须 True
(Get-Item "$B\generated_assets.bin").Length            # 带模型约 157 万字节；无模型时约 129 万
```

### 5.3 产物落在哪里

| 产物 | 路径 | 本机实测大小 |
| --- | --- | --- |
| 应用镜像 | `build\xiaozhi.bin` | **2,442,288 B（2.33 MB）** |
| assets（含唤醒词模型） / 合并整片 | `build\generated_assets.bin` / `build\merged-binary.bin` | **1,578,891 B（1.51 MB）** / 9,967,499 B |
| bootloader / 分区表 / otadata | `build\bootloader\bootloader.bin` / `build\partition_table\partition-table.bin` / `build\ota_data_initial.bin` | 刷到 0x1000 / 0x8000 / 0xd000 |
| sdkconfig 快照 / ELF / MAP | `build\config\sdkconfig.json` / `build\xiaozhi.elf` / `build\xiaozhi.map` | 自查 Kconfig 是否生效 / 排障 |

app 体积曲线：Phase 0 参考板 = 2,065 KB；Phase 2 本板 = 2,180 KB；唤醒词版本 = 约 2.33 MB —— **3.9 MB 的应用槽位一直宽裕。**

### 5.4 沙箱注意：`idf.py` 需要命名管道权限

在受限沙箱（禁止创建命名管道）里，`idf.py` 会因为 **"禁止创建命名管道"** 报 `WinError 5`，编译起不来 ⇒ 编译这一步必须放宽权限或换到不受限的会话里跑。（`esptool` 走 PowerShell 管道**不受影响**，所以刷机/读片在受限环境里也能做。）

### 5.5 忘记删 `sdkconfig` 会静默保留旧选择

`python scripts/build.py` 会**改动本地 `sdkconfig` 和构建状态**。如果你改了板级 `config.json` 里的某一项（例如把 40 MHz 改成 80 MHz）却发现**日志里还是旧值**，**十有八九是 `sdkconfig` 没重新生成**。处理办法：**删掉 `sdkconfig`（和 `sdkconfig.old`）强制重新生成**，或用 `idf.py reconfigure`，然后**去 `build\config\sdkconfig.json` 里核对那一项**再编译 —— 不要凭 `config.json` 的文字就相信它生效了。**【实测踩过：PSRAM 速度与 Flash 频率这一对就是这样。】**

## 6. Phase 2：屏幕 / 背光 / 内存 / 主频

### 6.1 屏：直接回放原厂初始化表（最稳的一条路）

**做法**：不要去猜 ILI9341 的通用初始化序列，而是**把原厂固件里那张初始化表原样回放**。

- 原厂表位置：**DROM `0x3f4bdbf2`**，**18 字节/条**（1 字节命令 + 16 字节参数 + 1 字节「长度|延时标志」），`0xFF` 结束。**【逆向】**
- 原始内容（按条展开）：

```
0xC0 00 | 0xC2 01 FF | 0xC3 13 | 0xC4 20 | 0xC6 0F | 0xCA 0F | 0xC8 08 | 0x55 B0
0x21     | 0x36 00    | 0x3A 55 | 0xB1 40 02 14 | 0x26 01
0x2A 00 00 00 EF    ← 列窗口 0..239（原厂表就是这么写的）
0x2B 00 00 01 3F    ← 行窗口 0..319
0x2C | 0xE0(14B) | 0xE1(14B) | 0x11 | 0x29
```

- 这套寄存器组（`0xC0/0xC2/0xC3/0xC4/0xC6/0xCA/0xC8/0x55/0x26/0xE0/0xE1`）是 **ILI9341 专有**的（ST7735 没有），所以控制器家族可以确定是 ILI9341 兼容。**【逆向】**

**代码落点**（`xiaomodou_b220.cc`）：把表塞进 `ili9341_vendor_config_t.init_cmds`：

```cpp
static const ili9341_lcd_init_cmd_t b220_lcd_init_cmds[] = {
    {0xC0, (uint8_t[]){0x00}, 1, 0},
    {0xC2, (uint8_t[]){0x01, 0xFF}, 2, 0},
    /* … 原表逐条照抄 … */
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},  // sleep out —— 原表没有延时，这里补 120 ms
    {0x29, (uint8_t[]){0x00}, 0, 20},   // display on
};

ili9341_vendor_config_t vendor_config = {
    .init_cmds = b220_lcd_init_cmds,
    .init_cmds_size = sizeof(b220_lcd_init_cmds) / sizeof(b220_lcd_init_cmds[0]),
};

panel_config.vendor_config = &vendor_config;
```

**三条必须同时记住的细节**：

1. **这张表会"替换"驱动的默认序列，不是追加**。所以 `0x11`(sleep out) 和 `0x29`(display on) 必须自己补在表尾（上面已补），否则屏永远不亮。
2. **表里已经发了 `0x21`(INVON)**（反色开）和 **`0x36`(MADCTL) = 0x00**（RGB 顺序、不镜像、不交换 X/Y）。因此**绝对不要**再调 `esp_lcd_panel_invert_color()`；镜像/交换也必须是 `false`。实测确认：方向测试卡显示"**竖条 + 白角在左上/右下**" ⇒ 不需要旋转/镜像。**【实测】**
3. 驱动会打印两条**无害的警告**（`The 36h/3Ah command has been used and will be overwritten…`），看到不用慌 —— 那正是"外部初始化表覆盖了驱动默认值"本身。

**复位时序自己来（别用驱动的默认）**：原厂是 `RST` 拉低 **100 ms** → 拉高 → 再等 **100 ms** 才发命令；`esp_lcd` 内置复位只等约 10 ms，**一些 ILI9341 克隆片不认**。SPI 参数取 `io_config.pclk_hz = 10 * 1000 * 1000`（与原厂一致）、`spi_mode = 0`、`lcd_cmd_bits = 8`、`lcd_param_bits = 8`、`trans_queue_depth = 10`，总线 `max_transfer_sz = 16 * 1024`、`SPI_DMA_CH_AUTO`。

```cpp
panel_config.reset_gpio_num = GPIO_NUM_NC;   // 驱动别碰 RST，下面自己来
gpio_set_level(DISPLAY_RST_PIN, 0); vTaskDelay(pdMS_TO_TICKS(100));
gpio_set_level(DISPLAY_RST_PIN, 1); vTaskDelay(pdMS_TO_TICKS(100));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
```

**分辨率是 128×160（实测），不是初始化表窗口的 240×320**：

| 说法 | 结论 |
| --- | --- |
| ❌ "面板是 240×320" | 这是**对初始化表里 `0x2A`/`0x2B` 窗口值的过度解读**。原厂表照抄了某个通用模板的窗口，不等于玻璃的物理尺寸。 |
| ✅ **物理是 128×160** | **实测**（上机出图、几何/色序都对得上）。板级 `config.h` 用 `DISPLAY_WIDTH 128 / DISPLAY_HEIGHT 160`。 |
| 附带教训 | 早期报告 `GPIO-FINDINGS.md` §2.1 记的"160×128"其实**更接近真相**（那附近确实有 `movi a12, 160; movi a13, 128`），当时被当成"文字画布参数"否掉了。**初始化表只能证明控制器型号，不能证明分辨率。** |

**★ 字节序陷阱（最容易误判"屏坏了/颜色错了"）**：

| 通路 | 谁负责字节交换 | 现象 |
| --- | --- | --- |
| LVGL 界面 | `SpiLcdDisplay` 的 `swap_bytes = 1` 已处理 | 颜色正常 ✅ |
| **裸 `esp_lcd_panel_draw_bitmap()`** | **必须自己 `__builtin_bswap16()`** | 不换 ⇒ **红蓝互换**（测试卡就踩过这个坑） |

```cpp
// 裸 panel 写入：面板要"高字节在前"，所以手动交换
const uint16_t c = __builtin_bswap16(bars[(x * 8) / w]);
```

⇒ **那次"方向测试卡颜色不对"，原因是字节序，不是面板错了、也不是 RGB/BGR 配错**；排查这类问题要先分清**是哪条通路在画**。

### 6.2 内存与主频

| 项 | 配置 | 说明 / 陷阱 |
| --- | --- | --- |
| CPU | `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` | 默认是 160 MHz。日志验收：`I cpu_start: cpu freq: 240000000 Hz` |
| PSRAM | `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_SPEED_80M=y` | 日志：`Found 8MB PSRAM device` / `Speed: 80MHz` / `Adding pool of 4096K of PSRAM memory to heap allocator` |
| Flash 频率 | `CONFIG_ESPTOOLPY_FLASHFREQ_80M=y` | 日志：`mode:DIO, clock div:1` |
| Flash 容量 | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` | 日志：`spi_flash: detected chip: gd` / `flash io: dio` |
| PSRAM 可映射量 | 只有 **4 MB** | `W esp_psram: Virtual address not enough for PSRAM, map as much as we can. 4MB is mapped` —— **经典 ESP32 的正常行为**（地址空间限制），不是坏了 |

> **★ `CONFIG_SPIRAM_SPEED_80M` 依赖 `CONFIG_ESPTOOLPY_FLASHFREQ_80M`**：PSRAM 与 Flash 共用同一个 SPI 时钟域，**Flash 跑 40 MHz 时 PSRAM 上不到 80 MHz**，所以这两个必须**成对**设置。实测踩过的现象是：只改了 PSRAM 那一项，而 `sdkconfig` 里 Flash 频率还是旧的 40M ⇒ PSRAM 速度被静默压回去。⇒ **改完删 `sdkconfig` 重新生成，并去 `build\config\sdkconfig.json` 里核对两项**（见 §5.5）。
>
> 原厂镜像头就是 **DIO / 80 MHz / 16 MB**，新固件与之一致最稳。

### 6.3 背光：AW9106B 初始化 + PA（**顺序有讲究**）

`InitializeBacklight()`（板级构造函数里调用）做的事情，按**必须遵守的顺序**列出：

```cpp
i2c_master_bus_config_t bus_cfg = {};
bus_cfg.i2c_port   = I2C_NUM_0;
bus_cfg.sda_io_num = GPIO_NUM_18;      // B220_AW9106_I2C_SDA
bus_cfg.scl_io_num = GPIO_NUM_23;      // B220_AW9106_I2C_SCL
bus_cfg.glitch_ignore_cnt = 7;
bus_cfg.flags.enable_internal_pullup = 1;
i2c_new_master_bus(&bus_cfg, &i2c_bus_);

i2c_device_config_t dev_cfg = {};
dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
dev_cfg.device_address  = 0x5B;        // B220_AW9106_ADDR
dev_cfg.scl_speed_hz    = 100000;
i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev);

static const uint8_t init_seq[][2] = {
    {0x7F, 0x00},  // 软件复位（会清掉整颗芯片配置！）
    {0x11, 0x10},  // CTL
    {0x12, 0x02},  // GPMD_A = 让 group A 处于能驱动 SHDN 的模式（0x08 = 驱动不了，见下）
    {0x04, 0x00},  // GPIO_CFG_A = group-A 全部当普通输出
    // ★ 这里【故意没有】{0x02, ...}：0x02 是 PA 的 SHDN，每次开机只允许出现一次 0→1
    {0x13, 0x00},  // GPMD_B
    {0x20, 0x00}, {0x21, 0x00}, {0x22, 0x00},
    {0x23, 0x00}, {0x24, 0x00}, {0x25, 0x00},
    {0x14, 0x07},  // 呼吸/渐亮渐灭
    {0x05, 0x07},  // GPIO_CFG_B
    {0x17, 0x01}, {0x18, 0x01}, {0x19, 0x01},
    {0x11, 0x1B},  // CTL（原厂尾值）
    // ★ 原厂尾部的 {0x12, 0x08} 在这里被【删掉】了，原因见下
    {0x20, 0x3F},  // DIM0 = 最大  ← 背光
    {0x21, 0x3F},  // DIM1 = 最大  ← 背光
    {0x22, 0x3F},  // DIM2 = 最大  ← 背光
    {0x23, 0x80},  // DIM3（指示灯）
    {0x24, 0x80},  // DIM4（指示灯）
};
```

**★ 被删掉的 `{0x12, 0x08}`（这条是"有声音 vs 没声音"的分水岭）**【实测，`B220_PA_BIT_FINDER` 走查得到】**：`GPMD_A = 0x08` 时 **group-A 输出什么都驱动不了** —— 走查的 1..9 步、包括 `0x02 <- 0xFF`，全都是静音；而 1 kHz 测试音**恰好**在写入 `0x12 <- 0x02` 加 `0x04 <- 0x00` 的那几步变响，并且一直响到关机。⇒ **必须在 PA 使能之后不再改 `0x12`**，否则功放会被悄悄关回去。（翻过来：如果哪天需要 group-A 的 DIM/PWM 引擎，那引擎正是 `0x08` 选中的；既要 DIM 又要 SHDN 的唯一可能是 `0x12 = 0x0A` —— **这个组合在本硬件上从未测试过，本移植不使用**。）

**验收日志**：

```
I (692)  XiaoMoDouB220: AW9106B backlight on (DIM0/1/2 = 0x3F)
I (704)  ili9341: LCD panel create success, version: 2.1.0
I (1182) LcdDisplay: Turning display on
```

**如果屏黑但设备能连服务器**：第一件事就是查 AW9106 这一段（地址 0x5B、SDA18/SCL23、`DIM0/1/2` 是否真写到 0x3F），不要再去翻 GPIO 背光脚 —— **这块板上不存在那个脚**。

### 6.4 WiFi 配网行为（本板无按键 ⇒ 全靠自动进配网）

因为这块板上**没有任何 ESP32 可见的按键**（启动键是底座软开关，GPIO0 被 DSP 片选 + strapping 占用），启动逻辑是：**NVS 里没有凭据就自动进配网 AP**。

```
I (2932) WifiConfigurationAp: Access Point started with SSID Xiaozhi-4CE6
I (3022) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 192.168.4.1
I (3032) WifiConfigurationAp: Web server started
I (3032) WifiBoard: WiFi config mode entered
```

操作：手机连 `Xiaozhi-4CE6` 这个 AP（SSID 后缀取 STA MAC 最后两字节），浏览器开 `http://192.168.4.1` 填 WiFi。配网成功后**凭据进 NVS（分区偏移 0x9000）**，此后每次开机直连。

> ⚠️ 两个连带后果：① 每次 `flash-b220.ps1` **不带 `-FullErase`** 时会保留 NVS ⇒ 保留 WiFi 凭据（这是好事）；② 若跑过原厂固件，原厂在 NVS 里存过 `yangsibei` 这类 SSID/密码（**而且还是明文**），换固件后命名空间不同、不冲突，但**分享备份前记得先清 NVS**。

## 7. 常见误区 / 被证伪的假设

> 这一节是**避坑清单**。

| # | 曾经的结论 | 现状 | 怎么纠正的 |
| --- | --- | --- | --- |
| 1 | ❌ "板上有 DTR/RTS 复位电路，esptool 能自动复位" | **错。** 进下载模式靠**插 micro-USB 时桥把 GPIO0 拉低**。TTL 没接 DTR/RTS ⇒ 永远 `--before no_reset` | 【实测】看到 `waiting for download` 时的接线状态 |
| 2 | ❌ "micro-USB 是刷机口" | **错，它是只读日志口**（另一颗芯片的 USB CDC，TX 根本没接到 UART0） | 【实测】10 项证据；必须飞线到 GPIO1/3/GND |
| 3 | ❌ "面板是 ILI9341 240×320" | **控制器家族对，分辨率错**：物理 **128×160** | 【实测】出图 + 几何确认；240×320 是对初始化表窗口值的过度解读 |
| 4 | ❌ "方向测试卡颜色不对 ⇒ 面板 RGB/BGR 配错了" | **错，是字节序**：裸 `draw_bitmap` 必须 `__builtin_bswap16` | 【实测】改字节交换后颜色正常；LVGL 通路本来就由 `swap_bytes=1` 处理 |
| 5 | ❌ "背光是某个 GPIO / 由底座控制 / 常亮" | **错，是 I2C 上的 AW9106B `DIM0/1/2 = 0x3F`**；LEDC/PWM 在原厂固件里完全不存在 | 【逆向】穷举 GPIO/IOMUX/RTC_IO + 【实测】六路走查时"第 30–34 秒屏亮、中间闪灭一次"闭环 |
| 6 | ❌ "第二条 400 kHz 的 SPI 是 ZL38063 控制口" | **错。** 那是 SD 卡的 SDSPI 分支（SD 卡 SPI 模式本来就从 400 kHz 起步）。**DSP 的 HBI 实际在 SPI2 上与 LCD 共用** | 【逆向】追调用链：`0x40234f90` 只被 `0x401fd919` 调用，所在函数引用 `vfs_fat_sdmmc` 字符串 |
| 7 | ❌ "GPIO19 是图灵 AI 模组的复位" | **是 ZL38063 的复位**（10 ms 低脉冲） | 【逆向】与 DSP 驱动同源代码对齐 |
| 8 | ❌ "GPIO0 可以当 BOOT 按键" | **不行**：它既是 DSP 的 HBI 片选，又是启动 strapping 脚 | 【逆向】`spics_io_num` 未被写入 ⇒ 0；驱动真的用它做片选 |
| 9 | ⚠️ "PSRAM 8 MB 全部可用" | **只有 4 MB 能映射进地址空间**（经典 ESP32 的地址空间限制） | 【实测】`Virtual address not enough for PSRAM … 4MB is mapped`；这是**正常**的 |
| 10 | ⚠️ "改了 `config.json` 就生效" | **不一定**：`sdkconfig` 是持久文件，**旧值会静默保留**。改完删 `sdkconfig` 重新生成，并核对 `build\config\sdkconfig.json` | 【实测】PSRAM/Flash 频率成对设置时踩到 |
| 11 | ❌ "只刷 app 更新就行"（在**跨布局**场景下） | **错，会无限重启**：两套分区布局偏移全不同，app 单独写过去引导器找不到可启动分区。**切换布局方向必须整片擦除 + 全区域写** | 【实测】真的发生过 |
| 12 | ❌ 早期报告说"DSP 固件镜像不在 dump 里、无法恢复，只能假设 DSP 自启动" | **已被推翻两次，最终结论在 PART 2**：镜像确实在 dump 里（DROM `0x3f4322e4`，3699 条 × 44 字节记录）；而**原厂到底是不是 host-boot** 取决于看哪份证据（原厂 60 s 启动日志里**没有** `1- Firmware boot loading started ....`）。**这是本移植中反复摇摆最久的一个点** | 【逆向】+【实测】见 PART 2 |
| 13 | ⚠️ 早期报告说 `VprocTwolfLoadConfig` 有"门控字节，只写 130 条 / 跳过 130 条" | **错。** 循环里只有一个回循环头的分支，**260 条全写**，与 esp-adf 参考实现一致 | 【逆向】字节级定点反汇编 `0x40125c30` |
| 14 | ⚠️ 两份逆向报告对 `prgmBase` / `execAddr` 谁是谁**结论相反** | 以带日志的那份为准（`zl38063_firmware_image.md`）：**`prgmBase = 0x00080200` / `execAddr = 0x00083040`**（`PROTOCOL-REPORT.md` §2.4 写反了）。**这个矛盾直接催生了"启动地址矩阵"实验** | 【逆向】见 PART 2 |
| 15 | ⚠️ "`VprocTwolfFirmwareStart` 是真正让 DSP 跑起来的 GO" | **存疑**：早期逆向把它标成 `ets_delay_us` 空壳；真正可靠的判据是 **`reg 0x0014 == 0x0200`**（原厂自己的路由写入就是拿它当门槛），因为 **`reg 0x0030`（app status）在这颗产品上从来不变** | 【实测】见 PART 2 的释放顺序实验 |

## 8. PART 1 的最小可复现检验清单

按顺序做，每一步都有明确的**通过标准**。任何一步不过，**不要往下走**。

| # | 动作 | 通过标准 |
| --- | --- | --- |
| 1 | `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` + `idf.py --version` | 打印 `ESP-IDF v6.1`（不是 v5.x） |
| 2 | 飞 3 根线（TX→GPIO3 / RX→GPIO1 / GND），万用表确认共地 | 通断正常 |
| 3 | 插 micro-USB + 按启动键开机 | 串口出现 `boot:0x27 (DOWNLOAD_BOOT…)` + `waiting for download` |
| 4 | `& $py @common flash_id` | `Chip is ESP32-D0WD (revision v1.0)` / `MAC: b4:e6:2d:fc:4c:e5` / `Detected flash size: 16MB` / `c8 6018` |
| 5 | `read_flash 0 0x1000000` 整片备份 + MD5 | 16,777,216 B；MD5 = `A6A27D2B4B7A8DE36636E9EF635BEA40`（A 机） |
| 6 | 往空 OTA 槽 `0xc40000` 写 64 KB 再读回 | 两个 MD5 一致 + `Hash of data verified.`；随后 `erase_region` 还原 |
| 7 | `python scripts/build.py xiaomodou-b220 --name xiaomodou-b220` | 构建成功；`build\xiaozhi.bin` 约 2.3–2.4 MB |
| 8 | 查 `build\config\sdkconfig.json` | `esp32` + `16MB` + `partitions/v2/16m.csv` + `SPIRAM=y` + `SPIRAM_SPEED_80M` + `FLASHFREQ_80M` + `CPU_FREQ_240` 全部为真 |
| 9 | 拔 micro-USB，`erase_flash` + 写 5 个区域 | `Hash of data verified.` + 读回 app MD5 与构建产物一致 |
| 10 | `capture-serial.ps1 -Seconds 30` 抓启动日志 | 出现下面全部关键行 |
| 11 / 12 | 屏亮 + 进配网 AP + 配网后激活 | 肉眼看到界面；`Access Point started with SSID Xiaozhi-4CE6` + `http://192.168.4.1`；`Application: Activation done` + `StateMachine: State: activating -> idle` |

**第 10 步的关键日志（逐条对照，缺一不可）**：

```
I (67)  esp_psram: Found 8MB PSRAM device
I (67)  esp_psram: Speed: 80MHz
W (72)  esp_psram: Virtual address not enough for PSRAM, map as much as we can. 4MB is mapped
I (569) cpu_start: cpu freq: 240000000 Hz
I (655) spi_flash: detected chip: gd
I (657) spi_flash: flash io: dio
I (682) Board: UUID=… SKU=xiaomodou-b220
I (692) XiaoMoDouB220: AW9106B backlight on (DIM0/1/2 = 0x3F)
I (692) ili9341: LCD panel create success, version: 2.1.0
I (1182) LcdDisplay: Turning display on
I (3012) WifiConfigurationAp: Access Point started with SSID Xiaozhi-4CE6
```

**允许出现、不必惊慌的四行**（前两行是初始化表覆盖驱动的默认命令；后两行是**原厂固件同样有**的冷启动老毛病，靠看门狗自愈）：

```
W (992)  ili9341: The 36h command has been used and will be overwritten by external initialization sequence
W (992)  ili9341: The 3Ah command has been used and will be overwritten by external initialization sequence
flash read err, 1000
rst:0x10 (RTCWDT_RTC_RESET),boot:0x3f (SPI_FAST_FLASH_BOOT)
```

**PART 1 结束。** 下一部分（PART 2）从 `PHASE3-DSP-DEBUG-STATUS.md` 的"ZL38063 完全不应答 HBI"接着讲，直到 `reg 0x0014 = 0x0200` 与云端多轮对话跑通。
