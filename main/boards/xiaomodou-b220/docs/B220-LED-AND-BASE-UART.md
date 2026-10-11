# LED 指示灯与底座通信：实现说明与待上机确认项

对照对象：[FlowerCat-chen/Flower_modou](https://github.com/FlowerCat-chen/Flower_modou)（`ref/Flower_modou/`），
同硬件开源适配工程。本轮按它补齐了两块之前完全没有的东西：**板载指示灯**和**底座（电机/头尾灯）通信**。

> 本轮**没有真机调试**（电脑未接开发板）。下面每条都标了"已核对什么 / 还需要上机确认什么"。

---

## 1. 指示灯（AW9106B 0x5B）—— 新增 `b220_led.c/h`

### 1.1 这个芯片上到底挂了几路什么

| 寄存器 | 本机（B220） | Flower_modou 的机器 |
| --- | --- | --- |
| `0x20` `0x21` `0x22` | **LCD 背光**（`InitializeBacklight()` 写 `0x3F`；实测"六路走查时屏亮"就是这三路） | 顶部 RGB 灯（G/R/B） |
| `0x23` `0x24` | 指示灯（工厂回放留在 `0x80`，之前**从来没有代码再碰过**） | 同两路（`0x23/0x24 ← 0x80`） |
| `0x25` | 本移植未用 | 初始化时清 0 |

> ⚠️ **关键差异**：参考项目把 `0x20/0x21/0x22` 当 RGB 灯，而在本机上这三路是背光
> （把 `0x3F` 写进去屏才亮）。所以**不能照抄它的颜色映射**，否则会把背光当成彩灯拉来拉去。
> 本轮的处理：`b220_led.c` **只驱动 `0x23/0x24`**，背光三路只提供"调亮度"接口、绝不参与灯效。

#### ❌ 上机实测发现：`0x23`/`0x24` **不能拉到 0x80 以下**（会关屏）

第一版 `b220_led.c` 把 `0x23/0x24` 当普通指示灯：待机写 `0x00`、按键时闪 `0x30 → 0x00`。
上机现象：**一按按键屏幕闪一下、随后黑屏**。原因就是这两路里有**至少一路接的是 LCD 的
电源/背光**（参考项目自己的备注也写着 `0x23/0x24 相关 LCD power/backlight 控制`），
把它拉到 `0x80` 以下屏就黑了。

**已改为**：`0x23/0x24` 的**下限锁死在 `0x80`**（= 工厂回放后屏正常工作时它们本来就有的值），
灯效只能在这个基线之上变化，绝不写更低的值；开机也不再"待机熄灭"。

**上机待办（重要）**：`0x23`/`0x24` 到底是"LCD 电源"还是"指示灯"，以及本机是否**根本没有**
可安全驱动的独立指示灯 —— 需要逐路点灯走查确认。在确认之前，灯效的可见效果可能不明显
（因为下限被锁住），但**不会再关屏**。

### 1.2 已实现

- `b220_led_start()`：在板级已建好的 I2C0 上挂一个**自己的** 0x5B 设备句柄（不创建/不释放总线，
  与板级的设备句柄互不干扰），把指示灯设为**待机熄灭**（参考实现的省电选择），并起一个 20 ms 的
  灯效任务。
- `b220_led_blink_once()`：两下短闪（`0x30`，60 ms 亮 / 70 ms 灭）。**非阻塞**，所以从按键回调里调是安全的。
- `b220_led_set_indicator()/set_both()/set_backlight()/get_backlight()/get_state()`。
- 按键动作会闪一下灯（`B220_ButtonAction()` 里）—— 这是这块玩具在没有屏幕时的唯一反馈。

### 1.3 安全边界（必须守住）

AW8737S 的 SHDN 是 `reg 0x02` 的 bit1，**靠上升沿计数选模式，一个 boot 只允许一个沿**（PART 2 §9）。
所以 `b220_led.c` **只写 `0x20..0x25`**，永不触碰 `0x02/0x04/0x12`（那三个由板级的 PA 使能独占并锁存）。
代码里有注释明写这条，改动前请先读 `b220_led.h` 的头注释。

### 1.4 还需要上机确认

| 待确认 | 怎么确认 |
| --- | --- |
| **`0x23`/`0x24` 是不是 LCD 电源/背光**（已实测"写到 0x00 会关屏"） | 用 `b220_led_set_indicator()` 逐路从 `0x80` 往下试（**会关屏，但重启即恢复**），或直接量屏供电/背光脚的通断 |
| `0x23`/`0x24` 各自对应哪颗实体灯 | 上机后 `b220_led_set_indicator(A, 0x3F)` / `(B, 0x3F)` 单独点，肉眼记录 |
| 本机是否也有"顶部 RGB"（挂在别的通道或别的芯片） | 参考项目说顶部 RGB 是 `0x20/0x21/0x22`，但本机那三路是背光 ⇒ 本机的顶部灯要么不存在，要么在别处。需要点灯走查一遍全部 6 路 |
| 待机熄灭是否合适 | 现在 `s_indicator_idle = 0x00`（参考实现同款省电策略）。若希望待机指示可视，改 `b220_led.h` 的 idle 值 |
| 亮度上限 | 跟参考实现一致用 `0x3F`（= `MODOU_KEY_LIGHTS_MAX_BRIGHTNESS`） |

### 1.5 有意**没有**照抄的部分：AW9106 初始化序列

参考项目的 `apply_factory_aw9106_led_init()`：

```
7F←00, 11←10, 12←02, 04←00, 02←00, 13←00,
20←00, 21←00, 22←00, 24←00, 24←00, 25←00,
14←07, 05←07, 17←01, 18←01, 19←01,
15←1B, 16←08,
23←80, 24←80,
02←02,          <-- 功放使能（唯一上升沿）
14←00, 11←12    <-- 使能之后的两条
```

与我们的 `InitializeBacklight()` 对比，**差异只有三处**：

| 差异 | 我们的值 | 参考值 | 是否照抄 |
| --- | --- | --- | --- |
| `0x15` | 不写 | `0x1B` | ❌ 本轮不抄（见下） |
| `0x16` | 不写 | `0x08` | ❌ 本轮不抄 |
| 末尾 | 停在 `0x11 ← 0x1B` | `0x14←00, 11←12` | ❌ 本轮不抄 |

**为什么不抄**：`0x11`/`0x14` 落在那组 **0x1B/0x12 的模数校准寄存器**里（0x11–0x19），
而我们的背光是靠 `0x11 ← 0x1B` + `0x12 ← 0x02` 这个组合亮起来的（实测确认）。
参考机的背光在别的通道，两者的校准点**不保证相同** —— 在不能上机验证的情况下改这三条，
风险是"屏不亮"或"背光亮度变化"，收益却只是"和参考项目更像"。所以本轮**只记录、不改**。

想试的话，改动点很集中：`xiaomodou_b220.cc` 的 `InitializeBacklight()` 的 `init_seq` 末尾加
`{0x15,0x1B},{0x16,0x08}`，并在 PA 使能之后补 `0x14←0x00, 0x11←0x12`。
**回滚方式**：删掉这三条即可（当前版本就是基线）。

---

## 2. 底座通信（UART0）—— 新增 `b220_base_uart.c/h`

### 2.1 总线与协议（已核对）

| 项 | 值 | 依据 |
| --- | --- | --- |
| 端口 / 引脚 | **UART0，TX GPIO1 / RX GPIO3** | 与移植手册一致；Flower_modou `modou_base_uart.c` 同 |
| 参数 | 115200 8N1 | 同 |
| 帧格式 | `{AT+...}\r\n`（大括号 + CRLF） | Flower_modou `modou_base_uart.c: snprintf(frame, "{%s}\r\n", cmd)`，其底座测试页在真机可用 |
| 已知可用命令 | `AT+Ver`（有回包）、`AT+Pon`（唤醒并解锁电机）、`AT+Onbase`、`AT+Run`、`AT+CMD=(...)` | 同上 |

### 2.2 ⚠️ 与日志/烧录口共用一根线（这是最大的坑）

GPIO1/3 同时是**控制台日志**和**烧录口**。参考实现自己也在源码里注明"日志可能会和底座协议
混在同一物理线上，所以只做诊断探测"。两个方向的后果：

- 我们发出的 `{AT+...}` 也会出现在串口日志里（**看到是正常的，不代表有问题**）；
- 我们打印的日志也会进底座，底座对不认识的文本忽略处理（原厂固件就是这么共存的）；
- 底座的回包会进控制台输入，可能被 IDF 控制台打印成"unknown command"之类的噪声。

另外注意：**烧录时不要指望底座那侧安静** —— 用 `flash-b220.ps1` 刷机和抓日志都走同一根线。

### 2.3 已实现

- `b220_base_uart_init(bool consume_console_input)`：默认**只发不收**（`false`），不动控制台的输入；
  需要读回包时显式传 `true`（会接管 RX 缓冲）。
- `b220_base_uart_send(cmd, tx_timeout_ms)`：加 `{}` + CRLF 后发送，带互斥锁，非阻塞（只等 FIFO 排空）。
- `b220_base_uart_send_and_receive()` / `b220_base_uart_read_pending()`：读回包（读到 `\n` 为止）。
- **开机握手**：`B220_BASE_UART_HANDSHAKE = 1` 时，延迟
  `B220_BASE_UART_HANDSHAKE_DELAY_MS = 3000` ms（让 ROM 横幅和开机日志先吐完）后发一次 `AT+Pon`
  —— 这是解锁电机的关键一步（参考实现："`AT+Pon` 成功发送一次后即可解锁电机"）。
- 运动/灯光封装：
  - `b220_base_motion(mode, steer, power, duration_ms)` → `AT+CMD=(9,1,0,1),(mode,units,steer,power,0),(0,0,0,0)×4`
    （`units` 按 **200 ms 向上取整**，`steer` 钳 0..180、中点 90，`power` 钳 1..100，`power=0` 等价于停止）
  - `b220_base_stop()` → 两条停止帧（逐字取自参考项目的 `BASE_STOP_CMDS`）
  - `b220_base_lights(front, rear)` → 头/尾灯帧

运动模式语义（参考项目 `docs/MOTOR_REMOTE_PROTOCOL_2026-07-30.md`）：`0/1` 原地左右转，
`2/3` 后退/前进（左半区），`4/5` 后退右弯/前进右弯；`81..99` 归一化为 `90` 直行。

### 2.4 还需要上机确认

| 待确认 | 说明 |
| --- | --- |
| **`AT+Pon` 一发出去就报错**（已实测） | 上机日志：`E uart: uart_write_bytes(1705): uart driver error` + `write short (-1 of 10 bytes)`。原因：**控制台占用了 UART0**，`uart_write_bytes` 那条路径没有可用的 TX 缓冲。下面的 §2.5 是修法 |
| 底座是否有回包 | 需要 `b220_base_uart_send_and_receive()`（即 `init(true)`）。**先不加**到默认路径，避免抢控制台输入 |
| `AT+Pon` 之后电机是否真的解锁 | 上机时看底座是否开始回 `{AT+OK}` 之类，并试一条 `b220_base_motion(3, 90, 40, 600)` |
| 模式 0/1 与左右方向是否要互换 | 依赖装配朝向，必须实机看 |
| 头/尾灯的"颜色索引" | 参考项目用 `7,<色号>,2000,<目标1/2>`；色号 0=白 1=红 … 7=紫。我们的 `b220_base_lights()` 只做"白/红 + 目标 1/2"，其余等实测 |
| 与烧录/日志的相互干扰程度 | 上机抓一次 `AT+Pon` 前后的完整串口日志即可看清 |

---

### 2.5 上机实测：`AT+Pon` 发不出去（`uart driver error`）—— 原因与修法

```
I (13994) b220-base: baseboard handshake: sending AT+Pon (this is what unlocks the motors)
E (13994) uart: uart_write_bytes(1705): uart driver error
W (13994) b220-base: baseboard write short (-1 of 10 bytes)
```

`uart_write_bytes()` 返回 −1 并打 `uart driver error`，只有一个含义：**这个 UART 的 TX
环形缓冲不可用**。本机 UART0 被 **ESP-IDF 控制台（`CONFIG_ESP_CONSOLE_UART_DEFAULT`）独占**——
控制台走的是 ROM 的 `ets_printf` 直写路径，并没有为 `uart_write_bytes()` 准备 TX buffer，
所以这个调用必然失败。参考项目之所以能发，是因为它的工程把控制台关掉了
（`CONFIG_ESP_CONSOLE_NONE` / USB-Serial-JTAG），两者不能同时成立。

三条可选路（按推荐顺序）：

1. **把控制台让给底座（推荐）** —— `sdkconfig` 里改
   `CONFIG_ESP_CONSOLE_UART_DEFAULT=n`，改用 `CONFIG_ESP_CONSOLE_NONE=y`
   （或若硬件支持则 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`）。这样 `uart_write_bytes()`
   可用，代价是**串口日志没了**。因为本机刷机/日志都靠 UART0，建议只在"专门验证底座"的构建里这么做。
   > 注意 `flash-b220.ps1` 靠 `--after watchdog_reset` 复位，不依赖控制台，所以关掉控制台不影响刷机。
2. **不装驱动，直接用 ROM 的写接口** —— 用 `esp_rom_printf()` / `ets_printf()` 发帧。
   它绕过 `uart_write_bytes()`，在控制台启用时也能出去（日志本来就靠它）。
   代价：阻塞式、没有互斥、而且**会和日志混在同一条流上**（底座要能容忍前面那串日志）。
3. **换到别的 UART** —— 本机**没有**第二组空闲脚可用（GPIO16/17 是 PSRAM，其余都被 LCD/I2S/HBI/I2C 占了），
   所以这条路不成立；除非愿意牺牲某个外设。

**本轮的处置**：默认路径保持"发不出去就明确报错"（不静默），并在下面 §4 的待办里列出
"是否改成方案 1/2"。这样上机时一眼能看到是"没通电"还是"控制台占着"。

---

## 2.6 按键长按：**结论是硬件做不到**（三次尝试、带仪器化实测）

### 最终结论（有测量数据，不是推理）

用带仪器化的固件跑了一轮"短按 / 长按"对照，窗口每次关闭都打印"活了多久 + 为什么关 +
这期间 status 读到了什么"：

| 操作 | 窗口关闭原因 | 寿命 | status 读取统计 | GPIO38 |
| --- | --- | --- | --- | --- |
| 短按 VOL− | `no signal for the whole gap window` | 198 ms | **1 press, 15 idle** | 高 |
| 短按 VOL− | `release event (status 0x0002)` | 142 ms | 1 press, 10 idle | 高 |
| 短按 VOL+ | `release event (status 0x0002)` | 184 ms | 1 press, 14 idle | 高 |
| **长按 VOL−（4 s）** | `no signal ...` | 197 ms | **1 press, 15 idle** | 高 |
| **长按 VOL+（4 s）** | `no signal ...` | 197 ms | **1 press, 15 idle** | 高 |

⇒ **长按 4 秒和短按 0.3 秒产生的序列逐条相同**。于是可以断定：

1. **一次物理按压只报一个 `0x0003`**（带按键码），此后不再有任何事件；
2. **按住时长不可读**：`status` 在按下后立刻回到 `0x0000`；
3. **`0x0002` 不是"手指松开"**：它在按下后 140~200 ms 出现，**与真实按住时长无关**，
   更像是"这个事件已被主机消费"的应答；
4. **GPIO38 不是"按住"信号**：按住全程读作**高**（它在启动瞬间读 0 是上电残留）。

### 三次失败的经历（全部保留，避免第四次）

| 版本 | 做法 | 结果 |
| --- | --- | --- |
| v1 | `key != 0x8400` 当"按着" | 把事件当电平，`held 9 ms` |
| v2 | 用"事件间隔"维持窗口 | 前端在按住期间**不发重复事件**，窗口自灭 |
| v3 | 用 **GPIO38 低电平**当"还按着" | 该线**不跟随按键**（本次已量到：按住时读高） |
| v4 | 每 10 ms 轮询状态寄存器 | 状态**不保持**（1 press + 15 idle） ⇒ 同样不可读 |
| **定稿** | **不做长按** | 每个键一次按下 = 一次动作 |

> 过程中的两次自我纠错也记一笔：我曾根据一份日志断言"一次按压会产生两个 `0x0003`"，
> 那是**误读**（把两次不同的按键当成一次）；后来加了"窗口开启/关闭都必有日志"才发现
> 真正的问题是窗口静默关闭 —— **日志设计不当会让两种完全不同的故障长得一模一样**。

### 代码现状

- `b220_buttons.h/.c`：删掉长按判定、hold 窗口、GPIO38 电平推断与所有相关日志
  （净减代码，也没有了误导性的 `held 0 ms`）；
- 回调签名简化为 `void (b220_button_action_t)` —— 不再有永远为 `short` 的时长参数；
- 每个键：**一次按下 = 一次动作**（音量 ±`B220_BUTTON_VOLUME_STEP`）；
- `B220_BUTTON_LONGPRESS_MS` 已从 `config.h` 删除（不再有人用）。

### 如果将来还想要"音量直接到顶"

唯一诚实的做法**不是长按**，而是：

- **连按**：快速按两下 → 直接到 100 / 0（需要实现一个双击窗口，约 400 ms，可靠）；
- 或者在本地 UI / 云端用一个语音指令。

---

## 2.7 原始记录（旧版分析，保留备查）

## 2.6 按键长按：两次上机失败后的最终模型（`b220_buttons.c`）

### 失败经历（都记下来，避免第三次）

| 版本 | 做法 | 上机结果 | 为什么错 |
| --- | --- | --- | --- |
| v1 | 把 `0x0002 != 0x8400` 当"按着" | `held 9 ms`，长按无效 | 把事件当电平 |
| v2 | 用"事件间隔"维持按住窗口 | `held 0 ms`，长按无效 | 前端在按住期间**不发重复事件**，窗口在 150 ms 后自己关掉 |
| v3 | 用 **GPIO38 低电平**当"还按着" | 仍无 `holding key` 行，长按无效 | 这条线**开机就是低、且不跟随按键**（板级实测），永远给不出"松开" |
| **v4（当前）** | **每 10 ms 轮询状态寄存器**，`0x0003` 即"还按着" | 待上机 | 这才是"物理按住"的真正信号 |

### v3 失败的两个具体 bug（你的日志直接指向它们）

```
W (6024) b220-btn: GPIO38 is LOW at start-up: ...        <- 这条线不可信
W (10664) b220-btn: key VOLUME_UP (short press, held 0 ms): status=0x0003 key=0x0008
（此后 3 分钟没有任何 holding key 行；期间你按住过按键）
```

1. **把 GPIO38 当成"还按着"的唯一依据** —— 它开机为低且不跟随按键，所以 `s_hold_last_us`
   永远得不到刷新，窗口在 `B220_BUTTON_HOLD_GAP_MS` 后关掉；
2. **只在状态寄存器"发生变化"时才读它**，而且读到空闲就把按住窗口清掉 ——
   按下的下一个 poll 只要读到一次 `0x0000`，窗口就没了（约 10 ms，正是日志里的 `held 0 ms`）。

### v4 的模型

```
每 10 ms（或 GPIO38 下降沿）：读 status = HBI 0x0000
  status != 上次 且 status & 1  -> 按下：读 key，执行一次；打开/刷新按住窗口
  status != 上次 且 status==2   -> 抬起：关闭按住窗口（不执行动作）
  status == 0x0003（仍为按下）  -> 算"还按着"，刷新 s_hold_last_us   <-- v4 的关键
"还按着" = (status 当前仍读 0x0003) OR (GPIO38 为低)
两者都不成立满 B220_BUTTON_HOLD_GAP_MS（150 ms）-> 判定松开
按住满 B220_BUTTON_LONGPRESS_MS（2000 ms）-> 触发一次长按（每按一次只报一次）
```

**空闲读数不再清窗口** —— 这条是 v3 的第二个 bug 的修法：一次孤立的 `0x0000`
采样不允许打断一次真实按住。

### 上机怎么判断（日志已经把两个候选信号都打出来）

按住时会每 500 ms 打一行：

```
I b220-btn: holding key 0x000B: 1520 ms | status reads 0x0003 (press) | GPIO38 level 0 | last evidence 0 ms ago
```

- 如果**有这行**且最后出现 `LONG press` → 成功；
- 如果**有这行**但一直没到 2000 ms 就消失 → 看消失前那一行的 `status reads`：
  - 变成 `0x0000 (IDLE)` ⇒ 前端在按住期间把状态清回了空闲，那就得改成"以按下事件为准、
    用固定 2 s 计时"（一次性长按，无法区分按住时长），届时按日志调整；
  - 一直是 `0x0003 (press)` 但窗口还是关了 ⇒ 是我这里的 bug，把日志发我；
- 如果**完全没有这行** → 说明 `handle_press()` 根本没把窗口打开，或者按键事件没到，
  这时看有没有 `key ... (short press` 行来区分。

---

## 2.8 音量：单击步进 / 双击到顶到底，并且**真正改音量**（本轮新增）

### 2.8.1 双击替代长按

长按做不到（§2.6），但**双击是纯序列关系**，而这个接口对序列是可靠的（一次按下 = 一个 `0x0003`，
按住期间不再有事件）。所以：

| 操作 | 行为 |
| --- | --- |
| 单击音量± | 音量 ±`B220_BUTTON_VOLUME_STEP`（默认 10） |
| **双击同一键**（`B220_BUTTON_DOUBLE_CLICK_MS` = 400 ms 内） | 直接到 100 / 0 |
| 单击录音键 | 立即 `ToggleChatState()`（**不延迟**，否则手感很怪） |

代价说明：单击的步进**延迟 400 ms 生效**（等窗口关闭），这样双击才不会"先走一步再跳"。
若不想要这个延迟，就得接受"双击 = 先 +10 再跳到 100"，可以改 `B220_ButtonAction()`。

### 2.8.2 真正改音量：写 DSP 的 0x0238/0x023A

之前 `SetOutputVolume()` 只存 NVS + UI 不生效（PART 3 §1.6 的设计），现在改成**真的写 DSP**：

```
Zl38063AudioCodec::SetOutputVolume(vol)
    -> AudioCodec::SetOutputVolume(vol)        // 仍然存 NVS（重启保留、UI/MCP 可见）
    -> zl38063_dsp_set_volume(vol)
         -> VprocTwolfSetVolume(gain)          // gain = vol - 90 (dB)
         -> HBI 0x0238 <- gain|gain<<8 ; HBI 0x023A <- gain|gain<<8
```

依据（都是既有证据，不是新猜的）：

| 事实 | 来源 |
| --- | --- |
| 原厂播放路径调 `VprocTwolfSetVolume(tblA[vol/10])`，写 `0x238/0x23A` | PART 2 §6.3（逐指令逆向） |
| 编码 `vol 0 -> -90 dB`、`100 -> +10 dB`（DSP 在 +6 dB 饱和） | `zl38063_dsp.c` 的 `ZL38063_VOLUME_ZERO_DB` 与实测 `0x238=0xECEC`（-20 dB） |
| 这两个寄存器**运行期可写** | 原厂自己的音量/路由例程就是运行期写它们 |
| 本移植已有实现且已接进 HBI | `zl38063_dsp_set_volume()`（原本只在诊断里用） |

**新增日志**（成功与失败都明说）：

```
I Zl38063AudioCodec: SetOutputVolume(80): stored AND applied to the DSP (HBI 0x0238/0x023A, gain = vol - 90 dB)
I XiaoMoDouB220: key VOLUME_UP (single press): volume 70 -> 80
I XiaoMoDouB220: key VOLUME_UP (double press): volume 80 -> 100
W Zl38063AudioCodec: SetOutputVolume(80): stored, but the DSP write failed (ESP_ERR_INVALID_STATE) - the speaker level did not change
```

失败（DSP 没起来 / HBI 写失败）**不影响其它功能**：值照样存下来，日志说清"声音没跟着变"。

### 2.8.3 上机怎么验收

1. 单击音量+ → 应听到**声音变大**（同时日志 `stored AND applied to the DSP`）；再单击几次 → 继续变大。
2. **双击音量−** → 直接静音档（vol 0）；**双击音量+** → 到 100。
3. 单击音量+ 后立刻看日志：`key VOLUME_UP (single press): volume X -> Y`，
   如果只看到 `stored, but the DSP write failed` ⇒ 说明 DSP 那侧没起来或 HBI 写被拒，
   把日志发我（那说明 `0x0238` 在运行期被锁，需要换用配置记录里的 `0x0494/0x049A` 那条路）。

> ⚠️ 一个已知的听感注意点：`0x0238` 的**低 8 位是 Gain B、高 8 位是 Gain A**，本实现两半写同一个值
> （与原厂一致）。如果实测发现"两个声道音量不一致"，那是通道增益对不上，需要分别写 A/B 两半，
> 这一条只能靠耳朵/示波器验，日志看不出来。

---

## 2.9 上机崩溃：HBI 不是线程安全的（已修，2026-10-10）

### 现象

改音量后设备立刻重启，循环反复：

```
W (37862) b220-btn: key VOLUME_DOWN: status(0x0000)=0x0003 key(0x0002)=0x000A
I (38262) AudioCodec: Set output volume to 60
assert failed: spi_device_transmit spi_master.c:1297 (ret_trans == trans_desc)
Backtrace: ... 0x401720d1 0x400f7114 ...
Rebooting...
rst:0xc (SW_CPU_RESET)
```

### 根因：**一条 HBI 命令不是原子的**

`hbi_send_cmd()` 访问 page 0 以外的寄存器要**两个 SPI 事务**（先 `SELECT_PAGE`，再读写），
即使 page 0 也是"命令事务 + 每个数据字一个事务"。而 `zl38063_hbi.c` **没有任何锁**：

- 改音量之前，HBI 的**唯一**使用者是启动任务（boot + 诊断），单线程，所以从来没暴露；
- 加入按键轮询（10 ms 读 `0x0000/0x0002`）后仍只是多了一个读者；
- **这一轮把音量写接到 `SetOutputVolume()`，它从另一个任务（esp_timer / 音频路径）调用 HBI**
  ⇒ 两个任务的事务交错：B 任务的 `SELECT_PAGE` 插进 A 任务的"选页"和"payload"之间，
  A 的数据被写到**错误的页**，SPI 驱动随即以 `ret_trans == trans_desc` 断言中止。

这是**我自己引入的并发缺陷**：新增 HBI 调用者时没有先确认底层是否可重入。

### 修法

在 `zl38063_hbi.c` 里加**递归互斥锁**（`xSemaphoreCreateRecursiveMutex`，惰性创建），
并让**整条命令**处于锁内：

| 函数 | 锁的粒度 |
| --- | --- |
| `zl38063_hbi_read()` / `zl38063_hbi_write()` | **整条命令**：SELECT_PAGE + 命令 + 全部数据字 |
| `zl38063_hbi_wr16_cmd()` / `wr16_data()` / `rd16()` | 单次事务（可被上面的组合调用重入） |
| `zl38063_hbi_set_swap()` | 改全局字节序状态时 |

递归是必须的：`hbi_read()` 会调 `hbi_send_cmd()` → `wr16_cmd()`，同一任务要拿两次锁。
超时用 500 ms：卡死的持锁者不能把调用者拖死。

### 仍然待验

- 修复后需重新刷机确认不再重启（本轮无硬件）；
- 若在**高频连按**音量时仍有异常，说明 500 ms 超时或锁粒度还不够，把日志发我。

---

## 2.9a-pre0 亮度与音量的持久化（2026-10-11）

用户要求："修改声音和背光亮度后保存到 flash，下次启动加载。"

### 音量：**框架本来就在存，但从来没被应用**（真 bug）

`AudioCodec::Start()` 只做一件事：把 `Settings("audio"/"output_volume")` 读进
`output_volume_`。它**不调用 `SetOutputVolume()`** —— 而在这台机器上，真正把电平写进
DSP（`HBI 0x0238/0x023A`）的正是这个函数。

后果很隐蔽：**UI 和 NVS 里的音量被恢复了，喇叭的实际电平却是 boot 配置记录里那个固定值。**
看起来"设置是持久的"，实际每次开机音量都不一样（跟上次调过什么无关）。

修法：`Zl38063AudioCodec::Start()` 在 `AudioCodec::Start()` 之后调用
`SetOutputVolume(output_volume())` —— **必须调重写版**，因为写 DSP 的是它。

### 背光：实现框架的 `Backlight` 接口，而不是自建 NVS

框架已经有完整的持久化：`Backlight::SetBrightness(pct, permanent=true)` 写
`Settings("display"/"brightness")`，`Backlight::RestoreBrightness()` 读回（0..100，默认 75）。
所以本板只需要实现**寄存器换算**：`B220_Backlight::SetBrightnessImpl()` 把 0..100 映射到
`0x23/0x24` 的 0..255（四舍五入），走 AW9106B 设备句柄。

### ★★ 上机又抓到一个：两个默认值打架，屏幕被"拉下去"

第一版（已提交为 `7b554cf`）上机日志：

```
52: backlight 100% -> DIM3/DIM4 = 0xFF                    <- 我们的代码应用了板级默认 100%
53: backlight: applied the saved brightness 100% at start-up (no fade)
54: Backlight: Set brightness to 75                        <- 基类随后把它拉回它自己的默认值
56: backlight 99% -> DIM3/DIM4 = 0xFC                     <- 并且开始缓动下降，一路到 75%
```

**根因**：`Backlight::RestoreBrightness()` 自己也会读同一个 NVS 键，而它的兜底默认是 **75**。
用户从没存过亮度 ⇒ 它走兜底 ⇒ **把板级的 100% 覆盖掉，还带一次可见的缓动**。

**这属于"我调用了两次恢复"** —— 我在 `ApplySavedBrightnessNow()` 之后又调了基类的
`RestoreBrightness()`，以为它是"同步基类状态"的无害操作，实际它是**第二次恢复**，
而且用的是另一个默认值。

**修法**：只由板级方法负责恢复；并且用**哨兵默认值**区分"从没存过"和"存过"：

```cpp
static constexpr int kNeverStored = -1;                   // Settings 没有"键是否存在"接口
const int stored = settings.GetInt("brightness", kNeverStored);
if (stored 无效) { percent = 板级默认; settings.SetInt("brightness", percent); }  // 落盘一次
else            { percent = stored; }
```

之后"没存过"这种状态就不存在了，基类再也无法用它的 75% 覆盖。同时**不再调用
`Backlight::RestoreBrightness()`**（注释里写明了原因，避免以后有人"顺手加回来"）。

> **教训：同一个设置不要接两套恢复逻辑。** 两处默认值（板级 100% / 框架 75%）
> 必然打架，而且症状是"屏幕亮度不对"这种看起来像硬件问题的现象。

### 音量那条修复，上机确认有效

```
I (6483) AudioCodec: Audio codec started
I (6483) AudioCodec: Set output volume to 40         <- 保存值从 NVS 读出
I (6483) ZL38063: volume 40 -> gain 0xCE (-50 dB)    <- 真的写进了 DSP
I (6493) Zl38063AudioCodec: SetOutputVolume(40): stored AND applied to the DSP
```

修复前这里只有第一行 —— NVS 里有值、UI 显示正确，而喇叭电平是 boot 配置记录的固定值。

### ★ 这里又踩了一次同一类陷阱

我先把 `SetBrightness()` 写成派生类的 `override` —— **编译直接报错**：
`Backlight::SetBrightness()` **不是虚函数**，`RestoreBrightness()` 内部调用它永远是基类版本。
（和前面 `WifiBoard::OnNetworkEvent` 完全同一类问题：**非虚函数 + 基类内部调用 = 派生版本永不执行**。
两次都是靠"加 override 让编译器报错"或"加日志发现没打印"才发现的。）

改法：**在开局时预置 `brightness_` 到目标值**，让基类的 `SetBrightness()` 命中
`brightness_ == brightness` 的提前返回 —— **缓动定时器根本不会启动**。

**为什么必须避免那个缓动**：`Backlight::SetBrightness()` 只存目标值，真正的写入发生在 5 ms
定时器里、**从当前值逐级**进行。开机时当前值是 **0**，于是"恢复保存亮度"会先往 `0x23` 写一百个
近黑值 —— 而**低值是实测会关掉这块屏的**。用户会看到每次开机屏幕先黑、再慢慢爬亮，还要多花
两百次 I2C 写。**缓动适合"用户主动改亮度"，不适合"开机恢复设置"。**

### 背光默认值改为 100%

实测 `0xFF`（100%）最亮且无异常，所以出厂默认改为满亮度。用户在系统里改过之后，
NVS 里的值才是准；`config.h` 的 `B220_LED_BACKLIGHT_DEFAULT_PERCENT` 只是**首次开机的默认**。

---

## 2.9a-pre2 ✅ 灯光策略上机验证通过（2026-10-11 08:07，`logs/boot-20261011-080701.log`）

### 策略（用户确认的最终版）

| 设备状态 | 灯 |
| --- | --- |
| 启动 / 配网 / 连接中 | 红色**呼吸** |
| 曾连上但**断链** | 红色**常亮**（需要用户处理） |
| **聆听中** | **绿色呼吸** |
| **说话中** / 通知中 | **白色呼吸** |
| 激活中 / 升级中 | 紫色呼吸（别断电） |
| 就绪空闲 | 蓝色**微光**（12%，不是全灭） |
| 致命错误 | 红色快闪 |
| 按键反馈（最高优先） | 录音白闪 / 音量+绿闪 / 音量−红闪 |

设计要点：**一色一义**（红=连接有问题、绿=轮到你说、白=轮到你听、紫=维护、蓝=待机）；
**呼吸=进行中/等待，常亮=需要你处理，快闪=出故障**；语音类呼吸 1.6 s、非语音类 3 s
（连接可能持续十几秒，快脉动看起来像恐慌）；按键反馈永远优先，否则"没网时的红"会吞掉按键确认。

### 实测轨迹（与设计逐条一致）

```
 6326  RED breathing (no cloud yet)   unknown       开机
 8806  MAGENTA breathing              activating    激活服务器
14336  RED breathing (no cloud yet)   connecting    开始会话
14486  GREEN breathing (listening)    listening     用户说话
19046  WHITE breathing (speaking)     speaking      机器回答
29496  GREEN breathing (listening)    listening     第二轮
33096  WHITE breathing (speaking)     speaking
36126  GREEN breathing (listening)    listening
```

### ★ 这一步踩了两个"看着对、其实没接上"的坑（方法论）

**要拿到"真实链路状态"**，因为设备状态机不够用：`Application::HandleNetworkDisconnectedEvent()`
**不会改变设备状态**，"连上后掉线"若只看状态机就仍然显示待机灯。

| 尝试 | 结果 |
| --- | --- |
| ① 遮蔽 `WifiBoard::OnNetworkEvent` | ❌ **静默失效**：该方法**非虚**，而 `WifiBoard::Initialize()` 在它自己捕获的 **`WifiBoard*`** 上调用它 ⇒ **静态绑定**，派生版本永不被调用。**能编译、看着对、什么都没发生** |
| ② 重写 `SetNetworkEventCallback` 包装 Application 的回调 | ❌ 同样不可靠：`WifiBoard::Initialize()` 在**构造函数里**就把自己的 lambda 直接注册给 WifiManager，根本不经过这个虚函数 |
| ③ **直接查询 `WifiManager::IsConnected()`** | ✅ 公开 API、同步、无中间环节 |

①为什么会被发现：**我当时给这条路径加了两行日志**（`network: CONNECTED/DISCONNECTED`），
上机后日志**一行都没打印**，于是立刻定位到"分发根本没生效"，而不是去猜策略逻辑。

> **教训：不可观测的接法等于没接。** 任何"新增一条数据通路"的改动，都必须带一条能证明它被走到的日志；
> 否则现象只会表现为"灯不对/功能不生效"，而人会在错误的地方（策略逻辑）反复排查。

③的验证也用**反汇编**而不是字符串搜索：
```
400f547b: call8 <WifiManager::GetInstance()>
400f547e: call8 <WifiManager::IsConnected() const>    ← 确实在查
400f548f: call8 <b220_led_fx_active>                   ← 优先级仍在
```
（`nm -C` 一开始搜不到是因为符号是 C++ 修饰名 `_ZNK11WifiManager11IsConnectedEv`；
另外 ESP-IDF 会把日志格式串去重进格式表，**所以"在镜像里搜字符串"不是可靠的验证手段** —— 它一度让我
误判自己的实现缺失。）

### 另一处可观测性修正

"就绪空闲"那条日志原本是 `ESP_LOGD`，运行时级别把它过滤掉了 ⇒ 蓝光**工作正常但在 trace 里看不见**。
已改为 `ESP_LOGI`：这块板子唯一的用户反馈就是这盏灯，每次策略切换都值得留一行。

---

## 2.9a-pre ✅ RGB 指示灯通了（2026-10-10 晚，实测定性）

### 一句话

**六颗 RGB 并联、共阳，R/G/B 分别接 AW9106B 的 OUT0/OUT1/OUT2，对应 `0x20/0x21/0x22`。**
它们之前不亮的原因**不是"没接"**，而是缺两笔**输出驱动配置**（`0x15`/`0x16`）+ 组 A 的
**LED 驱动位**。这三样配好之后：`0x20`→红、`0x21`→绿、`0x22`→蓝、三路一起→白。

### 走通的那一组寄存器值（已实机验证）

| 寄存器 | 值 | 作用 |
| --- | --- | --- |
| `0x12` GPMD_A | **`0x0A`** | **GPIO 位(0x02) \| LED 驱动位(0x08)** —— 关键 |
| `0x15` | `0x1B` | 输出驱动配置（本移植此前**从未写过**） |
| `0x16` | `0x08` | 同上 |
| `0x11` CTL | `0x12` | 参考实现点灯时收敛到的值 |
| `0x14` EN_BRE | `0x00` | 常亮（非呼吸） |
| `0x20/0x21/0x22` | `0x00..0xFF` | R/G/B 亮度，DIMx 是 **8 位** |
| `0x23/0x24` | `0x80` | **LCD 背光**（不可低于此值） |

### `0x0A` 是这次的真正突破口

规格书 9.2 / 表 4、5：**OUTx 上电默认是 GPIO，要靠 `GPMD_A`/`GPMD_B` 切到 LED 驱动模式**。

- 本移植一直把 `0x12` 钉在 `0x02`（GPIO 位）—— 因为 AW8737S 的 SHDN 在 **组 A 的 OUT1** 上，
  而功放走查证明只有 `0x02` 能让 SHDN 动（`0x08` 会让功放没声音，所以原厂尾段的
  `{0x12, 0x08}` 曾被本移植删掉）；
- 于是 OUT0/OUT2 **一直是普通 GPIO**，根本不是恒流灌电流口 —— 这才是"写 DIM 不亮"的根因；
- **`0x0A` = 两个位都置**：OUT0/OUT2 进 LED 驱动模式点灯，OUT1 仍是逻辑输出驱动 SHDN。
  **实测：灯亮，且喇叭照样有声音。**（文档里记了很久的"从未测试"组合，这次测掉了。）

### DIMx 是 8 位，我之前的"上限 0x3F"是错的

规格书 9.2：「8-bits DIMx can be configured to 256 levels, from 00H to FFH」，另有
`ISEL[1:0]` 选四档电流范围（默认 0~37mA）。

参考项目的 `MODOU_KEY_LIGHTS_MAX_BRIGHTNESS 0x3f` **只是它自己选的暗亮度**，我误当成硬件上限。
实测阶梯 `0x0F → 0x3F → 0x7F → 0xBF → 0xFF` **依次变亮、无钳位异常** ⇒ `0xFF` 才是满亮度。
`B220_LED_BRIGHTNESS_MAX` 已改为 `0xFF`（改完之后编译器立刻报出三处 `brightness > 0xFF`
恒为假 —— 那是我原来那段 clamp 变成了死代码，已删除）。

### 我在这件事上错过的三个结论（保留，作为方法论的反面教材）

| # | 当时的结论 | 实际 |
| --- | --- | --- |
| 1 | "`0x20/0x21/0x22` 本机未接，屏幕没反应" | 它们是 **RGB**，只是没配驱动；而且背光在 `0x23/0x24` |
| 2 | "指示灯不在 AW9106 上，去别的总线/GPIO 找" | 就在 `0x20/0x21/0x22` |
| 3 | "DIM 上限是 `0x3F`（照抄参考常量）" | 8 位，`0xFF` 满亮度；`0x3F` 只有 25% |

三个错误的共同点：**把"没反应"当成"没接"，把别人的常量当成硬件规格**。
真正解决它的是**用户用万用表量出的实际接线** + **与已验证实现做严格寄存器差集**
（差集只有 `0x15`、`0x16` 两个寄存器，一步就定位了）。

### 生产配置（本轮固化）

- `xiaomodou_b220.cc`：`B220_AW9106_GPMD_A_GPIO` 由 `0x02` 改为 **`0x0A`**（所有写 `0x12` 的地方
  统一用这一个常量，SHDN 脉冲逻辑 `0x0A→0x00→0x02` 原样成立、上升沿仍只有一个）；
  初始化序列新增 `{0x15,0x1B}`、`{0x16,0x08}`、`{0x14,0x00}`，并把 `0x20/0x21/0x22` 起始值改为 `0x00`；
- `b220_led.c/.h`：重写为**真正的 RGB 驱动**（只写 `0x20/0x21/0x22`），待机**全灭**，
  接口 `b220_led_set_color/pulse/show_temporary/set_idle_color`；
- 按键反馈带颜色：**录音键=白闪、音量+=绿闪、音量−=红闪**，全部非阻塞。

---

## 2.9a ✅ 上机验证通过（2026-10-10 21:12，`logs/boot-20261010-211246.log`）

修复后的实测结果，逐条对照：

| 判据 | 结果 |
| --- | --- |
| 构造函数走完 | ✅ `board init: starting the ZL38063 key poller` → `key poller started (task created)` → `starting the indicator-light module` → `indicator-light module returned` → **`board init complete: display, keys, indicator lights and the baseboard link are up`**（全部在 6.0~6.2 s 之间完成） |
| 任务看门狗 | ✅ **0 次** `task_wdt`（修复前每次启动都在 26 s 报警） |
| 崩溃/重启 | ✅ **0 次** `assert failed` / `Guru` / `Rebooting` |
| 指示灯模块 | ✅ `AW9106B 0x5B attached for the indicator channels` + `indicator channels ready: 0x23/0x24 at the proven-safe 0x80`（并且不再关屏） |
| 底座串口 | ✅ `baseboard link up: UART0 TX GPIO1 / RX GPIO3 @ 115200 8N1`（`AT+Pon` 仍因控制台占用而失败，见 §2.5） |
| **真实音量** | ✅ 每一次都生效：`volume 0 -> gain 0xA6 (-90 dB)`、`10 -> 0xB0 (-80 dB)`、`80 -> 0xF6 (-10 dB)`、`90 -> 0x00 (0 dB)`、`100 -> 0x0A (+10 dB)`，并带 `stored AND applied to the DSP (HBI 0x0238/0x023A)` |
| **双击到顶/到底** | ✅ `double press detected` → `volume 80 -> 0`、`volume 0 -> 100` |
| 单击步进 | ✅ `single press` → `volume 100 -> 90`、`90 -> 80`、`0 -> 10`、`10 -> 20` |

增益与 dB 的对应完全符合设计（`gain = vol - 90`，以 0x00 = 0 dB、0xA6 = −90 dB、0x0A = +10 dB 为标尺）。

### 结论：按键（单击/双击）、指示灯、音量全部可用

唯一仍未解决的是**底座 `AT+Pon` 发不出去**（§2.5，控制台独占 UART0）——那是取舍问题，不是 bug。

---

## 2.9b **真正的"开机卡住"**：按键任务忙等把 CPU 占死了（已修，2026-10-10）

### 现象（用户原话："这还不是初始化问题吗？不按按键就一直等"）

对，**这次是初始化卡住**——但不是 DSP，是**板级构造函数被按键任务饿死了**：

```
I (6104) b220-btn: ZL38063 key poller up: ...
W (6144) b220-btn: ignoring one unconfigured key 0x0301 ...      <-- 此后 57 秒一行日志都没有
E (26034) task_wdt: Task watchdog got triggered. ... - IDLE0 (CPU 0)
E (26034) task_wdt: CPU 0: b220_buttons                          <-- 看门狗直接点名
Backtrace: ... 0x4024108A (b220_buttons.c) ...
W (63374) b220-btn: key VOLUME_UP ...                            <-- 只有用户按了键才继续
```

注意时间线：`b220_led` / `b220-base` 的初始化日志**本来紧跟在 6104 之后**，却拖到 63374 才出现，
而且只在你按键之后 —— 说明**构造函数在 6104 之后就没能继续跑**，直到按键任务偶然阻塞了一下
（写日志）才让出 CPU。

### 根因：我把中断当成了"立即读"的触发器

```c
const bool irq_pending = (s_irq_count != last_irq);
const bool due = (now_us - last_poll_us) >= 10 ms;
if (!irq_pending && !due) continue;     // <-- 只有"没中断且没到点"才睡
```

GPIO38 是**只输入脚、没有内部上拉**（日志里 `gpio_pullup_en(89): GPIO number error` 就是这个），
开机时它读作 **0**：一条**没有驱动、悬空的线**在翻动，边沿中断几乎每轮都会触发 ⇒ `irq_pending`
**永远为真** ⇒ `continue` 之前从不 `vTaskDelay` ⇒ 任务**不睡**，把 CPU 0 占满。

后果连锁：IDLE0 被饿死 → 看门狗报警；构造函数（同为 CPU 0 上的任务）被饿死 → 板级初始化停住 →
**只有按键让任务阻塞在日志上时才继续**。这解释了你看到的一切。

### 修法

**中断不再参与控制流**：循环只按**固定 10 ms** 节拍走，`s_irq_count` 保留但**不参与判断**
（写注释说明为什么）、并且 `last_poll_us` 初始化成"已经过期"，让**第一次读立刻发生**。
中断本身还挂着（当活动提示、代价为零），但**即使这条线完全噪声化也不会再影响任何东西**。

另外新增一行**构造函数完成标记**，把"卡在初始化里"变成一眼可见：

```
I XiaoMoDouB220: board init complete: display, keys, indicator lights and the baseboard link
                 are up (application start follows)
```

### 教训（写给下一个改这里的人）

> **"中断只是提前一点"和"中断会让循环不睡"是两种完全不同的设计。**
> 前者安全，后者是 CPU 独占。在没有上拉、没有驱动源的引脚上，
> **任何以边沿为条件的循环都可能变成忙等**。

---

## 2.10 初始化时间到底稳不稳（量化，2026-10-10）

用 `tools/b220_boot_timing.py` 扫了 `logs/boot-*.log` 全部启动记录，把关键节点的时间差算出来：

| 阶段 | 各次启动的实测值 | 结论 |
| --- | --- | --- |
| GPIO19 复位脉冲时刻 | 1264~1404 ms | 稳（±140 ms） |
| **DSP 主机引导（复位 → 应用 READY）** | **4560 ms，绝大多数启动完全相同**（4560/4570；个别 5030/5040） | **非常稳** |
| 其中：3699 条固件记录上传 | 3640~4110 ms | 稳 |
| DSP 复位 → codec 启动 | **9870 ~ 77450 ms** | **差别巨大** |
| Wi-Fi 拿到 IP | 16544 ~ 80694 ms | **差别巨大，且与上一行同步变化** |

**结论：慢的不是我们，是 Wi-Fi/云端连接。** 每次启动，codec 启动时间与"Wi-Fi 拿到 IP"的时间
**差几乎恒定在 2.1~2.6 秒**。也就是说：

- 板级与 DSP 那条路是确定性的（4.56 s 的固件上传是固定成本，无法省，除非放弃音质）；
- 看到的"有时快有时慢"完全来自**路由器/AP 关联与 DHCP 的耗时**（16 s 到 80 s 不等，
  同一台设备同一固件）；
- 这一条**不需要也不应该改固件**：把 DSP 引导推迟到网络之后只会让"能对话"更晚。

> 顺带一个可以省时间的点：如果不想每次开机都花 4.6 s 重灌 DSP 固件，可以研究
> "DSP 已经跑着就跳过上传"（读 `reg 0x0014 == 0x0200` 判定）。但注意**软复位/掉电后 DSP 一定
> 是空的**，所以只在"浅重启"（ESP32 复位而 DSP 未断电）时才有意义，收益有限、分支变多，暂不做。

---

## 3. 音量键仍然"只存值"（未解决，且本轮未动）

按键现在正确了，但**音量不会变**，因为 `Zl38063AudioCodec::SetOutputVolume()` 按设计只写
NVS + UI 指示，真正的增益在 DSP 内部（`reg 0x0238/0x023A`，PART 3 §1.6）。

参考项目也没有解决这件事（它的音量键只驱动本地 UI/蜂鸣），所以**没有可抄的现成答案**。
要真正实现，得单独确认 `zl38063_dsp_set_volume()` 这条路的档位表（`0x0238/0x023A` 的
`-90 dB + vol` 编码，以及 `0x0494/0x049A` 的高低音量分支）—— 这是一件独立的事，建议在
按键长按行为验收通过之后再单独开一轮。

---

## 4. 本轮改动清单

| 文件 | 内容 |
| --- | --- |
| `b220_led.h` / `b220_led.c` | 新增：AW9106B 指示灯通道驱动 + 非阻塞灯效 |
| `b220_base_uart.h` / `b220_base_uart.c` | 新增：底座 UART 帧封装 + 开机 `AT+Pon` 握手 + 运动/灯光/停止 |
| `b220_buttons.h` / `b220_buttons.c` | 重写：按参考实现改正"标志当键值""事件当电平""按下抬起双触发"，并接上 GPIO38 下降沿中断 |
| `xiaomodou_b220.cc` | 接上两个新模块；发布 I2C0 总线句柄给 LED 模块；按键动作闪灯 |
| `config.h` | 新增 `B220_BUTTON_HOLD_GAP_MS`、`B220_LED_INDICATOR_IDLE_LEVEL`、`B220_BASE_UART_HANDSHAKE(_DELAY_MS)`；按键映射注释改为双重确认 |
| `docs/KEY-VERIFICATION-vs-Flower_modou.md` | 新增：按键逐条对照报告 |
| `docs/B220-LED-AND-BASE-UART.md` | 本文档 |
| `README.md` | 更新按键/指示灯/底座三节 |

编译状态：`Build completed successfully!`，无代码警告，`xiaozhi.bin` = 2,469,504 B。
**未做任何真机验证**（本轮无硬件）。
