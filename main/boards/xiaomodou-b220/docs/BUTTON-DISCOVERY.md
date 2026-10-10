# B220 按键定位固件操作手册

> 目标：找出「录音 / 音量+ / 音量−」三个实体按键到底接在哪颗芯片的哪个引脚/寄存器上。
> 现状：**接线未知**。原厂固件的按键引脚来自运行时配置（不是镜像立即数），原厂给 ZL38063
> 灌的 1042 条配置记录把疑似的 GPIO 寄存器区（`0x0700~0x0A22`）**全部写成 0**，ESP32 侧本移植
> 已占用 LCD / I2S / HBI / I2C / DSP 复位 —— 只剩 `20 / 36 / 37 / 38 / 39`。
> 相关：`b220_button_discovery.c` / `.h`、`ZL38063-GPIO-HOST-ACCESS.md`（DSP GPIO 的公开资料调研）。

---

## 0. 结论（**已完成** —— 这一节是最终答案）

按键**已经定案**，正式驱动已接入板级（`B220_BUTTON_DISCOVERY = 0`）。

### 0.1 硬件事实

三个键**不接 ESP32 GPIO、不走 AW9106B**，而是**经 ZL38063（音频 DSP）扩展**：原厂
`buttonTask`（`0x40124d4c`）从两个 HBI 寄存器取值，走的就是本移植给 DSP 用的那条 SPI 链路。

| 寄存器 | 空闲 | 按下时 | 它到底是什么 |
| --- | --- | --- | --- |
| HBI `0x0002`（key） | `0x8400` | **`0x0008` / `0x000A` / `0x000B`** | **哪一个键**（每个键一个固定值） |
| HBI `0x0000`（flag） | `0x0000` | `0x0003`（按住中）/ `0x0002`（已松开） | **按键过程**（不是键；**不能**用它划分一次按压） |

一次按压 = 两个寄存器**同时**出现一个几毫秒~几十毫秒的脉冲，10 ms 轮询能稳定抓到。

### 0.2 三个键的映射（实测）

"一次只按一个键"跑出来的三组事件（每组一次长按 + 一次短按）：

| 日志时间 | `status`(0x0002) | 判定 |
| --- | --- | --- |
| `t=+16.4 s`、`+26.6 s` | `0x000B` | **录音键** |
| `t=+36.9 s`、`+48.3 s` | `0x0008` | **音量+** |
| `t=+59.6 s`、`+70.8 s` | `0x000A` | **音量−** |

对应 `config.h`：

```c
#define B220_BUTTON_KEY_1   0x000B   /* 录音键 */
#define B220_BUTTON_KEY_2   0x0008   /* 音量+ */
#define B220_BUTTON_KEY_3   0x000A   /* 音量− */
```

> ⚠️ 置信度说明：寄存器角色与"每个键一个固定值"是**实测高置信**；
> "哪个值 = 哪个键"来自**按键顺序**推断（日志里三组事件的先后与操作顺序一致），
> 属**中高置信**。若上机后发现某个键功能不对，只需交换 `config.h` 里这三个值 ——
> 驱动会把每次动作的原始 `status` 打进日志，一眼能看出错配。

### 0.3 顺带排除掉的假设（都已实测否定）

| 假设 | 结论 | 证据 |
| --- | --- | --- |
| 按键接在 ESP32 的 20/36/37/38/39 | ❌ 否定 | 这四只脚里 37 恒 0、38/39 的翻转总是**跟在** HBI 读之后（是探测流量串到只输入脚上的串扰），与 `BTN` 脉冲**不重合** |
| 按键接在 AW9106B（0x5B）的 GPIO 扩展 | ❌ 否定 | `0x00`/`0x01` 输入寄存器在整个过程里**从未变化** |
| 按键走 SC7A20 的 CLICK/INT 引擎 | ❌ 否定 | 相关寄存器无对应变化 |
| 按键挂在底座板走 UART | ❌ 不必考虑 | 板上就能读到 |
| 粗扫里的 `0x00DC`/`0x00D0`/`0x0092` 等大幅跳变是按键 | ❌ 否定 | 那些是 HBI **命令字/参数寄存器**被回读的残影，与 `BTN` 脉冲无时间相关 |

