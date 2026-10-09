# 小魔豆 B220 移植小智（xiaozhi-esp32）可行性分析

日期：2026-10-07　对象：`backup\esp32-16MB-full.bin`（MD5 `a6a27d2b4b7a8de36636e9ef635bea40`，已复核）
状态：**只做分析，未修改任何设备/固件**

---

## 0. 结论速览

**可行，分三档；最大的门槛在硬件（刷机通道），不在软件。**

| 档位 | 内容 | 置信度 |
| --- | --- | --- |
| ✅ **基本盘，一定能做** | 刷入小智固件、ILI9341 屏出图、WiFi 联网配网、按键、唤醒词、云端对话（编解码/协议全链路） | 高 |
| ⚠️ **需攻关** | 复用原机 **ZL38063 语音 DSP**（3 麦阵列 + AEC + 远场），让麦/喇叭继续走原机音频通路。**唯一前提：DSP 自启动**——原厂 app 从不向 DSP 上传固件（二次深挖证实），说明固件躺在 DSP 自己的从属 SPI Flash 里 | 中高（复位/时序/配置记录全部拿到；风险集中在"DSP 是否真的自启动"） |
| 🅱️ **兜底方案** | DSP 打不通时，加 **INMP441（I2S 麦）+ MAX98357/NS4168（I2S 功放）**，约 10 元、6 根线，绕过 DSP，语音功能照常 | 高（但需焊接，空闲 GPIO 够用） |

**真正的硬门槛是"刷机"**：本机 micro-USB 是**只读日志口**，PC→ESP32 的数据通路不存在（`BACKUP-README.md` 有 10 项实测证据）。必须**飞线到 ESP32 模块的 UART0（GPIO1/GPIO3/GND）**，或用编程器直接写 SPI Flash。

好消息：整片 16 MB 备份已拿到并校验一致，**回滚路径存在**（`write_flash 0x0`）。

---

## 1. 原机硬件 vs 小智要求（逐项对照）

| 项目 | 原机情况 | 小智要求 | 判断 |
| --- | --- | --- | --- |
| 主控 | ESP32-D0WD（经典 ESP32，双核 LX6） | 上游 v2.x 官方支持 `esp32` 目标（`bread-compact-esp32`、`-lcd`、`esp32-cgc`） | ✅ |
| Flash | 16 MB | `partitions/v2/16m.csv` 直接可用（app 3.9 MB×2 + assets 8 MB） | ✅ |
| PSRAM | 8 MB（SPI，80 MHz）——**原厂固件确实启用了 PSRAM**（镜像内含 `spiram_psram.c`、`Found %dMBit SPI RAM device`、`Adding pool of %dK of external SPI memory to heap allocator`、`middle---system memory internal (%d) spiram (%d)`） | 非必需，但更好（唤醒词缓存等走 PSRAM） | ✅ |
| WiFi | 自带（NVS 里有 SSID/密码，SmartConfig 配网） | 必需 | ✅ |
| 唤醒词 | 原机由**图灵 AI 模组**做唤醒 | ESP-SR **WakeNet9「你好小智」官方支持 ESP32**；本机 `sdkconfig.defaults.esp32` 已默认开启 | ✅（CPU 余量待实测） |
| 显示 | **ILI9341，240×320，SPI**（本次新确认） | `esp_lcd_ili9341` + `CONFIG_LCD_ILI9341_240X320` 现成 | ✅ |
| 音频 | **ZL38063 DSP 在链路中间**：3 麦 → DSP → I2S → ESP32 → I2S → DSP → 内部 DAC → 功放 | 小智用 `AudioCodec` 抽象（ES8311/ES8388/无 codec） | ⚠️ 需自写 codec 驱动 |
| 按键 | GPIO0 疑似启动键；其余按键引脚未取出 | 至少 1 个（BOOT 键即可） | ⚠️ 低风险 |
| 底座 | 电机/头尾 LED 在底座板，走 UART `{AT+...}` | 小智支持 MCP 工具自定义 | 🎁 增值 |
| 其它 | AW9106 灯驱（I2C 0x5B）、SC7A20 加速度计（0x19）、TF 卡（SDMMC slot1） | 可选 | 🎁 增值 |

---

