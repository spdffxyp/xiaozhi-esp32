# 按键实现核对报告：本移植 vs. Flower_modou（同硬件开源项目）

对照对象：[FlowerCat-chen/Flower_modou](https://github.com/FlowerCat-chen/Flower_modou)（`魔豆小车`，
ESP32 + ZL38063 音频前端 + AW9106 + 底座 UART，与本机同硬件），已 clone 到
`ref/Flower_modou/`。

**结论：按键路径完全正确，而且这次对照直接找到了我们驱动里三个真实 bug 的根因，已全部修掉。**

---

## 1. 对照表

| 项目 | Flower_modou (`components/modou_drivers/modou_dsp_keys.c`) | 本移植（修改后 `b220_buttons.c`） | 判定 |
| --- | --- | --- | --- |
| 前端中断线 | `MODOU_DSP_KEYS_INT_GPIO = GPIO_NUM_38`，`GPIO_INTR_NEGEDGE`，空闲高 | 同：GPIO38 下降沿中断 | ✅ 一致 |
| HBI 状态寄存器 | `HBI_STATUS_REG = 0x0000` | 同 `0x0000` | ✅ |
| HBI 按键寄存器 | `HBI_KEY_RAW_REG = 0x0002` | 同 `0x0002` | ✅ |
| 按键码 | `0x08`=VOL+，`0x0a`=VOL−，`0x0b`=MIC | `B220_BUTTON_KEY_2=0x08`，`_3=0x0A`，`_1=0x0B` | ✅ 一致 |
| 空闲判定 | `status == 0` 为 idle | 同 | ✅ |
| **按下 vs 抬起** | `status & 0x0001` = 按下；**`0x0002` = 抬起，明确不作为动作**（源码注释：`抬起事件 status=0x0002，不作为 OTA 入口`） | 同：只有 `status & 1` 才动作，`0x0002` 只用来关闭长按窗口 | ✅ 一致 |
| HBI CS / 时钟 / 模式 | GPIO0 / 10 MHz / mode 0 / queue 6 | 同 | ✅ |
| DSP 复位 | GPIO19 低 10 ms → 高 → 等 500 ms | 同 | ✅ |
| 前端 0x19 | 探针 + `WHO_AM_I(0x0F) == 0x11` + 一组初始化 | 我们不初始化它（只读） | ⚠️ 差异（见 §4） |

按键码这一栏是**双重确认**：既是我们上机实测（`key(0x0002)=0x0008/0x000A/0x000B`），
也是这个独立项目在自己的机器上验证过的常量。

---

## 2. 对照找出的三个 bug（已修）

### bug 1：把"按下/抬起标志"当成了按键值

Flower_modou 的 `modou_dsp_keys_poll()` 先读 `0x0000`，只有非 0 时才读 `0x0002` 取键值：

```c
err = hbi_read(HBI_STATUS_REG, 1, &status);      // 0x0000
...
const bool active_read = state->irq_pending || state->gpio38_level == 0 || status != 0;
...
err = hbi_read(HBI_KEY_RAW_REG, 1, &raw);        // 0x0002，取键值
```

而我们早先的驱动拿 `0x0000` 去查表 —— 于是每次都报
`UNCONFIGURED code 0x0003 / 0x0002`，正是你看到的那两行。

### bug 2：把"事件"当成了"电平"

Flower_modou 在主循环里收到 `status=0x0002`（抬起）会**明确忽略**，说明这套接口是
**事件式**的：按下给一个事件、抬起给另一个事件，中间**不保证**持续给出"正按着"。
我们早先按"电平"处理，于是 `held 9 ms`、长按永远触发不了。

### bug 3：按下和抬起都触发动作

这就是你说"按下和松开都触发功能"的原因：`0x0003` 和 `0x0002` 各被当成一次完整按压。
现在只有 `status & 0x0001`（按下）才动作。

---

## 3. 修改后的实现要点（`b220_buttons.c`）

```
每 B220_BUTTON_POLL_MS（10 ms）或 GPIO38 下降沿中断触发：
    status = hbi_read(0x0000)
    if (status != 上一次):
        if (status & 0x0001)  -> 按下：key = hbi_read(0x0002)，查表，执行一次
        else if (status == 0x0002) -> 抬起：只关闭长按窗口，不执行任何动作
    service_hold(): 按住超过 B220_BUTTON_LONGPRESS_MS 且没有收起事件 -> 触发长按
```

- **GPIO38 下降沿中断**已按参考实现接上：中断只决定**什么时候看**，寄存器决定**看到了什么**；
  即使中断不工作，轮询也照样正确（这是刻意的冗余）。
- 长按窗口用 `B220_BUTTON_HOLD_GAP_MS = 300` ms 跨越按住期间的静默，收到抬起事件立即关闭。
- 全程只读 DSP：不写 `reg 0x0014`、不复位、不重灌固件。

> ⚠️ 仍需上机确认的一点：**按下事件是否每次按压只来一次**。Flower_modou 的用法
> （`fresh_irq && vol_up_press` 才进 OTA）强烈提示是"一次按压一个事件"，但我们没有在
> 自己的机器上验证过"按住 3 秒会不会连续来多个按下事件"。如果上机发现长按时报了多次短按，
> 把日志里的 `status` 序列发我即可（驱动已经打印原始 status/key）。

---

## 4. 已知差异（有意保留）

| 差异 | 说明 |
| --- | --- |
| 不初始化 0x19（SC7A20） | 参考实现会写 `0x20/0x21/0x22/0x23/0x25/0x30/0x32/0x20` 并校验 `WHO_AM_I=0x11`。我们是**只读**，因为本移植不需要 G-sensor，而且改动正在工作的 I2C 设备有风险。将来要用姿态/敲击功能时按它的序列加。 |
| 不做 DSP 固件的完整主机引导 | 参考实现自己灌固件（`modou_dsp_firmware.c`，1 MB 的数组）+ 配置 + 后置寄存器序列。本移植已有一套验证过的 release 路径（PART 2），不做替换。 |
| 未实现启动期"按住音量+进 OTA" | 参考实现有。这是产品行为，不属于本次范围。 |

---

## 5. 结论

- ✅ **按键路径、寄存器、键值映射全部正确**，与独立项目逐条一致；
- ✅ 三个真实 bug 已修：查错寄存器、事件当电平、按下抬起双触发；
- ✅ GPIO38 中断已按参考实现补上；
- 🔶 待上机：长按只触发一次；音量键的**实际音量**仍不生效（见 `B220-LED-AND-BASE-UART.md` §3）。
