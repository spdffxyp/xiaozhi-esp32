# 小魔豆 B220 移植小智（xiaozhi-esp32）完整指导书

> 本指导书由一次**已完成的真实移植**总结而成：把 `xiaozhi-esp32`（ESP-IDF v6.1）移植到 **小魔豆 B220 AI 玩具**（ESP32-D0WD + 16 MB Flash + 8 MB PSRAM），
> **完整复用原机的 ZL38063 音频 DSP 通路**（3 麦阵列 → DSP → I2S → ESP32 → 云端 → DSP → DAC → AW8737S → 喇叭），**未增加任何音频硬件**，
> 最终实现：语音唤醒 → 云端对话 → 喇叭回放，并保留随时回滚到原厂固件的能力。

## 如何使用本书

全文约 2900 行，分三部分，**每部分有独立的 § 编号**（合并时未强行重排，以免破坏内部交叉引用）：

| 部 | 标题 | 适合谁 / 什么时候读 | 篇幅 |
|---|---|---|---|
| **第一部分** | 硬件全貌 · 工具与准备 · 刷机纪律 · Phase 0 构建基线 · Phase 2 屏幕/背光/内存 | 拿到设备的第一步：先看清硬件、准备好工具、把刷机与备份的纪律立起来 | 800 行 |
| **第二部分** | **把 ZL38063 音频 DSP 点亮**（本项目最难的一章） | 做音频时反复查阅：I2S 时钟前提、HBI 协议与 page-255 校准、3699 条固件记录 + **1042 条配置**的逐步流程、状态判据陷阱、破坏性操作警告、功放单线脉冲与 `GPMD_A` 陷阱、逐段验收方法 | 1215 行 |
| **第三部分** | 正式 `AudioCodec` · 采样率坑 · 联网与服务器 · **唤醒词** · 排错表 · 方法论 · 验收清单 | 把"能出声"变成"能用"：codec 接入、服务器会话、语音唤醒、以及一份 22 行的合并排错表 | 900 行 |

## 阅读约定

- 全文对非显然结论标注证据等级：**【实测】**（设备上量到/日志里看到）、**【逆向】**（从原厂固件反汇编/数据推导）、**【规格书】**（芯片手册）、**【推断】**、**【被证伪】**（曾经相信但被推翻的假设）。
- 所有日志片段均为**逐字摘录**，并注明来源日志文件。
- 被证伪的假设与未解决项都**明确保留**（未编造）：这部分往往比结论更有价值。

## 快速索引

| 我想…… | 去哪里 |
|---|---|
| 先弄清这台机器有什么 | 第一部分 §2 硬件全貌（20 行清单 + 完整引脚表） |
| 学会安全地刷机/备份/回滚 | 第一部分 §4 刷机与运行纪律 |
| 让屏幕和背光先亮起来 | 第一部分 §6 Phase 2 |
| **让音频链路通** | **第二部分（全章）** |
| 知道 DSP 怎么引导、写哪些寄存器 | 第二部分 §5 主机引导流程 |
| 把固件镜像/配置记录从自己设备里挖出来 | 第二部分 §6 |
| 判断 DSP 到底起没起来 | 第二部分 §7 状态判据陷阱（**别信 `reg 0x0030`**） |
| 避免把 DSP 打回 boot ROM | 第二部分 §8 破坏性操作警告 |
| 修"有信号但喇叭不响" | 第二部分 §9 功放 / `GPMD_A` |
| 接正式 codec / 定采样率 | 第三部分 §1–§2 |
| 连服务器 / 排查"能连上但没声音" | 第三部分 §4 |
| 加语音唤醒 | 第三部分 §5 |
| 遇到问题快速定位 | 第三部分 §7 合并排错表（22 行） |
| 最终验收 | 第三部分 §9 检查清单（12 项） |

---

# 第一部分：硬件 · 工具 · 刷机纪律 · 屏幕与基础外设
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


---

# 第二部分：把 ZL38063 音频 DSP 点亮

# 小魔豆 B220 移植指南 · 第二部分：把 ZL38063 音频 DSP 点亮

> 适用对象：已完成第一部分（屏幕 / 背光 / WiFi / PSRAM / 分区 / 刷机链路）的移植者。
> 本章是整个移植里**最难、耗时最长、最容易被自己的日志骗**的一环。
>
> 证据标注：**【实测】**=真机日志/示波器/万用表/耳朵验证过；**【逆向】**=从原厂
> `backup/parts/wifi.bin` 逐指令反解出来的；**【推断】**=由前两者推出但未单独验证；
> **【被证伪】**=曾写进报告、后来被推翻（见 §12）。
>
> 主要证据：`logs/hbi-diag.log`、`logs/i2s-before-dsp.log`、`logs/boot-addr-matrix.log`、
> `logs/hands-off.log`、`logs/upload-verify.log`、`logs/page255-calib.log`、`logs/pre-go-verify.log`、
> `logs/downlink-tries.log`、`logs/pa-pulse.log`、`logs/final-pa.log`、`logs/mic-loopback.log`、
> `logs/production-codec.log`、`logs/wakeword.log`、`PHASE3-DSP-DEBUG-STATUS.md`、
> `ZL38063-ANALYSIS.md`、`PORTING-FEASIBILITY.md`。