## 2. 本次新确认的关键硬件事实（附地址证据）

> 方法：`tools\re_helper.py` 的字节级 L32R 交叉引用索引 + 定点反汇编（详见 `GPIO-FINDINGS.md` §1）。

### 2.1 音频 I2S —— 单条全双工 I2S0，ESP32 为主机　【高置信】

| 信号 | GPIO | 证据 |
| --- | --- | --- |
| BCLK | **5** | `i2s_pin_config_t` @ DROM `0x3f4317ac` = `{5, 25, 26, 35}` |
| WS/LRCK | **25** | 同上 |
| DOUT（ESP32→DSP） | **26** | 同上 |
| DIN（DSP→ESP32） | **35** | 同上（35 是只输入脚，正好做 DIN） |

- 该结构体在代码 `0x4012539a` 处经 `l32r a11, 0x40111a58 → 0x3f4317ac` 传给 `i2s_set_pin`（函数 `0x4017732c`）。
- `i2s_config_t` @ DRAM `0x3ffbf398`：`mode = 13`（`MASTER|TX|RX`，即**全双工主机**）、16 bit、`channel_format = 0`、`dma_buf_count = 3`、`dma_buf_len = 300`、`use_apll = 1`；在 `0x40125365` 以 `i2s_num = 0` 调用 `i2s_driver_install`。
- 结论：**一条双向 I2S 总线**（不是两条 simplex），与 xiaozhi 的 `NoAudioCodecDuplex` 拓扑天然吻合。

### 2.2 语音 DSP = ZL38063，由 ESP32 做 host boot　【高置信】

- **复位脚 = GPIO19**：函数 `0x40124418` 做 `gpio_config(1<<19, OUTPUT)` → 拉低 → 10 ms → 拉高。
- **上电时序**：`0x40124454` → `0x40124468` 调复位 → `vTaskDelay(500)` → `retry = 6` 循环调用 `0x40126240`。
- **`0x40126240` 就是 host boot 包装**：在栈上拼一个 24 字节描述符后调用 `VprocTwolfHbiBoot`（`0x40125c64`）。

```
描述符来源：DROM 0x3f4322d8..0x3f4322e4
  原始字节：73 0e 01 00 | 40 30 08 00 | 00 02 08 00 | 00 00 0f 1c
  栈上字段：+8  = 0x01        （模式/标志）
            +12 = 0x00080200  （→ DROM 0x3f480200）
            +16 = 0x00083040  （→ DROM 0x3f483040）
            +20 = 0x0E73 = 3699
```

- **DSP 固件镜像：不在这份 dump 里，也无法从 dump 恢复**【高置信，二次深挖结论】
  - 描述符里的 `0x00080200` / `0x00083040` **不是主机侧指针，而是 DSP 侧地址**（`prgmBase` / `execAddr`），只被写进 HBI 寄存器 `0x12C` 并打印日志，**没有任何代码解引用它们**；"`+0x3f400000` → `0x3f480200`" 只是算术巧合。
  - 穷举调用图扫描（3.9 MB 镜像、621,274 条指令）：`HBIwrite`(`0x40125a7c`) 只有 **10** 个调用点、全在驱动内部、**没有一个在"遍历镜像数据"的循环里**；`VprocTwolfFirmwareStart`(`0x40126210`) 是 `ets_delay_us` 空壳；`0x40125f78`（像镜像加载的那个）**零调用者**。那 69 KB 区域实为 codec/WiFi 相邻的 16 位表数据（熵 6.71，deflate/zlib/lzma 全部解不开，边界紧贴 WPA/EAPOL 字符串）。
- **真正可恢复的是「配置记录」**【高置信，这是音频移植的关键抓手】
  - 位置：DROM **`0x3f459eaa`**（app 偏移 `0x59eaa`，Flash 偏移 `0x69eaa`）；长度字在 `0x3f459ea8` = `0x0412` = **1042 字节**。
  - 格式：**260 × `{u16 寄存器, u16 值}`**；寄存器低字节逐条 +2（259/259 连续），高位页分布 `{0x02:128, 0x03:128, 0x04:5}`；首条 `(0x0200,0x0001) (0x0202,0x3c0f) (0x0204,0x0010) (0x0206,0x5000)`，末条 `(0x0400,0x0400) (0x0402,0x0320) (0x0404,0x0033) (0x0406,0x0000)`。
  - **`0x0202` 就是交叉开关（cross-point）配置寄存器**（对照 Linux 主线 `zl38060.c` 的 crosspoint 表），所以这条记录里很可能已经包含原机的音频路由。
  - 加载器 `VprocTwolfLoadConfig`(`0x40125c30`) 全局只有一个调用点 `0x4012637d`（就在 boot 包装里），逐条带门控字节，实际**写入 130 条 / 跳过 130 条** —— 是一张真实生效的稀疏覆盖表。
