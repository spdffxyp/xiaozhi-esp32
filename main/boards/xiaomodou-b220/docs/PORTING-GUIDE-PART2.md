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
