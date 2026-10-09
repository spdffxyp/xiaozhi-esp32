# ZL38063 工作原理与在本设备中的用法

依据：`D:\workspace\元器件文档\ZL38063.pdf`（Microsemi 官方数据手册 Rev 4.0，用自写的
纯 Python 提取器转成文本，见 `docs\ZL38063.txt`）+ B220 固件实测。

---

## 1. 这颗芯片是干什么的

**Microsemi（现 Microchip）AcuEdge ZL38063 —— 麦克风阵列 ASR 辅助音频处理器**
（Microphone Array ASR-assist Audio Processor）。

面向电视、机顶盒、智能音箱、数字助手。核心价值：**在远场、有回声、有噪声的环境里，
把麦克风阵列的原始声音处理成"适合云端语音识别"的干净单路语音**。

它内部是一颗专用 DSP，做的是：

| 处理 | 说明 |
| --- | --- |
| **AEC** 回声消除 | 双向通话固件支持长尾最多 **256 ms** 的尾音，容忍喇叭失真 |
| **非线性处理** | 残留回声抑制 |
| **降噪 NR** | 稳态/非稳态噪声抑制 |
| **远场麦克风处理** | 波束成形/阵列处理，最远 **16 英尺（≈4.9 m）** 拾音 |
| **Cross-Point Switch** | 高度可配置的音频交叉开关：输入（数字麦、I2S/TDM）与输出（DAC、I2S/TDM）之间任意路由 |
| 均衡等 | 收发（Tx/Rx）两条音频路径上的增益与滤波 |

**两套固件模式**（规格书明确区分）：

1. **双向语音通信（Two Way Voice Communication）**
   - 2×3 麦克风线性阵列 → **180°** 拾音
   - 3 麦离轴/三角形阵列 → **360°** 拾音
   - 最多支持 **6 麦**阵列
2. **ASR 辅助（ASR Assist）**
   - 同样支持 2 麦线性 / 3 麦离轴 / 环形阵列
   - **唤醒词 spotting**：即使在播放 DAC 音频时也能检测唤醒词
   - 降噪 + 远场

---

## 2. 硬件接口（规格书）

| 接口 | 作用 |
| --- | --- |
| **SPI Slave（HBI，Host Bus Interface）** | 与主控通信的**推荐主通道**；**主机启动（Host Boot）走这里**，速度最快 |
| **I2C Slave** | 备选主控通道（与 SPI slave 复用引脚）；**I2C 启动慢，可能 >5 秒** |
| **Master SPI** | 接**外部串行 Flash**，用于 **Auto Boot**（<1 秒） |
| **UART** | 调试与调音（配合 MiTuner GUI） |
| **4 × I2S / TDM** | 音频总线。每个口可主可从，速率 128 kb/s – 8 Mb/s，支持 I2S / 左对齐 / PCM / TDM；每口 2 通道，最多支持 4 个麦克风 |
| **14 个 GPIO** | 启动 strap、中断、事件、通用 IO（WLCSP 封装只有 11 个） |
| **HINT** | 主机中断引脚，属于 HBI；中断后主机读寄存器得知是哪个事件 |

时钟与电源：需要 **12.000 MHz 晶振**（或外部时钟），且在复位释放前必须稳定；
核心 DVDD12 建议用外部低噪 LDO 供电。

---

## 3. 它怎么"工作"起来 —— 内建 Bootloader 与两种启动方式

**关键概念：这颗 DSP 自己不带可执行代码，必须先被"灌"进固件才能工作。**

它内部有一个 bootloader。复位释放后：

```
1. 读 GPIO 上的 bootstrap 电阻设定 → 存进 Boot Sense 寄存器
2. 12 MHz 时钟必须已稳定
3. 按 strap 决定走哪条路：
   ├─ Auto Boot  ：自己通过 Master SPI 从外部 Flash 读出 firmware + config → Program RAM
   └─ Host Boot  ：拉 HINT 通知主机，等主机通过 HBI（SPI 或 I2C）把
                   firmware + config 灌进 Program RAM
4. 跳到固件入口开始执行
```