### 0.3b 关键补充：这是**脉冲/事件**接口，不是电平接口（上机踩过）

第一版正式驱动把 `key != 0x8400` 当成"按钮按着"，结果日志里**一次按 3 秒也报 `held 9 ms`**，
长按永远触发不了，而且**按下和松开各触发一次**功能。原因（实测）：

```
一次物理按压  ->  只在 HBI 0x0002 上产生一个很短的脉冲，寄存器随后自己回到 0x8400
松开          ->  又产生一个同键值的脉冲
```

所以正确模型是：

| 做法 | 结果 |
| --- | --- |
| `key != 0x8400` 视为"按住" | ❌ 一次按压在下一个 poll 就"结束"（10 ms），松开再算第二次 → 双触发 + 长按失效 |
| **脉冲检测 + 抑制窗口** | ✅ 收到脉冲就"打开一次按压"；只要还在收到脉冲就持续延长；停止收脉冲满 `B220_BUTTON_PRESS_WINDOW_MS`（默认 **50 ms**）后关闭，按窗口长度分短按/长按 |

这意味着：
- **按住按钮**时寄存器会持续给脉冲（所以按住 = 多个脉冲串），时长能累加到 2000 ms 以上 → 长按可用；
- **松开产生的第二个脉冲**落在同一个 50 ms 窗口里，被并进同一次按压 → 不再双触发；
- 抖动也被同一个窗口吸收。
- 窗口别调太大：它必须**明显小于两次有意连按的间隔**（遥控式连按约 100 ms 一次）。

驱动日志现在会把整个脉冲串报出来，便于调窗口：

```
W (…) b220-btn: key VOLUME_UP (short press, held 62 ms, 3 pulse(s), largest gap 20 ms): key(0x0002)=0x0008 flag(0x0000)=0x0000
W (…) b220-btn: key RECORD (LONG press, held 2140 ms, 100 pulse(s), largest gap 21 ms): key(0x0002)=0x000B flag(0x0000)=0x0003
```

### 0.4 正式驱动的行为

[`b220_buttons.c`](b220_buttons.c)：10 ms 轮询 `zl38063_hbi_read(0x0000/0x0002)`，
`status`=三个键之一 → 判键，`code`=`0x0003` 持续 ≥ 2000 ms → 长按。

| 键 | 短按 | 长按 |
| --- | --- | --- |
| 录音 | `ToggleChatState()` | 同左 |
| 音量+ | 音量 +10 | 直接 100 |
| 音量− | 音量 −10 | 直接 0 |

日志 tag `b220-btn`。**全程只读 DSP**（不写 `reg 0x0014`、不复位、不重灌固件）。

> ⚠️ 音量键目前只是"存值"：`SetOutputVolume()` 只写 NVS + UI，真正的增益在 DSP 内部
> （`reg 0x0238/0x023A`），运行期写法尚未验证（PART 3 §1.6）。这是独立的一件事。

---

## 1. 定位构建怎么开（保留备用）

> 下面这套流程**已经用过并得到上面的结论**。以后要再查别的寄存器/按键时按它重来。
>
> ### ⭐ 优先看 `BTN` 行
>
> 本固件**额外**以 10 ms 周期单独轮询 `HBI 0x0002`（status）与 `HBI 0x0000`（code），
> 每次变化打一行 `BTN t=+N ms: ...`。这一路的信息量远大于全寄存器粗扫。

`config.h`：

```c
#define B220_BUTTON_DISCOVERY      1     /* 0 = 生产值，整个探测任务编译掉 */
#define B220_BUTTON_DSP_PAGES      0x40  /* 深扫页数：0x40 页 × 128 字 = 8192 个地址 */
#define B220_BUTTON_DSP_LIVE_PAGES 4     /* 快扫页数：0..3（含全部系统/邮箱寄存器） */
#define B220_BUTTON_FAST_PERIOD_MS 300   /* 快扫周期 */
#define B220_BUTTON_DEEP_PERIOD_MS 30000 /* 深扫周期 */
#define B220_BUTTON_TOP_PERIOD_MS  30000 /* 排行榜周期 */
#define B220_BUTTON_BTN_PERIOD_MS  10    /* 单独轮询 HBI 0x0000 / 0x0002 的周期（对齐原厂） */
#define B220_BUTTON_PROBE_GPIO0    0     /* 1 = 也采 GPIO0（DSP 片选 + 启动 strapping 脚） */
```

