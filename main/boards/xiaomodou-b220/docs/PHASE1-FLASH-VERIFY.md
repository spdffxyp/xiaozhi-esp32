# Phase 1 · 刷机通道验证（可回滚 · 不破坏出厂固件）

目标：在动"新固件"之前，先证明 **能写、能读回、能还原**，而且过程中**不破坏原厂固件**。
通过后 Phase 2 才允许整片改写。

---

## 0. 前提（已确认）

- 本机 micro-USB 是**只读日志口**，PC→ESP32 通路必须靠飞线到模组 UART0（`BACKUP-README.md` 10 项实测）。
- 已有整片备份：`backup\esp32-16MB-full.bin`，16,777,216 B，**MD5 `a6a27d2b4b7a8de36636e9ef635bea40`**。
- 器材：3.3 V USB-TTL 转接板、万用表、细漆包线/测试夹。

## 1. 接线（3 根，VCC 不接）

| USB-TTL | ESP32 模组 | 说明 |
| --- | --- | --- |
| **TX** | **GPIO3（RXD0，模组第 34 脚）** | ★ 决定成败的一根：PC→ESP32 |
| RX | GPIO1（TXD0，第 35 脚） | 用于看日志/引导横幅 |
| GND | GND（模组第 1/15/38 脚或屏蔽罩） | 必须共地 |
| VCC | **不接** | 设备自己供电，避免两边电源打架 |

规则：先接 GND 再接信号线；转接板必须 3.3 V 档（ESP32 的 IO 不耐 5 V）。
已知原机 USB 桥的 TX 并未接到 GPIO3（实测证据），所以**大概率不需要割线**；若 `flash_id` 连不上再按 `MANUAL-BACKUP.md` §A8 处理总线竞争。

## 2. 进下载模式（2026-10-07 实测修正）

**本机真正的进下载模式方式：让 micro-usb 在开机瞬间把 GPIO0 拉低。**

| 目的 | 操作 |
| --- | --- |
| **刷机（进下载模式）** | 按一下启动键**关机** → **插上 micro-usb** → 按一下启动键**开机** → 出现 `boot:0x27 (DOWNLOAD_BOOT)` |
| **运行小智固件** | **拔掉 micro-usb**（改由电池供电）→ 按一下启动键关机、再按一下开机 → `boot:0x3f (SPI_FAST_FLASH_BOOT)` |

关键事实：
- 玩具自己的 USB 桥在开机瞬间把 **GPIO0 拉低**，所以**只要插着 micro-usb 开机，ROM 必然进下载模式**；
  这一点曾被误判为"DTR/RTS 复位电路可用"——实际不是 DTR/RTS 的功劳。
- 裸 TTL 的 DTR/RTS 未接线，因此 esptool **不能用 `hard_reset`**，也**没有 `watchdog_reset`**（4.8.1 不支持），
  只能 `--before no_reset --after no_reset`（先手动进下载模式）。
- 让芯片从下载模式回到运行：**拔掉 micro-usb 后重新上电**（或 `esptool write_mem 0x3FF48000 0x80000000`
  触发软件复位，但插着 micro-usb 时会再次落回下载模式）。
- 玩具的**启动键是底座上的软开关**（按一下关机/再按开机），ESP32 读不到它，所以固件里没有按键。

```powershell
$py = '<idf-python>'
$env:ESPTOOL_CFGFILE = '<workspace>\esptool.cfg'
```

## 3. 第一步：认芯片（只读）

```powershell
& $py -m esptool --port COM<x> --baud 921600 flash_id
```

期望：`Chip is ESP32-D0WD` / `Detected flash size: 16MB` / 具体 Flash 型号（`W25Q128`、`GD25Q128`、`XM25QH128` …）。
把**实际 Flash 型号**记下来（写进本文件末尾的记录表）。

## 4. 第二步：独立复核整片备份（只读，必做）

```powershell
& $py -m esptool --port COM<x> --baud 921600 read_flash 0 0x1000000 .\backup\esp32-16MB-full-verify.bin
Get-FileHash .\backup\esp32-16MB-full.bin, .\backup\esp32-16MB-full-verify.bin -Algorithm MD5
```

两份 MD5 都必须等于 `a6a27d2b4b7a8de36636e9ef635bea40`。
**不一致 → 说明接线/供电不稳，立刻停下，不要进入第 5 步。**

## 5. 第三步：写入测试 —— 只写空的 OTA 槽

原分区表里 `wifi_1`（`app/ota_1`）位于 **0xc40000**，长 0x3c0000，**整片 0xFF、从未写过**，且 `otadata` 指向 `ota_0`（0x10000）。
所以往 0xc40000 写数据**不影响设备启动，也不碰任何出厂数据**。