**要灌两样东西**（规格书特别强调这是两个独立概念）：

| 名称 | 内容 |
| --- | --- |
| **Firmware image** | AcuEdge 算法本体（回声消除/降噪/波束成形/交叉开关的 DSP 程序） |
| **Configuration record** | **全部用户寄存器的设置值**，用 PC 端 **MiTuner GUI** 调出来；决定实际音效、增益、路由 |

也就是：每次上电都要（重新）把这两样加载进去，配置记录决定"这套算法在这块板子上怎么调"。

---

## 4. 本设备（B220）实际是怎么用的 —— 固件实证

### 4.1 ESP32 里内置了整份 Microsemi VPROC 主机库

固件字符串里能找到完整的 VPROC 主机 API 及其命令行帮助文本：

```
VprocTwolfHbiBoot()            // 通过 HBI 引导设备
VprocTwolfFirmwareStart()      // 启动 RAM 中的固件
VprocTwolfLoadConfig()         // 把配置记录灌进设备 RAM
VprocTwolfReset()              // 复位，模式 [0:RAM | 1:ROM | 2:SOFT | 3:AEC | 4:BOOT]
VprocTwolfLoadFwrFromFlash()   // 从设备的从属 Flash 加载固件
VprocTwolfLoadFwrCfgFromFlash()
VprocTwolfFirmwareStop()
```

CLI 帮助文本也在（`-wr` 写 16 位寄存器、`-rd` 读、`-lffh-a` 从主机经 SPI 加载固件、
`-arec`/`-apla` 配置录音/放音模式、`-fclr` 擦除 ZL380xx 的从属 Flash …）。
**说明厂商是把 Microsemi 的参考驱动整包移植进来的。**

### 4.2 启动日志（固件字符串，运行时按顺序打印）

```
1- Firmware boot loading started ....
2- Loading the image to RAM....done
3- Loading the config file into the device RAM....
Device boot loading completed successfully...
```

**这三行就是 Host Boot 的全过程**：ESP32 每次上电把 firmware 和 config 推进 DSP 的
Program RAM，然后启动它。所以本机走的是 **Host Boot over SPI(HBI)**，而不是让 DSP 自己从
Flash 启动。

### 4.3 代码位置与接口参数

| 项目 | 值 |
| --- | --- |
| 驱动代码 | IROM `0x401256xx` – `0x401263xx`（源文件 `MediaHal/DSP/zl38063/vproc_common.c`） |
| HBI 寄存器读/写 | `0x40125924` / `0x40125a7c`（16 位寄存器模型） |
| 主机引导 | `0x40125c64`（`VprocTwolfHbiBoot`，含复位后状态校验，会比对 `0xD3D3` 之类的状态字） |
| 启动流程函数 | `0x40126240`：组装参数结构体 → 打日志 → 调 `0x40125c64` |
| **控制总线** | 第二条 SPI，**400 kHz**（`0x40234f90`；见 `AW9106-FINDINGS.md` 第 6 节） |
| 复位/使能脚 | 由配置结构体 `+12` 字段给出，代码用 `(1<<n)` 生成 `gpio_config` 的 `pin_bit_mask` 置成输出 —— 即 DSP 的 RESET/使能脚 |
| 复位脉冲候选 | **GPIO19**（固件里「拉低 10 ms → 拉高」那个脉冲，时序与复位脚吻合） |

一份**镜像清单**位于 DROM `0x3f4322d8`：
`{mode = 0x01, 0x00080200, 0x00083040}`，紧邻还有 `0x00010E73`（= 69235，疑似长度）。

### 4.4 未确定的一点（诚实说明）