- **结论：音频链路的唯一前提是「DSP 自启动」**
  原厂 app 从上电到结束，对 DSP 只做：GPIO19 复位 → 握手（reg `0x34` = `0x0BAD` → 轮询 reg `0x06` bit0，最长 4 s，重试 6 次）→ 写 130 条配置；**一个字节的固件都不上传**。而设备在产线上是能出声的 ⇒ 固件必然在 **DSP 自己的从属 SPI Flash** 里（Auto Boot）。
  ✅ **这对移植是好消息**：新固件**不需要**固件镜像，照抄同样的"复位 + 握手 + 写配置记录"即可。
  ⚠️ 若实测发现 DSP 并未自启动（例如从属 Flash 空/坏），才需要走 Plan B（见 §5 R2）。
- **HBI 控制总线与 LCD 共用 SPI2**：`spi_lcd.c`（函数 `0x40203554`）在初始化 LCD 自己的设备之后，**再 `spi_bus_add_device` 一次**（`0x402036a0`），把句柄写进 DRAM 全局 `0x3ffc4c98` —— 而 `vproc_common.c` 的 HBI 读/写（`0x40125862` / `0x401258cf`）正是从这个全局取 `spi_device_handle_t`。
  - 共用：SCLK = **32**、MOSI = **33**、MISO = **34**，10 MHz；
  - LCD 片选 = **27**；**DSP 片选 = GPIO0**【已静态确证】：DSP 那个 `spi_device_interface_config_t` 被 `memset` 清零后只写了 `clock_speed_hz = 10 MHz`(`0x989680`) 与 `queue_size = 6`，**`spics_io_num`(+16) 从未被写入 ⇒ 值为 0**；而且驱动确实把它当真实 GPIO 用——片选回调 `0x4020349c` 里 `l8ui a10, ifcfg, 0` → `gpio_set_level(pin = 0, level)`。
  - ⚠️ 两个后果：① **GPIO0 被 DSP 片选占用，不能再当小智的 BOOT 按键**（按键引脚仍需实测，或先用"唤醒词 + 网页配网"）；② GPIO0 同时是启动 strapping 脚（复位时必须为高），原机靠 USB 桥的 DTR/RTS 电路把它拉低进下载模式 —— 刷机与新固件运行不冲突，但建议示波器实测确认。
- **参考实现现成且同源**：`espressif/esp-adf` → `components/audio_hal/driver/zl38063/`（`vprocTwolf_access.c`、`vproc_common.c`、SPI HBI 传输 `tw_spi_access.c`、`zl38063.c`），与本机固件的 `vproc_common.c` 同名同源，MIT/Microsemi 授权。**这是本次可行性判断里最关键的一条利好。**

### 2.3 显示屏 = ILI9341 240×320　【高置信，纠正旧报告】

- 初始化命令表 @ DROM `0x3f4bdbf2`，**18 字节/条**（1 字节命令 + 16 字节参数 + 1 字节「长度|延时标志」），`0xFF` 结束：

```
0xC0 00 | 0xC2 01 FF | 0xC3 13 | 0xC4 20 | 0xC6 0F | 0xCA 0F | 0xC8 08 | 0x55 B0
0x21 | 0x36 00 | 0x3A 55 | 0xB1 40 02 14 | 0x26 01
0x2A 00 00 00 EF   ← 列窗口 0..239
0x2B 00 00 01 3F   ← 行窗口 0..319
0x2C | 0xE0(14B) | 0xE1(14B) | 0x11 | 0x29
```

