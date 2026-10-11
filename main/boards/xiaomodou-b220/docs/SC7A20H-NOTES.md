# SC7A20H accelerometer: what the datasheet says, and what this unit actually does

The B220's accelerometer is **not in the xiaozhi reference hardware**; it is part of this toy.
The reference port of the toy (Flower_modou) reads it, our own button-discovery probe already
touched its interrupt registers, and the part is now driven by
[`b220_gsensor.c`](../b220_gsensor.c) for `{BASE+ACCEL}` and the `self.motion.get` MCP tool.

The datasheet used is **Silan SC7A20H 说明书 v0.7 (31 pages)**. It matters because two of its
statements are measurable, and **this unit disagrees with one of them** - so the file records
both, and the code takes its scale from the sample instead of from the register.

## Wiring

| Item | Value | Source |
| --- | --- | --- |
| Part | SC7A20H (LIS3DH-register-compatible but **not** an LIS3DH) | datasheet; reference port |
| I2C address | **0x19** (SDO floating/logic high; 0x18 if SDO is tied low) | datasheet §10.1; `config.h` |
| Bus | **I2C0, SDA GPIO18 / SCL GPIO23** - shared with the AW9106B | `config.h`, `xiaomodou_b220.cc` |
| WHO_AM_I (0x0F) | **0x11** (with VERSION 0x70 = 0x28) | datasheet §12.2, §12.37 |
| Data registers | OUT_X_L..OUT_Z_H = **0x28..0x2D**; auto-increment read from **0xA8** (0x28 \| 0x80) | datasheet §10.1 |
| Status register (0x27) | bit3 `ZYXDA` = all three axes converted | datasheet §12.10 |

## Data format: 12-bit, left-justified - this is the fact that fixed the numbers

The datasheet states **12bit 数据输出** and works an example (§12.13): the pair
`OUT_X_H=0x40, OUT_X_L=0x00` is 1024 counts, equal to **1.0 g at FS=10 (+-8g)** and 4.0 g at
FS=00 (+-2g). So the twelve significant bits are the **high** half of the 16-bit pair; the low
four bits are discarded.

The code therefore does `counts = raw >> 4` and reports those, which makes its numbers directly
comparable with the datasheet's own 1024-per-g figure. Measured on the bench: **998 and 1020
counts for 1 g in two orientations.** That agreement is the reason the current implementation can
be trusted at all.

## Full scale and sensitivity (datasheet §12.7, mechanical table)

| FS1 FS0 | Range | Sensitivity | 1 g, per the datasheet's own example |
| --- | --- | --- | --- |
| 00 | +-2G | 1 mg/digit | 0x4000 = **1.0 g** |
| 01 | +-4G | 2 mg/digit | 0x4000 = 2.0 g |
| **10** | **+-8G** | **4 mg/digit** | 0x4000 = **4.0 g** |
| 11 | +-16G | 8 mg/digit | 0x4000 = 8.0 g |

Read the table this way: the raw pair `0x4000` is 1024 in the twelve significant bits, and what
that *means* in g depends on the range - so **1024 counts per g only at +-8g**. At +-2g, 1 g would
need four times as many counts (4096 in the twelve significant bits, 0x4000 being 1.0 g there).

`CTRL_REG4` bit layout: `BDU BLE FS1 FS0 DLPF[0] ST1 ST0 SIM`.

## Two places where this unit does not do what the datasheet says

Both are recorded because the code was written twice on the strength of the datasheet and the
hardware disagreed both times.

### 1. The full-scale bits do not appear to take effect

The firmware writes `CTRL_REG4 = 0x08`, i.e. `FS[1:0] = 00` = **+-2g** (and the reference port for
this toy writes the same value). At that range the datasheet's own example makes `0x4000` equal to
1.0 g, i.e. **4096 counts per g in the twelve significant bits**.

**Measured: about 1024 counts per g** - the datasheet's figure for **+-8g**. So either the write is
not taking effect, or this part's sensitivity differs from its own datasheet at this setting. The
register is read back after the write and reported in the log so the next person can see which.

**What the code does instead of picking a side:** every reading takes its scale from itself. A
stationary accelerometer's largest axis is 1 g *by definition of gravity*, so
`mg = counts * 1000 / largest_count` is correct at any full scale. The reported `1g = N counts`
makes the scale visible, and the off-scale readings - the first sample after configuration, or a
unit being moved - are rejected rather than reported.

### 2. The BDU bit makes every reading collapse

The datasheet puts **BDU** (block data update: "output registers are not updated until MSB and
LSB have been read") in bit 7 of `CTRL_REG4`. That is exactly what a six-byte read wants, so the
driver was changed to write `0x88`.

**Measured: with `0x88` every reading came back near zero** - the largest axis never reached the
plausibility window and `{BASE+ACCEL}` answered "moving (or no valid sample)" for all eight
attempts. Reverting to `0x08` restored normal readings immediately.

Whatever bit 7 does on this part, it is not the datasheet's BDU. The value the reference port
writes (`0x08`) is used, and **sample coherency is not attempted through that bit**.

## Which way is up: the axis mapping, measured

Three stationary readings, each with the dominant count within a few percent of 1024:

| Unit held | Reading | Conclusion |
| --- | --- | --- |
| screen down | z = **-1000 mg** | **Z is normal to the screen**; +z with the screen up |
| rotated 90 deg from upright | x = **+1000 mg** | **X runs along the bottom edge** |
| upright and level | x, z near 0, y = -1000 mg | **Y runs along the left edge** |

## What this part offers that is not used yet

Interrupts are the interesting part for this board, because polling would sit on the I2C bus that
the AW9106B (backlight and audio PA) also uses:

- **6D/4D orientation detection** - six independent position states (registers 0x30..0x37).
- **Free-fall detection**, **sleep/wake**, **single and multi-click** (`CLICK_CRTL_REG` 0x38,
  `CLICK_SRC` 0x39, coefficients 0x3A..0x3D). The click threshold is expressed in 16/32/64/128 mg
  steps depending on full scale, so the same full-scale question above applies.
- **INT1/INT2 pins** carry any of these. **Which ESP32 GPIO they are wired to on this board has
  not been identified** - the button-discovery probe guesses at the interrupt *status registers*,
  not at the pins. That is the first thing to establish before building anything on interrupts.
- A **32-level FIFO** (0x2E/0x2F, data 0x69) which would let a sampler read a batch per bus
  transaction instead of one sample per transaction.

## Bus-sharing note

The accelerometer, the AW9106B (LCD backlight, audio PA SHDN) and the RGB indicator share I2C0.
`b220_gsensor.c` takes a mutex for everything it does, and only reads on demand - there is no
sampling task. Anything that later polls this part has to keep the PA's write path in mind: the
AW8737S is enabled by a single 0 -> 1 edge on its SHDN line, and the driver goes to some trouble
to make sure that edge happens exactly once per boot.