目录：1 [为什么最难](#1-为什么这颗-dsp-是本项目最难的环节) ·
2 [硬件连接](#2-硬件连接与信号) · 3 [⭐I2S 必须先跑](#3--第一条决定性规律i2s-时钟必须先跑起来) ·
4 [HBI 协议](#4-hbi-协议帧格式) · 5 [主机引导流程](#5-主机引导固件的完整流程) ·
6 [镜像与记录的提取](#6-固件镜像与配置记录怎么从自己的设备里取出来) · 7 [⭐状态判据陷阱](#7--状态判据的陷阱) ·
8 [⭐破坏性操作](#8--破坏性操作警告) · 9 [功放与 GPMD_A](#9-功放这一级aw8737s-单线脉冲--aw9106-的-gpmd_a-陷阱) ·
10 [验收方法](#10-验收方法怎么证明每一段都通了) · 11 [排错表](#11-排错表) · 12 [被证伪的假设](#12-被证伪的假设清单) ·
13 [附录](#13-附录)

---

## 1. 为什么这颗 DSP 是本项目最难的环节

### 1.1 它卡在两条音频链路正中间

```
【上行】3×MEMS 麦克风 ──I2S/TDM──► ZL38063 (AEC/NR/波束成形) ──I2S──► ESP32 ──Opus──► WiFi ──► 云 ASR
【下行】云 TTS ──WiFi──► ESP32 ──I2S──► ZL38063 (cross-point) ──DAC──► AW8737S ──► 喇叭
```

两条路都从这颗芯片过。它不是"可选外设"，而是**唯一通路**：

| 对比项 | 常见小智板（ES8311/ES7210 之类） | 小魔豆 B220（ZL38063） |
| --- | --- | --- |
| 上电后能不能出声 | 能，codec 自己跑，I2C 配几个寄存器就有声音 | **不能**，DSP 内部是空 RAM，必须由 ESP32 灌固件 |
| ESP32 的角色 | I2C 配置者 + I2S 主控 | **I2S 主控 + HBI 主机 + 引导者**（三重身份） |
| 失败表现 | 音质不对 | **完全没声音、HBI 全部读 `0x0000`** |
| 有没有替代路径 | 有（换个 codec 驱动） | **没有**，麦克风进不来、喇叭出不去 |

### 1.2 三个"先有鸡才有蛋"的死结

1. **DSP 的时钟从 ESP32 的 I2S BCLK 来。** 本板**没有 MCLK 引脚**（原厂 `i2s_pin_config_t` @ DROM
   `0x3f4317ac` 只带 bck/ws/dout/din）【逆向】。ESP32 不先把 I2S 主模式跑起来，DSP 连 boot ROM 都出不来（§3）。
2. **DSP 的程序不在 DSP 里。** 原厂每次上电都通过 HBI 把 118,244 字节镜像 + 1042 对配置推下去
   【逆向，`zl38063_firmware_image.md`】。
3. **HBI 和 LCD 共用 SPI2。** 总线由 LCD `spi_bus_initialize()` 占有，DSP 只是同一条 SCLK/MOSI/MISO 上的
   **第二个 device**，片选 **GPIO0** —— 而 GPIO0 又是 ESP32 的启动 strapping 脚。

### 1.3 为什么它"骗人"

调试中会出现大量**看起来成功、其实没成功**的状态：

| 现象 | 真实含义 |
| --- | --- |
| 3699 条记录全部"传输成功" | 只证明 HBI 出现了，不证明 DSP 在跑 |
| `reg 0x0030` 一直是 `0x0000` | 本产品上它**永远是 0**，不是判据（§7） |
| DAC1_P/DAC1_M 上有 1 kHz 方波 | 只证明 DSP 的 DAC 在动，喇叭可能一点声都没有（§9） |
| I2S TX `write_err=0`、`tx_peak=32759` | 只证明 ESP32 发出了数据，不证明 DSP 收了 |
| 读 `reg 0x0034` 得到 `0xD3D3` | 证明 DSP **正坐在 boot ROM 里**，而不是在跑应用（§7） |

**结论：必须"一段一段证明"，任何一步的"成功"都不能外推。** §10 的验收方法就是为此设计的。

---

## 2. 硬件连接与信号

### 2.1 引脚总表【逆向 + 实测】

| 功能 | 引脚 | 方向（相对 ESP32） | 说明 |
| --- | --- | --- | --- |
| **HBI SCLK** | **GPIO32** | 输出 | 与 LCD 共用，10 MHz |
| **HBI MOSI** | **GPIO33** | 输出 | 与 LCD 共用 |
| **HBI MISO** | **GPIO34** | 输入 | 仅输入脚（ESP32-D0WD 的 34/35 无内部上下拉） |
| **HBI CS（DSP 片选）** | **GPIO0** | 输出 | 见 §2.3，**同时是启动 strapping 脚** |
| LCD CS / DC / RST | GPIO27 / 21 / 22 | 输出 | 与 DSP 同总线 |
| **DSP RESET** | **GPIO19** | 输出 | 低 10 ms → 高，之后一直保持高 |
| **I2S BCLK** | **GPIO5** | 输出 | ESP32 是主：48 kHz × 2 slot × 16 bit = **1.536 MHz** |
| **I2S WS** | **GPIO25** | 输出 | **48 kHz** |
| **I2S DOUT（ESP32→DSP）** | **GPIO26** | 输出 | 下行 |
| **I2S DIN（DSP→ESP32）** | **GPIO35** | 输入 | 上行，仅输入脚 |
| I2C0 SDA / SCL（AW9106B @0x5B） | GPIO18 / GPIO23 | 双向 | 100 kHz，背光 + 功放 SHDN |
| HBI 总线 | **SPI2_HOST** | — | LCD 拥有总线，DSP 绝不可 `spi_bus_free()` |

### 2.2 HBI = SPI2 上共用总线的第二个 device【逆向】

原厂 `spi_lcd`（`0x40203554`）装好 LCD 自己的 device 后**又 `spi_bus_add_device` 一次**，
句柄写进 DRAM 全局 `0x3ffc4c98`；VPROC 的 HBI 读写（`0x40125862`/`0x401258cf`）正是从这个全局取句柄。

| 项目 | 值 | 证据 |
| --- | --- | --- |
| host | `SPI2_HOST` | `0x4020369c: movi.n a2, 6` |
| clock | **10 000 000 Hz**（`0x00989680`） | `0x4020368d` 载入该常量 → devcfg `+8` |
| mode | **0** | devcfg 被 `memset` 清零，`+20` 未被改写 |
| CS | **0**（GPIO0） | devcfg `+16`（`spics_io_num`）**从未被写入** ⇒ 0；随后 `gpio_set_level(0, …)` 直接操作 GPIO0 |
| queue_size | 6 | `0x4020369c` |
| 一次 CS 拉低的传输 | **16 bit = 1 个 16 位字** | 事务 `length` 写 16（bit），不是字数 |

> ⚠️ 上游 esp-adf 参考实现写的是 **1 MHz**（`ref/zl38063/api_lib/vproc_common.c`，
> `.clock_speed_hz = 1000000`）。B220 原厂镜像里真是 10 MHz —— 厂商改过。
> 以 **10 MHz** 为准；实测 1 MHz 也能通（§3.2 的扫描），但没必要降速。

### 2.3 CS = GPIO0 的三个后果【实测 + 逆向】

1. **不能再把 GPIO0 当"小智的 BOOT 按键"。** 它被 DSP 片选占用；B220 本来也没有可读按键
   （电源键是底座软锁存），所以本项目最终靠"唤醒词 + 网页配网"启动会话。
2. **GPIO0 是启动 strapping 脚，复位瞬间必须为高。** 原机靠 USB 桥的 DTR/RTS 电路把它拉低进下载模式；
   刷机与运行不冲突，但**调试时不要把 GPIO0 直接短到 GND**。
3. **CS 在不通信时必须悬空**，绝不能常驻驱动：
   `err = gpio_set_pull_mode((gpio_num_t)cs_gpio, GPIO_FLOATING);`

### 2.4 复位脚 GPIO19【逆向；示波器复核过】

原厂 `0x40124418` 逐指令：`pin_bit_mask = 1<<19` → `mode = GPIO_MODE_OUTPUT`、pull 全关 →
`gpio_config()` → `gpio_set_level(19, 0)` → **`vTaskDelay(10)`（10 ms）** → `gpio_set_level(19, 1)`。
即 **配置输出 → 拉低 → 10 ms → 拉高 → 保持高**。随后 `0x40124454` 再 `vTaskDelay(500)`（500 ms）
才第一次碰 HBI，并且最多重试 **6** 次。

用户示波器实测原厂机：**电源开关关时 GPIO19 为低，按下开机后拉高并一直保持高**【实测】。
⚠️ **GPIO19 的读回值不可信**：取证日志里有 `GPIO19 output, no pulls, driven HIGH -> read 0`
【实测，`logs/boot-addr-matrix.log`】。**一律以示波器为准**，不要用读电平判断复位是否释放。

### 2.5 通断验证方法（万用表，设备断电）

```text
0) 设备完全断电、电池拔掉、USB 不插（否则 ESP32 会通过 IO 脚给对端供电，读数会骗人）
1) 万用表打到"通断/蜂鸣"档
2) 先拿已确证的 LCD 网络当标尺，验证找法没错：
     通断测模组 GPIO32/33/34 ↔ LCD 的 SCL/SDA/SDO，GPIO27 ↔ LCD 的 CS
   —— 对得上，说明你认引脚的方法是对的
3) 再推未确证的：
     GPIO0  ↔ ZL38063 的 HCS / SPI-CS 脚      GPIO19 ↔ ZL38063 的 RESET 脚
     GPIO5  ↔ I2S BCLK / SCLK                 GPIO25 ↔ I2S WCLK / WS
     GPIO26 ↔ I2S SDIA（数据入）              GPIO35 ↔ I2S SDOA（数据出）
4) QFN/BGA 探针够不到球脚时，从过孔 / 0 Ω 电阻 / 测试点上量。
```

**为什么必须做**：静态分析能给出"结构体字段 = 0 ⇒ CS = GPIO0"这样的强证据，但**给不出
"这根线真的连到 DSP 的 HCS"**。这一步是把"逆向结论"升级成"电气事实"的唯一廉价手段
【实测方法，`GPIO-FINDINGS.md` §5.3 同款做法】。

---

## 3. ⭐ 第一条决定性规律：I2S 时钟必须先跑起来

> **规则：在拉 GPIO19 复位、进 boot ROM、灌固件之前，ESP32 的 I2S0 主模式必须已经在输出
> BCLK/WS，而且此后到关机为止都不能停。**
> 顺序是 **[I2S 起] → [GPIO19 复位脉冲] → [HBI 主机引导]**，不是反过来。

### 3.1 症状：HBI 全部读回 `0x0000`

最初的固件只做 HBI、不碰 I2S。`logs/hbi-diag.log`（早期版本）：

```
I (1674) ZL38063: host boot: 3699 records, prgmBase 0x00080200, execAddr 0x00083040
E (1974) SPI: ERROR: HBI is not accessible (0x0000, want 0xd3d3)
E (1974) ZL38063: firmware transfer failed (17): the DSP did not enter boot ROM (reg 0x34 != 0xD3D3) or an HBI access failed
I (2014) ZL38063: swapped reg 0x0006 = 0x0000   （0x0020/0x0022/0x0024/0x0030/0x0032/0x0034/0x012C 同）
I (2054) ZL38063: raw    reg 0x0006 = 0x0000   （另一种字节序也是 0x0000）
I (2094) ZL38063: SCLK GPIO32 = 0    MOSI GPIO33 = 0    MISO GPIO34 = 0
I (2104) ZL38063: CS   GPIO0  = 1          <-- 片选空闲电平是对的
I (2104) ZL38063: RST  GPIO19 = 0   (expect 1)
```

读要点：`0x0000` 在**交换字节序和原始字节序两种读法下都是 0x0000** ⇒ 不是字节序问题
（字节序错的读法会读出 `0x0100` 这类可见交换过的值，§4.2 就是据此判断的）；`CS GPIO0 = 1`
⇒ 片选没卡死；40 个寄存器全 0、MISO 恒定 ⇒ **对端根本没在驱动 MISO**，不是"读错地址"。

### 3.2 怎么找到的

**第一步：穷举扫描**（`B220_ZL_DIAG` / `zl38063_dsp_hbi_sweep()`）。把 `cs × mode × clock` 全扫一遍，
每个组合只做**只读**访问，读 `reg 0x34`，期望 `0xD3D3`：

```
I (30062) ZL38063: sweep HIT: cs=0 mode0 10MHz reg0x34=0xD3D3 reg0x30=(vproc error 0)
I (30072) ZL38063: sweep HIT: cs=0 mode0  1MHz reg0x34=0xD3D3 reg0x30=(vproc error 0)
I (30092) ZL38063: sweep HIT: cs=0 mode3 10MHz reg0x34=0xD3D3   （cs=0 mode3 1MHz 同）
I (30672) ZL38063: sweep HIT: cs=27 mode0  1MHz reg0x34=0xC760 reg0x30=0xC760  <-- 串扰：两个寄存器同值
I (31032) ZL38063: ===== sweep done: 60 combos, 8 hits =====
I (31032) ZL38063: sweep summary: CS with a non-0x0000/0xFFFF value: 0,2,13,19,27
```

结论（`logs/i2s-before-dsp.log`）：**只有 CS = GPIO0** 能读到 boot ROM 魔数 `0xD3D3`；其它 CS
（2/13/19/27）读到的是**别的电路的串扰**，判据是"两个不同寄存器读出同一个值"（GPIO27 是 LCD 的 CS，
日志里同时有 `W spi_common: GPIO 27 is conflict with others and be overwritten`）；
mode 0 与 mode 3、10 MHz 与 1 MHz 都能通（mode 0 / 10 MHz 仍是首选）。

**第二步：hands-off 对照**（`B220_DSP_HANDS_OFF`，`logs/hands-off.log`）。只挂 HBI + 写一次 `0xFD80`，
**完全不动 GPIO19、不写 reg 0x14、不灌任何东西**，然后每秒只读一次状态、连续 20 秒：

```
I (1174) XiaoMoDouB220: I2S up before DSP boot: 48 kHz, 16-bit, stereo (bck=5 ws=25 dout=26 din=35)
I (1184) ZL38063_HBI: HBI attached to SPI2 (cs=0, 10000000 Hz, mode 0)
W (1194) ZL38063: hands-off: HBI attached (cs=0) and HBI config written; GPIO19 (reset) untouched, no reset pulse, no reg 0x14 <- 1, no host-boot upload ...
I (2204) ZL38063: hands-off +1s: 0x0030=0x0000(app status: not running) 0x0032=0x0000(host command) 0x0034=0xD3D3(cmd result) 0x0006=0xFFFE(host flags) 0x0020=0x0101(id word 0) 0x0022=0x022A(id word 1) 0x0024=0x0000(id word 2)
   （+2s .. +20s 每一行完全相同）
W (21204) XiaoMoDouB220: hands-off verdict: DSP application never reported running within 20 s
```

| 问题 | 答案 | 证据 |
| --- | --- | --- |
| DSP 会不会自己启动（Auto Boot）？ | **不会**。20 秒后 `0x0034` 仍是 `0xD3D3`（"我在 boot ROM 等你灌"） | 上方日志 |
| 在有 I2S 时钟的前提下，不碰复位就能读通 HBI 吗？ | **能**。`0x0034=0xD3D3`、`0x0020=0x0101`、`0x0022=0x022A` 全是稳定真值 | 上方日志 |

**第三步：把顺序写进代码。** 板级初始化把 I2S 提到 DSP 引导之前：

```
I (1212) XiaoMoDouB220: I2S up before DSP boot: 48 kHz, 16-bit, stereo (bck=5 ws=25 dout=26 din=35)
I (1222) ZL38063: reset: GPIO19 low 10 ms          <-- 顺序：I2S 先起，再复位
I (1732) ZL38063_HBI: HBI attached to SPI2 (cs=0, 10000000 Hz, mode 0)
```

**第四步：为什么**【推断，机制未在硅片层面确证】。本板 DSP **没有 MCLK 引脚**，时钟只能从 I2S BCLK
派生；没有时钟，boot ROM 的 HBI 状态机就跑不起来，对端自然既不更新 `0x0034` 也不驱动 MISO。
原厂自己的上电顺序也是 `[codec/I2S init] → [GPIO19 复位脉冲] → [host boot over HBI]`
【逆向，`0x40124454`】。

> ⚠️ **诚实标注一条边界条件**：在 `logs/i2s-before-dsp.log` 里 **I2S 已经起来了**（第 61 行），
> 但紧接着的主机引导 + 6 次重试里 HBI 仍全部读 `0x0000`（第 137–158 行），直到扫描在 30.0 s
> **重新绑定了一次 HBI device** 之后才读出 `0xD3D3`（第 191 行）。
> 也就是说：**这一次的"翻转点"是扫描里的重新挂载，而不是 I2S 使能本身。**
> "BCLK 缺失"与"attach 之后需要一次重新选择"这两条解释在现有日志里**没有被彻底分离**。
> 实践建议：**两边都满足** —— I2S 先起；且 HBI 首次 attach 后如果读到 `0x0000`，
> 用 `zl38063_hbi_rebind()` 重挂一次再读（比"再复位一次 DSP"便宜得多，且不破坏状态）。

### 3.3 I2S 的具体配置（照抄即可）

```c
chan_cfg.id = I2S_NUM_0;  chan_cfg.role = I2S_ROLE_MASTER;   /* TX 通道真正产生时钟 */
chan_cfg.dma_desc_num = 6; chan_cfg.dma_frame_num = 240;      /* 240 帧 = 5 ms @48 kHz */
chan_cfg.auto_clear_after_cb = true;                          /* DMA 饿死时继续送 0，别停钟 */
std_cfg.clk_cfg.sample_rate_hz = 48000;  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;    /* 32 BCLK/帧 */
std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
std_cfg.slot_cfg.ws_width = 16; std_cfg.slot_cfg.ws_pol = false; std_cfg.slot_cfg.bit_shift = true;
std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;                       /* ★ 本板没有 MCLK */
std_cfg.gpio_cfg.bclk = GPIO_NUM_5;  std_cfg.gpio_cfg.ws = GPIO_NUM_25;
std_cfg.gpio_cfg.dout = GPIO_NUM_26; std_cfg.gpio_cfg.din = GPIO_NUM_35;
```

四个必须照抄的细节：

1. **`i2s_new_channel()` 必须同时给出 tx 和 rx 两个 handle**，否则 ESP32 的 I2S 不会组成全双工对；
   组成之后驱动会把 RX 降级为 slave 并在片内环回 BCLK/WS，所以**TX 才是产生时钟的那一半，
   必须一直 enable 且一直被喂数据**。
2. **先 `i2s_channel_preload_data()` 再 `i2s_channel_enable()`**：TX 的 DMA 描述符链不能是空的。
3. **TX 先 enable，RX 后 enable**（RX 失败不致命，TX 保住时钟就行）。
4. **BCLK 预期 1.536 MHz** = 48 kHz × 2 slot × 16 bit，即 32 BCLK/帧。原厂机示波器实测
   **BCLK ≈ 1.54 MHz、WS = 48 kHz** ⇒ 16 bit slot 得到确认【实测】。量到 3.072 MHz 说明 slot 被配成 32 bit 了。

---

## 4. HBI 协议（帧格式）

### 4.1 命令字编码（逐位）

```c
#define HBI_PAGED_READ(offset,length)    ((uint16)(((uint16)(offset) << 8) | (length)))
#define HBI_DIRECT_READ(offset,length)   ((uint16)(0x8000 | ((uint16)(offset) << 8) | (length)))
#define HBI_PAGED_WRITE(offset,length)   ((uint16)(HBI_PAGED_READ(offset,length)  | 0x0080))
#define HBI_DIRECT_WRITE(offset,length)  ((uint16)(HBI_DIRECT_READ(offset,length) | 0x0080))
#define HBI_CONFIGURE(pinConfig)         ((uint16)(0xFD00 | (pinConfig)))
#define HBI_SELECT_PAGE(page)            ((uint16)(0xFE00 | (page)))
```

| 位域 | 含义 |
| --- | --- |
| `[15]` | 0 = 分页访问（先发 SELECT_PAGE），1 = 直接访问（page 0） |
| `[14:8]` | **7 位寄存器偏移，单位是"字"** = `(addr & 0xFF) / 2` |
| `[7]` | 0 = 读，1 = 写 |
| `[6:0]` | **字数 − 1**（所以"最大 126 字"= 7 位字段最大 126） |

地址解码【逆向，`spis_tw_hbi_wr16_cmd`/`spis_tw_hbi_rd16_cmd` 逐指令核对】：

```c
page = addr >> 8;   offset = (addr & 0xFF) / 2;   /* 寄存器地址是"字偏移"，不是字节偏移 */
```

页号特例：**page 0xFF 保持自己的编号**，page 1..254 会**先减 1** 再发 `HBI_SELECT_PAGE(page-1)`
（被 `0xfffffe00` 掩码截成 16 位）【逆向，conf 高】。

### 4.2 每个 16 位量都必须交换字节序（双向，线上大端）

原厂不是逐字节算，而是把每个 16 位字**整字交换**再塞进 SPI buffer：`0x40256d5c` 就是
`__builtin_bswap16(x)`（`slli 8` / `srli 8` / `or`）。调用点：**读**路径在 `hbi_read16_buf`（`0x401258ea`）
对收到的字调用一次；**写**路径在 `hbi_write16`（`0x40125a7c`）对命令字和每个数据字各调用一次。
**命令字和数据字都要换，两个方向都要换。**

**怎么用日志判断字节序对不对**（本项目最省事的自检）——同一批寄存器用"交换"和"不交换"两种读法各读一遍：

```
I (29772) ZL38063: swapped reg 0x0006 = 0xFFE0      I (29812) raw reg 0x0006 = 0x0000
I (29772) ZL38063: swapped reg 0x0020 = 0x0101      I (29822) raw reg 0x0020 = 0x0000
I (29792) ZL38063: swapped reg 0x0022 = 0x94AF      I (29822) raw reg 0x0022 = 0x0000
I (29792) ZL38063: swapped reg 0x012C = 0x0008      I (29842) raw reg 0x012C = 0x0000
```

- 交换侧读出**成对出现的值**（`0x0101`、`0xFFE0`…）而原始侧全 `0x0000` ⇒ **交换是对的**。
- 两边都是 `0x0000`/`0xFFFF` ⇒ **MISO 根本没被驱动**，问题在电气/时钟，不在字节序。
- 交换侧是原始侧的半字互换（`0x0001` vs `0x0100`）⇒ **交换开关搞反了**。
- `0x0BAD` 是原厂读临时变量的初值；日志里出现它 ⇒ 这次读**根本没执行/没返回**。

### 4.3 每条命令的线上字节

以"读 `reg 0x0034` 一个 16 位字"为例：`page=0x00`、`offset=(0x34&0xFF)/2=0x1A`、`n-1=0`
⇒ `HBI_DIRECT_READ(0x1A,0) = 0x9A00`，线上 `9A 00`（高字节先）；随后每读 1 个字 =
独立的一次 CS 拉低 + 16 个时钟。常用命令字（**交换后实际出现在 MOSI 上的字节**）：

| 操作 | HBI 命令字 | 线上字节 |
| --- | --- | --- |
| 读 `0x0034` / `0x0030` / `0x0014` / `0x0006`（各 1 字） | `0x9A00` / `0x9800` / `0x8A00` / `0x8300` | `9A 00` / `98 00` / `8A 00` / `83 00` |
| 写 `0x0014 <- 1` | `0x8A80` | `8A 80`，然后 `00 01` |
| 写 `0x0032 <- 0x000D` / `<- 0x0008` | `0x9980` | `99 80`，然后 `00 0D` / `00 08` |
| 写 `0x0006 <- 0x0002`（软复位） | `0x8380` | `83 80`，然后 `00 02` |
| 写 `0x000C`（2 字 = 窗口基址） | `0x8680` | `86 80`，然后 `hi lo` |
| 写 `0x012C`（2 字 = 入口点） | `0x9680` | `96 80`，然后 `08 30`、`00 40` |
| HBI 端口使能（`HBI_CONFIG_REG|VAL`） | `0xFD80` | `FD 80` |
| 选页 0xFF | `0xFEFF` | `FE FF` |
| 窗口写 `0xFF24`（16 字） | 解码 → `0xFEFF` + `0x128F` | `FE FF`、`12 8F`，然后 16×数据字 |

### 4.4 每个字一次 CS，最大 126 字

- **每条 16 位命令 / 每个数据字都有自己的 CS 拉低**（一次 `spi_device_transmit`），不是多字突发。
  证据：原厂 `t.length = 16`（bit），循环体里每个字调用一次 16 位 helper【逆向】。
- **写操作单次最多 126 字**：`0x40125a82: addi.n a5,a3,-1`（`numwords-1`）→
  `0x40125a87: movi a9,125` → `0x40125a8d: bgeu a9,a8,ok`，否则打印
  `"number of words is out of range. Maximum is 126"` 并返回 **13**。读侧没有这种硬上限
  （上游 CLI 自己限制在 128）。
- **忙等待语义**：`hbi_block_read(0x06,1,&tmp)`，只要 `tmp & 0xFFFF0000` 非 0 就重试，
  最多 **1000** 次、每次间隔 **10 000 µs** ⇒ 最长 10 秒；超时返回 **19**。

### 4.5 page-255 窗口：写容易，读没有参考实现

**写**（原厂 `HbiSrecBoot_alt` 的两条命令）：

```c
gTargetAddr[0] = (targetAddr >> 16) & 0xFFFF;      /* 1. 32 位目标地址写进 reg 0x000C，2 字，先高后低 */
gTargetAddr[1] =  targetAddr        & 0xFFFF;
VprocTwolfHbiWrite(0x000C, 2, gTargetAddr);
VprocTwolfHbiWrite(0xFF00 | (targetAddr & 0xFF), numWords, buf);   /* 2. 数据走窗口 */
```

**关键陷阱（不理解就会读回一堆垃圾）**：`zl38063_hbi_write()` 把任何 16 位地址解码成
`page = addr>>8, offset = (addr & 0xFF)/2`。所以它看到 `0xFF24` 时实际发出的是：

```text
page   = 0xFF   -> SELECT_PAGE(0xFF) = 0xFEFF  （page 0xFF 不减 1）
offset = (0x24)/2 = 0x12
cmd    = HBI_PAGED_WRITE(0x12, 15) = 0x128F
```

即：**设备收到的偏移字段是"低字节 ÷ 2"，也就是窗口内的字偏移，不是字节偏移。**

**读**：Microsemi 的驱动**只写窗口、从不读窗口**（`adf_ref/` 里搜不到任何窗口读实现）【逆向】。
读侧编码只能**自己标定**（`B220_DSP_PAGE255_CALIB`）：

```text
1) 选一个"图像没写过、但确定有 RAM"的 scratch 地址：0x00080A24
   —— 它是 3699 条记录覆盖范围 0x00080128..0x0009f4e6 里唯一空洞（0x00080A24..0x00082FFF，
      9694 B）的第一个字，空洞两侧都有记录 ⇒ 那里有 RAM。低字节 0x24 是偶数（合法字边界），
      且两种候选编码在它上面结果不同（0x24 字节 vs 0x12 字）。
2) 用与上传完全相同的写路径写入 16 个已知字：pattern[i] = 0xC0DE ^ (0x1111 * i)
   -> C0DE D1CF E2FC F3ED 849A 958B A6B8 B7A9 4856 5947 6A74 7B65 0C12 1D03 2E30 3F21
   （不含 0x0000 / 0xFFFF，所以死 MISO 或没上电都匹配不上）
3) 按 2(编码) × 2(是否先发 SELECT_PAGE) × 2(reg 0x0C 两字顺序) = 8 种组合读回，数匹配个数
```

`logs/page255-calib.log` 的结果：

```
I (42024) page255 calib: write = upload path: reg 0x0C <- {0x0008,0x0A24} (2 words) then VprocTwolfHbiWrite(0xFF24, 16) -> SELECT_PAGE(0xFF)=0xFEFF then PAGED_WRITE(offset field 0x12, 15)=0x128F then 16 data words
I (42044) page255 calib: reg 0x0C reads back {0x0008,0x0A00} (wrote {0x0008,0x0A24})
I (42054) calib enc=b preselect=1 base=hi_lo -> matched 16/16 (read C0DE D1CF E2FC F3ED, expected C0DE D1CF E2FC F3ED)
I (42084) calib enc=b preselect=0 base=hi_lo -> matched 16/16
I (42074) calib enc=a preselect=1 base=hi_lo -> matched 0/16 (read 0000 0000 0000 0000, ...)
I (42114) calib enc=b preselect=1 base=lo_hi -> matched 0/16
I (42154) page255 calib: scale probe target 0x00080A26 (low byte 0x26 -> field 0x13) read D1CF E2FC, pattern[1..2] = D1CF E2FC -> field+1 steps one WORD (low byte is a byte offset, field = low byte / 2)
I (42174) page255 calib: CONCLUSION enc=b preselect=1 base=hi_lo reproduces the pattern 16/16 -> a window READ must put (targetAddr & 0xFF) / 2 in command bits 14..8; the verify will use enc=b
```

**读窗口的正确编码（已实测标定）**：`Z = targetAddr & 0xFF` → 发 `0xFEFF`（SELECT_PAGE 0xFF）→
发 `HBI_PAGED_READ(Z/2, n-1)` → 读 n 个 16 位字（每个字一次 CS）。

> 顺带一个实测出来的怪事：往 `reg 0x0C` 写 `{0x0008, 0x0A24}`，读回是 `{0x0008, 0x0A00}` ——
> **低 5 位被抹掉了**（32 字节对齐）【实测】。不影响上传（窗口写用的偏移字段本来是 `0x24/2 = 0x12`），
> 但**做读回校验时不要拿 `reg 0x0C` 的读回值去反推地址**。

### 4.6 用到的寄存器

| 寄存器 | 名字 | 用法 |
| --- | --- | --- |
| `0x0006` | `HOST_SW_FLAGS_REG` | 邮箱/主机标志。`0x0001` = host 拥有命令寄存器；写 `0x0002` = 软复位 |
| `0x000C` / `0x000E` | 窗口基址高/低半 | page-255 目标地址 |
| `0x0014` | `CLK_STATUS_REG` | **写 1 = 强制进 boot ROM（破坏性！§8）**；应用就绪后应读作 `0x0200` |
| `0x0030` | "app status" | 本产品**永远 0**，不是判据（§7） |
| `0x0032` | `HOST_CMD` | `0x0000` 空闲 / `0x0008` GO / `0x000D` host load complete / `0x8000` 停止固件 / `0xFFFF` 进行中 |
| `0x0034` | `HOST_CMD_PARAM_RESULT` | boot ROM 魔数 `0xD3D3` |
| `0x0020`/`0x0022`/`0x0024` | boot ROM id 块 | 实测 `0x0101` / `0x022A` 或 `0x94AF` / `0x1407`（随状态变） |
| `0x012C` | 执行入口点 | 2 个字，先高后低 = `execAddr` |
| `0xFF24` 之类的窗口地址 | page-255 数据窗口 | 见 §4.5 |

---

## 5. 主机引导固件的完整流程

### 5.1 原厂顺序与逐字实现对照

| # | 动作 | 精确参数 | 来源 |
| --- | --- | --- | --- |
| 0 | **I2S0 主模式起来** | 48 kHz / 16 bit / stereo / BCLK 5, WS 25, DOUT 26, DIN 35 | §3 |
| 1 | **GPIO19 复位脉冲** | 配置输出 → 低 **10 ms** → 高 | `0x40124418` |
| 2 | **等 500 ms** | `vTaskDelay(500)` | `0x4012446b: movi a10, 0x1f4` |
| 3 | **HBI attach + 端口使能** | `spi_bus_add_device(SPI2, cs=0, 10 MHz, mode 0)`，然后写 **`0xFD80`** | `VprocTwolfHbiInit()` |
| 4 | **强制进 boot ROM** | **`reg 0x0014 <- 0x0001`**（1 字） | `VprocTwolfHbiBootPrepare()` |
| 5 | **等 300 ms** | 原厂是 `ets_delay_us(300000)` | `0x40125c8a` |
| 6 | **读 `reg 0x0034` 并要求 `0xD3D3`** | 不等 ⇒ `"ERROR: HBI is not accessible"`，返回失败 | `0x40125cc8` |
| 7 | **灌 3699 条固件记录** | 见 §5.2 | `HbiSrecBoot_alt()` |
| 8 | **写入口点 `reg 0x012C <- execAddr = 0x00083040`** | 2 个字：`0x0008`、`0x3040` | `0x40125d99` |
| 9 | **写 `reg 0x0032 <- 0x000D`**（host load complete） | 1 字 | `0x40125f8c` |
| 10 | **灌 1042 对配置记录** | 见 §6.4 | `VprocTwolfLoadConfig()` |
| 11 | **写 `reg 0x0032 <- 0x0008`（GO）** | 1 字 | `VprocTwolfFirmwareStart()` |
| 12 | **`VprocTwolfReset(VPROC_RST_SOFTWARE)`** | **`reg 0x0006 <- 0x0002`**，然后 **200 ms** | `zl38063_vproc.c:899` |

> 本项目在 `B220_DSP_ADF_RELEASE_TX` 路径里把上传路径自带的 `0x000D` 与 `0x0008` **在调用点抑制掉**，
> 由 release 序列按上表显式发出（`zl38063_vproc_release_cmd_host_load_complete_from_host_boot(true)` /
> `zl38063_vproc_firmware_start_from_host_boot(true)`），这样"谁发了哪条命令"在日志里一目了然。

### 5.2 3699 条固件记录怎么写（44 字节步长）

记录结构（与原厂同构，`sizeof == 44`，41 字节向上 4 字节对齐）【逆向，`zl38063_firmware_image.md` §2】：

```c
typedef struct {            /* 44 字节 */
    uint16_t buf[16];       /* +0  .. +31   数据字（小端） */
    uint16_t numWords;      /* +32 .. +33，+34..+35 填充 */
    uint32_t targetAddr;    /* +36 .. +39   DSP 侧 RAM 地址 */
    uint8_t  useTargetAddr; /* +40，+41..+43 填充 */
} twFwr;                    /* 与 adf_ref/vproc_data_types.h 完全一致 */
```

原厂循环（`0x40125cfd` 起，逐指令）+ C 侧等价实现（`zl38063_vproc.c: HbiSrecBoot_alt()`）：

```
40125cfd: addx4 a2, a6, a6      ; a2 = 5*i
40125d00: addx2 a2, a2, a6      ; a2 = 11*i
40125d05: slli  a2, a2, 2       ; a2 = 44*i     <-- 步长 44
40125d08: add.n a3, a3, a2      ; a3 = &record[i]
40125d0a: l32i.n a8, a3, 36     ; targetAddr
40125d1d: l16ui a8, a3, 32      ; numWords   == 0 -> 跳过
40125d30: l8ui  a3, a3, 40      ; useTargetAddr == 0 -> 跳过"设基址"
40125d36: hbi_write16(0x00C, 2, {hi16(target), lo16(target)})
40125d68: hbi_write16(0xFF00|lowbyte(target), numWords, buf)

while (index < st_firmware->twFirmwareStreamLen) {          /* 3699 */
    gTargetAddr[0] = (targetAddr >> 16) & 0xFFFF;
    gTargetAddr[1] =  targetAddr       & 0xFFFF;
    if (rec.numWords != 0) {
        if (rec.useTargetAddr) VprocTwolfHbiWrite(0x000C, 2, gTargetAddr);
        TwolfHbiPage255Write(0xFF, lowbyte(targetAddr), rec.numWords, rec.buf);
    }
    index++;
}
```

**数值体检（用来证明你取到的镜像不是垃圾）**【逆向/实测，`zl38063_firmware_image.md`】：

| 指标 | 值 |
| --- | --- |
| 记录数 / 步长 | **3699**（`0x0E73`，u16 @ `0x3f4322d8`）/ **44** |
| 数组字节 / 载荷字节 | 3699 × 44 = **162 756** / **118 244**（= 59 122 字） |
| `numWords` 直方图 | `{16:3692, 12:2, 10:1, 6:2, 2:2}` |
| `useTargetAddr == 1` | **3699/3699** |
| 覆盖的 DSP 字地址 | `0x00080128 .. 0x0009F4E6`，**零重叠** |
| 唯一空洞 | `0x00080A24 .. 0x00082FFF`（9694 B） |
| 零字占比 | 8.6%（`0xFFFF` 占 0.1%）⇒ 真代码/系数，不是填充图案 |
| 数组末尾 | **正好等于配置记录的长度字** `0x3f459ea8`（独立交叉验证） |
| 传输耗时 | I2S 起 + 复位 + 完整上传一轮 ≈ **3.1 s**（`1222 → 4852`）【实测】 |

### 5.3 "怎么知道它成功了"

按阶段给判据，**不要跳级**：

```
[复位 + attach 之后]
  I (1732) ZL38063_HBI: HBI attached to SPI2 (cs=0, 10000000 Hz, mode 0)
  I (1732) ZL38063: app status = 0x0000 (masked 0x00)

[boot ROM 门] 读 reg 0x0034 必须 == 0xD3D3，否则：
  E ... SPI: ERROR: HBI is not accessible (0x0000, want 0xd3d3)
  E ... firmware transfer failed (17): the DSP did not enter boot ROM (reg 0x34 != 0xD3D3) or an HBI access failed

[上传完成]
  I (4852) ZL38063: firmware image transferred (3699 records)
  I (4931) SPI: prgmBase 0x00080200      I (4931) SPI: execAddr 0x00083040

[上传正确性（GO 之前！）—— §10.4]
  I (4944) XiaoMoDouB220: pre-GO verify: SUMMARY run=1 records=3699 visited=9 ok=9 words=140 matched=140 mismatching_words=0 read_errors=0
  I (4974) XiaoMoDouB220: pre-GO state (read-only): reg 0x0034 (boot ROM param/result)=0xD3D3 reg 0x0030 (app status)=0x0000 reg 0x0032 (host cmd)=0x0000 reg 0x0006 (host flags)=0xFFFE

[release 五步全绿]
  I (5194) ZL38063: adf-tx: release sequence COMPLETE: all five steps ran in the ADF / variant-6 order ... all five reported success: yes

[★ 真正的"应用起来了"判据 —— 用 0x0014，不是 0x0030]
  I (5834) XiaoMoDouB220: codec: reg 0x0014 = 0x0200 -> the ZL38063 application is READY
```

**注意**：release 之后 `reg 0x0030` 仍是 `0x0000`，而 `reg 0x0034` 变成 `0x0000`（boot ROM 魔数消失），
`reg 0x0006` = `0xFFE0`【实测，`logs/pa-pulse.log:80`】。这三条一起看才是"boot ROM 已退出、应用已接管"。

### 5.4 两个死路（别再走一遍）

**死路 A：把 `prgmBase` / `execAddr` 对调（入口点 0x80200）。** `B220_DSP_ADDR_MATRIX` 跑了两组配对，
每组都是完整复位 + 完整上传：

```
W (31104) matrix pairing A = prgmBase 0x00080200 / execAddr 0x00083040 (today's values)
W (31114) matrix pairing B = prgmBase 0x00083040 / execAddr 0x00080200 (the swap)
matrix attempt 1/2: ... -> ESP_OK，上传 3699 条成功
  I (36874) status dump entry point (0x012C/0x012D, 2 words) = 0x00083040
  I (36894) matrix attempt 1/2: app status 0x0000 (masked 0x00) -> not running
matrix attempt 2/2: the swap
  E (40564) SPI: Command failed...Resultcode = 0x0002
  E (40564) SPI: ERROR: failed to load the firmware
  I (41634) status dump entry point (0x012C/0x012D, 2 words) = 0x00080200
E (41664) matrix: neither pairing produced a running application; the boot addresses stay at the last attempt
```

结论：① **交换后更糟**（`0x80200` 不是合法入口块起点，上传直接失败）；
② **但两组都"没让应用跑起来"** ⇒ **入口点不是当时的问题所在**（当时真正缺的是 §9 的功放 GPMD_A，
以及 §7 的判据用错了 —— `0x0030` 永远 0，"not running"这个判断本身无效）；
③ 正确的值来自镜像自身的标量：`prgmBase = 0x00080200`、`execAddr = 0x00083040`
【逆向，`zl38063_firmware_image.md` §1；注意 `PROTOCOL-REPORT.md` §2.4 把这两个写反了，该处是错的】。

**死路 B：相信"DSP 会自己启动"。** 早期报告的核心假设是 **Auto Boot**（DSP 从自己的从属 SPI Flash
读固件，ESP32 只做寄存器配置）。**`logs/hands-off.log`（§3.2）直接推翻了它**：在有 I2S 时钟的前提下，
不碰复位、不灌东西，连续 20 秒 `reg 0x0034` 都是 `0xD3D3`、`reg 0x0030` 都是 `0x0000`
—— 设备**一直坐在 boot ROM 里等**。同时原厂镜像里确实带着完整 host-boot 载荷和
`"1- Firmware boot loading started ...."` 之类字符串。⇒ **必须每上电 host boot 一次**；
而且 `reg 0x0014 <- 1` 本身就会强制进 boot ROM，所以即使 DSP 真 auto-boot 过也会被覆盖。

---

## 6. 固件镜像与配置记录怎么从自己的设备里取出来

> **为什么不直接用 esp-adf 的 `libfirmware.a`？** ADF 那份是同样的 `twFwr` 布局、同样的版本横幅
> `Microsemi_ZLS38063_1_P1_4_0`（Sep 2018），但是**不同的 build**：记录数 2453（我们 3699）、
> 入口点 `0x84040`（我们 `0x83040`）、地址轴上是我们的**真子集**（ADF-only 地址 = 0，
> ours-only = 19918），共有的 39204 个地址里 **96.96%** 的值不同
> 【实测，`ref/analysis/zl38063_adf/REPORT.md`】。**ADF 更旧更小，能用但没有任何好处**，
> 配置还是别人的产品档。正确做法是**从自己的设备里取**。

### 6.1 从原厂 dump 里取（推荐）

准备：把原厂 flash 整片 dump 出来（`backup/parts/wifi.bin` = 明文 ESP32 应用镜像，
DROM 段加载地址 `0x3f400020`，文件偏移 `0x000020`，长度 `0x1ce1c8`）。

```powershell
# 1) 固件镜像：3699 条 44 字节记录 + 载荷 + C 头（写在脚本旁边）
python xiaozhi-esp32-b220\main\boards\xiaomodou-b220\zl38063\zl38063_extract_fw.py
#   -> zl38063_firmware_image.bin    118244 B  拼接后的 DSP RAM 镜像（sha256 c8267e3c…30ac）
#      zl38063_firmware_records.bin  162756 B  记录数组的逐字节拷贝（sha256 ce9d8e83…a344）
#      zl38063_firmware_image.h      3699 个 twFwr 条目 + 标量（默认 static；加
#        -DZL38063_FWIMAGE_EXTERNAL 才导出 st_twFirmware / firmwareStreamLen /
#        executionAddress / haveProgramBaseAddress / programBaseAddress 这组强符号）

# 2) 配置记录：完整的 1042 对；--check 是 CI 可用的漂移校验
python xiaozhi-esp32-b220\main\boards\xiaomodou-b220\zl38063\gen_full_config_record.py
python xiaozhi-esp32-b220\main\boards\xiaomodou-b220\zl38063\gen_full_config_record.py --check
#   -> OK: ...\zl38063_config_record_full.h matches wifi.bin

# 3) （可选，只是留档）旧的 260 条版本 —— 不要再用，见 §6.3
python xiaozhi-esp32-b220\main\boards\xiaomodou-b220\gen_zl38063_config_record.py
```

### 6.2 关键地址与字段

| 项目 | 地址 / 值 | 说明 |
| --- | --- | --- |
| 记录数组基址 | DROM **`0x3f4322e4`**（文件偏移 `0x322e4`） | 由 boot wrapper `0x4012628b` 的 `l32r` 载入，不是 DRAM 全局 |
| 数组前的标量区 | DROM `0x3f4322d8` | `+0=0x0E73(3699)`、`+2=0x01`、`+4=0x00083040`、`+8=0x00080200` |
| `execAddr` | **`0x00083040`** | 写入 `reg 0x012C`（2 字，先高后低） |
| `prgmBase` | **`0x00080200`** | 只被打印；`havePrgmBase` 在上传里**故意不使用**（每条 `useTargetAddr=1` 的记录自己重写窗口基址） |
| 记录数 | **3699** = `0x0E73` | |
| 配置记录长度字 | DROM **`0x3f459ea8`**（文件偏移 `0x59ea8`），u16 = **`0x0412` = 1042** | **是"元素个数"，不是字节数！（§6.3）** |
| 配置记录数据 | DROM **`0x3f459eaa`**（文件偏移 `0x59eaa`） | 1042 × `{u16 reg, u16 value}` = 4168 B |
| 寄存器范围 | `0x0200 .. 0x0A22`，**步长 +2、连续** | 前 260 条页分布：page 2 有 128 条、page 3 有 128 条、page 4 有 5 条 |
| 数组末尾 | `0x3f4322e4 + 3699×44 = 0x3f459ea8` | **正好撞上配置记录的长度字** ⇒ 独立的长度交叉验证 |

### 6.3 代价很大的那个 bug：把 1042 当**字节数**

原厂加载循环（`0x40125c30`，逐指令）【逆向，conf 高】：

```
40125c33: extui a3, a3, 0, 16     ; a3 = numElements（= 长度字段 = 1042）
40125c36: addx4 a3, a3, a2        ; end = data + 4 * a3 = data + 4168
40125c3c: l16ui a8, a2, 2         ; value = p[1]
40125c45: l16ui a10, a2, 0        ; reg   = p[0]
40125c48: addi  a2, a2, 4         ; p += 4
40125c51: call8 hbi_write16       ; 每对一次 1 字写（numwords 硬编码 1）
40125c56: bne   a2, a3, 40125c3c  ; 1042 次
```

`addx4` 是 `a3 = a2 + (a3 << 2)`，**步长 4 字节、循环 1042 次**。但第一版提取脚本
（`gen_zl38063_config_record.py`）把 `0x0412` 当成**载荷字节数**，只取了前 `1042 / 4 = 260` 对
（外加 2 个尾巴字节），于是 `zl38063_config_record.h` 是
`#define ZL38063_CONFIG_RECORD_COUNT 260`、寄存器只到 `0x0406`：
**漏掉 782 条（寄存器 `0x0408..0x0A22`），其中 157 条非零。**

**为什么这个 bug 致命**：下行通路的关键寄存器就在被漏掉的尾部里。

| 寄存器 | 记录索引 | 原厂值 | 原厂谁写它 |
| --- | --- | --- | --- |
| `0x0494` | **330** | `0x0666`（音量 < 80 档）/ `0x16C3`（≥ 80 档） | 原厂 volume/route 例程 `0x401256D8` |
| `0x049A` | **333** | `0x0002` / `0x0001` | 同上 |
| `0x04C0` | 尾部 | `0x0033`（场景 profile A 里被写成 `0x0022`） | `0x400D6493` |
| `0x0622` | 尾部 | `0x0087`（profile B 里被 XOR `0x0100`） | `0x4012953C` |

原厂自己的播放启动路径（`0x40125358` → `ops[+44] = 0x401256D8`，音量 50）逐条是：

```text
0x238 <- 0xECEC    (tblA[5] | tblA[5]<<8)      0x23A <- 0xECEC
0x24E <- 0xF3F3    (tblB[5] | tblB[5]<<8)
  -- 这里有一条门：读 reg 0x0014，只有 == 0x200 才继续写下面两条 --
0x494 <- 0x0666    (音量 < 80 档)               0x49A <- 0x0002
VprocTwolfReset(VPROC_RST_SOFTWARE)   -> reg 0x006 <- 0x0002
```

`0x238/0x23A/0x24E` 在我们的 260 条里**有**；`0x494/0x49A` **没有**（索引 330/333 落在被漏掉的尾部），
而原厂为它们写的值**正好等于原厂配置记录里携带的值**。这就是"**记录被截成 260 条**"与
"**下行不出声**"之间的机械联系。

**定位过程**【实测，`logs/downlink-tries.log`】：把 19 种候选写序列排成表，每次写完只读回一行：

```
I (7920)  downlink try 1: readback (read-only) 0x0030=0x0000 0x0032=0x0000 0x0034=0x0000 0x0006=0xFFE0 0x0014=0x0200 0x0238=0xF1F1 0x024E=0xFAFA 0x0494=0x16C3 0x049A=0x0002 0x0A1C=0x0100
W (9920)  ===== downlink try 2/19: the 782 record entries the 260-element header omitted (regs 0x0408..0x0A22) + soft reset =====
I (10070) downlink try 2: record entries [260..1042) = regs 0x0408..0x0A22, 782 register write(s) -> 782 ok, 0 FAILED
I (10270) downlink try 2: readback ... 0x0494=0x0666 0x049A=0x0002 ...      <-- 尾部值真的变了
W (11920) ===== downlink try 3/19: the WHOLE 1042-entry record from the stock image + soft reset =====
I (12090) downlink try 3: record entries [0..1042) = regs 0x0200..0x0A22, 1042 register write(s) -> 1042 ok, 0 FAILED
I (12300) downlink try 3: readback ... 0x0014=0x0200 0x0238=0xF1F1 0x024E=0xFAFA 0x0494=0x0666 0x049A=0x0002 0x0A1C=0x0100
```

try 1 与 try 3 的差别：`0x0494` 从 `0x16C3` 变成 `0x0666`；`0x0014` 始终 `0x0200`。
**这行 readback 就是"记录到底写进去了没有"的直接证据。**
（完整记录多花 4168 字节 `.rodata`；生产构建换成 `zl38063_config_record_full.h` 即可，
不必保留 19 次试探的 stepper。）

### 6.4 配置记录的加载方式（与固件记录不同！）

配置记录**不走 page-255 窗口**，是**一对一次的直接寄存器写**：

```c
for (i = 0; i < numElements; i++) {                         /* numElements = 1042 */
    buf = pCr2Buf[i].value;
    status = VprocTwolfHbiWrite(pCr2Buf[i].reg, 1, &buf);    /* 每次都 numwords = 1 */
}
```

原厂里 `numwords` **硬编码 1**（`0x40125c42: movi a11, 1`），循环里**没有任何门控字节、没有条件分支、
没有索引算术**，唯一跳转是回循环头 `bne a2, a3`【逆向，conf 高】。所以**不要**给配置记录加任何
"跳过条件"（早期报告的"门控字节 / `rec+4k+42` 规则"在二进制里不存在，见 §12）。
一行日志证明 1042 条真的都发出去了：

```
I (12090) ZL38063: downlink try 3: record entries [0..1042) = regs 0x0200..0x0A22, 1042 register write(s) -> 1042 ok, 0 FAILED
```

### 6.5 怎么验证取出来的东西是对的

**（a）与 ADF 参考实现做"地址轴"对比**（不要按记录序号逐字节比 —— 两边记录顺序不同，
末尾都有重定向到低地址的 overlay 记录，按序对比会得出错误结论）：

```powershell
python ref\analysis\zl38063_adf\compare_addr.py
# 判据：
#   ADF-only addresses  = 0        <- 我们的地址集合是 ADF 的超集
#   ours-only addresses = 19918    <- 0x80a00..0x9f4e6，78.9% 非零 => 真代码
#   common 上值不同      = 38011 / 39204 (96.96%)  <- 不是字节序伪影（字节序匹配的更少：854）
#   入口点               = ADF 0x84040 / 我们 0x83040
# => 同一个 twFwr 布局、同一个版本横幅，但我们的更新更全，ADF 是更旧的 build
```

**（b）结构自检**（写进生成脚本，任何一条不过就 `SystemExit`）：

| 检查 | 期望 |
| --- | --- |
| 记录数 × 44 + 基址 == 配置记录长度字地址 | `0x3f4322e4 + 3699*44 == 0x3f459ea8` ✅ |
| 每条记录 `numWords != 0` 且 `<= 126`（HBI 写上限） | 3699/3699，直方图最高 16 ✅ |
| `useTargetAddr ∈ {0,1}` | 全为 1 ✅ |
| 载荷字地址零重叠 | ✅ |
| 配置记录寄存器号是 +2 等差 | `regs[i+1]-regs[i] == 2`，否则报 `"wrong offset?"` ✅ |
| 长度字段与 `count * 4` 的字节数一致 | ✅ |

**（c）上机自检**：§10.4 的 pre-GO 读回（`mismatching_words=0`）就是最终裁决。

---

## 7. ⭐ 状态判据的陷阱

### 7.1 `reg 0x0030`（"app status"）在本产品上**永远是 0**

上游 `VprocTwolfGetAppStatus()` 读 `0x030` 并返回 `buf & 0x0F`，语义是"0 = 应用没在跑"。
移植时**照抄了这个判据**，结果是一批假阴性：

```
I (5872) ZL38063: app status = 0x0000 (masked 0x00) after the host boot
W (5872) ZL38063: the ZL38063 application still reports 'not running' after the host-boot upload; the image transfer itself reported success, so check the entry point (0x00083040) and the I2S/clock wiring
```

而**在同一台机器、同一次启动、声音已经正常工作的状态下**，`0x0030` 依然是 0：

```
I (5764) ZL38063: adf-tx: ~500 ms after the release (read-only, no device write): reg 0x0030 (app status)=0x0000 ... -> masked app status=0x00
I (5834) XiaoMoDouB220: codec: reg 0x0014 = 0x0200 -> the ZL38063 application is READY (0x0200 is the value the stock volume/route routine gates its route writes on; reg 0x0030 means nothing on this product)
```

**结论【实测，高置信】：`reg 0x0030` 在本产品上从不更新，任何基于它的判断都是无效的。**
不要拿它当成功/失败判据，更不要因为它而发起重试或复位（那会把已经跑起来的应用打掉）。

### 7.2 `reg 0x0014 == 0x0200` 才是"应用就绪"

原厂自己的 volume/route 例程就是用它当门【逆向】：

```asm
40125710: VprocTwolfSetVolume(tblA[5])          ; -> 0x238, 0x23A
4012571c: r = VprocTwolfHbiRead(0x0014)
40125748: bne r, 0x200, return                   ; ★ 只有 0x14 == 0x200 才继续
40125756: 0x24E <- tblB[5] | (tblB[5]<<8)
40125762/73: 0x494 <- 0x16C3 (idx>=8) else 0x0666
4012577c: 0x49A <- 0x0001 (idx>=8) else 0x0002
40125781: VprocTwolfReset(VPROC_RST_SOFTWARE)
```

也就是说：**原厂在"路由寄存器要不要写"这件事上，唯一信任的判据是 `0x0014 == 0x200`。**
我们照抄这一条（只读，引导结束后读一次）：

```c
if (read_reg(0x0014) == 0x0200)
    ESP_LOGI(TAG, "codec: reg 0x0014 = 0x0200 -> the ZL38063 application is READY ...");
```

配套的 `b220_audio_dsp_ready()` 语义要写清楚：**false = "没被证明就绪"，不是"确定死了"**
（HBI 读可能压根没成功过）。codec 层把它当**警告**，绝不能当"拒绝运行"的理由。

### 7.3 `reg 0x0034` 的三种取值要分清

| `reg 0x0034` | 含义 | 该做什么 |
| --- | --- | --- |
| **`0xD3D3`** | DSP **正坐在 boot ROM 里**等你灌固件 | 走 §5 的主机引导流程 |
| `0x0000` | 已不在 boot ROM（应用接管了），或一次命令已完成 | 读 `0x0014` 确认 |
| `0x0002` | 上一次 host 命令的**结果码**：失败 | 看是哪条命令（§5.4 死路 A 交换入口点后就是这个值） |

⚠️ **`0xD3D3` 是"还没起来"，不是"起来了"**。§5.4 死路 B 的实验里它稳了 20 秒，这是"没自启动"的铁证。

### 7.4 一组"看状态"的推荐读法

```text
0x0030 app status        (本产品恒 0，只当参考)      0x0032 host cmd   (0x0000 空闲 / 0xFFFF 忙 / 0x0008 / 0x000D)
0x0034 cmd result        (0xD3D3 = boot ROM)         0x0006 host flags (0xFFFE/0xFFE0 都见过，看 bit0)
0x0014 route gate        (★ 0x0200 = 应用就绪)        0x0020/0x0022/0x0024 boot ROM id 块
```

---

## 8. ⭐ 破坏性操作警告

### 8.1 `mailbox_probe` / 任何写 `reg 0x0014` 的动作都会**静默杀掉正在跑的应用**

```c
unsigned short buf = 1;
VprocTwolfHbiWrite(0x014, 1, &buf);   /* ★ 强制 DSP 停止固件、复位进 boot ROM */
Vproc_msDelay(300);
VprocTwolfHbiRead(0x034, 1, &buf);    /* 期望 0xD3D3 */
```

**为什么危险**：它**不报错**。写进去返回成功，DSP 就回 boot ROM 了，应用没了，喇叭和麦克风立刻全哑，
而日志上看不出任何异常 —— 你只会觉得"刚才还好好的，怎么又不出声了"。
**必须做的防护**（我们最终就是这样做的）：

```c
if (zl38063_dsp_is_running()) {
    ESP_LOGW(TAG, "mailbox probe skipped: an application is already running "
                  "(probing would reset the DSP into boot ROM)");
    return ESP_OK;
}
```

**同样禁止的范围**：任何"再复位一次试试"的 debug 路径（`VprocTwolfReset(VPROC_RST_HARDWARE_RAM/ROM)`
写的也是 `0x014`，值 `0x05`/`0x09`）；`VprocTwolfReset(VPROC_RST_AEC)` 写 `reg 0x0300 <- 0x01`，
同样打乱运行中的 AEC 状态；以及 **release 成功之后**再跑任何"上传验证 / 地址矩阵 / 页面标定"旧诊断。
**§5.4 死路 A 的教训**：当时矩阵跑完，最后那条 `mailbox probe` 之所以能"确认"DSP 在 boot ROM，
是因为矩阵**已经把应用折腾掉了** —— 别把这种"探针成功"当成好消息。

### 8.2 page-255 标定 / 读回校验会**改写窗口基址寄存器 `reg 0x0C`**

读回校验为了逐条读图像，必须对每条记录重写一次 `reg 0x000C`（2 个字）—— 这本身是**上传过程每一步
都在做的事**，所以在**上传过程中**做是安全的（`B220_DSP_VERIFY_BEFORE_GO` 就是这么用的）：

```c
/* zl38063_dsp.c 的原注释就是设计约束 */
 * It WRITES nothing except the page-255 window base register 0x0C, exactly as
 * many times as HbiSrecBoot_alt() writes it for the records it visits - a state
 * every upload already produces - so it is safe both inside the upload (pre-GO)
 * and after it (post-release).
```

**但标定（`B220_DSP_PAGE255_CALIB`）会在 scratch 地址 `0x00080A24` 写 16 个**图案字**。**
它落在图像**空洞**里，不覆盖镜像，但仍然**改写了 DSP RAM**。强制条件写死在代码里：

```c
if (zl38063_dsp_is_running()) {
    ESP_LOGI(TAG, "page255 calibrate: skipped, an application is already "
                  "running (its RAM is not scratch space)");
    return ESP_OK;
}
```

**规则汇总（贴在手边）**：

| 想做的事 | 应用在跑时允许吗 |
| --- | --- |
| 读 `0x0030/0x0032/0x0034/0x0006/0x0014`、`0x0238/0x024E/0x0494/0x049A/0x0A1C` | ✅ 只读，随时可以 |
| page-255 窗口读（改 `reg 0x0C`） | ⚠️ 只在"刚上传完还没 GO"这个窗口里做；生产构建关掉 |
| page-255 标定（写 scratch 图案） | ❌ 绝对不行 |
| 写 `reg 0x0014`（mailbox / 硬件复位） | ❌ 绝对不行 |
| 写 `reg 0x0006 <- 0x0002`（软复位） | ⚠️ 只在 release 那一刻发一次 |
| 再跑一次完整主机引导 | ❌ 会先把好镜像丢掉 |

---

## 9. 功放这一级：AW8737S 单线脉冲 + AW9106 的 GPMD_A 陷阱

这一步的症状极具欺骗性：**DSP 在跑、DAC 上有 1 kHz 方波、喇叭一点声都没有。**

### 9.1 硬件事实

| 项目 | 事实 | 来源 |
| --- | --- | --- |
| 功放 | **AW8737S** | 【逆向 + 实测】 |
| SHDN | **低有效**，而且**不是普通 enable**：靠**上升沿个数**选模式 | 【实测 + 数据手册】 |
| 模式 | **1 个上升沿 → mode 1（1.2 W）**；`NCN off`（静音）在 **4 个沿**那一档 | 【实测】 |
| 换模式条件 | SHDN 必须先拉低 **> 1 ms**（TOFF）才算"登记了一次关断" | 【实测，我们用 2 ms】 |
| SHDN 由谁驱动 | **AW9106B（I2C 0x5B）group-A 输出 A1 = reg `0x02` 的 bit1** | 【逆向，`0x40207ae8` = `audio_pa_on()`；实测】 |
| 关键陷阱 | **group A 必须处于 GPIO 模式：`reg 0x12 (GPMD_A) = 0x02`** | 【实测，§9.2】 |
| 额外条件 | `reg 0x04 (GPIO_CFG_A) = 0x00`（每根 group-A 脚都是普通输出） | 【实测】 |

原厂 `audio_pa_on(arg)`（`0x40207ae8`）只是简单地 `reg 0x02 <- 0x02`（arg==1）/ `<- 0x00`
（`START 0xB6 0x02 <byte> STOP`），**没有单线脉冲计数** —— 它整个开机只切换一次，所以能用。

### 9.2 那个"静默杀手"：背光初始化把 GPMD_A 改回 0x08

`config.h` 里记录的结论（原文）：

```text
 * The walk proved the root cause on hardware: with GPMD_A (AW9106B reg 0x12) at
 * 0x08 the group-A outputs drive NOTHING - steps 1..9, including 0x02 <- 0xFF,
 * were silent - and the 1 kHz tone became audible EXACTLY at the steps that wrote
 * 0x12 <- 0x02 together with 0x04 (GPIO_CFG_A) <- 0x00, and stayed audible until
 * power-off.  Our InitializeBacklight() replayed the stock tail 0x11(CTL)<-0x1B +
 * 0x12(GPMD_A)<-0x08 AFTER the PA enable, so it switched the amplifier straight
 * back off: that overwrite is the difference between no sound and sound here.
```

**这是一个"顺序覆盖"型 bug**：我们的背光初始化照抄了原厂 tail（`0x11 <- 0x1B`、`0x12 <- 0x08`），
而 `0x08` 是**把 group A 交给 DIM/PWM 引擎**的模式。此时无论 `reg 0x02` 写 `0x00`、`0xFF` 还是别的，
**SHDN 都不动**（走位 1..9 全静音）。把 `0x12` 改回 `0x02` 的那一刻，声音出来了。

### 9.3 怎么找到的：22 步走位 + 用户的秒表

`B220_PA_BIT_FINDER` 是一段**把 AW9106B 的每个可疑位单独动一次**的走位表
（每步保持几毫秒，同时 1 kHz 全幅正弦一直在播）：

| 步骤 | 动作 | 目的 |
| --- | --- | --- |
| 1..9 | 只动 `reg 0x02`（`0x01/0x02/0x04/.../0x80/0xFF`），GPMD_A 保持 **0x08** | 问"哪个 bit 是 SHDN" |
| 10..12 | `0x12 (GPMD_A) <- 0x02` + `0x04 (GPIO_CFG_A) <- 0x00/0x0F/0xF0`，同时 `0x02 <- 0xFF` | **问"是不是驱动模式的问题"** |
| 13..21 | `0x13 (GPMD_B) <- 0x02`，然后 `0x03 (OUT_B)` 逐位 `0x01..0x80/0xFF` | 万一 SHDN 挂在第二组 |
| 22 | 恢复：`GPMD_A=0x02, GPIO_CFG_A=0x00, GPMD_B=0x00, OUT_B=0x00, OUT_A` 全高 | 最后确认 |

走位开始前的日志（提醒仪器该放哪）：

```
I ... pa-find: put the meter on the AW8737S SHDN pin (red = SHDN, black = GND) and watch it ...
I ... pa-find: EXPECTED: the AW8737S counts rising edges on SHDN, so this walk will leave it ...
I ... pa-find: 10-12 = 0x12(GPMD_A)<-0x02 + 0x04(GPIO_CFG_A)<-0x00/0x0F/0xF0 with ...
I ... pa-find step %u/%u: reg 0x%02X <- 0x%02X (%s) -> %s; check SHDN now
```

**实际定案靠耳朵 + 秒表**【实测，用户操作】：1 kHz 正弦全程在响，用户一边听一边报"第几秒听到了"
—— 声音**正好出现在走位表的第 10~12 步**（也就是写 `GPMD_A <- 0x02` 的那几步），并一直响到关机。
走位表本身还顺便证明了"`GPMD_A = 0x08` 时 `0x02 <- 0xFF` 都是静音"。

> 诚实说明：**万用表在 SHDN 上的绝对电平没有成为最终证据**（走位步长只有几毫秒，手持表跟不上；
> 而且 SHDN 的"模式"由**沿数**决定，稳态电平本身不区分模式）。
> 定案证据是**听感 + 走位表的结构**：唯一变量是 `GPMD_A`，它一变声音就出来，一恢复又一直在。

### 9.4 最终启用序列（生产代码，五步）

```c
/* xiaomodou_b220.cc —— b220_pa_enable_one_pulse()，整个开机只跑一次 */
1) 0x12 (GPMD_A)     <- 0x02         /* group A 进入"输出能驱动 SHDN"的模式 */
2) 0x04 (GPIO_CFG_A) <- 0x00         /* 每根 group-A 脚都是普通输出 */
3) 0x02              <- 0x00         /* SHDN 拉低 */
   esp_rom_delay_us(2000);           /* 保持 >= 2 ms：AW8737S 的 TOFF，登记一次关断 */
4) 0x02              <- 0x02         /* ★ 正好一个上升沿 -> mode 1 = 1.2 W */
5) 锁存：0x12 / 0x04 / 0x02 之后任何"改动"都被 b220_aw9106_frozen_refuses() 拒绝
```

对应的日志（`logs/final-pa.log`，这就是"我做对了"的样子）：

```
I (5941) XiaoMoDouB220: pa: SHDN enable sequence: 0x12(GPMD_A)<-0x02, 0x04(GPIO_CFG_A)<-0x00, 0x02<-0x00, hold 2 ms, 0x02<-0x02 (exactly 1 rising edge -> AW8737S mode 1, 1.2 W)
I (5951) XiaoMoDouB220: pa: AW9106B 0x5B reg 0x12 (GPMD_A) <- 0x02 (PA enable step 1/5: GPMD_A <- GPIO mode) -> ESP_OK | write #2, group-A drive mode = GPIO/output: SHDN can reach the AW8737S
I (5961) XiaoMoDouB220: pa: AW9106B 0x5B reg 0x04 (GPIO_CFG_A) <- 0x00 (PA enable step 2/5) -> ESP_OK | write #2, group-A pin config = 0x00 (all group-A pins are outputs)
I (5971) XiaoMoDouB220: pa: AW9106B 0x5B reg 0x02 <- 0x00 (PA enable step 3/5: low hold (shutdown registered)) -> ESP_OK | write #2, SHDN bit1=0, rising edges so far=0
I (5991) XiaoMoDouB220: pa: AW9106B 0x5B reg 0x02 <- 0x02 (PA enable step 4/5: the one rising edge) -> ESP_OK | write #3, SHDN bit1=1, rising edges so far=1
I (6011) XiaoMoDouB220: pa: enable sequence done: 3 write(s) to reg 0x02 this boot, 1 rising edge(s) on bit 1 (AW8737S mode 1 = 1.2 W), last result ESP_OK
I (6031) XiaoMoDouB220: pa: 0x12(GPMD_A)/0x04(GPIO_CFG_A)/0x02 are now LATCHED - any later change is refused by b220_aw9106_frozen_refuses()
I (6041) XiaoMoDouB220: pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02 | 0x02 writes=3, SHDN rising edges=1 (AW8737S mode 1)
```

### 9.5 沿数审计（edge counting）—— 必须做

因为 SHDN 是**计沿**的，任何"顺手再写一次 `reg 0x02`"都可能把功放推到 mode 2/3，
或者第 4 个沿直接静音。所以对 `0x02` 的每一次写都要**计数并记录**：

```c
/* 每次写 0x02 都过这个 writer：只有 0->1 才 +1 */
if ((s_aw9106_reg02_last_value & 0x02) == 0 && (value & 0x02) != 0) {
    s_aw9106_reg02_rising_edges++;
    if (s_aw9106_reg02_rising_edges > B220_PA_SHDN_MAX_RISING_EDGES /* 1 */)
        ESP_LOGW(TAG, "pa: !!! AW8737S SHDN RISING EDGE #%lu (bit 1 of reg 0x%02X) - "
                      "the amplifier is now in a DIFFERENT mode !!!", ...);
}
/* 锁存后的"写同一值"允许（SHDN 不动），"改成别的值"拒绝：
   pa: ... after the enable sequence; the value does not change, so SHDN does not move (allowed) */
```

**三条配套规则**：
1. **不要用"再写一次 `0x02 <- 0x02`"当开机使能** —— 如果之前已经写过一次，这就是第二个沿。
   必须走 §9.4 的五步（先低 ≥2 ms 再高）。
2. **关机时不要靠"关功放"实现静音**：本板唯一的关断手段就是再写 `reg 0x02`（= 又一个沿）。
   正确做法是**让 DSP 不再被喂音频**，功放保持开着：

```
I ... Set output enable to false: the AW8737S PA is left ON (a second SHDN edge would change its mode) - playback silence comes from the DSP no longer being fed audio
```

3. **背光与功放共用 group A**：DIM 寄存器（`0x20..0x25`）不受影响，但**绝对不能再往 `0x12` 写 0x08**。
   若面板真需要 `0x08` 那套 DIM/PWM 引擎，唯一可能兼得的 `GPMD_A` 候选值是 **`0x0A`**
   （同时保留 bit1 的输出模式和 DIM 脚的模式）—— **该组合从未在本硬件上验证过，生产构建不要用**。

---

## 10. 验收方法（怎么证明每一段都通了）

原则：**每一段都要有一个"只有这一段通了才会出现"的观测量**，优先选**能写进日志、事后能回看**的。

### 10.1 上行：麦克风回环（`B220_AUDIO_LOOPBACK`，`B220_AUDIO_TEST_MODE=0`）

I2S RX 收到的数据直接写到 I2S TX，**每 ~2 秒打一行统计**，这样"静音"和"有信号"在日志上长得完全不同：

```
I (6024) loopback: first non-empty block after start - first 16 of 480 sample(s), hex, L/R interleaved: FFFA FFFA FFF3 FFF3 FFDE FFDE FFCF FFCF FFD3 FFD3 FFF2 FFF2 0022 0022 004C 004C
I (8014)  loopback t=+2s:  frames_in=96000   frames_out=96000   read_err=0 write_err=0 empty_reads=0 blocks=400 last_block=240 frames peak=121   mean=43
I (10014) loopback t=+4s:  frames_in=192240  frames_out=192240  read_err=0 write_err=0 empty_reads=0 blocks=401 last_block=240 frames peak=12656 mean=5937
I (12024) loopback t=+6s:  frames_in=288480  frames_out=288480  read_err=0 write_err=0 empty_reads=0 blocks=401 last_block=240 frames peak=1142  mean=330
I (16024) loopback t=+10s: frames_in=480480  frames_out=480480  read_err=0 write_err=0 empty_reads=0 blocks=400 last_block=240 frames peak=131   mean=41
I (36024) loopback t=+30s: frames_in=1440720 frames_out=1440720 read_err=0 write_err=0 empty_reads=0 blocks=401 last_block=240 frames peak=466   mean=158
```

读法：`peak` 几十~一百多、`mean` 15~60 ⇒ **房间本底噪声**（DSP 在送数据，只是没有声源）；
`peak` 跳到 **12000 上下 / mean 5000 上下** ⇒ **有人说话**（`+4s` 那一行）；
`peak = 0` 且 `mean = 0` 持续出现 ⇒ DSP 没在驱动 DIN 或 RX 通道没跑；
`frames_in` 与 `frames_out` 每 2 秒各 +96000（= 48000×2）⇒ 时钟和 DMA 是稳的；
`read_err / write_err / empty_reads` 应恒为 0。

**第一行 hex dump 是必须的**：它是"通道顺序没搞反 + 数据不是常量"的最短证据。本板实测是**成对重复**
（`FFFA FFFA ...`）⇒ DSP 把同一个样点同时送进 L 和 R，所以 codec 层可以诚实地声明"交错立体声"，
把左声道喂给唤醒词/编码器。

### 10.2 下行：1 kHz 全幅正弦（`B220_AUDIO_TEST_MODE=1`）

**只往 TX 写 1 kHz 满幅正弦，RX 完全不读**。这样喇叭里能出来的东西**只可能来自 ESP32**，
所以"能听到"就一次性洗清了 **ESP32 → I2S → DSP cross-point → DAC → 功放 → 喇叭** 整条腿。

```
I (5861) tone: 1 kHz cycle table built, 48 entries of int16 LSB (one 48-sample cycle), one-cycle dc_mean=0 (of 255 = digital zero), peak=32760; phase_step(1 kHz)=89478485, 0 index step(s) per sample
I (5881) tone: expected mean = peak * 2/pi = 0.63662 * peak (peak 32760 -> mean 20856)
I (5891) loopback: DOWNLINK TEST MODE 1/3 = 1 kHz FULL-SCALE SINE ON TX ONLY, RX is never read; everything the speaker can carry comes from the ESP32, so hearing it clears the whole ESP32 -> DSP -> PA -> speaker leg
I (8061) tone t=+2s:  mode=1 (1 kHz full-scale sine, TX only, RX never read) tx_frames=48000 tx_blocks=200 tx_last=240 frames tx_peak=32759 tx_mean~20855 write_err=0
I (10071) tone t=+4s: ... tx_peak=32759 tx_mean~20855 write_err=0
I (30081) tone t=+24s: ... tx_peak=32757 tx_mean~20854 write_err=0
```

判据：`tx_peak ≈ 32757..32759`、`tx_mean ≈ 20851..20855`（≈20856 = peak×2/π）、`write_err=0`。
**这三条只证明 ESP32 写对了；"能听到"才是 DSP+功放那一段的证明。**

### 10.3 示波器检查点（每一段一个探点）

| 探点 | 期望波形 | 它证明什么 |
| --- | --- | --- |
| **BCLK GPIO5** | 方波 **1.536 MHz**（32 BCLK/帧） | ESP32 I2S 主模式在跑（DSP 的时钟源） |
| **WS GPIO25** | 方波 **48 kHz** | 采样率/声道结构正确（原厂机实测 48 kHz）【实测】 |
| **DOUT GPIO26** | 播 1 kHz 全幅正弦时是**方波** | ESP32 真把数据送上去了（与 `tx_peak` 日志互证） |
| **DIN GPIO35** | 有信号时数据跳变；静音时接近常量 | DSP 在驱动上行 |
| **HBI SCLK GPIO32** | 上传期间**连续 10 MHz 突发**（约 3 秒） | 是 Host Boot（ESP32 在灌固件），不是 Auto Boot |
| **HBI CS GPIO0** | 空闲**高**，通信期间密集拉低 | 片选网络通、没被别的电路占住 |
| **DAC1_P / DAC1_M** | 原厂机播音频时约 **100 mV 方波**；我们不出声时**平** | DSP 的 DAC 到底有没有输出（区分"DSP 侧"和"功放侧"）【实测对比】 |
| **喇叭端子** | 与 DAC1 同频、幅度大得多 | 功放这一级通不通 |
| **AW8737S SHDN 脚** | 使能后**保持高** | 功放有没有被正确开启（稳态高只说明"开过"，模式由沿数决定） |
| **GPIO19 RESET** | 上电时序中**低 10 ms → 高并保持** | 复位网络正常，DSP 没被按住 |

**DAC1 与喇叭端子的对比是整个调试里最有价值的一次二分**：DAC1 有波形、喇叭没有 ⇒ 问题**一定**在
功放这一级（§9）；DAC1 也是平的 ⇒ 问题在 DSP 侧（配置记录 / I2S / 时钟）。

### 10.4 上传正确性：GO 之前的 in-RAM 校验（最有力的一步）

在 `HbiSrecBoot_alt()` 里、**所有记录刚写完、`reg 0x12C` 和 `reg 0x32 <- 0x000D` 还没发**的那个瞬间，
挂一个钩子把采样记录从 DSP RAM 读回来比（`B220_DSP_VERIFY_BEFORE_GO`）：

```
I (4814) ----- pre-GO verify (run 1, B220_DSP_VERIFY_BEFORE_GO): called from INSIDE the upload, all image records written, no host command sent yet (reg 0x12C and reg 0x32 <- 0x000D come after this) -----
I (4824) pre-GO verify: 3699 records, sampling every 463 (record 0 always), window read enc=b, base = reg 0x0C <- {hi16, lo16}
I (4834) pre-GO verify: record 0    target=0x00080200 matched 16/16 (read 0000 1C0F 0010 0041 expected 0000 1C0F 0010 0041 )
I (4844) pre-GO verify: record 463  target=0x000861A0 matched 16/16 (read 29AE 8008 33A4 2870 expected 29AE 8008 33A4 2870 )
I (4854) pre-GO verify: record 926  target=0x00089B80 matched 16/16 (read A000 EC3A 34A3 A000 expected A000 EC3A 34A3 A000 )
I (4874) pre-GO verify: record 1389 target=0x0008D560 matched 16/16
I (4884) pre-GO verify: record 1852 target=0x00090F40 matched 16/16
I (4894) pre-GO verify: record 2315 target=0x00094920 matched 16/16
I (4904) pre-GO verify: record 2778 target=0x00098300 matched 16/16
I (4924) pre-GO verify: record 3241 target=0x0009BCB8 matched 16/16
I (4934) pre-GO verify: record 3698 target=0x000801E8 matched 12/12 (read 0000 0000 0000 0000 expected 0000 0000 0000 0000 )
I (4944) pre-GO verify: SUMMARY run=1 records=3699 visited=9 ok=9 words=140 matched=140 mismatching_words=0 read_errors=0 window_enc=b calibrated=0
I (4974) pre-GO state (read-only): reg 0x0034 (boot ROM param/result)=0xD3D3 reg 0x0030 (app status)=0x0000 reg 0x0032 (host cmd)=0x0000 reg 0x0006 (host flags)=0xFFFE
```

**为什么这一步价值极高**：① `mismatching_words = 0` ⇒ **上传的字节级正确性在 GO 之前就被证明了**，
与"应用跑没跑起来"完全解耦；② `reg 0x0034` 此时仍是 `0xD3D3` ⇒ 确实是在 boot ROM 里、且确实是 GO 之前；
③ 同一组采样点在 run 1..8 里读出的起始字**逐次完全一致**（`0000 1C0F 0010 0041` 等，
见 `logs/pre-go-verify.log`）⇒ 上传是**确定性**的，没有"读到别的东西"的随机性。

**反例也是宝贵的**：在窗口读编码还没标定、且配置记录被截断的那一版构建里，同一个校验是**大红**的：

```
I (42034) upload verify: record 0 target 0x00080200 -> page-255 window base low byte 0x200 (window offset 0)
E (42044) upload verify: mismatch: <0> target=0x00080200 word 1: sent 0x1C0F read 0x0000
I (42814) upload verify: SUMMARY records=3699 verified=929 words=14856 mismatching_words=13780 (mismatching_records=900, read_errors=0)
E (42844) upload verify: RESULT mismatches are scattered inside an image that partly matches ...
```

**注意 `low byte 0x200` 这一行** —— 它把 `targetAddr` 的低 **16** 位当"低字节"打印了，这正是
"读侧编码是猜的"的破绽（正确是 `0x24`）。**当你的校验大面积失败时，先怀疑校验器自己，
而不是被校验的对象。** 标定之后（§4.5）同一校验变成 `0` 失配。

### 10.5 端到端

真正跑通之后，日志里应该同时出现这几行（`logs/production-codec.log`）：

```
I (5224) ZL38063: adf-tx: release sequence COMPLETE: ... all five reported success: yes
I (5824) XiaoMoDouB220: codec: reg 0x0014 = 0x0200 -> the ZL38063 application is READY
I (5944) XiaoMoDouB220: pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02 | 0x02 writes=3, SHDN rising edges=1 (AW8737S mode 1)
I (5964) Zl38063AudioCodec: ZL38063 audio codec: 48000 Hz in / 48000 Hz out, 16-bit, 2 slot(s), I2S0 borrowed from the board bring-up
I (5974) Zl38063AudioCodec: audio bring-up: I2S0 tx/rx handles are up (tx=0x3f800dd0 rx=0x3f800fa4)
```

再往后接唤醒词（`logs/wakeword.log`，用的是 esp-sr WakeNet9，跑在 ESP32 上，与 DSP 无关）：
`I (13814) Application: Wake word detected: ... (state: 3)`。

> ⚠️ 唤醒词这一环**与原厂不是同一条路**：原厂是**跑在 ESP32 上的 WakeNet5 "魔豆精灵"**（16 kHz 单声道，
> 阈值 0.90/0.85、6 帧、3 个关键词），**DSP 里没有任何触发器**（原厂对 DSP 的运行时访问只有 reg 0x06
> 邮箱轮询）。"唤醒词要不要移植"是**独立于本章**的另一个课题（见 `zl38063/WAKE-WORD-PATH-REVERSE.md`、
> `zl38063/wakenet/README.md`、`wakenet5_tool.py`）。本章只需保证：**codec 交付给唤醒引擎的是
> 16 kHz 单声道**（48 kHz 立体声下采样）。

---

## 11. 排错表

| 症状（日志/现象） | 根因 | 修法 | 证据 |
| --- | --- | --- | --- |
| **HBI 全读 `0x0000`**（交换/不交换都是 0） | ① I2S BCLK 没跑（DSP 没时钟）② HBI 首次 attach 后需要重新选择 | ① 先起 I2S0 主模式再复位（§3）② attach 后若仍 0，用 `zl38063_hbi_rebind()` 重挂再读 | `logs/hbi-diag.log:68-85`、`logs/i2s-before-dsp.log:137-158,191` |
| HBI 读出 `0xFFFF` / 或两个不同寄存器读同一值（如 `0x0034=0x0030=0xC760`） | 片选选错了别的东西（串扰），不是 DSP 应答 | 确认 CS = **GPIO0**，mode 0，10 MHz；不要扫到 LCD 的 CS(27) 上 | `logs/i2s-before-dsp.log:279` |
| 交换侧读数有意义、原始侧全 0（或反之）；或读回 `0x0100` 而期望 `0x0001` | 16 位字节序开关方向 / 只在一个方向做了 swap | 命令字和数据字都要双向 `bswap16`；两个方向都测一遍（§4.2） | `logs/boot-addr-matrix.log:141-158` |
| **`reg 0x34 != 0xD3D3`**，`"HBI is not accessible (0x0000, want 0xd3d3)"` | 设备没进 boot ROM：写 `reg 0x14 <- 1` 没生效 / reset 没释放 / HBI 不通 | 先确认 `0xD3D3` 可读（§3），再发 `reg 0x14 <- 1` + 等 300 ms，再读 `0x34` | `logs/hbi-diag.log:61` |
| **"传输报告成功但应用从来没跑"** | ① 判据用错（`0x0030` 恒 0）② 入口点/地址写错 ③ 配置记录被截断 | ① 改用 `reg 0x0014 == 0x0200`（§7）② 用 `0x00080200/0x00083040` ③ 用完整 1042 对（§6.3） | `logs/pa-pulse.log:80`、`logs/downlink-tries.log:145` |
| `reg 0x0030` 一直 `0x0000` | **正常**，本产品不更新该寄存器 | **不要**据此重试/复位；改用 `0x0014` | §7.1 |
| `reg 0x0014 != 0x0200`（例如 `0x0000`） | 应用没就绪：上传没完成 / 配置记录没灌 / 软复位没发 | 检查 release 五步顺序（§5.1）；确认最后一步 `reg 0x0006 <- 0x0002` + 200 ms 真的发了 | `logs/final-pa.log:77-81` |
| `Command failed...Resultcode = 0x0002` + `ERROR: failed to load the firmware` | 入口点/地址配对错了（`prgmBase`/`execAddr` 对调） | 用镜像自带的 `0x00080200` / `0x00083040`，别换 | `logs/boot-addr-matrix.log:380-382` |
| 上传中途报 `number of words is out of range. Maximum is 126` | 单次 `VprocTwolfHbiWrite` 的字数 > 126 | 按记录拆包（记录本身最大 16 字） | `PROTOCOL-REPORT.md` §1.4 |
| **DAC1_P/DAC1_M 有波形，喇叭完全没声** | **`GPMD_A (reg 0x12) = 0x08`，group A 不在 GPIO 模式 ⇒ SHDN 完全不动** | `0x12 <- 0x02`、`0x04 <- 0x00`，然后 `0x02 <- 0x00`（≥2 ms）→ `0x02 <- 0x02`（§9.4） | `config.h` PA 段、`logs/final-pa.log:90-97` |
| 喇叭响了一会儿之后自己变小声/变哑 | **SHDN 上多了上升沿**（又写了一次 `reg 0x02`），功放换到 mode 2/3/4 | 沿数审计 + 锁存（§9.5）；关静音靠"不喂音频"，不靠关功放 | `zl38063_audio_codec.cc:177-188` |
| 配网/操作后突然没声音，`0x0034` 又变回 `0xD3D3` | 有代码路径写了 `reg 0x0014`（mbox probe / 硬件复位） | 加 `is_running()` 守卫，禁掉所有开发期探针（§8） | `zl38063_dsp.c:2156-2167` |
| 读回校验大面积失配（`mismatching_words` 上千） | ① 读侧编码是猜的（`& 0xFF` vs `(& 0xFF)/2`）② 打印时把低 16 位当低字节 | 跑 `B220_DSP_PAGE255_CALIB` 标定，用 `enc=b`（§4.5） | `logs/upload-verify.log:405-418`、`logs/page255-calib.log:417` |
| 配置记录只写了 260 条；下行 `0x0494/0x049A` 读回还是上电默认 | 把长度字段 `0x0412` 当**字节数**；两个寄存器在索引 330/333（被漏掉的尾部） | 用 `gen_full_config_record.py`，灌完整 1042 对 | §6.3、`logs/downlink-tries.log:171-173` |
| 回环里 `peak=0/mean=0` 一直不变 | DSP 没在驱动 DIN（没启动 / 没配置） | 先过 §10.4 的 pre-GO 校验，再过 §7.2 的 `0x0014` 判据 | `logs/mic-loopback.log:152-184` |
| 开机偶尔能通、偶尔不能 | I2S / PA / DSP 三者的**时序**边缘 | 固定顺序：`I2S 起 → GPIO19 复位(10 ms) → 500 ms → HBI attach → …`；PA 使能只做一次并锁存 | §3.3、§5.1、§9.4 |
| 一上电就进下载模式 / 启动失败 | GPIO0 被外部拉低（strapping） | 检查 GPIO0 上有没有飞线/短接；空闲保持高、悬空 | §2.3 |
| 2 ms 的 SHDN 低电平保持不准 | 用了 `vTaskDelay`（tick 粒度 10 ms） | 用 `esp_rom_delay_us(2000)` | `xiaomodou_b220.cc:1209` |

---

## 12. 被证伪的假设清单

按时间顺序，**每条都附上推翻它的证据**，方便后来者别再走：

| # | 曾经的假设 | 结论 | 推翻它的证据 |
| --- | --- | --- | --- |
| 1 | **DSP 会 Auto Boot**，ESP32 只需握手再写配置 | ❌ **被证伪** | `logs/hands-off.log`：有 BCLK 的前提下，不碰复位、不灌东西，连续 20 s `reg 0x0034` 都是 `0xD3D3`、`0x0030` 恒 0 ⇒ 一直坐在 boot ROM 等。原厂镜像里也确实有完整 host-boot 载荷 |
| 2 | **HBI 帧格式/字节序错了** | ❌ 被证伪 | 与 ADF 参考实现 5 个代表性操作逐位一致；且 `0xD3D3` 是回文，字节序写反也会读出它，而我们读到 `0x0000` |
| 3 | **缺一个"上电使能"步骤**（原厂复位 DSP 前调了三个函数） | ❌ 被证伪 | `0x40203350` = `xTimerStop`×2（纯软件）；`0x40124ce8` = `gpio_set_intr_type(38, 0)`（GPIO38 在 ESP32-D0WD 上不存在）；`0x40207ae8` = `audio_pa_on`（只影响功放出不出声） |
| 4 | **配置记录有"门控字节"**，只写 130/260 条 | ❌ 被证伪 | `VprocTwolfLoadConfig` 循环里**只有一个分支（回循环头）**，`numwords` 硬编码 1，**260 条全写**；`rec+4k+42` 规则在二进制里不存在 |
| 5 | **配置记录是 260 条**（把 `0x0412` 当字节数） | ❌ **被证伪** | `addx4 end = data + 4*numElements`，`numElements = 1042` ⇒ **1042 对**；ADF 的 `zl38063_config.o` 也是 `configStreamLen = 1042` |
| 6 | **复位网络被按住**（`gpio_get_level(19)` 读回 0） | ❌ 被证伪 | 示波器实测：开机后 GPIO19 拉高并保持高。读回 0 是读数本身不可信（同一日志里"输出驱动高"也读 0） |
| 7 | **`prgmBase` 和 `execAddr` 写反了** | ❌ 被证伪（**报告写反了，不是代码**） | `zl38063_firmware_image.md` §1：`prgmBase = 0x00080200`、`execAddr = 0x00083040`；实测对调后上传直接失败（`Resultcode = 0x0002`） |
| 8 | **入口点不对是"应用不跑"的原因** | ❌ 被证伪 | `B220_DSP_ADDR_MATRIX` 两组配对都跑完，都没让应用起来；而且当时的判据（`0x0030`）本身就无效 |
| 9 | **`reg 0x0030`（app status）是就绪判据** | ❌ **被证伪** | 声音已经正常的机器上它依然是 `0x0000`；原厂自己的 route 例程用的门是 `reg 0x0014 == 0x200` |
| 10 | **"传输成功"就等于"DSP 在跑"** | ❌ 被证伪 | 上传返回成功、`app status` 仍 0；真正的分界是 `reg 0x0014` 与 DAC/喇叭的实际信号 |
| 11 | **DAC 有波形就等于有声** | ❌ 被证伪 | DAC 有 1 kHz 方波、喇叭全静音 ⇒ 功放级被 `GPMD_A = 0x08` 关死 |
| 12 | **AW8737S 的 SHDN 是普通"高有效 enable"** | ❌ 被证伪 | 它是低有效 + **按上升沿个数选模式**（1 沿 = mode 1 / 4 沿 = NCN off），换模式前需要 SHDN 低 > 1 ms |
| 13 | **第二次 SPI（400 kHz）是 DSP 控制口** | ❌ 被证伪 | 那条路径实际是 `esp_vfs_fat_sdmmc_mount` 的 **SDSPI 分支**（SD 卡 SPI 模式初始化本来就 400 kHz）；DSP 的 HBI 在 SPI2 上 |
| 14 | **`0x00080200/0x00083040` 是 ESP32 侧地址**（`+0x3f400000` 的算术巧合） | ❌ 被证伪 | 它们是 **DSP 侧**地址，只被写进 `reg 0x012C` 并打印；没有任何代码解引用 `0x3f480200` |
| 15 | **配置记录（260 条）与下行无关** | ❌ 被证伪 | `0x0494/0x049A` 就在被漏掉的尾部（索引 330/333），而它们正是原厂播放路径要写的两个寄存器 |
| 16 | **上传的读回校验失败 ⇒ 上传真的错了** | ❌ 被证伪 | 那批失配来自读侧编码是**猜的**（把低 16 位当低字节，`0x200`）；标定后同一校验 `mismatching_words = 0` |
| 17 | **`reg 0x0C` 读回值可以用来反推窗口地址** | ❌ 被证伪 | 写 `{0x0008,0x0A24}` 读回 `{0x0008,0x0A00}`（低 5 位被抹），只有 /2 之后的字偏移有意义 |
| 18 | **`GPIO0` 还能当小智的 BOOT 按键** | ❌ 被证伪 | 它被 DSP 片选占用，且是启动 strapping 脚 |

---

## 13. 附录

### 13.1 上机命令速查

导固件镜像 / 记录数组 / C 头、导完整 1042 对配置记录、与 ADF 对比 —— 三条命令见 **§6.1 与 §6.5**，不重复。另两条：

```powershell
# 反汇编辅助：本项目所有"逐指令"结论都是用逐字节步进解出来的。
# ★ 不要相信 objdump 的线性列表：literal pool 会让边界每 1~2 字节漂移，
#   必须从"上一个已确认的指令地址"开始逐窗口问。
python tools\zl_step.py ...

# 唤醒词模型（独立课题，见 WAKE-WORD-PATH-REVERSE.md / wakenet/README.md）
python xiaozhi-esp32-b220\main\boards\xiaomodou-b220\zl38063\wakenet\wakenet5_tool.py info
python xiaozhi-esp32-b220\main\boards\xiaomodou-b220\zl38063\wakenet\wakenet5_tool.py emit <dir>
```

### 13.2 `config.h` 里与本章相关的开关

| 开关 | 生产值 | 作用 |
| --- | --- | --- |
| `B220_AUDIO_BRINGUP` | **1** | 上电做 HBI 引导 |
| `B220_I2S_BEFORE_DSP` | **1** | ★ I2S 先于 DSP 复位起来（§3） |
| `B220_DSP_ADF_RELEASE_TX` | **1** | 走 §5.1 的五步 release（生产路径） |
| `B220_DSP_BOOT_ATTEMPTS` | 6 | 重试次数（对齐原厂 6 次） |
| `B220_ZL_DIAG` / `B220_DSP_HANDS_OFF` | 0 / 0 | 寄存器 dump + CS/mode/clock 扫描 / "只挂 HBI 不动复位"对照（§3.2） |
| `B220_DSP_VERIFY_UPLOAD` / `_VERIFY_BEFORE_GO` | 0 / 0 | 上传后抽样读回 / ★ GO 之前的 in-RAM 校验（§10.4） |
| `B220_DSP_PAGE255_CALIB` | 0 | 窗口读编码标定（§4.5，**会写 scratch RAM**） |
| `B220_DSP_ADDR_MATRIX` / `_RELEASE_MATRIX`(`_V2`) | 0 / 0 | 地址配对矩阵（§5.4 死路 A）/ release 顺序矩阵 |
| `B220_DSP_DOWNLINK_TRIES` | 0 | 19 种下行写序列试探（§6.3） |
| `B220_AUDIO_LOOPBACK` / `AUDIO_TEST_MODE` | 0 / 0 | 回环任务 / 0=回环 1=1 kHz 单音 2=扫频 3=回环+蜂鸣 |
| `B220_I2S_TX_VARIANT_SWEEP` / `B220_PA_BIT_FINDER` | 0 / 0 | I2S 电平极性扫描 / 22 步 AW9106B 走位（§9.3） |
| `B220_AUTO_LISTEN` | 0 | 无按键时自动进会话（本板没有可读按键） |

> **借用原则**：这些诊断开关都是**编译期**决定的，且**绝大多数会写设备寄存器**。
> 上线构建里确保它们**全是 0**，只留 `B220_AUDIO_BRINGUP=1 / B220_I2S_BEFORE_DSP=1 / B220_DSP_ADF_RELEASE_TX=1`。
> 诊断构建里一次只开一个，否则日志会互相污染（本项目就是这么做的，每个日志文件对应一组开关）。

### 13.3 关键日志行 → 证据文件对照

| 阶段 | 代表性日志行 | 文件 |
| --- | --- | --- |
| HBI 不通（早期） | `ERROR: HBI is not accessible (0x0000, want 0xd3d3)` | `logs/hbi-diag.log:61` |
| 扫描定案 | `sweep HIT: cs=0 mode0 10MHz reg0x34=0xD3D3` | `logs/i2s-before-dsp.log:191` |
| hands-off 对照 | `hands-off +1s: ... 0x0034=0xD3D3 ...` | `logs/hands-off.log:61-80` |
| I2S 先行 | `I2S up before DSP boot: 48 kHz, 16-bit, stereo` | `logs/i2s-before-dsp.log:61` |
| 地址矩阵 | `matrix: neither pairing produced a running application` | `logs/boot-addr-matrix.log:398` |
| 上传校验（旧） | `mismatching_words=13780` | `logs/upload-verify.log:416` |
| 窗口标定 | `CONCLUSION enc=b ... reproduces the pattern 16/16` | `logs/page255-calib.log:417` |
| GO 前校验 | `SUMMARY run=1 ... mismatching_words=0` | `logs/pre-go-verify.log:75` |
| 尾部寄存器 | `record entries [0..1042) = regs 0x0200..0x0A22, 1042 register write(s) -> 1042 ok` | `logs/downlink-tries.log:171` |
| 应用就绪 | `codec: reg 0x0014 = 0x0200 -> the ZL38063 application is READY` | `logs/production-codec.log:86` |
| 功放定案 | `pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02` | `logs/final-pa.log:97` |
| 上行有信号 | `loopback t=+4s: ... peak=12656 mean=5937` | `logs/mic-loopback.log:153` |
| 下行单音 | `tone t=+2s: ... tx_peak=32759 tx_mean~20855 write_err=0` | `logs/pa-pulse.log:146` |

### 13.4 一页纸的最短可行路径

```text
1. 确认引脚（万用表通断，断电）：GPIO0=HCS, 19=RESET, 5/25/26/35=I2S, 32/33/34=SPI2
2. 起 I2S0 主模式：48 kHz / 16 bit / stereo / bck5 ws25 dout26 din35，TX+RX 都 enable，别停
3. GPIO19 低 10 ms → 高；等 500 ms
4. SPI2 上挂 HBI device：cs=0, 10 MHz, mode 0, queue 6；GPIO0 设为 floating
5. 写 0xFD80（HBI 端口使能）
6. 写 reg 0x0014 <- 1；等 300 ms；读 reg 0x0034，必须 == 0xD3D3   ← 到不了这里就别往下走
7. 灌 3699 条 twFwr 记录（reg 0x000C 设窗口基址 + 0xFF00|lowbyte 写数据）
   —— 灌完（还没发 GO）读回抽样，mismatching_words 必须 == 0
8. 写 reg 0x012C <- 0x00083040（2 字）
9. 写 reg 0x0032 <- 0x000D
10. 灌 1042 对配置记录（一对一次写，numwords=1）
11. 写 reg 0x0032 <- 0x0008（GO）
12. 写 reg 0x0006 <- 0x0002（软复位）；等 200 ms
13. 读 reg 0x0014，必须 == 0x0200                                    ← 应用就绪的唯一判据
14. AW9106: 0x12<-0x02, 0x04<-0x00, 0x02<-0x00, ≥2 ms, 0x02<-0x02（一个沿！）然后锁存
15. 播 1 kHz 全幅正弦到 I2S TX → 听到 = 整条下行通了
16. 回环 + 每 2 秒 peak/mean 日志 → 说话时 peak 上万 = 整条上行通了
```

到此为止：**上行、下行、功放、状态判据**四段全部有独立证据。本章结束。
再往下是唤醒词与协议层的事（不属于本章范围）。


---

# 第三部分：正式 codec · 联网 · 唤醒词 · 排错与方法论

# 小魔豆 B220 → 小智（xiaozhi-esp32）移植实战指南 · PART 3

> **把移植做成一台"真的小智"**：生产级音频 codec、联网与服务器、唤醒词，外加合并排错表、经验方法论与验收清单。
>
> 阅读前提：PART 1（可行性 + 引脚逆向 + 刷机）、PART 2（板级骨架：屏幕 / 背光 / WiFi / DSP host-boot）。
> 本部分只写**让设备成为一台可用语音助手**所需的最后一层，以及这一层的所有坑。
>
> 证据标注约定（全文统一）：
> - **[实测]** — 在本机 B220 上跑过、有串口日志或仪表读数。
> - **[逆向]** — 从原厂固件 `backup/esp32-16MB-full.bin` 反汇编/数据挖掘得到。
> - **[推断]** — 由已知事实推导，尚未单独验证。
>
> 本机基线：MAC `b4:e6:2d:fc:4c:e5`，刷机口 **COM25**（3.3 V TTL，`--before no_reset`），
> ESP-IDF **v6.1**，应用版本 `2.5.1`，ESP32-D0WD rev1.0 / 16 MB flash / 8 MB PSRAM。

---

## 0. PART 3 的边界：PART 2 结束时还缺什么

PART 2 结束时设备的真实状态是：屏幕亮、背光亮、WiFi 能连、PSRAM 与 240 MHz CPU 正常、
ZL38063 已经 host-boot 成功并能通过 `reg 0x0014 == 0x0200` 证明"应用 READY"——
但**音频还是 `DummyAudioCodec`**，说话没人听、回答没人听、也没有唤醒词。

PART 3 要补的正是这三块：

| 缺口 | 本部分章节 | 完成判据（日志） |
| --- | --- | --- |
| 没有真的音频 codec | §1 / §2 / §3 | `Zl38063AudioCodec: audio bring-up: I2S0 tx/rx handles are up` |
| 没有连上服务器 | §4 | `MQTT: Session ID: xxxxxxxx` + `StateMachine: activating -> idle` |
| 没有唤醒词（也没有按键） | §5 | `Application: Wake word detected: …` |

这三件事**互相依赖**：唤醒词要 16 kHz 单声道（由 codec 的采样率声明驱动重采样），
服务器要 24 kHz 播放（由 codec 的 48 kHz 输出重采样），功放要一次性使能（由 codec 的
`EnableOutput(true)` 触发）。所以顺序必须是 **codec → 服务器 → 唤醒词**。

---

## 1. 写一个真实的 `AudioCodec` 子类

### 1.1 要实现的接口（直接引自 `main/audio/audio_codec.h`）

`AudioCodec` 是一个只有两个纯虚函数的抽象基类，其余都可覆盖：

```cpp
class AudioCodec {
public:
    virtual void SetOutputVolume(int volume);      // 基类：存 NVS + 打日志
    virtual void SetInputGain(float gain);         // 本板不用
    virtual void EnableInput(bool enable);         // 基类：只置标志
    virtual void EnableOutput(bool enable);        // 基类：只置标志

    virtual void OutputData(std::vector<int16_t>& data);   // 基类 → Write()
    virtual bool InputData(std::vector<int16_t>& data);    // 基类 → Read()
    virtual void Start();                                   // 基类：读 NVS 音量

    // 只读属性，音频服务靠它们决定要不要重采样
    inline bool duplex() const;
    inline int  input_sample_rate() const;
    inline int  output_sample_rate() const;
    inline int  input_channels() const;
    inline int  output_channels() const;

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;
    bool duplex_          = false;
    int  input_sample_rate_  = 0;
    int  output_sample_rate_ = 0;
    int  input_channels_  = 1;
    int  output_channels_ = 1;

    virtual int Read(int16_t* dest, int samples) = 0;     // 唯一必写的两个
    virtual int Write(const int16_t* data, int samples) = 0;
};
```

关键点：**`Read` / `Write` 是 `protected` 且是纯虚**。外部永远通过
`InputData()` / `OutputData()` 调用它们（`audio_codec.cc:17-27`），而这两个包装会处理
`std::vector` 的尺寸与返回值。B220 的实现文件是
[zl38063_audio_codec.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063_audio_codec.h) 与
[zl38063_audio_codec.cc](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063_audio_codec.cc)。

本板重写了 6 个：`Read`、`Write`、`EnableOutput`、`EnableInput`、`SetOutputVolume`、`Start`。
`OutputData` / `InputData` / `SetInputGain` 用基类即可。

### 1.2 三个必须设置对的成员（否则声音一定错）

| 成员 | 值 | 理由 | 证据 |
| --- | --- | --- | --- |
| `duplex_` | `true` | ESP32 是 I2S0 **master**，BCLK/WS 由 TX 产生、RX 借用同一组时钟；声明全双工后音频服务不会去关 TX 时钟 | **[实测]** `W (1204) i2s_common: the rx channel on I2S0 is switched from master to slave for full-duplex mode` |
| `input_channels_` | `2` | ZL38063 把**同一个样本同时驱动到 L/R 两个 slot**（回环日志里 RX 十六进制是成对重复：`FFED FFED FFF2 FFF2`）；声明 2 通道后音频服务的"取左声道"路径是**无损**的 | **[实测]** [mic-loopback.log](logs/mic-loopback.log) |
| `output_channels_` | `1` | Opus 解码器按 `ESP_AUDIO_MONO` 打开，`Write()` 收到的是**单声道 PCM**，由 codec 自己复制到两个 slot | **[实测]** 1 kHz 测试音写法相同（"the same sample, no channel swap"） |

`input_sample_rate_` / `output_sample_rate_` 来自板级 `config.h` 宏，见 §2（**这一条最贵**）。

### 1.3 核心：**复用板级已经建好的 I2S 句柄，绝不 `i2s_new_channel`**

这是整个 codec 最重要的一条设计约束，理由有两条：

1. **一个 port 只能建一次 channel。** `i2s_new_channel()` 对同一个 I2S 控制器第二次调用必然失败。
   I2S0 已经被板级的 `B220_I2sUpBeforeDspBoot()`（[xiaomodou_b220.cc](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/xiaomodou_b220.cc) 第 574 行起）占用了。
2. **DSP 需要时钟先于它自己的复位。** ZL38063 是 I2S **slave**，没有自己的 MCLK，
   它从上电起就靠 ESP32 的 BCLK/WS 活着；`B220_I2sUpBeforeDspBoot()` 在 `zl38063_dsp_begin()`
   **之前**建好 48 kHz/16-bit/stereo 的全双工口并 `i2s_channel_enable()`，正是为了让 DSP 能应答 HBI。
   **[实测]** 这是 Phase 3 的关键突破：I2S 先起，host-boot 才从"读不到 `0xD3D3`"变成"上传成功"。

共享接口放在
[b220_audio_share.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/b220_audio_share.h)，
用 `extern "C"` 声明，**板级文件和 C++ codec 都 include 它**——签名漂移变成编译错误而不是链接惊喜：

```c
/* b220_audio_share.h */
#ifdef __cplusplus
extern "C" {
#endif
i2s_chan_handle_t b220_audio_i2s_tx(void);   // NULL = bring-up 没跑或失败
i2s_chan_handle_t b220_audio_i2s_rx(void);
bool b220_audio_dsp_ready(void);             // reg 0x0014 == 0x0200 才是 true
esp_err_t b220_audio_pa_enable_once(const char* tag);   // 幂等（见 §3）
#ifdef __cplusplus
}
#endif
```

实现侧（`xiaomodou_b220.cc:1298-1324`）就是"把一个静态句柄交出去"：

```cpp
extern "C" i2s_chan_handle_t b220_audio_i2s_tx(void) {
#if B220_I2S_BEFORE_DSP
    return s_i2s_tx;
#else
    return nullptr;   /* 关掉 I2S-before-DSP 时明确给 NULL，而不是野指针 */
#endif
}
extern "C" esp_err_t b220_audio_pa_enable_once(const char* tag) {
    return b220_pa_enable_one_pulse(tag);
}
```

消费侧（codec 构造函数，成员初始化列表里就取句柄）：

```cpp
Zl38063AudioCodec::Zl38063AudioCodec(int input_sample_rate, int output_sample_rate)
    : tx_handle_(b220_audio_i2s_tx()), rx_handle_(b220_audio_i2s_rx()) {
    duplex_ = true;
    input_reference_  = false;
    input_channels_   = 2;      /* 见 §1.2 */
    output_channels_  = 1;
    input_sample_rate_  = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    ReportBringupOnce();
}
```

codec 在**析构函数里什么都不做**——这是刻意的：

```cpp
Zl38063AudioCodec::~Zl38063AudioCodec() {
    /* 不 disable / 不 delete / 不 re-init：
     * DSP 是 I2S slave，时钟停了它就掉应用，而这是它直到断电都需要的时钟。 */
}
```

> ⚠️ **不要**照抄绝大多数板子的 `i2s_channel_disable(tx_handle_)` 式析构。B220 上那样做等于
> 把正在运行的 DSP 时钟掐掉。同理 `EnableInput(false)` 也**不**去 disable RX channel。

### 1.4 失败模式：句柄为 NULL 时只打一次日志，然后返回 −1

板级的 RX enable 是**非致命**的（`ESP_LOGW(TAG, "I2S: RX enable failed …; TX keeps the clock running")`），
所以"tx 有效、rx 为 NULL"是**合法状态**。codec 必须能在这个状态下不死机、不崩、不刷屏：

```cpp
void Zl38063AudioCodec::ReportBringupOnce() {
    if (reported_) return;                     /* 一辈子只打一次 */
    reported_ = true;
    if (tx_handle_ == nullptr || rx_handle_ == nullptr) {
        ESP_LOGE(TAG, "audio bring-up did NOT leave a usable I2S0 pair (tx=%p rx=%p): "
                      "B220_I2sUpBeforeDspBoot() failed or never ran.  This codec does "
                      "NOT create a channel of its own (a second i2s_new_channel() on "
                      "I2S0 would fail), so audio input returns silence (-1/0) and "
                      "playback is dropped; the rest of the firmware keeps running.",
                 (void*)tx_handle_, (void*)rx_handle_);
    } else {
        ESP_LOGI(TAG, "audio bring-up: I2S0 tx/rx handles are up (tx=%p rx=%p)", …);
    }
    if (!b220_audio_dsp_ready()) {             /* 只是警告，从不拒绝运行 */
        ESP_LOGW(TAG, "the ZL38063 application is not proven ready (reg 0x0014 != 0x0200 …)");
    }
}
```

`Read()` / `Write()` 开头的守卫：

```cpp
int Zl38063AudioCodec::Read(int16_t* dest, int samples) {
    if (dest == nullptr || samples <= 0) return 0;
    if (rx_handle_ == nullptr) { ReportBringupOnce(); return -1; }   /* 绝不碰 NULL 句柄 */
    …
}
```

三条设计原则，值得照搬到任何板子：

- **绝不在音频任务里分配内存。** `Write()` 用固定成员 `int16_t tx_block_[240*2]` 做暂存，
  不是每次 `std::vector`。音频输出路径每 60 ms 走一次，堆分配会碎。
- **单次调用有硬预算。** `Read()` 200 ms、`Write()` 1 s 上限（`esp_timer_get_time()` 判定），
  超了就"有多少交多少"。音频任务被一次卡死会让整个 `AudioService` 停摆。
- **错误只报一次。** `rx_warned_` / `tx_warned_` 两个 bool，避免 48 kHz 下每秒几千行日志。

### 1.5 帧与速率：所有尺寸都从 DMA 配置推导，不要写死魔数

| 常量 | 值 | 来源 |
| --- | --- | --- |
| `B220_CODEC_I2S_SLOTS` | 2 | 端口是 stereo |
| `B220_CODEC_FRAME_BYTES` | 4 | 2 slot × `sizeof(int16_t)` |
| `B220_CODEC_BLOCK_FRAMES` | **240** | 恰好是端口 `dma_frame_num = 240`（`B220_I2sUpBeforeDspBoot()`），即 5 ms 音频 |
| `B220_CODEC_BLOCK_BYTES` | 960 | 240 × 4 |
| `B220_CODEC_READ_TIMEOUT_MS` | 100 | 单次 `i2s_channel_read` 超时 |
| `B220_CODEC_READ_BUDGET_US` | 200000 | 一次 `Read()` 的总预算 |
| `B220_CODEC_WRITE_TIMEOUT_MS` | 200 | 单次 `i2s_channel_write` 超时 |
| `B220_CODEC_WRITE_BUDGET_US` | 1000000 | 一次 `Write()` 的总预算 |

**按 DMA block 分块**的原因：`i2s_channel_read/write` 只会按整描述符推进，
一次给 960 字节正好一个描述符，短传输永远不会切裂一个 stereo frame。
`Read()` 里另有一道保护——**奇数样本请求直接丢掉尾巴一个**（`if (samples & 1) --samples;`），
否则 L/R 从此整体旋转。音频服务总是请求整数帧，所以这是防御而非正常路径。

`Read()` 的正常返回是"写入 `dest` 的 int16 个数"（交错 L/R），`Write()` 返回"消费的**单声道**样本数"。
`ESP_ERR_TIMEOUT` 在 `Read()` 里**不是错误**（RX 由 TX 时钟驱动，没数据往往只是端口没跑），
收集到什么返回什么。

### 1.6 `Start()` 与 `SetOutputVolume()`：诚实地"什么都做不了"

```cpp
void Zl38063AudioCodec::Start() {
    AudioCodec::Start();      /* 基类：读 NVS 音量 + 打 "Audio codec started" */
    ReportBringupOnce();
    ESP_LOGI(TAG, "ZL38063 codec started: %d Hz / 16-bit / 2 slot(s); …");
}
```

`SetOutputVolume()` 是这块板子最反直觉的一个：**数值被存进 NVS（重启后保留、UI 可见），
但完全不影响音量**——真正的输出电平在 DSP 内部（`reg 0x238/0x23A`，`-90 dB + vol`），
由 boot 时灌入的配置记录 / `zl38063_dsp_set_volume()` 写死。所以实现是
"调基类 + 一条 `ESP_LOGW` 说明它不生效"。**不要**在这里做软件增益：那要给每个播放样本做乘法，
而已经验证过的通路并不那么做。

---

## 2. ⭐ 采样率这一坑：硬件真的是 48 kHz，而且改不了

### 2.1 结论与证据

`config.h` 里必须是这样（这是**唯一正确**的写法）：

```c
/* Phase 4 (production audio): these are the rates the ONE I2S0 link to the
 * ZL38063 really runs at. */
#define AUDIO_INPUT_SAMPLE_RATE  48000
#define AUDIO_OUTPUT_SAMPLE_RATE 48000
```

证据链（三条独立证据，互相印证）：

| # | 事实 | 证据 |
| --- | --- | --- |
| 1 | 板级初始化就写死 48 kHz，且在 DSP 复位**之前** | **[实测]** `I (1214) XiaoMoDouB220: I2S up before DSP boot: 48 kHz, 16-bit, stereo (bck=5 ws=25 dout=26 din=35)` |
| 2 | 往这个 48 kHz 端口写 **1 kHz 正弦**，喇叭里听到的就是 **1 kHz** | **[实测]** 耳朵判定；软件侧同时证明数据正确：`tx_peak=32759 tx_mean~20855 write_err=0`（`tx_mean/peak = 0.6366 ≈ 2/π`，正是满幅正弦的均值） |
| 3 | 原厂机示波器实测 **I2S_WSA = 48 kHz**，BCLK 1.54 MHz（32 BCLK/frame = 16-bit slot） | **[实测]** 用户示波器，见 [PHASE3-DSP-DEBUG-STATUS.md](PHASE3-DSP-DEBUG-STATUS.md) §2；BCLK 与 slot 宽度在 `i2s-tx-sweep` 期间复核 |

**为什么"改不了"**：`B220_I2sUpBeforeDspBoot()` 在 DSP 复位前就把端口配好，之后**从不重配**。
DSP 是 I2S slave，上电起就必须有 BCLK/WS；把端口停掉去改采样率 = 把 DSP 的应用打断。
而 ZL38063 自己**不做任何采样率转换**（证据 #2：写什么频率就出什么频率）。

### 2.2 如果误写 16000 会怎样（这是真踩过的坑）

`AudioCodec::input_sample_rate()` 是**给音频服务看的声明**，不是"去设置硬件"。
音频服务的逻辑是：**声明值 ≠ 16000 就建重采样器**。

```cpp
/* main/audio/audio_service.cc:73-80 */
if (codec->input_sample_rate() != 16000) {
    esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
        codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
    esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
}
```

所以：

| 声明 | 实际 | 后果 |
| --- | --- | --- |
| 48000 | 48 kHz | 建 48k→16k 重采样器，一切正确 |
| **16000** | **48 kHz** | **不建重采样器**，服务以为拿到的就是 16 kHz；于是采集/播放全部以 **3 倍速度**跑——音调升三个八度都不止，且 Opus 编码器/解码器的时间戳全错 |

同样地，`output_sample_rate()` 决定 Opus 解码器的打开速率（`audio_service.cc:53`），
写错会让播放变调。codec 里因此放了一条**自检警告**：

```cpp
if (input_sample_rate_ != 48000 || output_sample_rate_ != 48000) {
    ESP_LOGW(TAG, "the codec was told %d/%d Hz but the I2S0 port runs at %d Hz "
                  "(B220_I2S_SAMPLE_RATE_HZ) - the audio service will resample, and "
                  "playback/capture would be pitch-shifted if it did not", …);
}
```

看到这条警告 = `config.h` 被人改错了。

### 2.3 小智内部的重采样发生在哪、怎么发生的

三处，全在 [audio_service.cc](xiaozhi-esp32-b220/main/audio/audio_service.cc)：

**(a) 上行 48k → 16k（给唤醒词 / Opus 编码器 / ASR）**
`ReadAudioData(data, 16000, samples)` 是唯一入口：

```cpp
/* audio_service.cc:195-225 */
bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) { … codec_->EnableInput(true); }
    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) return false;
        if (input_resampler_ != nullptr) {
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            esp_ae_rate_cvt_process(input_resampler_, data.data(), in_sample_num, resampled.data(), …);
        }
    } else { … }
    last_input_time_ = std::chrono::steady_clock::now();   /* ← 15 s 掉电计时基准 */
}
```

调用点有两类（`audio_service.cc:294` 与 `:313`）：编码器/ASR 每帧 60 ms、唤醒词 **10 ms（160 样本）**。
**[实测]** 启动日志：`I (7884) AudioService: Resampling audio from 16000 to 48000`（播放侧）与
`I (103764) AudioService: Resampling audio from 16000 to 48000`（唤醒词侧首次分配）。

**(b) 下行 服务器 24k → 设备 48k（播放）**
Opus 解码器以 `codec->output_sample_rate()`（48000）打开，于是 `decoder_sample_rate_ = 48000`；
而服务器音频包自带 24000：

```cpp
/* audio_service.cc:553-563 */
if (decoder_sample_rate_ != codec->output_sample_rate()) { … }
/* 实际生效的是解码后按包内 rate 判断（:430-437） */
```

**[实测]** `I (21074) AudioService: Resampling audio from 24000 to 48000`。

**(c) 那条"无害警告"是怎么来的**

服务器在音频通道打开的 hello 里说 24 kHz，设备的输出声明是 48 kHz，于是：

```cpp
/* main/application.cc:560-568 */
protocol_->OnAudioChannelOpened([this, codec, &board]() {
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
        ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, "
                      "resampling may cause distortion",
                 protocol_->server_sample_rate(), codec->output_sample_rate());
    }
});
```

**[实测]** `W (13924) Application: Server sample rate 24000 does not match device output sample rate 48000, resampling may cause distortion`

> ✅ **这条警告在本板是正常的，不要试图消除它。** 它只是说"服务器 24 k、设备 48 k，中间要重采样"。
> 重采样确实发生了（`decoder` 输出 24 k → `esp_ae_rate_cvt` → 48 k），而且**音质实测可接受**
> （唤醒词对话全程 60 s、多轮问答清晰，见 [wakeword.log](logs/wakeword.log)）。
> 唯一"消除"它的办法是把设备声明成 24 kHz——那会直接违反 §2.1，绝不要做。

---

## 3. 功放使能与"锁存"设计：**整个 boot 只允许一个上升沿**

### 3.1 硬件为什么这么脆

功放是 **AW8737S**。它的 SHDN 引脚（A4）**低有效**，由 AW9106B（I2C `0x5B`）的
group-A 输出 `OUT_A1` = **寄存器 `0x02` 的 bit 1** 驱动。它不是普通 enable：
SHDN 先被拉低超过 TOFF（数据手册建议 1 ms）之后，芯片按**随后的上升沿个数**选工作模式。

| 上升沿数 | 模式 | 输出功率 |
| --- | --- | --- |
| **1** | mode 1 | **1.2 W ← 本板要的** |
| 2 | mode 2 | 1.0 W |
| 3 | mode 3 | 0.8 W |
| 4 | mode 4 | NCN 功能关 = **完全没有输出** |
| >4 | 不允许 | — |

**[实测]** 症状与结论完全吻合：boot 时曾在**三个地方**写 `reg 0x02`（原厂背光初始化序列、
ZL38063 group-A 诊断、回环的 PA-on），每次 `0 → 1` 都算一个沿。"DAC1_P/DAC1_M 上有 1 kHz 方波、
喇叭一点声音没有"就是 **mode 4**。

另一个致命细节 **[实测]**：`InitializeBacklight()` 回放原厂尾部时写 `0x11(CTL)<-0x1B` +
`0x12(GPMD_A)<-0x08`，如果这发生在 PA 使能**之后**，`GPMD_A = 0x08` 会让 group-A 输出**完全不驱动**
——1 kHz 音在 `0x02 <- 0xFF` 时都听不到，直到把 `0x12 <- 0x02` + `0x04(GPIO_CFG_A) <- 0x00` 写对才出声。

### 3.2 正确的五步序列（板级唯一实现）

实现在 `b220_pa_enable_one_pulse()`（[xiaomodou_b220.cc:1136](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/xiaomodou_b220.cc) 起）：

| 步 | 动作 | 为什么必须在这一步 |
| --- | --- | --- |
| 1/5 | `0x12 (GPMD_A) <- 0x02` | group-A 进入能驱动 SHDN 的模式；`0x08` 会让它失效 |
| 2/5 | `0x04 (GPIO_CFG_A) <- 0x00` | 所有 group-A 引脚设为普通输出 |
| 3/5 | `0x02 <- 0x00`，保持 **≥ 2 ms** | 登记 shutdown（TOFF）；用 `esp_rom_delay_us()` 忙等而不是 `vTaskDelay(1)`（tick 粒度只能给 1 ms，不够余量） |
| 4/5 | `0x02 <- 0x02` | **唯一一个上升沿** → mode 1 / 1.2 W |
| 5/5 | 锁存 `0x12` / `0x04` / `0x02` | 之后任何改动都被 `b220_aw9106_frozen_refuses()` 拒绝 |

### 3.3 单例 / 锁存：第二次调用必须是 no-op

```cpp
static bool s_pa_enable_done;                                  /* 一生只跑一次 */
static esp_err_t s_pa_enable_result = ESP_ERR_INVALID_STATE;   /* 第一次的结果，转发给后来者 */

static esp_err_t b220_pa_enable_one_pulse(const char* tag) {
    if (s_pa_enable_done) {
        ESP_LOGW(TAG, "pa: enable sequence already ran this boot (requested again by "
                      "%s) - IGNORED on purpose: a second 0 -> 1 on reg 0x02 bit 1 "
                      "would be SHDN rising edge #%lu and would move the AW8737S out "
                      "of mode 1 (1.2 W)", tag ? tag : "?", (unsigned long)(s_aw9106_reg02_rising_edges + 1u));
        return s_pa_enable_result;      /* 返回第一次的结果，不是"失败" */
    }
    s_pa_enable_done = true;
    …
}
```

同时 `b220_aw9106_write_reg02()` 是 `reg 0x02` 的**唯一写者**，它统计
`0x02` 写次数与 bit 1 的**上升沿次数**，超过 1 就大声报警：

```
pa: !!! AW8737S SHDN RISING EDGE #2 (bit 1 of reg 0x02 went 0 -> 1) - only 1 edge is
allowed, the amplifier is now in a HIGHER mode (2 edges = mode 2 1.0 W, 3 = mode 3
0.8 W, 4 = mode 4 NCN OFF = NO OUTPUT); offending write #N by <caller> !!!
```

**证明锁存生效的那两行日志**（同一次 boot，先板级后 codec）：

```
I (5844) XiaoMoDouB220: pa: SHDN enable sequence: 0x12(GPMD_A)<-0x02, 0x04(GPIO_CFG_A)<-0x00, 0x02<-0x00, hold 2 ms, 0x02<-0x02 (exactly 1 rising edge -> AW8737S mode 1, 1.2 W)
I (5924) XiaoMoDouB220: pa: AW9106B 0x5B reg 0x02 <- 0x02 (PA enable step 4/5: the one rising edge) -> ESP_OK | write #3, SHDN bit1=1, rising edges so far=1
I (5944) XiaoMoDouB220: pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02 | 0x02 writes=3, SHDN rising edges=1 (AW8737S mode 1)
        ↓ 3240 ms 后，音频服务进入 idle/使能输出
W (7844) XiaoMoDouB220: pa: enable sequence already ran this boot (requested again by Zl38063AudioCodec::EnableOutput(true)) - IGNORED on purpose: a second 0 -> 1 on reg 0x02 bit 1 would be SHDN rising edge #2 and would move the AW8737S out of mode 1 (1.2 W)
I (7874) Zl38063AudioCodec: Set output enable to true: the ZL38063 -> AW8737S PA -> speaker leg is live (PA enable returned ESP_OK)
```

最后两行就是**验收证据**：`SHDN rising edges=1` 且第二次请求被 IGNORED。
（这一对日志在 [production-codec.log](logs/production-codec.log) 与 [wakeword.log](logs/wakeword.log) 里都有。）

### 3.4 codec 侧怎么写 `EnableOutput`

```cpp
void Zl38063AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) return;      /* 幂等：状态没变直接返回 */
    AudioCodec::EnableOutput(enable);

    if (!enable) {
        /* 关不了。唯一的关断手段是再写一次 0x02，而每个 0->1 都是新的 SHDN 上升沿。
         * 静音来自"DSP 不再被喂音频"，不来自 PA。 */
        ESP_LOGI(TAG, "Set output enable to false: the AW8737S PA is left ON …");
        return;
    }

    /* 唯一被授权的入口：五步序列，第一次调用后锁存；板级 boot 通常已经先跑过，
     * 所以这里是一个打了日志的 no-op。两个调用点都安全。 */
    const esp_err_t pa = b220_audio_pa_enable_once("Zl38063AudioCodec::EnableOutput(true)");
    if (pa == ESP_OK) {
        ESP_LOGI(TAG, "Set output enable to true: the ZL38063 -> AW8737S PA -> speaker leg is live (PA enable returned ESP_OK)");
    } else {
        ESP_LOGW(TAG, "Set output enable to true, but the AW8737S PA enable did not complete cleanly (%s); …", esp_err_to_name(pa));
    }
}
```

谁先调用？**板级构造函数**（`xiaomodou_b220.cc:3746`）：

```cpp
s_b220_dsp_ready = B220_AudioProbeDspReady();
{
    const esp_err_t pa_err = b220_pa_enable_one_pulse("board init (production codec)");
    …
}
```

这样做的原因：`EnableOutput(true)` 只在音频服务第一次真的要出声时才被调用——可能是启动后
几十秒；而板级在 boot 就使能，能保证**任何时候出声都来得及**。而且两条路径共用同一个锁存，
所以无论谁先到，`0x02` bit 1 的上升沿总数永远是 1。

> ⚠️ **迁移警告**：`B220_BACKLIGHT_SWEEP=1` 会调用 `ProbeAw9106()`，它对 AW9106B 发 `0x7F`
> 软件复位，会**清掉 `0x02` 和 `GPMD_A`**。如果确实要用，必须在探针**之后**重新跑一次完整的
> PA 使能序列（或去掉 `0x7F` 复位），**不要只是把开关打开**。

### 3.5 与音频服务 15 秒空闲掉电的交互

`main/audio/audio_service.h:48`：

```c
#define AUDIO_POWER_TIMEOUT_MS 15000
```

`AudioService::CheckAndUpdateAudioPowerState()`（`audio_service.cc:833-853`）每秒检查一次：

```cpp
if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);  /* 让输入任务自己关 */
}
if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
    if (!(codec_->duplex() && codec_->input_enabled())) {   /* ← 本板命中的分支 */
        codec_->EnableOutput(false);
    }
}
```

对本板的具体含义（**实测**，见 [production-codec.log:153](logs/production-codec.log)）：

- 空闲 15 s 后 `EnableOutput(false)` 被调用 → 我们的实现**只打一行日志、不动硬件**，
  PA 依然在 mode 1、DSP 时钟依然在跑。所以"静音"仅仅是因为没人喂音频。
- 因为 `duplex_ == true`，所以当输入还使能时**连 `EnableOutput(false)` 都不会被调用**。
- 下一次要出声时 `EnableOutput(true)` 会再次被调用 → 走到锁存的 no-op 分支 →
  **不会产生第二个上升沿**。这正是 §3.3 那个设计要保护的东西。
- 输入侧是另一条路：15 s 无音频输入 → `AS_EVENT_AUDIO_INPUT_STOP_REQUEST` →
  输入任务调 `codec_->EnableInput(false)`。我们的 `EnableInput()` 只置标志，
  **不 disable RX channel**（BCLK/WS 必须继续给 DSP）。唤醒词要重新开始听时，
  `ReadAudioData()` 会自己 `EnableInput(true)`（`audio_service.cc:196-200`）。

> 💡 **这就是为什么本板不能"靠关功放省电"**：功放一旦使能就锁死在 mode 1 直到断电。
> 想做省电只能在 DSP 侧或整机电源侧动手，不在这个 codec 的职责内。

---

## 4. 联网与服务器

### 4.1 两条传输路径

小智固件在 OTA 之后从服务器下发的配置里选择传输（`main/application.cc:540-545`）：

```cpp
} else if (ota_->HasWebsocketConfig()) {
    protocol_ = std::make_unique<WebsocketProtocol>();
} else {
    ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
    protocol_ = std::make_unique<MqttProtocol>();
}
```

| | **MQTT + UDP（官方默认）** | **WebSocket** |
| --- | --- | --- |
| 控制通道 | MQTT over TLS，端口 **8883** | WSS |
| 音频通道 | **UDP**（AES-CTR 加密，密钥经 MQTT 下发） | WebSocket 二进制帧 |
| 延迟 | 低 | 中 |
| NAT 友好性 | **差**（UDP 打洞/端口可达性依赖网络） | **好**（走 443/TCP） |
| 本板状态 | **[实测] 控制通道通、音频通道不出声** | 推荐的回退路径 |

OTA 地址在 `sdkconfig`：`CONFIG_OTA_URL="https://api.tenclass.net/xiaozhi/ota/"`。
`MqttProtocol::GetHelloMessage()` 明确申请 UDP 传输：

```cpp
cJSON_AddStringToObject(root, "type", "hello");
cJSON_AddNumberToObject(root, "version", 3);
cJSON_AddStringToObject(root, "transport", "udp");   /* ← 只接受 udp */
```

### 4.2 成功时的日志长什么样（**[实测]**，逐字摘自 [wakeword.log](logs/wakeword.log)）

```
I (8354) WifiBoard: Connected to WiFi: dlink
I (8354) WifiBoard: Connected to WiFi: … ; sta ip: 192.168.x.x
I (8354) Application: Network connected
I (8354) StateMachine: State: starting -> activating
I (8364) Assets: The assets map size is 1600 KB (partition 8192 KB)
I (8594) MODEL_LOADER: Successfully load srmodels
I (8614) Ota: Current version: 2.5.1
I (9444) HttpClient: Established new connection to api.tenclass.net:443 protocol=https cost=830
I (9664) Ota: Current is the latest version
I (9674) Ota: Running partition: ota_0
I (9684) MQTT: Connecting to endpoint api.tenclass.net:8883
I (9684) MQTT: Resolved MQTT host api.tenclass.net -> 47.112.110.152
I (10514) MQTT: Connected to endpoint
I (10514) Application: Activation done
I (10514) StateMachine: State: activating -> idle
I (10524) wifi:Set ps type: 2, coexist: 0        ← 空闲省电模式
   … 唤醒词触发后 …
I (13924) MQTT: Session ID: 4c74af7f
W (13924) Application: Server sample rate 24000 does not match device output sample rate 48000, …
I (13944) StateMachine: State: connecting -> listening
I (17544) Application: >> 你好…                    ← 上行：设备把识别文本发过去
I (17554) StateMachine: State: listening -> speaking
I (19554) Application: << % get_weather...        ← 下行：服务器调 MCP 工具
I (21074) AudioService: Resampling audio from 24000 to 48000
I (25754) StateMachine: State: speaking -> listening
   … 多轮问答 …
I (99734) MQTT: Received goodbye message, session_id: 4c74af7f
I (99754) MQTT: Closing audio channel, send_goodbye: 0
I (99754) StateMachine: State: listening -> idle
```

要认的三个句号：

| 日志 | 含义 |
| --- | --- |
| `MQTT: Connected to endpoint` | 控制通道建立（TLS 到 8883 成功） |
| `MQTT: Session ID: <8 hex>` | 服务器接受了 `hello`，分配了一次会话 |
| `StateMachine: activating -> idle` | **激活完成，设备进入待命**——这是"这台机器能用了"的分界线 |

反向状态机也要会读：`idle -> connecting -> listening -> speaking -> listening` 是一次完整对话。

### 4.3 观察到的失败模式：控制通道通、**UDP 音频通道一个字节都没有**

**[实测]** 在**没有唤醒词、靠 auto-listen 主动开麦**的那轮（[auto-listen.log](logs/auto-listen.log)）里：

```
I (10144) StateMachine: State: activating -> idle
I (10984) XiaoMoDouB220: auto-listen: arm #1 (state idle -> listening)
I (10984) StateMachine: State: idle -> connecting
I (11154) MQTT: Session ID: fc729ef9
I (11154) Application: Server sample rate 24000 does not match device output sample rate 48000, …
I (11164) StateMachine: State: connecting -> listening
I (11314) AudioCodec: Set input enable to true
        … 整整 68 秒，没有任何 >> / << / 重采样日志 …
I (79464) MQTT: Received goodbye message, session_id: fc729ef9
I (79464) MQTT: Closing audio channel, send_goodbye: 0
I (79474) StateMachine: State: listening -> idle
I (80284) XiaoMoDouB220: auto-listen: arm #2 (state idle -> listening)
```

读法：

- **控制通道完全正常**：`Session ID` 拿到了，状态机走到 `listening`。
- **音频通道完全静默**：68 s 里没有任何 `Application: >>`（服务器没收到可识别语音）、
  没有任何 `Application: <<`（服务器没下发言论），也没有 `Resampling audio from 24000 to 48000`
  （服务器没发过一个音频包）。
- 服务器大约 **65 秒**后主动发 `goodbye` —— **这是服务端的会话超时**，不是崩溃：
  它按"这个会话从没收到过音频"判定为空会话并回收。

**判别方法**（一条命令的差别）：

```powershell
# 在设备处于 listening 状态时抓 10 s 日志，看有没有这两类行
Select-String -Path .\logs\xxx.log -Pattern 'Application: >>|Resampling audio from 24000'
```

| 现象 | 结论 |
| --- | --- |
| 有 `>>`、有 24000→48000 | 双向音频都通，会话正常（[wakeword.log](logs/wakeword.log) 就是这样） |
| 无 `>>`、65 s 后 `goodbye` | 上行 UDP 到不了服务器（**本条**） |
| 有 `>>`、无 24000→48000 | 上行通、下行丢 → 回程 UDP 被挡或服务器没回 |
| 连 `Session ID` 都没有 | 控制通道问题（不是本条的范畴） |

**推荐缓解手段**（按成本从低到高）：

1. **换 WebSocket 传输**——只要 OTA 响应里带 `websocket` 段，`HasWebsocketConfig()` 就为真，
   固件自动切到 `WebsocketProtocol`（`application.cc:540`）。走 443/TCP，UDP 完全不需要打洞。
   在服务器侧（xiaozhi-server 的 `data/.config.yaml` / 管理后台）启用 websocket 段即可，**固件不用改**。
2. **验证 NAT / 路由器**：把设备放到路由器 DMZ 或对 UDP 做放行；或换一台 AP 测试，
   排除运营商 CGNAT（**[推断]** 这是最常见的成因）。
3. **对照实验**：同一台设备在同一 WiFi 下用官方 App 或另一台成品小智连同一服务器。
   如果它也静默 → 网络/服务器问题；如果它正常 → 本机 NAT 会话或固件端问题。
4. 代码级确认：`mqtt_protocol.cc:371` 的 `udp->Connect(udp_server_, udp_port_)` 若失败会打
   `Failed to connect UDP audio channel: …`。**我们的日志里没有这一行**，
   说明 UDP socket 打开了、`connect()` "成功"了，但服务器从未收到包——
   所以问题在**包的投递**（NAT/端口可达性），不在 socket 建立。

### 4.4 WiFi 凭据放在哪、重新刷机要不要重配

- 凭据在 **NVS 分区（`0x9000`，长 `0x4000`）**，命名空间 `wifi`（见 `WifiManager` / `SsidManager`）。
- `flash-b220.ps1`（不带 `-FullErase`）**只写 5 个区域**：`0x1000` / `0x8000` / `0xd000` /
  `0x20000`（app）/ `0x800000`（assets）——**都不碰 NVS**。
  所以"改代码 → 重新刷 → 不用重配 WiFi"是成立的，这是日常迭代的主要省事点。
- 只有 `flash-b220.ps1 -FullErase` 或 `esptool erase_flash` 会清掉凭据。

**没有凭据时的配网流程（config AP）**——**[实测]** [production-codec.log:132-160](logs/production-codec.log)：

```
W (6204) SsidManager: NVS namespace wifi doesn't exist
I (7714) StateMachine: State: starting -> wifi_configuring
I (7714) WifiManager: Starting config AP
I (7784) wifi:mode : sta (b4:e6:2d:fc:4c:e5) + softAP (b4:e6:2d:fc:4c:e6)
I (7794) WifiConfigurationAp: Access Point started with SSID Xiaozhi-4CE6
I (7804) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 192.168.4.1
I (7814) WifiConfigurationAp: Web server started
W (7844) Application: Alert [gear] … Xiaozhi-4CE6 … http://192.168.4.1
        ↓ 手机连上之后
I (34244) WifiConfigurationAp: Station <手机MAC> joined, AID=1
I (36354) esp_netif_lwip: DHCP server assigned IP to a client, IP is: 192.168.4.2
I (40374) WifiConfigurationAp: SSID: dlink, RSSI: -78, Authmode: 3
I (56344) WifiConfigurationAp: Connecting to WiFi dlink (attempt 1/2)
```

| 项 | 值 |
| --- | --- |
| AP SSID | `Xiaozhi-4CE6`（`Xiaozhi-` + MAC 后 4 位；本机 MAC 尾 `4c:e5` → softAP 尾 `4c:e6`） |
| 配网页面 | `http://192.168.4.1` |
| DHCP 给手机 | `192.168.4.2` |
| 重试 | `Connecting to WiFi <ssid> (attempt 1/2)`，失败后 3 s 再试一次 |

> ⚠️ 本板**没有按键**（`InitializeButtons()` 是空实现），所以"进配网模式"只有自动一条路：
> 无凭据时自己进。想强制重配，清 NVS：`esptool erase_region 0x9000 0x4000`。

---

## 5. ⭐ 唤醒词（本项目最后一块拼图）

### 5.1 逆向结论：原厂的关键词是 **ESP32 软件识别**，而且**没有任何外部触发线**

来源：[WAKE-WORD-PATH-REVERSE.md](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063/WAKE-WORD-PATH-REVERSE.md)（对 `backup/parts/wifi.bin` + `backup/disasm/b220_full.elf` 的反汇编）。

```
3 个麦克风 → ZL38063（AEC/NR/波束成形）→ I2S 16 kHz 单声道（ESP32 是 master）
  → ESP32: rec_engine_task (0x400fac04) 读定长块 → VAD（WebRTC vad_filterbank.c/vad_sp.c）
  → 本地关键词识别（Turing/esp32-sr DNN + MFCC，"### spot keyword ###" @0x3f4122e1）
  → 命中（0x400fb500 处 bnei a10,1）: flag 0x3ffc5830<-1, state 0x3ffc5838<-2,
     启动 tm_wakeup 0x3ffc3850（0xb40 tick = 4000 ms），经 0x3ffc3864 推 wake 事件
```

| 结论 | 内容 | 置信度 |
| --- | --- | --- |
| 检测位置 | **ESP32 内部软件**，不是 DSP、不是 AI 模块、不是底座 | **[逆向] H** |
| 模型 | `wakeNet5_v3_modoujingling_6_0.90_0.85_kuaisuqianjin_6_0.90_0.85_tiaogewu_6_0.90_0.85` @DROM `0x3f413134` | **[逆向] H** |
| 关键词名 | `modoujingling` / `kuaisuqianjin` / `tiaogewu` | **[逆向] H** |
| 引擎输入 | 配置结构 @DRAM `0x3ffbeeb0`：`+0x34` 之后是 1, **`0x3e80` = 16000 Hz**, 30, 1, 0, 1 → **16 kHz 单声道** | **[逆向] H** |
| 阈值 | 0.90 / 0.85，窗口 6 帧，3 个关键词槽 | **[逆向] H** |
| 触发传输 | **只有内部状态机事件**。DSP 侧唯一 literals-verified 的 HBI 流量是 boot（3699 条固件记录 + 1042 条配置）+ reg 0x06 邮箱轮询 → **没有运行期 arm/trigger 命令**，也没有能携带触发的 HBI 读 | **[逆向] H**（完整性 M） |
| GPIO / HBI / UART 触发 | **不存在**。`gpio_set_intr_type(38, INTR_DISABLE)` @`0x40124ce8` 只是 disable，而且 **ESP32-D0WD 没有 pin 38** | **[逆向] H** |

> 📌 **这一条决定了整个方案**：既然原厂就是"ESP32 上跑软件唤醒词"，
> 我们就不需要任何 DSP 触发协议、不需要底座握手、不需要 GPIO 中断——
> 只需要一条 **16 kHz 单声道**的麦克风数据流和一个能在 ESP32 上跑的模型。

### 5.2 模型文件长什么样（**[逆向]**）

| 项 | 值 |
| --- | --- |
| 文件 | `…\zl38063\wakenet\wakenet5_modoujingling.bin` |
| 大小 | **84 160 字节**（`0x148c0`） |
| sha256 | `fb786b2ae14479141f6124b91dfdc93cba8a9e9bca889332d52a441bdc4aa910` |
| DROM 范围 | `0x3f4131b0 .. 0x3f427a70`（app 镜像内文件偏移 `0x131b0..0x27a70`） |
| 结尾 | 正好等于 `model_info_t` @`0x3f427a70` |
| 头 | 24 字节 `model_coeff_getter_t`：`getter_q = 0x4010070c`（IROM，名字→矩阵查表），`getter_info = 0x40100704` |
| 载荷 | 头之后是 **20 个 `dl_matrix2dq_t` 记录**（10 个 `kernel_i` + 10 个 `bias_i`），`{int w; int h; int stride; int flags; int exponent; qtp_t *itemq;}`，int16 项紧随其后，首尾相接 |

范围不是猜的：镜像是自描述的记录链，链**正好终止在 `model_info_t` 起始处**；
独立交叉验证是原厂 `get_coeff_q`（IROM `0x4010070c`）里的名字→矩阵表恰好引用这 20 个地址、不多不少。
完整表在 `wakenet5_modoujingling_layout.txt`，工具是
[wakenet5_tool.py](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063/wakenet/wakenet5_tool.py)：

```powershell
python wakenet5_tool.py info                                # 打印头 + 20 条记录 + sha256
python wakenet5_tool.py emit <outdir> modoujingling_wn5     # 输出可重定位 C
```

### 5.3 为什么不用原模型：esp-sr 2.4.7 **没有 wn5 引擎**

仓库 `main/idf_component.yml` 钉的是 `espressif/esp-sr: ==2.4.7`。逐版本核对结果
（[wakenet/README.md](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063/wakenet/README.md) §2）：

| esp-sr 版本 | ESP32 classic 上的 wn5 引擎 | 证据 |
| --- | --- | --- |
| v1.0 | ✅ 有 | `lib/esp32/libwakenet.a` 含 `wakenet5_quantized_mono.c.obj`，日志串 `Quantized wakeNet5: %s, mode:%d (%s %s)` |
| v1.9.5 | ✅ 仍有 | 同目标 `.obj` 定义 `esp_sr_wakenet5_quantized` / `model_create` / `model_detect`，UND 里有 `srmodel_get_model_coeff` |
| **2.4.7（本仓库）** | ❌ **任何目标都没有** | `lib/esp32/libwakenet.a` 里只有 `wakenet9_quantized/l/s.c.obj` + `esp_wn_models.c.obj`；字符串 `Quantized wakeNet5` / `wakenet5_create` 在整个 `lib/esp32/` 下**不存在**。`src/model_path.c` 不再定义 `srmodel_get_model_coeff`，但 `src/include/model_path.h:166-174` **还留着它的声明**，注释写着 *"@warning Just support ESP32 to load old wakenet"* |

同时 **2.4.7 的 Kconfig 里没有任何 wn5 选项**，而 `sdkconfig.defaults.esp32:3` 已经写死：

```
CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y
```

**即：2.4.7 在经典 ESP32 上唯一还能跑的 wakenet 家族就是 WakeNet9。**

### 5.4 采用的方案：用 esp-sr 现成的 wn9 中文模型（**你好小智**）

**决策**：唤醒短语从"魔豆精灵"改成 **"你好小智"**（模型 `wn9_nihaoxiaozhi_tts`，284 KB）。
理由：**零逆向、零训练、当天可验证**；而"用原关键词"要走 §5.6 那条数小时到数天的路。

#### 5.4.1 构建命令（以及为什么 `idf.py build` 是错的）

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd <repo>
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi
```

> ⚠️ **为什么不能直接 `idf.py build`：`sdkconfig` 是"粘"的。**
> 工作目录里的 `sdkconfig` 会保留上一次配置；如果它带着 `CONFIG_WAKE_WORD_DISABLED=y`，
> 那么直接 `idf.py build` 会**沿用**这个值，唤醒词代码整段编译不进去，
> 而你会在日志里找不到任何 `EspWakeWord` 行，却怎么改 `config.json` 都没用。
> `scripts/build.py` 是仓库的规范入口，它负责把 board `config.json` + variant 的
> `sdkconfig_append` + `--wake-word` 展开的值一起重新生成 `sdkconfig`——
> **构建一定要走它**。

`--wake-word nihaoxiaozhi` 是仓库的官方开关（`docs/custom-board.md:376-388`）。
对 `esp32` 目标它展开成（`scripts/build.py:338-385`）：

```
CONFIG_USE_ESP_WAKE_WORD=y
CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y
CONFIG_WAKE_WORD_DISABLED=n
```

> 📌 **仓库规则，别踩**：`CONFIG_SR_WN_*`（选模型）**不能**写进板级 `config.json` ——
> `scripts/tests/test_build.py:172-184` 专门断言这一点，写了会让 host 测试挂掉。
> 模型选择只有两个合法来源：`sdkconfig.defaults.esp32:3`（已存在）或命令行 `--wake-word`。
>
> 补充：`esp32` **不在** `_LITE_WAKE_WORD_TARGETS` 里，所以选到的是 `wn9_*` **大模型**
> 而不是 `wn9s_*` 小模型——这也是为什么 assets 会长 320 KB 而不是更少。

#### 5.4.2 `config.json` 需要加的三行

[xiaomodou-b220/config.json](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/config.json)：

```json
{
    "type": "xiaomodou-b220",
    "target": "esp32",
    "builds": [
        {
            "name": "xiaomodou-b220",
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
        }
    ]
}
```

三行的作用与理由：

| 行 | 作用 |
| --- | --- |
| `CONFIG_USE_ESP_WAKE_WORD=y` | 启用**无 AFE** 的唤醒词路径（`LiteAudioEngine` + `EspWakeWord`）。`main/Kconfig.projbuild:908-912` 的依赖是 `IDF_TARGET_ESP32 && SPIRAM`——**本板正好满足**（`CONFIG_SPIRAM=y` + `target: esp32`） |
| `CONFIG_USE_AFE_WAKE_WORD=n` | **经典 ESP32 没有 AFE**（AFE 走 S3/P4 路径，见 `audio_service.cc:82-86` 的 `#if CONFIG_IDF_TARGET_ESP32S3 …`）。开着只会浪费内存并可能让引擎初始化失败 |
| `CONFIG_WAKE_WORD_DISABLED=n` | 明确关掉"禁用唤醒词" |

放在板级 `config.json` 里的额外好处：**即使漏掉 `--wake-word`，唤醒词仍然有**
（模型由 `sdkconfig.defaults.esp32` 提供，两处覆盖值一致、不冲突）。

#### 5.4.3 不需要 `model` 分区：模型进的是 `generated_assets.bin`

| 项 | 值 |
| --- | --- |
| 模型 | `managed_components/espressif__esp-sr/model/wakenet_model/wn9_nihaoxiaozhi_tts`（284 KB） |
| 打包 | `scripts/build_default_assets.py` 按 esp-sr `pack_model.py` 的同一格式打成 `srmodels.bin`，作为 assets 里的一个文件（`index.json` 的 `srmodels` 项） |
| 产物 | `build\generated_assets.bin` |
| 刷写位置 | **assets 分区 `0x800000`**（8 MB） |
| 大小变化 | **[实测]** assets 映射 **1280 KB → 1600 KB**；`generated_assets.bin` 约 **1,578,891 字节** |
| 分区表 | **不变**（仍是 `partitions/v2/16m.csv`，没有 `model` 行） |

运行时链路：

```
Assets::LoadSrmodelsFromIndex()           main/assets.cc:110-131
    ↓ 把 mmap 到的那块数据交给
srmodel_load(...)                          esp-sr
    ↓ 经
AudioService::SetModelsList()              main/audio/audio_service.cc:855-861
    ↓
LiteAudioEngine::Initialize(codec, frame_ms, models_list)
    ↓
EspWakeWord::Initialize(codec, models_list)  → esp_srmodel_filter(…, ESP_WN_PREFIX, NULL)
                                             → esp_wn_handle_from_name(model_name)
                                             → create(model_name, DET_MODE_95)
```

> 📌 **为什么不用独立 `model` 分区**：esp-sr 自己支持它，但要求
> `CONFIG_PARTITION_TABLE_CUSTOM` **且**分区表里真的有 `model` 行
> （`managed_components/espressif__esp-sr/CMakeLists.txt:102-126`）。
> 本仓库 v2 布局的惯例（`espressif/esp-hi`、`m5stack/stick-s3`）是走 assets，
> 于是**分区表不用动、刷机偏移一个都没变**——迁移成本最低。
>
> `flash-b220.ps1` 也做了对应处理：assets 里没有 `srmodels` 时黄字警告"语音唤醒不会工作"
> 并给出重建命令；**如果**将来分区表真出现 `model` 行，它会自动把
> `build\srmodels\srmodels.bin` 按分区表偏移一起写。

#### 5.4.4 构建期与运行期的验收日志（**[实测]**）

构建日志（证明模型真的被打进去了）：

```
  wakenet models: wn9_nihaoxiaozhi_tts (will be packaged)
Generated: .../srmodels/srmodels.bin
```

不刷机也能自检 assets 里有没有模型：

```powershell
$B = '<repo>\build'
$txt = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$B\generated_assets.bin"))
"assets 含 srmodels: " + $txt.Contains('srmodels')      # 必须 True
(Get-Item "$B\generated_assets.bin").Length             # 约 157 万（带模型）/ 约 129 万（无模型）
```

开机日志（COM25 @115200，`capture-serial.ps1`）——**逐字摘自 [wakeword.log](logs/wakeword.log)**：

```
I (8364) Assets: The assets map size is 1600 KB (partition 8192 KB)   ← 无模型时是 1280 KB
I (8594) MODEL_LOADER: Successfully load srmodels                     ← 关键：模型装上了
I (10524) wifi:Set ps type: 2, coexist: 0
MC Quantized wakenet9: wakenet9l_tts1h8_…_3_0.631_0.635, tigger:v4, mode:1, p:0, (Apr 17 2026 17:25:50)
I (10634) EspWakeWord: Wake word(wn9_nihaoxiaozhi_tts),freq: 16000, chunksize: 512
I (10634) LiteAudioEngine: Initialized, WakeNet: yes                  ← 关键：引擎起来了
I (10644) AudioCodec: Set input enable to true
W (10664) XiaoMoDouB220: pa: enable sequence already ran this boot …  ← PA 锁存在工作
I (10704) AudioService: Resampling audio from 16000 to 48000
```

对着麦克风说 **"你好小智"**，应出现：

```
I (13814) Application: Wake word detected: 你好小智 (state: 3)
I (13814) StateMachine: State: idle -> connecting
I (13924) MQTT: Session ID: 4c74af7f
I (13944) StateMachine: State: connecting -> listening
```

四个验收点的含义：

| 行 | 说明 |
| --- | --- |
| `MODEL_LOADER: Successfully load srmodels` | `srmodels.bin` 从 assets 加载成功 |
| `LiteAudioEngine: Initialized, WakeNet: yes` | 引擎初始化成功、唤醒词开启 |
| `EspWakeWord: Wake word(wn9_nihaoxiaozhi_tts),freq: 16000, chunksize: 512` | **[实测]** 引擎自报 16 kHz / 512 样本（32 ms）一块 |
| `Application: Wake word detected: …` | 真的识别出来了 |

> 💡 注意 **[实测]** 的 `chunksize: 512` 与 [FLASH-MANUAL.md](FLASH-MANUAL.md) §4.7 里写的
> `chunksize: 320` 不同——那是早期文档的笔误/预估值。**以日志为准**（512 样本 @16 kHz = 32 ms）。
> 而 `EspWakeWord::Feed` 无论 chunksize 是多少都能工作（它内部按
> `get_samp_chunksize()` 切块，且多通道时只取左声道，见 `esp_wake_word.cc:86-92`）。
>
> 💡 `MC Quantized wakenet9: …_0.631_0.635, tigger:v4` 这行是 **esp-sr 自己的模型自述**
> （注意它把 trigger 拼成了 `tigger`，是上游拼写）。两个 0.631/0.635 是模型的内部参数，
> **不是**我们能调的检测阈值：阈值是 `esp_wake_word.cc:47` 写死的 `DET_MODE_95`。

#### 5.4.5 装上了但唤不醒 / 唤醒了但对话不通

| 日志现象 | 含义 / 处理 |
| --- | --- |
| `The assets map size is 1280 KB` | 刷进去的 assets 还是旧的（不带模型）→ 重建 `generated_assets.bin` 再刷 |
| `MODEL_LOADER: Can not find model in partition table` | 走到了"独立 model 分区"后备路径（本板没有该分区）。正常流程不该出现 |
| `Failed to initialize wakenet model` / `No model found` / `No WakeNet model found` | assets 里没有 `srmodels` 或模型名不对 → 先做 §5.4.4 的 assets 自检 |
| 日志里根本没有 `Wake word(wn9_…)` | `CONFIG_USE_ESP_WAKE_WORD` 没生效 → 查 `build\config\sdkconfig.json` 里 `USE_ESP_WAKE_WORD=True`、`WAKE_WORD_DISABLED=False`。**99% 是漏了 `--wake-word` 而 `sdkconfig` 是旧的**（§5.4.1） |
| `LiteAudioEngine: Initialized, WakeNet: no` | 引擎起来了但没加载唤醒词——典型是 assets 无模型（[auto-listen.log:178](logs/auto-listen.log) 就是这一版：`assets map size is 1280 KB` + `WakeNet: no`） |
| 装上了但一直唤不醒 | 先查 mic 通路与底噪/削顶（§1.5 的 `peak`/`mean` 日志）；`DET_MODE_95` 偏严，说话要清楚、距离要近（**实测** 1 m 内正常音量即可） |
| 唤醒了但服务器侧静默 | §4.3（UDP 音频通道） |

#### 5.4.6 16 kHz 是自动的，板级无需改动

本板 codec 输入 48 kHz，`AudioService::Initialize()` 在 `input_sample_rate() != 16000` 时
建 `esp_ae_rate_cvt`（`audio_service.cc:73-80`），`ReadAudioData(data, 16000, samples)`
把 48 kHz 降到 16 kHz（`:202-219`），再按 10 ms 一帧喂引擎（`:310-316`）。
**[实测]** `I (10704) AudioService: Resampling audio from 16000 to 48000` 与后续的唤醒成功，
证明这条链路端到端可用。

> ⚠️ 千万不要为了"省掉一次重采样"把 `AUDIO_INPUT_SAMPLE_RATE` 改成 16000——见 §2.2。

### 5.5 auto-listen（已弃用）与唤醒词的关系

`B220_AUTO_LISTEN` 是"这台设备没有按键、又没有唤醒词"那个阶段的临时拐杖
（[config.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/config.h) 的 Phase 5 块，
`xiaomodou_b220.cc:2762` 起）。它的做法是：监督任务轮询 `Application::GetDeviceState()`，
**每次进入 `kDeviceStateIdle` 就 800 ms 后调 `Application::StartListening()`**——
用的还是按键/唤醒词同一个事件位 API，不碰 core。

2026-10-09 **唤醒词验证成功之后，它被置 0**：

```c
/* Why it is 0 now (2026-10-09): voice wake-up does work on this chip. … 
 * The phrase "ni hao xiao zhi" is what starts a conversation now.
 * Auto-listen would be a second, competing way to open the microphone, hence this is 0. */
#define B220_AUTO_LISTEN 0
```

**[实测]** `I (5974) XiaoMoDouB220: auto-listen: DISABLED (B220_AUTO_LISTEN=0 in config.h): the esp-sr WakeNet9 wake word (CONFIG_USE_ESP_WAKE_WORD) starts conversations now`

置 0 时整个监督任务、状态与日志**完全编译掉**（`B220_AutoListenStart()` 退化成一个只打日志的 stub），
没有运行期开销。**这是"有了唤醒词之后必须做的一步清理"**：两个开麦来源会互相打架。

### 5.6 可选升级：换回原关键词「魔豆精灵」（**未实现**）

**[逆向]** 模型已经提取并重生成为可重定位 C：

| 项 | 值 |
| --- | --- |
| 二进制 | `…\zl38063\wakenet\wakenet5_modoujingling.bin`，84 160 B，sha256 `fb786b2a…` |
| 布局 | 24 字节 `model_coeff_getter_t` + **20 个 `dl_matrix2dq_t` 记录** + `model_info_t` |
| 生成的 C | [modoujingling_wn5.c](ref/analysis/wakenet/gen/modoujingling_wn5.c)（262 436 B，其中 rodata 约 84 KB）+ `modoujingling_wn5.h` |
| 生成方式 | `python wakenet5_tool.py emit <dir> modoujingling_wn5` —— 把原厂绝对指针换成链接符号，并把 `get_coeff_q` 换成 `strcmp` 链 |

它需要的引擎是 **esp-sr v1.x 的 wn5**，而 v1.x 的加载机制是**编译期描述符、不是运行期文件**
（引自 v1.9.5 源码）：

```c
/* include/esp32/esp_wn_models.h:90-96 */
#include "customized_word_wn5.h"
#define WAKENET_MODEL_NAME "wn5_customizedword"
#define WAKENET_COEFF get_coeff_customizedword_wn5

/* src/model_path.c:313-317 */
model_coeff_getter_t *srmodel_get_model_coeff(char *model_name)
{ model_coeff_getter_t *gettercb = (model_coeff_getter_t *)&WAKENET_COEFF; return gettercb; }
```

也就是说：**wn5 引擎根本不看运行时给的名字，它要一个编译期宏指定的描述符**。
定义自己的 `get_coeff_modoujingling_wn5` 并让 `WAKENET_COEFF` 指向它是**受支持的用法**，不是 hack。
（wn5 **没有**外部 blob/分区加载器——`srmodel_load`/`movemodel.py` 只覆盖 wn7/wn8/wn9。）

落地步骤（按可行性排序）：

1. 从 `v1.9.5 lib/esp32/libwakenet.a` 取 wn5 引擎（`ref\analysis\wakenet\v1.9.5_libwakenet_esp32.a` 已下载备用）；
2. 补上 2.4.7 已删除的 `srmodel_get_model_coeff`，或**加性**引入 v1.9.5 的 `model_path.c`；
3. 放入生成的 `modoujingling_wn5.c`，设好 `WAKENET_COEFF` / `WAKENET_MODEL_NAME`；
4. `WAKENET_MODEL_NAME` 用原厂 info 串，这样 `get_word_name` 仍能报出 `modoujingling`。

**工作量与风险（明确评估）：**

| 项 | 评估 |
| --- | --- |
| 工作量 | **数小时到数天** |
| 风险 | **中** |
| 具体风险 1 | 与 2.4.7 的 wn9 引擎存在 `esp_wn_handle_from_name` / `model_create` **符号重复**，要在链接层解决 |
| 具体风险 2 | v1.9.5 的预编译归档是 GCC-13 / IDF-5.x 时代产物，**必须**对 IDF 6.1 做链接测试 |
| 具体风险 3 | 没有运行期 blob 加载，模型常驻 ~84 KB DROM/flash |
| 兜底 | 若混两代 esp-sr 太乱，就把整个组件钉到 **v1.9.5**，用它的 API |
| 未验证 | `esp_sr_wakenet5_quantized` + 我们的描述符是否真的给出正确识别——**这需要构建 + 麦克风实测**，不是看代码能定的 |

> ❗ **状态：未实现（NOT implemented）。** 本文档记录的是**已完成的分析与准备**：
> 模型已提取、已重生成 C、目标归档已取得、机制已从上游源码确认。
> 实际设备上跑的是 §5.4 的 `wn9_nihaoxiaozhi_tts`（"你好小智"）。
> 另外仍未确定的一点（[逆向] L）：原厂 6 槽 `model_coeff_getter_t` 的字段名与任何已发布头文件都不完全一致
> （v1.0 的是 9 槽、`getter_info` 在 +0x18；我们的是更早的 6 槽、槽 4 是 info getter），
> 所以**任何重发的描述符都必须按目标版本的头文件布局来**。

---

## 6. 不要去做的方案（以及原因）

这一节是"省下几天时间"的清单。每条都写清**为什么不做**，避免后来者重走。

| 方案 | 状态 | 为什么不做 |
| --- | --- | --- |
| **auto-listen / 常听 watch** | **试过、不喜欢、已关**（`B220_AUTO_LISTEN 0`） | 它在**每一轮**对话结束后自动重新开麦（`arm #N`，见 [auto-listen.log](logs/auto-listen.log)），设备变成"永远竖着耳朵"；隐私与体验都差，而且会和唤醒词抢麦。功能够用后被明确退役 |
| **移植原厂 IROM 里的 wn5 引擎** | **不做** | 它是**位置相关**的机器码，对 `dl_lib`、cJSON、FreeRTOS、newlib 的调用全部解析到**原厂地址**。搬过来要重定位成千上万个 literal、逐个重新绑定 ROM/newlib 符号（例如 `strcmp` 在 `0x4009e394`）。工作量 >> 收益，而且 esp-sr v1.x 已经**以可链接归档的形式**提供了同一个引擎（§5.6） |
| **自己训练一个新的 wn5 模型** | **拿不到** | Espressif 的唤醒词定制现在只产出 **WakeNet9 / wn9s / wn9l**（和 MultiNet）；当前 esp-sr 里唯一的 "custom" 产物是 `wn9_customword`。2.4.7 的 Kconfig 里**连 wn5 选项都没有**。而且我们**不需要**：原厂固件里已经带着这个关键词训练好的 wn5 模型 |
| **在 ESP32-S3 专属路径上做软件唤醒词（AFE）** | **本芯片不可用** | AFE 引擎只在 S3/P4/S3.1 上构造（`audio_service.cc:82-86` 的 `#if CONFIG_IDF_TARGET_ESP32S3 \|\| …`）。经典 ESP32 走的是 `LiteAudioEngine`。开 `CONFIG_USE_AFE_WAKE_WORD=y` 只会失败或白占内存 |
| **给唤醒词加 HBI 轮询 / GPIO 中断 / 底座 UART 触发** | **不存在这条路** | **[逆向] H**：原厂的检测全在 ESP32 内部软件里，"没有 GPIO 沿、没有 HBI 轮询、没有 UART 消息参与唤醒"。自己造一条只会引入新的失败面 |
| **为了"省一次重采样"把声明改成 16 kHz** | **绝不要** | §2.2：端口实际是 48 kHz，声明 16 kHz 会去掉重采样器 → 采集/播放 3 倍速 |
| **靠关功放来省电 / 做静音** | **做不到** | AW8737S 的模式由**上升沿个数**决定，锁存后不能再动；关功放 = 一次新的 0→1 = 换模式（第 2 次 mode 2，第 4 次直接没输出）。静音只能靠"不喂音频" |
| **在 `SetOutputVolume()` 里做软件增益** | **不做** | 要给每个播放样本做乘法，而已经验证的通路不那么做；DSP 侧 `reg 0x238/0x23A` 才是真的音量，由 boot 配置记录负责 |
| **`idf.py build` 直接编译**（不走 `scripts/build.py`） | **不要** | §5.4.1：陈旧 `sdkconfig` 会把 `WAKE_WORD_DISABLED` 粘住，症状是"改了配置却没反应" |
| **`CONFIG_SR_WN_*` 写进板级 `config.json`** | **不允许** | `scripts/tests/test_build.py:172-184` 专门断言，写了 host 测试挂 |

---

## 7. 合并排错表（symptom → 根因 → 精确处置）

> 用法：先按"现象"定位行，再看"先看哪行日志"——**不要凭感觉改配置**。
> 每行都给了文件路径或确切日志串，便于直接搜索。

| # | 现象 | 最可能根因 | 精确处置 / 要看的那行日志 |
| --- | --- | --- | --- |
| 1 | 串口**一个字节都没有** | ① 没共地 ② TX/RX 接反 ③ TTL 不是 3.3 V 档 ④ 设备没开机（电池） | `capture-serial.ps1`；万用表确认 TTL GND ↔ 模组 GND 通断、TTL TX 通到 GPIO3（模组第 34 脚）。运行固件时电脑只靠 TTL 供电**不够**，要靠玩具电池 |
| 2 | `No serial data received` / `Invalid head of packet (0x2B/0xFE)` | 没进下载模式（`0xFE` 那类其实是**读到应用日志**，说明芯片在跑固件） | 进下载模式：① 按启动键**关机** ② **插 micro-USB** ③ 按启动键**开机** → 横幅须为 `boot:0x27 (DOWNLOAD_BOOT…)` + `waiting for download`。退出：**拔 micro-USB** 再开关机 → `boot:0x3f (SPI_FAST_FLASH_BOOT)` |
| 3 | `PermissionError` / 端口打不开 / 拒绝访问 | 别的串口工具占着 COM25（串口助手、`idf.py monitor`、上一个没退的 esptool） | 关掉所有占用者；`Get-Process` 查残留 python。**不要**改用 `--before default_reset`（本机 TTL 没接 DTR/RTS，一律 `--before no_reset`） |
| 4 | `Corrupt data, expected 0x1000 bytes but received 0xffd bytes` | **串口链路误码**（读到一半丢字节），不是芯片坏 | **降波特率重试**：`--baud 460800` 或 `115200`；缩短飞线；确认 TTL 是 3.3 V 档。稳定后 921600 下有效约 490 kbit/s |
| 5 | 部分刷写之后**反复重启 / boot loop** | 混用了两套**不兼容**的分区布局：原厂 app 在 `0x10000`（3.75 MB，`wifi`/`wifi_1`），小智 app 在 `0x20000`（3.9 MB，`ota_0`/`ota_1`）；原厂音频区 `flashTone` 在 **`0x42d000`**（8 MB），小智 `assets` 在 **`0x800000`** | **整片擦除 + 整片写回**是最省心的：切到小智用 `.\flash-b220.ps1 -FullErase`；回原厂用 `write_flash 0x0 .\backup\esp32-16MB-full.bin`。原始布局见 [FLASH-MANUAL.md](FLASH-MANUAL.md) §6 |
| 6 | 屏幕**全黑**（但能连服务器） | 背光不在 GPIO 上，在 **AW9106B（I2C `0x5B`）** 的 DIM0/DIM1/DIM2 | 查日志 `XiaoMoDouB220: AW9106B backlight on (DIM0/1/2 = 0x3F)`；同时确认 I2C 在 SDA 18 / SCL 23、地址 `0x5B` |
| 7 | 屏幕**颜色反了**（红蓝互换） | 裸 `esp_lcd_panel_draw_bitmap()` 需要**手动字节交换**（panel 要高字节在前）；LVGL 那条路靠 `SpiLcdDisplay` 的 `swap_bytes=1` 已处理 | 自绘缓冲要 `__builtin_bswap16()`。**[实测]** 方向自检卡就踩过这个坑（`config.h` 的 `B220_ORIENT_CARD` 注释里记着） |
| 8 | 屏幕**分辨率/几何不对** | 初始化表的窗口让人误判成 ILI9341 **240×320**；实测物理是 **128×160** | `config.h`：`DISPLAY_WIDTH 128` / `DISPLAY_HEIGHT 160`，`DISPLAY_MIRROR_X/Y false`、`DISPLAY_SWAP_XY false`、`DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB`（原厂自身发 `0x21 INVON`、`MADCTL(0x36)=0x00`，驱动**不得**再反相/重映射） |
| 9 | ZL38063 所有寄存器读回 **`0x0000`**（HBI 全零） | ① DSP 完全没被驱动（MISO 恒低） ② BCLK/WS 没起（DSP 是 I2S slave，没时钟就不应答） ③ 片选/引脚不通 | 先确认 I2S 已起：`I (1214) XiaoMoDouB220: I2S up before DSP boot: 48 kHz, 16-bit, stereo …`；再按 [PHASE3-DSP-DEBUG-STATUS.md](PHASE3-DSP-DEBUG-STATUS.md) §4 量 GPIO19（应"约 1.2 s 处低 10 ms → 随后保持高"）、GPIO0（空闲应高、仅通信瞬间拉低）、GPIO34(MISO)（通信时应有跳变）。字节序不是问题：`0xD3D3` 是回文，交换写反也会读出它 |
| 10 | `reg 0x34` 读不到 **`0xD3D3`**（host-boot 中止） | ① I2S 时钟未先于 DSP 复位建立 ② 复位脉冲没真正释放 ③ DSP 没通电 | 关键顺序：**I2S 先起 → GPIO19 复位脉冲 → host-boot**（`B220_I2S_BEFORE_DSP 1`）。**[实测]** 加了"I2S before DSP"之后 host-boot 才从"读不到 `0xD3D3`"变成"上传成功" |
| 11 | host-boot 上传成功但**应用一直不报 running**（`reg 0x0030 & 0x0F == 0`） | **本产品的应用从不更新 `reg 0x0030`**，这个寄存器在这里没有意义 | **改用 `reg 0x0014 == 0x0200`**：`I (5834) XiaoMoDouB220: codec: reg 0x0014 = 0x0200 -> the ZL38063 application is READY`。这也是原厂自己的音量/路由 routine 的 gate 条件。若 `0x0014` 也读不到 → 回到 #9/#10 |
| 12 | DAC1_P/DAC1_M 上有波形，**喇叭没声音** | AW8737S 被切成 **mode 4**（NCN OFF）或 **`GPMD_A != 0x02`** 导致 group-A 根本不驱动 SHDN | 查三行：`pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02 \| 0x02 writes=N, SHDN rising edges=1 (AW8737S mode 1)`；出现 `!!! AW8737S SHDN RISING EDGE #2 …` 就是有第二个写者在写 `0x02`（§3）。也要确认背光初始化没有在 PA 之后写 `0x12 <- 0x08` |
| 13 | 唤醒词**一直不触发** | ① assets 里没模型 ② `CONFIG_USE_ESP_WAKE_WORD` 没生效 ③ 漏了 `--wake-word` 而 `sdkconfig` 陈旧 ④ mic 通路问题/阈值偏严 | 依次看：`Assets: The assets map size is 1600 KB`（1280 = 旧 assets）→ `MODEL_LOADER: Successfully load srmodels` → `LiteAudioEngine: Initialized, WakeNet: yes` → `EspWakeWord: Wake word(wn9_nihaoxiaozhi_tts),freq: 16000, chunksize: 512`；再确认 mic 有信号（回环日志的 `peak`/`mean`）；`DET_MODE_95` 偏严，靠近点说清楚 |
| 14 | `WakeNet: no` / `Failed to initialize wakenet model` / `No WakeNet model found` | assets 无 `srmodels`，或模型名不对 | 先做 §5.4.4 的 assets 自检（`$txt.Contains('srmodels')` 必须 `True`），再重建 `generated_assets.bin` 并刷 `0x800000` |
| 15 | 服务器 **65 s 静默后发 `goodbye`** | **UDP 音频通道一个字节都没送到服务器**（NAT / 端口可达性），控制通道本身是好的 | 判据：`Session ID` 有，但 68 s 内**没有** `Application: >>` 也**没有** `Resampling audio from 24000 to 48000`（[auto-listen.log](logs/auto-listen.log) 的 arm #1 段）。处置：换 WebSocket 传输（OTA 响应里带 `websocket` 段即可，固件不改）→ 或排查 NAT/CGNAT → 或对照实验。注意 `mqtt_protocol.cc:371` 的 `Failed to connect UDP audio channel` **没有**出现，说明 socket 建起来了、是包没到 |
| 16 | `MODEL_LOADER` 失败 | ① assets 分区没刷/刷旧 ② 偏移写错 ③ 分区表与构建不一致 | 确认写机清单：`0x1000` bootloader、`0x8000` partition-table、`0xd000` ota_data_initial、`0x20000` xiaozhi.bin、`0x800000` generated_assets.bin；`partition-table.bin` 与 `partitions/v2/16m.csv` 必须同源 |
| 17 | 开机第一行 `flash read err, 1000` 然后 `rst:0x10 (RTCWDT_RTC_RESET)` | **本机已知的 Flash 冷启动裕量问题**（原厂固件同样现象），靠看门狗复位自愈 | **不是刷坏了，不用处理。** 后续 `boot:0x3f (SPI_FAST_FLASH_BOOT)` 正常启动即可（每份日志开头都有这两行） |
| 18 | `SetOutputVolume` 调了但音量没变 | **设计如此**：音量在 DSP 内部（`reg 0x238/0x23A`），由 boot 配置记录写死；codec 只存 NVS | 日志里会有 `SetOutputVolume(N): stored, but NOT applied by this codec …`。这是**预期行为**，不是 bug |
| 19 | `Server sample rate 24000 does not match device output sample rate 48000` | **正常**：服务器 24 k、设备 48 k，中间有重采样 | **不要消除它**（消除的唯一办法是把设备声明成 24 kHz，那会违反 §2.1）。确认后续有 `Resampling audio from 24000 to 48000` 且对话可听即可 |
| 20 | `write_mem` 后 esptool 报 `Packet content transfer stopped` | 芯片在命令执行中途复位了（如 `write_mem 0x3FF48000 0x80000000` 软复位） | **预期行为**。注意：复位瞬间若 GPIO0 仍为低（micro-USB 还插着），芯片会**再次落回下载模式** |
| 21 | 跨机恢复后两台设备**用同一个 MAC**、互相干扰 | 把甲机整片备份写到乙机时，NVS 里的 `nvs.net80211:sta.mac` 一起搬过去了 | 写完后清 NVS：`esptool erase_region 0x9000 0x4000`（eFuse 里的真实 MAC 无法被 flash 写入改变，清完即恢复），代价是**要重新配网** |
| 22 | 整片重新读出来的 MD5 与旧备份**不同** | 设备只要运行过，**NVS 分区（`0x9000–0xd000`）就会变**（WiFi 状态、日志游标） | 不是接线问题。要确认零误码就**按 64 KB 分块比较**，只应看到 NVS 所在的那一块有差异 |

---

## 8. 经验方法论：真正让这个项目推进的调试纪律

这一节的每一条都是本项目**实际起过作用**的做法，不是泛泛之谈。

### 8.1 一次只改一个变量（per flash）

这个项目里同时打开过 10+ 个 `B220_*` 诊断开关（`B220_ZL_DIAG`、`B220_DSP_VERIFY_UPLOAD`、
`B220_DSP_PAGE255_CALIB`、`B220_DSP_VERIFY_BEFORE_GO`、`B220_DSP_RELEASE_MATRIX`、
`B220_DSP_ADDR_MATRIX`、`B220_DSP_HANDS_OFF`、`B220_AUDIO_LOOPBACK`、`B220_AUDIO_TEST_MODE`、
`B220_I2S_TX_VARIANT_SWEEP`、`B220_PA_BIT_FINDER`、`B220_AUTO_LISTEN`）。

它们存在的意义是**每一个都对应一个单独的假设**，而且典型做法是：

- **单行开关**：翻一个 0/1 就换一个实验，其余全部保持上一版的已知状态；
- **每次实验只测一个变量**，并把"其他开关的值"写进该开关的注释里（`config.h` 里每个块结尾都有
  "everything else is left exactly as the ADF build had it: `B220_ZL_DIAG 0`, …"）；
- **关掉时完全编译掉**："0 means the switch, the table, the record array and the call site are all
  compiled out and the build is byte-for-byte the previous one."

反例就是踩过的坑：`B220_I2S_TX_VARIANT_SWEEP` 开着的时候每 3 s 重配一次 I2S 格式，
而那一轮要回答的是"功放到底有没有出声"——**两个变量同时在动，结论就不可信**。
所以那一版把它明确置 0，并在注释里写清"this run needs one STABLE 1 kHz sine"。

### 8.2 让设备在日志里**自己诊断**

不要靠"看起来不对"。这个项目的每一条结论背后，都是设备自己报出来的数字或十六进制：

| 做法 | 实例（日志串） |
| --- | --- |
| **峰/均值**区分"静音"与"有信号" | `loopback t=+4s: … last_block=240 frames peak=12656 mean=5937` vs `peak=121 mean=43`（空闲底噪） |
| **十六进制 dump** 首块样本 | `loopback: first non-empty block after start - first 16 of 480 sample(s), hex, L/R interleaved: FFFA FFFA FFF3 FFF3 …`（一眼看出 L/R 成对重复 = DSP 双 slot 同数据） |
| **写后读回**验证 | `page255: calib enc=b preselect=0 base=hi_lo -> matched 16/16`；`verify: mismatching_words=0` |
| **仪表数值写进日志** | `pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02 \| 0x02 writes=3, SHDN rising edges=1` |
| **计数器**防漂移 | `write #3`、`SHDN bit1=1, rising edges so far=1`、`frames_in=96000 frames_out=96000 read_err=0 write_err=0 empty_reads=0` |
| **一次性的 bring-up 裁决** | `ReportBringupOnce()` 只在第一次报告句柄与 DSP 就绪状态 |
| **正弦表的理论值自检** | `tone: expected mean = peak * 2/pi = 0.63662 * peak (peak 32760 -> mean 20856)` —— 之后 `tx_mean~20855` 正好对上，同时证明了表、DMA 与计数都对 |

一个很好的反例教训：早期只看 `reg 0x0030`（"app status"）就得出结论"应用没跑起来"，
浪费了大量时间。后来发现**这个产品的应用从不更新 `0x0030`**，
真正的就绪指标是 `reg 0x0014 == 0x0200`（原厂自己的音量/路由 routine 也用它做 gate）。
**教训：要确认你读的那个寄存器在这个产品上真的有意义。**

### 8.3 优先测量（示波器 / 万用表），而不是猜

硬件层的问题在软件侧是**不可判定**的，早点拿表就早点结案：

| 问题 | 猜的代价 | 测量给出的答案 |
| --- | --- | --- |
| "DSP 是不是没在跑？" | 反复改 HBI 时序 | **量 I2S_WSA = 48 kHz**（原厂机）→ DSP 确实在跑，问题不在 DSP 有没有启动 |
| "复位脚是不是被按住？" | 反复改复位时序 | **量 GPIO19**：开关关时低、按下后拉高并保持高 → 复位网络正常释放，不是被按住 |
| "功放为什么不出声？" | 反复改 I2S 格式 | **量 SHDN 电平**：固件在跑时读 LOW → 功放确实被关；再量 DAC1_P/DAC1_M → 有 100 mV 方波说明 DSP 侧没问题，问题在 SHDN → 定位到 AW9106B `GPMD_A`/`0x02` |
| "BCLK 对不对？" | 改 `mclk_multiple`、试各种 slot 配置 | 原厂机 BCLK **1.54 MHz = 48 k × 32**，即 16-bit slot；与我们的配置一致 → ESP32 侧不是问题 |

**[实测]** 这一条直接把"我猜是软件"变成了"就是 AW9106B 的驱动模式"：
`0x12(GPMD_A)` 必须写 `0x02`，`0x08` 会让 group-A 输出完全不驱动 SHDN——
这个结论是从"用表量着一步步走、写一个值听一次"（`B220_PA_BIT_FINDER`）里得到的。

### 8.4 保留原厂固件，把它当 A/B 参照

`backup/esp32-16MB-full.bin`（A 机，MD5 `A6A27D2B…`）与
`backup/esp32-16MB-unitB-stock.bin`（B 机，MD5 `9F9FABA6…A49D`，**版本与 A 不同，不可互刷**）
是整个项目最有价值的两个文件。它们解决的问题是**"我们的固件做不到，是硬件不行还是代码不行"**：

| 悬而未决的问题 | 用原厂机得到的答案 |
| --- | --- |
| I2S 到底该跑多少采样率？ | 原厂机 **I2S_WSA = 48 kHz** → 我们的 48 kHz 是对的 |
| BCLK 与 slot 宽度？ | 原厂机 BCLK **1.54 MHz = 32 BCLK/frame** → 16-bit slot，和我们一致 |
| DAC 有没有输出？ | 原厂机播放时 **DAC1_P/DAC1_M 有 100 mV 方波** → 说明"有波形"是正常状态，我们的"没有波形"才是异常 |
| 唤醒词是软件还是硬件触发？ | 反汇编原厂 app → **[逆向] ESP32 软件 WakeNet5**，无 GPIO/HBI/UART 触发 |
| `reg 0x0030` 为什么不更新？ | 原厂也从不用它做判定，用的是 `reg 0x0014` |

操作纪律：

- **备份先做，而且读两遍比 MD5**（见 [FLASH-MANUAL.md](FLASH-MANUAL.md) §4.2）；
- **还原时各用各的备份**（两台原厂版本不同，应用分区约 20% 差异；音频素材区完全一致）；
- **跨机恢复记得清 NVS**（否则两台同 MAC，见排错表 #21）。

### 8.5 把每条结论和它的证据一起写下来

本项目所有 `*.md` 的写法都是同一个模式：**结论 + 证据 + 置信度 + "已经排除的假设"**。
最有价值的部分往往是"已经排除"那张表——它防止后来的自己（或别人）重走一遍：

> `| 假设 | 结论 | 证据 |`
> `| HBI 帧格式/字节序错 | ❌ 排除 | 逐位对照 ADF 参考实现：5 个代表性操作…全部一致；且 0xD3D3 是字节序回文 |`

同样地，本指南每一处判断都标了 **[实测] / [逆向] / [推断]**，
`config.h` 里每个开关的注释都写着"State of the bring-up when this was added"。
**没有证据的结论不准写进代码注释**，因为下一个读它的人会当成事实。

### 8.6 诊断写入**绝不允许是破坏性的**

这是硬规则。观察这个项目里所有"取证"型开关的写入目标：

| 诊断 | 写了什么 | 为什么安全 |
| --- | --- | --- |
| `B220_DSP_VERIFY_UPLOAD` | 只重指 page-255 窗口基址（upload 本来每记录都写），**只读** DSP RAM | 读完 DSP 与 boot attempt 留下的一模一样 |
| `B220_DSP_PAGE255_CALIB` | 往 **scratch RAM `0x00080A24`** 写 16 个字（镜像范围内唯一有文档的空洞） | 不碰任何有效镜像区域 |
| `B220_DSP_VERIFY_BEFORE_GO` | 只读采样记录 + 读 `reg 0x0034/0x0030/0x0032/0x0006` | 在**任何 host 命令之前**取证，不改变后续 |
| `B220_DSP_RELEASE_MATRIX` | 每个变体各做**一次完整全新** reset + upload | 不做增量修改，失败也不留半状态；且"应用已在跑时**拒绝运行**" |
| `B220_ZL_DIAG` 的 `hbi_sweep` | 每个组合**一次只读**寄存器读，之后**恢复**正常设备 | 明确写出"one read-only register read per combination, then the normal device is restored" |
| `B220_PA_BIT_FINDER` | 会写 `0x12/0x04/0x02`（故意） | 这是**唯一**被允许"解除冻结"的诊断，而且解锁时大声报警 |
| 写能力验证（§4.5 of FLASH-MANUAL） | 只往**空的 OTA 槽 `0xc40000`** 写 64 KB 再读回、然后擦除 | 不影响启动、不碰任何出厂数据 |

反过来，`B220_DSP_HANDS_OFF` 那次的教训值得记：
"我们的固件在 reset + `reg 0x14 <- 1` + 重灌镜像，这可能**毁掉一个本来正常运行的 DSP 应用**"——
于是一个专门的"hands off"开关被造出来，**完全不动 GPIO19、不写任何设备寄存器**，只做只读轮询。
**当你不确定某个动作是否安全时，先造一个不写它的版本。**

### 8.7 一条副作用：永远给"关掉诊断"留一条**一行**的路

每个诊断开关的注释里都有 "One-line kill switch" 或 "Set to 0 to get the previous boot
behaviour back"。这条纪律的收益在这个项目里非常具体：生产构建（`B220_AUDIO_LOOPBACK 0` +
全部诊断 0）与实验构建之间只差**几个 0/1**，所以"演示用的稳定版本"和"正在做的实验版本"
永远不会分叉成两条代码线。

---

## 9. 检查清单

### 9.1 最终验收清单（按 boot 顺序，逐项打勾）

串口在 COM25 @115200（`capture-serial.ps1`），往下逐条对：

| # | 项目 | 期望证据（日志 / 现象） | ✅ |
| --- | --- | --- | --- |
| 1 | **屏幕** | `I (704) ili9341: LCD panel create success, version: 2.1.0` + `LcdDisplay: Turning display on`；物理 128×160、颜色正确（不反红蓝） | ☐ |
| 2 | **背光** | `XiaoMoDouB220: AW9106B backlight on (DIM0/1/2 = 0x3F)`（不是 GPIO，是 I2C `0x5B`） | ☐ |
| 3 | **PSRAM / CPU** | `esp_psram: Found 8MB PSRAM device` / `Speed: 80MHz` / `SPI SRAM memory test OK` / `cpu freq: 240000000 Hz` | ☐ |
| 4 | **WiFi** | 首次：`Access Point started with SSID Xiaozhi-4CE6` + `192.168.4.1`；已配网：`Connected to WiFi: <ssid>` + `Got IP: 192.168.x.x` | ☐ |
| 5 | **DSP host-boot** | `I2S up before DSP boot: 48 kHz, 16-bit, stereo` → `reset: GPIO19 low 10 ms` → `HBI attached to SPI2 (cs=0, 10000000 Hz, mode 0)` → `firmware image transferred (3699 records)` → `codec: reg 0x0014 = 0x0200 -> the ZL38063 application is READY` | ☐ |
| 6 | **mic 回环可听** | 回环诊断构建（`B220_AUDIO_LOOPBACK 1` + `B220_AUDIO_TEST_MODE 1`）下 1 kHz 音可听；速率行 `tx_peak=32759 tx_mean~20855 write_err=0` | ☐ |
| 7 | **PA 单一边沿** | `pa: final AW9106 state … \| 0x02 writes=N, SHDN rising edges=1 (AW8737S mode 1)`，且 codec 的 `EnableOutput(true)` 只打 `already ran this boot … IGNORED on purpose` | ☐ |
| 8 | **生产 codec 48 kHz** | `Zl38063AudioCodec: ZL38063 audio codec: 48000 Hz in / 48000 Hz out, 16-bit, 2 slot(s), I2S0 borrowed from the board bring-up` + `audio bring-up: I2S0 tx/rx handles are up` | ☐ |
| 9 | **服务器会话** | `MQTT: Connected to endpoint` + `MQTT: Session ID: xxxxxxxx` + `StateMachine: activating -> idle` | ☐ |
| 10 | **唤醒词识别** | `MODEL_LOADER: Successfully load srmodels` + `LiteAudioEngine: Initialized, WakeNet: yes` + `EspWakeWord: Wake word(wn9_nihaoxiaozhi_tts),freq: 16000, chunksize: 512`；说"你好小智"出现 `Application: Wake word detected: 你好小智 (state: 3)` | ☐ |
| 11 | **完整对话往返** | `connecting -> listening` → `Application: >> …` → `listening -> speaking` → `AudioService: Resampling audio from 24000 to 48000` → `<< …` → `speaking -> listening`；**喇叭能听到回答** | ☐ |
| 12 | **空闲回落** | 空闲后 `Application: Wake word detected` 仍可再次唤醒（第二次会话拿到新的 `Session ID`） | ☐ |

**收尾核对（每次改动都做）**：

```powershell
# 1) 是否走了规范构建入口（而不是裸 idf.py build）
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi

# 2) 生成结果的配置自检
Select-String -Path .\xiaozhi-esp32-b220\build\config\sdkconfig.json `
              -Pattern 'USE_ESP_WAKE_WORD|WAKE_WORD_DISABLED|SR_WN_WN9_NIHAOXIAOZHI_TTS'
#    期望：USE_ESP_WAKE_WORD=True / WAKE_WORD_DISABLED=False / SR_WN_WN9_NIHAOXIAOZHI_TTS=True

# 3) assets 里到底有没有模型
$B='.\xiaozhi-esp32-b220\build'
[System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$B\generated_assets.bin")).Contains('srmodels')  # True

# 4) 刷写 + 读回校验（不带 -FullErase 时保留 NVS/WiFi 凭据）
.\flash-b220.ps1
#    判定标准：Hash of data verified. 并且读回 MD5 与 build\xiaozhi.bin 一致
```

### 9.2 本部分产出/涉及的文件与脚本清单

**生产代码（PART 3 新增或定稿）**：

| 文件 | 作用 |
| --- | --- |
| [zl38063_audio_codec.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063_audio_codec.h) | 生产 codec 的类声明与设计说明 |
| [zl38063_audio_codec.cc](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063_audio_codec.cc) | `Read`/`Write`/`EnableInput`/`EnableOutput`/`SetOutputVolume`/`Start` 的实现（48 kHz 借用 I2S0） |
| [b220_audio_share.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/b220_audio_share.h) | 板级 ↔ codec 的**唯一**接口（`extern "C"`，5 个访问器） |
| [xiaomodou_b220.cc](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/xiaomodou_b220.cc) | `B220_I2sUpBeforeDspBoot()`、PA 五步单例 `b220_pa_enable_one_pulse()`、访问器实现、`GetAudioCodec()`、`InitializeButtons()` 空实现 |
| [config.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/config.h) | `AUDIO_INPUT/OUTPUT_SAMPLE_RATE 48000`、全部 `B220_*` 开关（含 `B220_AUTO_LISTEN 0`） |
| [config.json](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/config.json) | 板级 sdkconfig_append（含三行唤醒词配置） |
| [README.md](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/README.md) | 板级硬件表、引脚表、构建方式、已知待测项 |
| `main/audio/audio_codec.h` / `audio_codec.cc` | 基类（**未改**）：`Read`/`Write` 纯虚、`duplex_`/rate/channels 约定 |
| `main/audio/audio_service.cc` / `audio_service.h` | **未改**：48k↔16k 与 24k↔48k 重采样、`AUDIO_POWER_TIMEOUT_MS 15000` 空闲掉电、`SetModelsList()` |
| `main/audio/wake_words/esp_wake_word.cc` | **未改**：`esp_srmodel_filter(ESP_WN_PREFIX)` → `esp_wn_handle_from_name` → `create(name, DET_MODE_95)`，`Feed()` 多通道取左 |
| `main/application.cc` | **未改**：传输选择（`HasWebsocketConfig()` → WS，否则 MQTT）、24 k/48 k 警告 |

**逆向与素材产物**：

| 路径 | 内容 |
| --- | --- |
| `…\zl38063\WAKE-WORD-PATH-REVERSE.md` | 唤醒词路径逆向报告（软件检测、16 kHz 单声道、无外部触发） |
| `…\zl38063\wakenet\README.md` | wn5 模型提取 + esp-sr 兼容性/选项排序的完整分析 |
| `…\zl38063\wakenet\wakenet5_modoujingling.bin` | 提取出的原厂 wn5 模型（84 160 B，sha256 `fb786b2a…`） |
| `…\zl38063\wakenet\wakenet5_modoujingling_layout.txt` | 机器生成的布局 dump（头 + 20 条 `dl_matrix2dq_t` + `model_info_t`） |
| `…\zl38063\wakenet\wakenet5_tool.py` | `info` / `emit` 工具 |
| [ref/analysis/wakenet/gen/modoujingling_wn5.c](ref/analysis/wakenet/gen/modoujingling_wn5.c) | 重新生成的可重定位 C（262 436 B；**尚未接入构建**） |
| [ref/analysis/wakenet/gen/modoujingling_wn5.h](ref/analysis/wakenet/gen/modoujingling_wn5.h) | 对应头文件 |
| `ref\analysis\wakenet\v1.9.5_libwakenet_esp32.a` / `v1.0_libwakenet_esp32.a` | 备用的 wn5 引擎归档（供 §5.6 使用） |
| `ref\analysis\wakenet\v1.9.5_model_path.c` | v1.9.5 的 `model_path.c`（含 `srmodel_get_model_coeff`） |
| [ref/analysis/flashtone/README.md](ref/analysis/flashtone/README.md) | 原厂 `flashTone` 容器格式的独立重推导（74 条记录 × 64 B，token = `0x28 + (n<<8)`） |
| [ref/analysis/flashtone/tone-report.csv](ref/analysis/flashtone/tone-report.csv) | 74 条素材的属性表；唤醒提示音候选是 **032/033/034**（16 kHz 单声道、1.19/1.55/1.30 s、无 ID3） |
| `logs\production-codec.log` | 生产 codec 启动 + PA 锁存（[排错表](#7-合并排错表symptom--根因--精确处置) #7 #8 的证据） |
| `logs\wakeword.log` | **唤醒词成功的完整证据链**（模型加载 → 唤醒 → 多轮对话 → goodbye） |
| `logs\auto-listen.log` | auto-listen 版本 + **UDP 音频通道静默 65 s** 的实证（排错表 #15） |
| `logs\mic-loopback.log` | mic 回环 + peak/mean + L/R 成对样本 hex（§1.2 / §8.2 的证据） |
| `logs\listen-diagnose.log` | 无唤醒词时期"进入 listening 但服务器静默"的对照日志 |
| `logs\i2s-before-dsp.log` / `logs\i2s-tx-sweep*.log` | "I2S 先起"带来的变化 + I2S 变体扫描（48 kHz/16-bit slot 的实测依据） |
| `logs\pa-pulse.log` | PA 单一边沿与 1 kHz 满幅音的成功实验（`tx_peak=32759 tx_mean~20855`） |

**脚本与手册**：

| 文件 | 作用 |
| --- | --- |
| `scripts\build.py` | **唯一规范的构建入口**：`python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi` |
| `scripts\tests\test_build.py` | host 测试（断言 `CONFIG_SR_WN_*` 不能进板级 `config.json`） |
| `scripts\build_default_assets.py` | 把 `srmodels.bin` 打进 `generated_assets.bin` |
| [flash-b220.ps1](flash-b220.ps1) | 一键刷写 + 读回校验；`-FullErase` 首次切换用；自动自检 assets 里有没有 `srmodels` |
| [capture-serial.ps1](capture-serial.ps1) | 抓启动日志到 `logs\`（默认 115200） |
| [FLASH-MANUAL.md](FLASH-MANUAL.md) | 刷机指导书：接线、进/退下载模式、备份/恢复、唤醒词验收（§4.7） |
| [PHASE3-DSP-DEBUG-STATUS.md](PHASE3-DSP-DEBUG-STATUS.md) | ZL38063 调试状态与"已排除的假设"表 |
| [backup/esp32-16MB-full.bin](backup) / `backup/esp32-16MB-unitB-stock.bin` | 两台原厂整片备份（**版本不同，不可互刷**） |

---

## 10. 一句话总结（PART 3）

> 把 B220 变成一台真小智，靠的不是更聪明的代码，而是**三条诚实**：
> **对硬件诚实**（I2S 就是 48 kHz，声明成 16 kHz 只会跑 3 倍速）；
> **对时序诚实**（功放一个 boot 只允许一个上升沿，第二次必须锁存成 no-op）；
> **对证据诚实**（读那个真的会被更新的 `reg 0x0014`，不是看起来更"标准"的 `reg 0x0030`）。
>
> 唤醒词的答案则来自一个更早的逆向结论：**原厂本来就是在 ESP32 上用软件识别关键词**，
> 所以不需要 GPIO、不需要 HBI 触发、也不需要底座。
> 于是"最后一块拼图"落到了一句务实的取舍上——
> 用 2.4.7 现成的 `wn9_nihaoxiaozhi_tts`（**你好小智**，当天可验证），
> 而不是为了原关键词去接 esp-sr v1.x 的 wn5 引擎（**模型已备好、机制已确认、数小时到数天、中等风险，
> 明确未实现**）。

---

*PART 3 完。相关文档：PART 1（可行性/引脚/刷机）、PART 2（板级骨架）、
[FLASH-MANUAL.md](FLASH-MANUAL.md)、[PHASE3-DSP-DEBUG-STATUS.md](PHASE3-DSP-DEBUG-STATUS.md)、
[zl38063/WAKE-WORD-PATH-REVERSE.md](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063/WAKE-WORD-PATH-REVERSE.md)、
[zl38063/wakenet/README.md](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063/wakenet/README.md)。*