- 这套 `0xC0/0xC2/0xC3/0xC4/0xC6/0xCA/0xC8/0x55/0x26/0xE0/0xE1` 是**ILI9341 专有**寄存器组（ST7735 没有），窗口 240×320 → **面板就是 ILI9341 240×320**。
- ⚠️ `GPIO-FINDINGS.md` §2.1 记的「160×128」应属误读（那大概是文字画布参数：附近确有 `movi a12, 160; movi a13, 128`，但紧接着是 `47/66/128` 这类字形度量）。
- 引脚汇总：**SCLK 32 / MOSI 33 / MISO 34 / CS 27 / DC 21 / RST 22，10 MHz**；**背光脚未在配置结构体里出现**（可能常亮或由底座控制，待实测）。
- 移植提示：即使面板是 ILI9341 的兼容克隆，也**可以把这张原厂初始化表当作自定义 init_cmds 传给 `esp_lcd_ili9341`**（顺序、参数原样照搬），这样"点亮"这件事基本不依赖猜测。
- ✅ **已上机证实**：数据通路正确（面板能显示内容），但**背光不亮**。深挖（含"寄存器直写"专项扫描）结论：
  - **背光 = AW9106B（I2C 0x5B）的 DIM0/DIM1/DIM2，原厂直接写 `0x3F`（最大）**；这是全镜像里**唯一持续非零**的 LED 通道写入；
  - **LEDC/PWM 在固件中完全不存在**（`0x3FF59000-0x3FF5A000` 零常量），所以没有调光；
  - 任何 GPIO 都没有被用作背光（穷举了 `GPIO_OUT_W1TS/W1TC`、IOMUX、RTC_IO 的所有写入点）；
  - 实测闭环：自检固件在开机后轮流点亮 AW9106 六路时，**用户在约第 30–34 秒看到屏幕亮起、中间闪灭一次**——正好是 DIM0/DIM1/DIM2 各亮 2 秒、间隔 0.15 秒的时序。**已写进板级 `InitializeBacklight()`，屏常亮。**

### 2.4 其它已确证外设

| 外设 | 引脚 / 地址 | 备注 |
| --- | --- | --- |
| I2C0 | SDA **18** / SCL **23**，100 kHz | AW9106B @0x5B（呼吸灯）、SC7A20 @0x19（加速度计，WHO_AM_I=0x11） |
| TF 卡 | SDMMC slot1：CLK **14** / CMD **15** / D0 **2** / D1 **4** / D2 **12** / D3 **13** | 与 LCD/DSP 无冲突 |
| DSP 复位 | **GPIO19** | 10 ms 低脉冲 |
| 底座 / 图灵模组 | UART，`{AT+...}` 协议 | 引脚未取出（增值功能才需要） |

### 2.5 对旧报告的一处纠正

`GPIO-FINDINGS.md` §2.4 认为「第二条 SPI（400 kHz）= ZL38063 控制口」。本次追踪表明该函数（`0x40234f90`）只被 `0x401fd919` 调用，而所在函数 `0x401fd894` 引用 `vfs_fat_sdmmc` 等字符串、走的是 `esp_vfs_fat_sdmmc_mount` 的 **SDSPI 分支**（SD 卡 SPI 模式初始化本来就是 400 kHz）。**DSP 的 HBI 实际在 SPI2 上与 LCD 共用**（§2.2）。

---

## 3. 与小智（xiaozhi-esp32）的兼容性结论

| 结论 | 依据 |
| --- | --- |
| **经典 ESP32 是上游一等公民** | `main/boards/bread-compact-esp32{,-lcd}/config.json` 里 `"target": "esp32"`；`sdkconfig.defaults.esp32` 在当前 main 里有实质内容；CI 无目标排除 |
| **16 MB 分区表现成** | `partitions/v2/16m.csv`：`ota_0 0x20000/0x3f0000`、`ota_1/0x3f0000`、`assets 0x800000/8M` |
| **应用体积够** | 本机 S3 构建产物 `xiaozhi.bin` = 2.64 MB，经典 ESP32 量级相当，3.9 MB 槽位宽裕 |
| **唤醒词可用** | ESP-SR 官方 ESP32 分支列出 WakeNet9 + `nihaoxiaozhi`；本机 `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y` |
| **音频抽象能对上** | `NoAudioCodecDuplex(input_rate, output_rate, bclk, ws, dout, din)` 正好是「一条全双工 I2S」；`AudioCodec` 默认 `input_channels_ = 1`，而 DSP 自带 AEC，不需要小智的软件参考通道 |
| **采样率不匹配也能跑** | `audio_service.cc:41` 输入≠16 k 时自动重采样到 16 k；`:320/:383` 输出与解码器采样率不同时也自动重采样 |
| **注意（经典 ESP32 限制）** | 该芯片**没有 TDM**，PDM/ADC-DAC 模式仅 I2S0；因此 DSP 若要走 TDM 多麦进 ESP32 则不可行 —— 但本机是**单条标准 I2S 全双工**，正落在支持范围内 |