**DSP 固件镜像本体存在哪里，我没能确证。** 我按「app 偏移 0x80200 / 0x83040」去找，
对应到 DROM 虚拟地址 `0x3f480200` / `0x3f483040`，那片数据确实是 16 位流
（熵 6.6–6.7，形态像 DSP 指令/系数表），**但没有任何代码以指针方式引用这两个地址**，
所以不能认定。

三种可能，按可能性排序：

1. 镜像在**某个不透明分区**里 —— 别忘了 `Dram0`(128 KB) / `Dram1`(112 KB) 至今是
   高熵、无已知文件头、无法解开的谜（见 `DEVICE-ANALYSIS.md` 第 7 节）。**它们很可能就是
   DSP 的固件/配置镜像**（可能被厂商做了混淆或压缩）。
2. 设备其实走 **Auto Boot**：ZL38063 自己的从属 SPI Flash 里已经有固件，ESP32 只做
   寄存器级配置（音量/模式/静音）—— 那么 ESP32 里就不该有镜像。
3. `0x00080200`/`0x00083040` 是 **DSP 侧的地址或参数**，不是 ESP32 的地址。

---

## 5. 这台设备完整的音频链路

```
【上行 · 录音】
  3 个麦克风
      ↓ (I2S/TDM)
  ZL38063 ：AEC 回声消除 + 降噪 + 波束成形 + 远场增强
      ↓ (I2S，干净的单路语音)
  ESP32 ：Opus 编码
      ↓ WiFi
  图灵云 ASR（smartdevice.ai.tuling123.com）

【下行 · 播放】
  云端 TTS / 本地音频（flashTone 里 74 个 MP3）
      ↓
  ESP32 → I2S → ZL38063 cross-point switch → DAC / 功放 → 喇叭
```

支撑证据：`CONFIG_CODEC_CHIP_IS_MICROSEMI`、`opus_encoder.c`（`esp-audio-app`）、
工厂测试项 `Mic 1: / Mic 2: / Mic 3:`、`DSP/zl38063/vproc_common.c`、
以及 `enable_vad_flag / voice_work_mode / wakeup_flag` 这些运行时日志字段。

**所以 ZL38063 在这台设备里的定位是：麦克风阵列的前端"语音净化器"** ——
它不做识别，只负责让 ESP32 拿到干净、无回声、指向性良好的语音，再送去云端。

---

## 6. 下一步怎么验证（都可以用万用表/逻辑分析仪做）

1. **SPI HBI 四根线**（SCLK/MOSI/MISO/CS）→ 量到 ESP32 的哪几个 GPIO。
   用逻辑分析仪挂上开机：如果看到 **几百 KB 级别的连续 400 kHz 数据流**，
   就是 Host Boot（ESP32 在灌固件）；如果只有零星几笔寄存器写，就是 Auto Boot。
2. **HINT 脚** → 中断线，接到 ESP32 的某个 GPIO。
3. **RESET 脚** → 确认是否就是 GPIO19。
4. **I2S 四根线**（BCLK/LRCLK/DIN/DOUT）→ 音频总线走向。
5. 如果确认是 Host Boot，就顺着镜像清单去找固件在哪个分区
   —— 优先怀疑 `Dram0`/`Dram1`。

---

## 附：顺手可用的工具

`tools\pdf_text.py` 是这次写的**纯 Python PDF 文本提取器**（系统里没有 pypdf/pdfplumber，
也没有 LibreOffice，所以自己写了一个）。用法：

```powershell
$py = '<user-home>\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
& $py .\tools\pdf_text.py 'D:\workspace\元器件文档\ZL38063.pdf' .\docs\ZL38063.txt
```

同一个目录下还有 **`AW9106B.pdf`** 和 **`SC7A20HTR 规格书`** —— 用它可以回验我前面关于
AW9106 地址 0x5B 和 SC7A20 WHO_AM_I=0x11 的结论（要不要我顺手做掉？）。