构建 + 刷机（与平时一样）：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd <repo>
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi
cd <workspace>
.\flash-b220.ps1                 # 不带 -FullErase 时保留 WiFi 凭据
.\capture-serial.ps1 -Seconds 180 -Watch
```

> 抓日志前先关掉其它占用 COM25 的工具。设备运行时**只靠 TTL 供电不够**，必须靠玩具电池。

---

## 2. 这台固件做了什么（以及它**不**做什么）

每个源都只是**读**，所以可以放心刷在一台正常工作的机器上：

| 源 | 采样内容 | 为什么 |
| --- | --- | --- |
| **ZL38063 按键寄存器** | **`0x0000`（status）与 `0x0002`（事件/按键码）**，**每 10 ms** 一次，变化就打 `BTN` 行 | **原厂 buttonTask 的唯一输入**（见上面方框），信息量最大 |
| ZL38063（HBI）全空间 | 快扫 pages 0~3、深扫 pages 0~0x3F | GPIO Read 地址未公开，只能整片扫（兜底） |
| ESP32 | GPIO 20 / 36 / 37 / 38 / 39（可选 0），每个脚读两次：**内部上拉** 与 **无上下拉** | 接 GND 的键读 `0/0`；接 3V3 的键读 `1/1`；悬空读 `1` + 泄漏值 |
| AW9106B @0x5B | `0x00` `0x01`（输入寄存器）、`0x02` `0x03` `0x04` `0x05` `0x11` `0x12` `0x13`（上下文） | 它是 I2C 上的 GPIO 扩展，GPIO 组可以配成输入（原厂没用，但值得排除） |
| SC7A20 @0x19 | `0x39` CLICK_SRC、`0x38` CLICK_CFG、`0x3A` CLICK_THS、`0x31` INT1_SRC、`0x27` STATUS | 加速度计的敲击/中断引擎也可能被拿去接键 |

**绝不做的四件事**（都是"会改变被测量对象"的操作）：

1. **不写 DSP `reg 0x0014`** —— 写它会**静默把正在运行的应用打回 boot ROM**（PART 2 §8.1）；
2. **不发 GPIO19 复位脉冲、不重灌固件、不复位**；
3. **不写 page-255 窗口基址 `reg 0x000C`**；
4. **不写 AW9106B `reg 0x02`** —— AW8737S 的 SHDN 一个 boot 只允许**一个**上升沿。

另外 `AW9106B` / `SC7A20` 两个 I2C 设备句柄是本模块自己挂上去的**只读从设备**；总线
（SDA 18 / SCL 23）仍由 `InitializeBacklight()` 拥有，本模块只借用句柄。

---

## 3. 操作流程（照着做）

```
上电 ──► 等 3 s  ──► 基线采集（含深扫 8192 个地址）
                     └─► 日志出现：baseline captured - you may start pressing the keys now
        ──► 静置 30 s，不要碰机器（这时日志应该基本安静）
        ──► 【录音键】短按 1 下 → 等 10 s → 再按住 2 s 松开 → 等 10 s
        ──► 【音量+】同上：短按 1 下 → 等 10 s → 按住 2 s → 等 10 s
        ──► 【音量−】同上
        ──► 读日志里的 BTN 行，把每个键出现的 code 值记下来