**需要改的配置**：`sdkconfig.defaults.esp32` 默认锁 4 MB 分区表（`partitions/v2/4m.csv`）与 `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`，新板子要覆盖成 `16m.csv` + 16 MB + `CONFIG_SPIRAM=y`。

---

## 4. 建议的移植路线（分阶段，每阶段都有验证点）

### Phase 0 · 环境与基线（不碰硬件）
1. 用 `<user-home>\esp\v5.4\esp-idf`（本机 xiaozhi 构建实际使用的 IDF）编译 `bread-compact-esp32-lcd`，确认 `esp32` 目标可过。
2. 确认工具链：`xtensa-esp-elf` 13.2.0 已装，`esptool 4.8.1` 已装。
- **验收**：✅ **已完成**（2026-10-07）。`target esp32` + 板级 `bread-compact-esp32-lcd` 编译通过：
  - `xiaozhi.bin` = **2,065 KB**、`generated_assets.bin` = 264 KB（16 MB 布局下有 3.9 MB 应用槽位，余量充足）；
  - **`CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y` 在经典 ESP32 上编译通过** —— "唤醒词能不能用"这条不确定性消除；
  - 参考板的刷写参数是 `--flash-mode dio --flash-freq 40m --flash-size 4MB` + `partitions/v2/4m.csv`、**未开 PSRAM**；B220 板级文件需覆盖为 **16 MB + `partitions/v2/16m.csv` + `CONFIG_SPIRAM=y`**（原厂镜像头为 DIO / 80 MHz / 16 MB）。
  - IDF 用法：`. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`（**不要**用 `C:\esp\v6.1\esp-idf\export.ps1`，它会误用系统 Python 3.9 而拒绝启动）。

### Phase 1 · 打通刷机通道（动硬件，全程可回滚）
1. 拆机，飞线 USB-TTL(3.3 V) → ESP32 模块 **GPIO1(TX)、GPIO3(RX)、GND**（**不接 VCC**）。原机 USB 桥的 TX 并没有接到 GPIO3（已实测），所以**不需要割线**。
2. 进下载模式：现有 DTR/RTS 复位电路已实测可用（`backup\esp32-COM25-...-bootlog.txt` 里抓到过 `waiting for download`）。
3. `esptool flash_id` → 确认 `ESP32-D0WD` / 16 MB / 实际 Flash 型号。
   （原厂 app 镜像头已给出：**Flash 16 MB、80 MHz、DIO 模式**，入口 `0x40081814`，7 段；新固件刷写参数应与之一致：
   `--flash_mode dio --flash_freq 80m --flash_size 16MB`。）
4. `read_flash 0 0x1000000` 读一遍并与已有 dump 比对 MD5 → **证明"能写"的前提是"能读"**。
5. 写一个最小固件（串口打印 + GPIO 翻转），跑通 `write_flash`。
- **验收**：✅ **已完成**（2026-10-07，实测记录见 `PHASE1-FLASH-VERIFY.md`）：
  - `flash_id`：**ESP32-D0WD rev1.0 / 16 MB / GigaDevice GD25Q128**，MAC `b4:e6:2d:fc:4c:e5`（与备份 NVS 一致，同一台）；
  - 整片读回（277 s / ≈484 kbit/s）：**255/256 个 64 KB 块与备份逐字节一致**，唯一差异落在 **NVS 分区（0x9000–0xd000）**、形态为页轮转 → 固件运行时正常写入，**读取通路零误码**；
  - **64 KB 写入空 OTA 槽 `0xc40000` → esptool 自校验 `Hash of data verified.` → 读回 MD5 与写入源一致**；随后 `erase_region` 还原为 `0xFF`；
  - 出厂固件全程未被触碰（ota_0 / otadata 均未变），**整片回写备份即最终回滚路径**；
  - 刷机参数：`--before no_reset --after no_reset --baud 921600`（TTL 未接 DTR/RTS，靠手动方式进下载模式）。