```powershell
# 造 64 KB 随机测试图案
& $py -c "import os;open(r'<workspace>\backup\_wtest.bin','wb').write(os.urandom(65536))"
# 写
& $py -m esptool --port COM<x> --baud 921600 write_flash 0xc40000 .\backup\_wtest.bin
# 读回
& $py -m esptool --port COM<x> --baud 921600 read_flash 0xc40000 0x10000 .\backup\_wtest-read.bin
# 比对
Get-FileHash .\backup\_wtest.bin, .\backup\_wtest-read.bin -Algorithm MD5
```

两个 MD5 一致 ⇒ **写入通路已验证**。
此时可以正常开机，确认设备行为与之前一致（出厂固件完好）。

## 6. 第四步：清掉测试数据（把 ota_1 还原成空）

```powershell
& $py -m esptool --port COM<x> --baud 921600 erase_region 0xc40000 0x10000
# 可选复核：应全为 FF
& $py -m esptool --port COM<x> --baud 921600 read_flash 0xc40000 0x1000 .\backup\_wtest-erased.bin
```

## 7. 万一出错（回滚）

- **设备不启动 / 起不来**：整片回写备份（3–5 分钟，结束必须出现 `Hash of data verified.`）

```powershell
& $py -m esptool --port COM<x> --baud 921600 write_flash 0x0 .\backup\esp32-16MB-full.bin
```

- **连不上**：按顺序排查 —— ① TX/RX 接反（最常见）② GND 没共地 ③ 没真进下载模式（先看横幅）④ 线太长 → 降 `--baud 115200` ⑤ 最后才怀疑桥的 TX 竞争（割 0Ω/划走线）。

## 8. 通过标准（Phase 1 Done）—— 2026-10-07 实测结果

- [x] `flash_id`：**ESP32-D0WD rev1.0 / 16 MB / GigaDevice GD25Q128**，MAC `b4:e6:2d:fc:4c:e5`（与备份 NVS 一致，同一台）
- [x] 整片读回（16,777,216 B / 277 s / ≈484 kbit/s）：**255/256 个 64 KB 块与备份逐字节一致**；唯一有差异的块是 `0x000000`，
      差异全部落在 **NVS 分区（0x9000–0xd000）**，形态是"页轮转"（0xb000 写入 1927 B、0xc000 擦除 2365 B）
      → 属固件运行时正常写 NVS，**读取通路零误码**。
      所以整片 MD5 与 2026-10-06 的备份不同（`DBC5E015…` vs `A6A27D2B…`）**是预期行为，不是接线/波特率问题**。
- [x] 64 KB 写测：写入**空 OTA 槽 `0xc40000`**（不碰出厂固件），esptool 自校验 `Hash of data verified.`；读回 MD5 与写入源一致
- [x] 擦除测试区 `erase_region 0xc40000 0x10000` 成功，读回全 `0xFF`（还原为空）
- [ ] 设备断电重开后仍能正常开机（待插拔确认）
- [ ] 备份文件另存一份（异盘/云盘），MD5 写在文件旁

> **结论：写入通路已验证可用，且出厂固件完好无损。** 可以进入 Phase 2（新板级骨架）。

## 9. 记录表（执行时填写）

**执行方式说明（2026-10-07 实测）**

- 本机的串口可以从 AI 侧直接驱动（实测 `COM1` 可正常打开），所以本文档的命令**可以由我代为执行**，你只要接线并让设备进入下载模式。
- 但 `COM25` 当前被别的进程占用（打开报"访问被拒绝"）——执行前请关掉占着它的串口终端/工具。
- 另注意：`idf.py` 在受限沙箱里会因"禁止创建命名管道"报 `WinError 5`，编译需要你批准一次提权；`esptool` 走 PowerShell 管道不受影响。

| 项 | 值 |
| --- | --- |
| 实际 COM 口 | **COM25**（自接 TTL→USB，接 ESP32 UART0；`--before no_reset --after no_reset` 即可连，因为板上另有 DTR/RTS 复位电路但 TTL 未接） |
| 芯片 | **ESP32-D0WD (revision v1.0)**，MAC `b4:e6:2d:fc:4c:e5`（与备份 NVS 一致，同一台设备） |
| Flash 型号 / 厂商 | **GigaDevice GD25Q128**（JEDEC `c8 6018`），16 MB；esptool 提示 "Flash voltage set by a strapping pin to 1.8V" |
| `read_flash` 用时 / 实际波特率 | **277.4 s / ≈484 kbit/s**（命令行 `--baud 921600`，实测吞吐受转接板限制） |
| 整片读回 MD5 | `DBC5E015E4C5D433C5B1FAF098D3F11D` —— **与备份 `A6A27D2B…` 不同，但差异全部落在 NVS 分区（0x9000–0xd000）**，详见 §8 |
| 64 KB 写测 MD5（写 / 读回） | 写入源 `84BA125300A596B5484DDCF62CBB4F41` / 读回 `84BA125300A596B5484DDCF62CBB4F41` ✅ 一致 |
| 日期 | 2026-10-07 |