```

**一次只按一个键**是唯一要紧的纪律 —— 否则日志里只有三个 `code` 值，无法知道哪个属于哪个键。
每个键"短按 + 长按"各来一次，是为了顺便看出 `status` 的 `0x0002` / `0x0003` 是不是"短按/长按"之分。

### 3.1 开机段应该长这样

```
W (…​) btn-disc: ================= BUTTON DISCOVERY =================
W (…​) btn-disc: Nothing is written to any device - every source is only READ.
W (…​) btn-disc: Sources sampled: ESP32 GPIOs 20/36/37/38/39, AW9106B 0x5B, SC7A20 0x19, ZL38063 HBI pages 0..3 (fast) / 0..63 (deep)
W (…​) btn-disc: PLUS, at the stock firmware's own 10 ms cadence, the two ZL38063 registers the stock buttonTask reads: 0x0002 (status) and 0x0000 (event/key code).  Lines tagged 'BTN' are those.
W (…​) btn-disc: Reverse engineering says the keys reach the ESP32 through the ZL38063, NOT through an ESP32 GPIO - so if only one source reacts, expect it to be 'BTN'.
I (…​) btn-disc: GPIO20: idle baseline pullup=1 nopull=1 (GND-key reads 0/0, 3V3-key reads 1/1, floating reads 1 then whatever leaks)
I (…​) btn-disc: AW9106B 0x5B attached read-only
I (…​) btn-disc: SC7A20 0x19 attached read-only
W (…​) btn-disc: BTN idle value: status(0x0002) = 0x8400, code(0x0000) = 0x8400 - this is the 'nothing pressed' pair. ...
I (…​) btn-disc: deep baseline sweep: pages 0..63 (8192 addresses) - this takes a moment
I (…​) btn-disc: sweep: 8192 address(es) read in NNN ms, M answered non-0x0000/0xFFFF, 0 changed >= 64, 0 read error(s)
W (…​) btn-disc: baseline captured - you may start pressing the keys now
I (…​) btn-disc: DSP health (read-only): app-status 0x0000 (never meaningful here), host-cmd 0x0000 (0 = idle), cmd-result 0x0000 (0xD3D3 = stuck in boot ROM), route-gate 0x0200 (0x0200 = application READY), host-flags 0xFFE0
```

**先看这三条**：

| 行 | 含义 |
| --- | --- |
| `route-gate 0x0200` | DSP 应用在跑（唯一可信的就绪判据）。**不是 0x0200** ⇒ DSP 那一半的扫描结果不能信 |
| `cmd-result 0xD3D3` | DSP 卡在 boot ROM：任何按键状态都不会被采样，先修 host boot |
| `M answered non-0x0000/0xFFFF` | `M` 很大（几十~上百）说明 HBI 读通道是活的；`M = 0` 说明整片都没应答，先查 HBI |

### 3.2 按键时应该看到什么

**首选看 `BTN` 行**（10 ms 轮询那两个寄存器，时间戳直接告诉你按下的先后）。首次上机实测的样子：

```
W (…​) btn-disc: BTN idle value: status(0x0002) = 0x8400, code(0x0000) = 0x8400 - this is the 'nothing pressed' pair. ...
W (…​) btn-disc: BTN t=+12698 ms: status(0x0002) 0x8400 -> 0x0003 | code(0x0000) 0x8400 -> 0x000B  [stock branch: GPIO_BTN]
W (…​) btn-disc: BTN t=+12713 ms: status(0x0002) 0x0003 -> 0x8400 | code(0x0000) 0x000B -> 0x8400
W (…​) btn-disc: BTN t=+13003 ms: status(0x0002) 0x8400 -> 0x0002 | code(0x0000) 0x8400 -> 0x000B
W (…​) btn-disc: BTN t=+13013 ms: status(0x0002) 0x0002 -> 0x8400 | code(0x0000) 0x000B -> 0x8400
```

读法：**一次按压 = 一次 `0x8400 → 小值 → 0x8400` 的脉冲**（约 10~15 ms），
`status` 与 `code` 同时变。所以「哪个键」就是「哪个 `code` 值反复出现」。

> ⚠️ 只看到 `code` 是 `0x0008 / 0x000A / 0x000B` 这三个值，说明**三个键各自对应其中之一**，
> 但**光看日志无法知道哪个是哪个** —— 必须"一次只按一个键"，这就是 §3.3 第一行的做法。
>
> ⚠️ 同一段日志里 `GPIO36..39` 也会跟着翻转（`CHANGE esp32-gpio: GPIO39 pullup 0 -> 1 ...`）。
> 那是**探头自己产生的 HBI 流量串到只输入脚上的串扰**（这几只脚没有内部上拉，读数本来就不硬），
> 不是按键。判断依据：它们的变化总跟在 `BTN` 脉冲之后、而且几个脚一起动。

其余源的变化长这样：

```
W (…​) btn-disc: CHANGE esp32-gpio: GPIO39 pullup 0 -> 1, nopull 0 -> 1 [NOTE: ... treat with suspicion ...]
W (…​) btn-disc: CHANGE aw9106: reg 0x00 (0x5B) 0x00 -> 0x04
W (…​) btn-disc: CHANGE zl38063-hbi: reg 0x00DC 0xB6E2 -> 0x00C2 (delta -46624)
```

（粗扫里 `0x00DC`、`0x0002` 之类的大幅跳变是 HBI **命令字本身**的回读假象，见 §3.3 最后一行。）

每 30 s 一次排行榜（谁动得最多）：

```
I (…​) btn-disc: ----- input-source report (after 600 s) -----
W (…​) btn-disc: TOP: 3 input source(s) moved since the baseline, largest first:
W (…​) btn-disc: TOP  1/3: zl38063-status-reg0x0002                 0x8400 -> 0x0003 (delta 33789)
W (…​) btn-disc: TOP  2/3: zl38063-code-reg0x0000                   0x8400 -> 0x000B (delta 33787)
```

### 3.3 判定表

| 现象 | 结论 | 下一步 |
| --- | --- | --- |
| **`BTN` 行随按压出现脉冲**（`0x8400 → 小值 → 0x8400`） | ✅ **已经证实**：按键就在 ZL38063，`status`=`0x0002`、`code`=`0x0000` | 做这一步：**一次只按一个键**（每个键按 1 短 + 1 长，间隔 10 s），把三个键各自的 `code` 值记下来 → 我据此写正式驱动 |
| 短按、长按、连按产生的 `code` 不同 | `status` 的 `0x0002`/`0x0003` 很可能就是"短按/长按"，`code` 是"哪个键" | 三条 `code` 各自固定对应一个键，`status` 表达按下/长按 —— 用日志验证 |
| 某个 **ESP32 GPIO** 在恰好三次按压中各变一次 | 键接在 ESP32 上（与原厂逆向矛盾，属意外） | 记下脚号 + 按下电平（`0/0` = 接 GND；`1/1` = 接 3V3），用现成的 `Button` 类 |
| 某个 **AW9106B 寄存器**（很可能是 `0x00`/`0x01`）随按压变化，且 `bit n` 分别对应三个键 | 键接在 AW9106B 的 GPIO 扩展上 | 记下"寄存器 + bit ↔ 按键"映射，写 I2C 轮询驱动 |
| **`BTN` 行完全不动** | `BTN` 的读没通（看 `DSP health` 的 `route-gate` 是否 `0x0200`），或按键真在别处 | 见 §4 |
| 粗扫里一堆地址每次都在跳（`0x00DC`/`0x00A4`/`0x0300`…） | 那是 HBI 的**命令字/参数寄存器**在回读时留下的残影，不是按键 | 忽略；以 `BTN` 行为准（它只读 1 个字，不产生这种残影） |

---

## 4. 全都扫不到怎么办

按优先级排查：

1. **先怀疑硬件**：ZL38063 的 GPIO 复位后是 **输入 + 内部下拉**（datasheet Table 15 / §8.2）。
   ⇒ 接在 DSP GPIO 上的按键**必须另一端接 3V3**，接 GND 的键按下和松开都读 0，**物理上不可读**。
   用万用表量一下三个键的另一端到底是 GND 还是 3V3。
2. **确认读通道是活的**：`sweep: ... M answered ...` 里 `M` 不能是 0，且 `dsp_health_check()` 要
   打印出 `route-gate 0x0200`。
3. **把 `B220_BUTTON_PROBE_GPIO0` 打开**再扫一轮（万一按键正好在 GPIO0 上）。
4. **盯 GPIO38**：`zl38063/BRINGUP-ABC-REVERSE.md` 判定 ESP32 的 pin 38 是"HINT 风格输入线"，
   如果 ZL38063 的 HINT 接到了按键而不是 GPIO，`GPIO38` 的 `nopull` 读数会随按压变化。
   （但原厂固件既不用 HINT 也不轮询 HBI，所以这条路没有原厂参照。）
5. **换一个方向**：按键也可能挂在**底座板**上（电机/头尾灯都在底座，走 UART `{AT+...}`）。
   如果是这样，ESP32 只能通过 UART 问底座要按键状态 —— 那就需要先在 UART 上抓到
   "按下录音键"时到底座发来的报文。这一步需要逻辑分析仪或底座 UART 抓包。
6. **最后一条**：`ZL38063-GPIO-HOST-ACCESS.md` 记录了唯一还有希望的公开来源
   （《ZLS38100 SDK User Guide》，含 "Events Mask" 表，但站点有 Cloudflare 保护），
   以及 MiTuner 的 `.cr2` 配置文件里可能带寄存器名↔地址映射。

---

## 5. 定位完成之后

> **当前进度**：寄存器与脉冲形态**已经上机确认** —— `status` = HBI `0x0002`、`code` = HBI `0x0000`，
> 空闲 `0x8400`，按下瞬间 `status` `0x0002`/`0x0003` + `code` `0x0008`/`0x000A`/`0x000B`。
> **只差"哪个码 = 哪个键"**，量到之后填三个宏即可。
> 正式驱动已经写好并接进板级：[b220_buttons.c](b220_buttons.c) / [b220_buttons.h](b220_buttons.h)。

1. 把一次只按一个键量到的三个码填进 `config.h`：

   ```c
   #define B220_BUTTON_KEY_1   0x000B   /* 录音键 */
   #define B220_BUTTON_KEY_2   0x0008   /* 音量+ */
   #define B220_BUTTON_KEY_3   0x000A   /* 音量− */
   ```

2. 把 `B220_BUTTON_DISCOVERY` 改回 **0**：诊断任务整个编译掉，正式驱动接管。
   两者**互斥**（诊断开着时正式驱动不编译，反之亦然），所以不会有两个东西同时读那几个寄存器。
3. 重新构建刷机。正式驱动的行为：

   ```
   每 10 ms：code = zl38063_hbi_read(0x0000);  status = zl38063_hbi_read(0x0002);
             空闲 0x8400 → 检测到脉冲 → 去抖 → 分类
   短按（< 2000 ms）：录音键 = ToggleChatState()；音量± = 每按一次 ±10
   长按（≥ 2000 ms）：录音键 = 同上；音量± = 直接跳到 100 / 0
   ```

   对应日志（tag `b220-btn`）：

   ```
   I (…) b220-btn: ZL38063 key poller up: reading HBI 0x0000 (code) and 0x0002 (status) every 10 ms, long press at 2000 ms (read-only; the running DSP application is not disturbed)
   W (…) b220-btn: key RECORD (short press): code(0x0000)=0x000B status(0x0002)=0x0002
   I (…) XiaoMoDouB220: key RECORD (short): ToggleChatState() requested (state idle)
   W (…) b220-btn: key VOLUME_UP (short press): code(0x0000)=0x0008 status(0x0002)=0x0002
   I (…) XiaoMoDouB220: key VOLUME_UP (short): volume 70 -> 80 (stored; this codec does not apply it - the gain lives inside the DSP)
   ```

   **映射还没量到之前**（三个宏都是 0）驱动也是安全的：每次按压都会打印原始 `code`，但**不执行任何动作**，
   并提示去跑诊断 —— 不会出现"按音量键结果开了对话"这种错配。

4. ⚠️ **音量键目前只是"存值"**：`Zl38063AudioCodec::SetOutputVolume()` 按设计只写 NVS 与 UI 指示，
   真正的增益在 DSP 内部（`reg 0x0238/0x023A`），本移植还没有验证过的运行期写法（PART 3 §1.6）。
   要让音量真的变化，需要单独确认 `zl38063_dsp_set_volume()` 这条路的档位表 ——
   **这是独立的一件事，不要和按键混在一起改**。

5. 把结论与证据写进文档：`README.md` 的按键章节、`config.h` 的宏注释，以及
   `PORTING-GUIDE-PART3.md` 的新一节 —— 按本项目一贯的规矩：**结论 + 证据 + 置信度 + 已排除的假设**。