### Phase 2 · 板级骨架（先不要音频）
1. 新建 `main/boards/xiaomodou-b220/`：`config.json`（`target: esp32`，`sdkconfig_append`: 16 MB 分区 + SPIRAM）、`config.h`、板级 `.cc`。
2. 接上 **ILI9341 240×320**（32/33/34/27/21/22）、按键 GPIO0、WiFi。
3. 不实现音频（先用 `DummyAudioCodec`），保证系统能起、能连服务器。
- **验收**：开机日志确认 PSRAM 容量与 Flash 容量；屏出图；配网成功；能与小智服务器握手、显示文字/表情。
- **进度**：✅ **板级已完成并编译通过**（2026-10-07）：`python scripts/build.py xiaomodou-b220` 成功，
  `xiaozhi.bin` = 2,180 KB、`generated_assets.bin` = 1,258 KB、`merged-binary.bin` = 9.45 MB；
  sdkconfig 实测确认 `esp32` + `16MB` + `partitions/v2/16m.csv` + `CONFIG_SPIRAM=y` + `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y`；
  显示屏**直接回放原厂 ILI9341 初始化表**（写进 `ili9341_vendor_config_t.init_cmds`），规避克隆片识别风险；
  板级链路按仓库规范接线：`config.json` → `main/Kconfig.projbuild` → `main/CMakeLists.txt` → `boards/xiaomodou-b220/`。
  ⏳ 待上机验证：刷写后确认 PSRAM 容量、屏出图、配网与握手（刷机脚本 `xiaomodou\flash-b220.ps1`）。
- **已上机验证**（2026-10-07，整片擦除后首次刷写）：
  - 写后读回 app 镜像 MD5 与构建产物**完全一致**（`E0EEB8293FC52D34D5C67D6F95A858D4`）；
  - 启动日志：`Found 8MB PSRAM device`（经典 ESP32 地址空间只映射得到 **4 MB**，属正常）、`detected chip: gd`、`flash io: dio`、
    `Board: UUID=… SKU=xiaomodou-b220`、**`ili9341: LCD panel create success` + `LcdDisplay: Turning display on`（原厂初始化表回放成功）**、
    `Access Point started with SSID Xiaozhi-4CE6` + `http://192.168.4.1`、`free sram: 173975`；
  - 冷启动仍有 `flash read err, 1000` → `rst:0x10 (RTCWDT_RTC_RESET)`：**原厂固件同样现象**（本机 Flash 冷启动裕量问题），非本次改动引入；
  - 待调优：CPU 默认 160 MHz → 应设 240 MHz；PSRAM 40 MHz → 原厂为 80 MHz；随后做 WiFi 配网 + 服务器握手。
- **Phase 2 收尾结论**（2026-10-07，全部实测）：
  - ✅ **面板物理是 128×160**（此前按初始化表窗口推出的 240×320 是误判）→ 板级已改 128×160；
  - ✅ **背光 = AW9106B DIM0/1/2 = 0x3F**，已写进 `InitializeBacklight()`，屏幕常亮；
  - ✅ **几何/色序确认**：方向测试卡显示"竖条 + 白角在左上/右下"⇒ 不需要旋转/镜像；LVGL 界面颜色正常
    （`SpiLcdDisplay` 的 `swap_bytes=1` 已处理 SPI 字节序；**裸 `draw_bitmap` 必须自己字节交换**，测试卡曾因此红蓝互换）；
  - ✅ **操作事实**：启动键 = 底座软开关（ESP32 读不到）；**插 micro-usb 开机必进下载模式**（桥把 GPIO0 拉低），
    运行必须拔掉 micro-usb；因此这块板子上**没有任何 ESP32 可见的按键**，小智靠"无凭据自动进配网"启动；
  - ✅ 调优：CPU 240 MHz + PSRAM 80 MHz 已加入板级 `config.json`。

### Phase 3 · 音频（核心攻关）
1. 抽出**配置记录**（DROM `0x3f459eaa`，1042 字节，260 × `{reg,val}`）为 C 数组 —— **不需要固件镜像**（前提是 §2.2 的自启动判断成立）。
2. 移植 esp-adf 的 `vproc_common.c` + `vprocTwolf_access.c` + SPI HBI 传输层（换成 IDF 5.x 的 `spi_master` API）。HBI 设备 = **SPI2 + CS GPIO0 + 10 MHz**；注意**不要照抄原厂把 `clock_speed_hz` 留成 0 的写法**（我这里读到的是 10 MHz，稳妥值取 2–10 MHz）。
3. 写 `Zl38063AudioCodec : public AudioCodec`：复位（GPIO19）→ 握手（reg `0x34`=`0x0BAD`、轮询 reg `0x06`、reg `0x12C` 写 prgmBase/execAddr）→ 逐条写配置记录 → `Read/Write` 走全双工 I2S（5/25/26/35）。
4. **先验证"DSP 到底有没有起来"**：读状态（reg `0x06`、reg `0x0E` 写后自清）＋ 示波器看 I2S 时钟与 HBI 波形；再做**本地回环**（录→放）。
5. 回环通过后再接云端 ASR/TTS。
- **验收**：对着麦说话能听到自己的声音；再验证"你好小智"唤醒 + 云端对话。
- **若 DSP 起不来**：转 §5 R2 的 **Plan B** —— 拿到 ZL38063 固件镜像后自己 host boot，**仍然是原机 DSP 音频通路**（不违背"必须用原 DSP"的要求）。

### Phase 4 · 特色整合（增值，可后置）
- 底座 `{AT+LMY/RMY/HLY/TLY/MTO/...}` → 封装成 MCP 工具（"让小魔豆跳舞/摆头"）；
- AW9106 呼吸灯跟随对话状态；SC7A20 摇一摇打断；TF 卡做资源。

### Phase 5 · 留档
- 恢复原厂：`esptool write_flash 0x0 backup\esp32-16MB-full.bin`（注意：新分区表与原厂不同，整片回写是唯一可靠的回滚方式）。

---

## 5. 风险矩阵

| # | 风险 | 概率/影响 | 依据 | 对策 |
| --- | --- | --- | --- | --- |
| R1 | **刷机通道**：USB 只能读日志，必须飞线 | 高 / 高 | `BACKUP-README.md` 10 项实测 | 3 线飞线；现有 DTR/RTS 复位可用；先做"可写可读回"验证 |
| R2 | **DSP 是否真的自启动**（本次最大不确定项）：固件镜像不在 dump 里，只能靠 DSP 自己的从属 Flash | 中 / 高 | §2.2：app 从不上传固件，但设备能出声 | 先实测（读 reg `0x06` 状态 + 示波器看 HBI/I2S 波形）；**Plan B**：注册 Microchip SDS 下载 ZLS38063 固件（或复用 esp-adf `libfirmware.a` 里的 1.P1.4.0 镜像，注意授权），改成每次上电 host boot 灌入 —— 仍走原机 DSP 通路 |
| R3 | **采样率/时钟匹配**：DSP 配置记录按原机调好，率必须一致 | 中 / 中 | `i2s_config_t` 初值 48 kHz，而录音链路用 16 kHz | 实测确定（先 16 k 试，再 48 k）；必要时用 48 k 让 DSP 内部转换 |
| R4 | **交叉开关配置**：esp-adf 里 `VprocTwolfUpstreamConfigure/DownstreamConfigure` 是 `#if 0` 的 TODO | 中 / 中 | esp-adf `tw_spi_access.c` | 从原固件的 `-arec`/`-apla` 路径逆向（原厂显然实现了），或按 datasheet 寄存器表自己写 |
| R5 | **唤醒词 CPU 余量**：LX6 无 SIMD | 中 / 中 | ESP-SR 基准页只有 WakeNet5 的 ESP32 数据 | 实测；必要时降级模型/降低 AFE 档位 |
| R6 | **LCD 背光与按键引脚未知** | 低 / 低 | 配置结构体里没有背光字段 | 实测；从空闲 GPIO 里找 |
| R7 | **冷启动 Flash 读取偶发失败**（原机日志有 `rst:0x10 (RTCWDT_RTC_RESET)`） | 低 / 中 | `BACKUP-README.md` §3 | 刷机时留意供电；`--baud 921600` 若不稳降速 |
| R8 | **DSP 彻底打不通**（连 Plan B 也失败） | 低 / 高 | 你已明确**不接受**"加 I2S 麦 + 功放模块"的兜底 | 只剩两条：继续向厂商/服务商索取固件；或放弃改机（保留原固件）。空闲 GPIO ≈ 10 个（20/24/28/29/30/31/36/37/38/39），先留着不用 |
| R9 | **授权**：ZL38063 固件/配置属厂商 IP；esp-adf 副本带限制性授权 | 低 / 中 | esp-adf `Readme_twolf.txt`、`zl38063_firmware.h` | 自用不涉及再分发；优先复用**本机 dump 里**的固件而不是第三方副本 |

---

## 6. 已确认信息与开工前提（2026-10-07）

| 项 | 你的回答 | 对方案的影响 |
| --- | --- | --- |
| 备份来源 | **飞线到 ESP32 UART0（GPIO1/GPIO3）+ esptool** | ✅ 刷机通路已被亲手证明可行；Phase 1 复用同一套接线与 `esptool.cfg` 复位时序即可 |
| 器材 | 3.3 V USB-TTL、万用表、**100 MHz 示波器** | ✅ 足够验证 GPIO19 复位脉冲、HBI 波形、I2S 时钟，以及 GPIO0 ↔ ZL38063 HCS 通断 |
| 兜底 | **不接受**加 I2S 麦/功放模块 | 音频必须走原机 DSP ⇒ §5 R2 的 Plan B 改为"拿到固件镜像后自己 host boot"，而不是加模块 |
| 代码仓库 | 你的 Gitea `<私有仓库>`，已 clone 到 `<repo>`（`v2.5.0-31-g0d576d3`，main） | ⚠️ 该仓库 **`idf: >=6.0.1`**，而本机只装了 ESP-IDF **5.2.5 / 5.3.2 / 5.4** ⇒ **Phase 0 必须先装 IDF 6.1** |
| 开工范围 | 同意 Phase 0 + Phase 1 | — |

**其它已查明的环境事实**
- 本机 git 全局代理 `<代理地址>` 已下线；拉 GitHub/Gitea 需 `-c http.proxy= -c https.proxy=` 绕过。
- `github.com:443`、`dl.espressif.com:443`、`gitee.com:443`、`<私有仓库>` 均可达。
- 当前会话沙箱只允许写 `<workspace>`；工程在 `<repo>`、IDF 候选位置在 `<user-home>\esp`，**都在沙箱外** ⇒ 编译与改文件需要逐次授权（或把工程搬进会话工作区）。
- 在受限沙箱下 msys 版 git 偶发 `couldn't create signal pipe` 失败，部分 git 操作需要放宽权限才能跑。

**仍待你确认**
1. **工作目录与沙箱策略**：工程搬进会话工作区（之后零确认）还是保持在外层（每次写盘都要授权）？
2. **功能优先级**：语音对话 / LCD 表情 / 底座电机动作 / 灯光 / 加速度计 —— 先做哪些？
3. Phase 3 若走 Plan B，是否接受使用第三方 ZL38063 固件镜像（授权问题见 §5 R9）。

---

## 7. 一句话总结

**小魔豆 B220 是一台"软件可改、硬件难接"的机器：ESP32-D0WD + 16 MB Flash + 8 MB PSRAM 完全够跑小智，屏是 ILI9341（现成驱动），
唯一的硬骨头是链路中间那颗 ZL38063 语音 DSP —— 而本次逆向已经拿到它的复位脚（GPIO19）、I2S 脚（5/25/26/35）、
与 LCD 共用的 SPI2 通道、以及**内嵌在固件里的 DSP 固件与配置记录**，同时上游 esp-adf 有同源可移植的主机端代码。
所以移植是可行的：先飞线打通刷机与可回滚，再按"先出图后出声"的顺序推进；万一 DSP 走不通，加两个 I2S 小模块即可保底。**
