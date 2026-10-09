# 小魔豆 B220 人工刷机指导书

适用设备：**小魔豆 B220**（ESP32-D0WD rev1.0，16 MB Flash `GD25Q128`，8 MB PSRAM）
本机实测记录：MAC `b4:e6:2d:fc:4c:e5`，刷机口 **COM25**（接在 ESP32 UART0 上的 3.3 V TTL 转接板）

---

## 0. 先看这张表：我要做什么？

| 目的 | 走哪节 | 关键点 |
| --- | --- | --- |
| 备份整片出厂固件 | §4.2 | 只读，最安全；读两遍比 MD5 |
| **恢复出厂固件** | §4.3 | 整片 16 MB 写回，约 5–8 分钟 |
| **刷小智固件** | §4.4 | 写 5 个区域；建议用 `flash-b220.ps1 -FullErase` |
| **确认唤醒词（"你好小智"）生效** | §4.7 | 模型在 `generated_assets.bin` 里，随 assets 写到 `0x800000`；**偏移一个都没变** |
| 只做一次写入能力验证（不破坏出厂固件） | §4.5 | 往**空的 OTA 槽** `0xc40000` 写 64 KB 再读回 |
| 看芯片寄存器（例如 GPIO0 电平） | §4.6 | `read_mem` / `dump_mem` |

---

## 1. 硬件与接线

| USB-TTL（3.3 V 档！） | ESP32 模组 | 说明 |
| --- | --- | --- |
| **TX** | **GPIO3**（RXD0，模组第 34 脚） | ★ 决定性的一根：PC → ESP32 |
| RX | GPIO1（TXD0，第 35 脚） | 看日志/引导横幅 |
| GND | GND（第 1/15/38 脚或屏蔽罩） | **必须共地** |
| VCC | **不接** | 设备自己供电，避免两边电源打架 |
| DTR/RTS | **不接** | 本机 TTL 没接，所以 esptool 一律用 `--before no_reset` |

规则：先接 GND 再接信号线；ESP32 的 IO **不耐 5 V**。
万用表可先确认：TTL 的 GND ↔ 模组 GND 通断；TX 应通到 GPIO3（模组第 34 脚）。

---

## 2. 进入 / 退出下载模式（本机最容易搞错的一步）

> **本机的"进下载模式"不是靠 DTR/RTS，而是靠插着 micro-USB 上电。**

| 目的 | 操作顺序 | 会看到什么 |
| --- | --- | --- |
| **进下载模式（刷机）** | ① 按一下启动键**关机** → ② **插上 micro-USB** → ③ 按一下启动键**开机** | 串口横幅 `boot:0x27 (DOWNLOAD_BOOT...)` + `waiting for download` |
| **退出下载模式（运行固件）** | ① **拔掉 micro-USB** → ② 按一下启动键关机 → ③ 再按一下开机 | 横幅 `boot:0x3f (SPI_FAST_FLASH_BOOT)`，屏幕亮起 |

原理：玩具自己的 USB 桥在**开机瞬间把 GPIO0 拉低**；而 GPIO0 是 ESP32 的启动 strapping 脚，
低电平 ⇒ ROM 必然进下载模式。所以**插着 micro-USB 是没法正常运行的**（这条踩过很久）。

补充事实：
- 启动键是**底座上的软开关**（按一下关机、再按一下开机），ESP32 读不到它 → 固件里没有按键。
- 运行小智固件时，电脑这边只靠 TTL 供电是**不够**的（TTL 只接 TX/RX/GND），要靠玩具电池供电。

---

## 3. 工具与前置

```powershell
# esptool（本机已就绪，4.8.1）
$py = '<idf-python>'

# 工作目录：仓库里的 esptool.cfg 会被自动加载（已针对本机调过复位等待）
cd <workspace>

# 本机 TTL 没有 DTR/RTS，所以公共参数永远用 no_reset
$common = @('-m','esptool','--port','COM25','--before','no_reset','--after','no_reset','--baud','921600')
```

实测吞吐：`921600` 波特率下有效约 **490 kbit/s** ⇒ 16 MB 整片读写各约 **4.5–6 分钟**。
想更快可试 `--baud 1500000`，连不上就退回 921600。

---

## 4. 常用命令

### 4.1 认芯片（只读，第一步永远做这个）

```powershell
& $py @common flash_id
```

期望输出：

```
Chip is ESP32-D0WD (revision v1.0)
MAC: b4:e6:2d:fc:4c:e5
Manufacturer: c8
Device: 6018
Detected flash size: 16MB
```

看到 `A fatal error occurred: ... No serial data received` ⇒ **没进下载模式**（回到 §2）。

### 4.2 备份整片固件（只读，强烈建议先做）

```powershell
& $py @common read_flash 0 0x1000000 .\backup\esp32-16MB-full.bin
Get-FileHash .\backup\esp32-16MB-full.bin -Algorithm MD5
```

**判定标准**：命令末尾出现 `Staying in bootloader.`，且文件正好 **16,777,216 字节**。
本机出厂备份的 MD5 是 **`A6A27D2B4B7A8DE36636E9EF635BEA40`**（2026-10-06 采集）。

> 注意：设备只要运行过，**NVS 分区（0x9000–0xd000）就会变**（WiFi 状态、日志游标等），
> 所以"重新读一遍"的整片 MD5 很可能与旧备份**不同**——这不是接线问题。
> 想确认读取是否零误码：比对时按 64 KB 分块比较，只应看到 NVS 所在的那一块有差异。

### 4.3 恢复出厂固件（整片 16 MB 写回）

```powershell
& $py @common write_flash 0x0 .\backup\esp32-16MB-full.bin
```

**判定标准**：最后必须出现 **`Hash of data verified.`**（esptool 用片上 MD5 校验，这是唯一可信的成功标志）。

可选复查（只读前 64 KB，几秒）：

```powershell
& $py @common read_flash 0 0x10000 "$env:TEMP\b220-spot.bin"
& $py -c "import sys;a=open(sys.argv[1],'rb').read(65536);b=open(sys.argv[2],'rb').read();print('identical:',a==b)" `
      .\backup\esp32-16MB-full.bin "$env:TEMP\b220-spot.bin"
```

写完后按 §2 退出下载模式（**拔 micro-USB**）即可开机。

> ⚠️ **把甲机的整片备份写到乙机**会连 NVS 一起搬过去，其中包括 `nvs.net80211:sta.mac` —— WiFi 驱动会采用这个缓存值，
> 于是两台机器**用同一个 MAC** 上同一网络，会互相干扰。若确实要跨机恢复，写完后清一下 NVS：
> ```powershell
> & $py @common erase_region 0x9000 0x4000    # 清 NVS：MAC 回到芯片 eFuse 出厂值，但需重新配网
> ```
> （eFuse 里的真实 MAC 无法被 flash 写入改变，所以清 NVS 之后就恢复正常。）

### 4.4 刷小智固件

**推荐用脚本**（会在写完后读回校验）：

```powershell
cd <workspace>
.\flash-b220.ps1 -FullErase      # 首次从原厂切到小智：整片擦除，避免 assets 分区残留原厂音频
.\flash-b220.ps1                 # 之后迭代：只写 5 个区域，保留 NVS/WiFi 凭据
.\flash-b220.ps1 -NoBoot         # 刷完不让它复位（用于排查）
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

**判定标准**：`Hash of data verified.`；随后 `read_flash 0x20000 <app长度>` 读回、与
`build\xiaozhi.bin` 比 MD5，两者必须一致。

> **唤醒词（2026-10-09 起）**：本板用的是 esp-sr WakeNet9 中文模型
> `wn9_nihaoxiaozhi_tts`（唤醒词"你好小智"，284 KB）。它**不在任何独立分区里** ——
> 构建时由 `scripts/build_default_assets.py` 打成 `srmodels.bin` 塞进
> `build\generated_assets.bin`，所以它随上面第 5 个区域一起写到 `0x800000`。
> 写机清单仍是 5 项、偏移全未变，assets 分区也不需要整片重刷（只覆盖前 ~1.6 MB）。
> 细节与验收见 §4.7。

固件本身这样编译（需要 ESP-IDF 6.1）：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd <repo>
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi
```

`--wake-word nihaoxiaozhi` 是仓库的官方开关（`docs/custom-board.md:376-388`）：对
`esp32` 目标它会展开成 `CONFIG_USE_ESP_WAKE_WORD=y` + `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y`
（`scripts/build.py:338-385`，esp32 不在 `_LITE_WAKE_WORD_TARGETS` 里，所以选的是
`wn9_*` 大模型而不是 `wn9s_*`）。

> **注意（仓库规则）**：`CONFIG_SR_WN_*`（选模型）**不能**写进板级 `config.json` ——
> `scripts/tests/test_build.py:172-184` 专门断言这一点，写了会让 host 测试挂掉。
> 所以模型选择只有两个合法来源：
> 1. `sdkconfig.defaults.esp32:3` 的 `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y`（已存在，
>    实测已进入 `build/config/sdkconfig.json`）；
> 2. 命令行 `--wake-word nihaoxiaozhi`。
> 板级 `config.json` 只负责 `CONFIG_USE_ESP_WAKE_WORD=y` / `CONFIG_WAKE_WORD_DISABLED=n`
> / `CONFIG_USE_AFE_WAKE_WORD=n`，这样即使漏掉 `--wake-word` 也仍然有唤醒词
> （模型由 defaults 提供，覆盖值一致、不冲突）。

构建日志里应能看到（证明模型真的打进 assets 了）：

```
  wakenet models: wn9_nihaoxiaozhi_tts (will be packaged)
Generated: .../srmodels/srmodels.bin
```

```powershell
# 不去刷机也能先自检：assets 里到底有没有模型
$B = '<repo>\build'
$txt = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$B\generated_assets.bin"))
"assets 含 srmodels: " + $txt.Contains('srmodels')     # 必须 True（重新构建后）
(Get-Item "$B\generated_assets.bin").Length            # 约 157 万字节（带模型；无模型时约 129 万）
```

`flash-b220.ps1` 也会自动做这一步自检：`assets` 里没有 `srmodels` 时会黄字警告
"语音唤醒不会工作"并给出重建命令；如果（将来）分区表里出现 `model` 分区，
它会自动把 `build\srmodels\srmodels.bin` 也按分区表偏移一起写。

### 4.5 写入能力验证（安全：只写空的 OTA 槽）

原厂分区表里 `wifi_1`（app/ota_1）位于 **0xc40000**、长 0x3c0000、**整片 0xFF 从未写过**，
且 `otadata` 指向 `ota_0`，所以往这里写不会影响启动、也不碰任何出厂数据：

```powershell
& $py -c "import os;open(r'.\backup\_wtest.bin','wb').write(os.urandom(65536))"
& $py @common write_flash 0xc40000 .\backup\_wtest.bin
& $py @common read_flash  0xc40000 0x10000 .\backup\_wtest-read.bin
Get-FileHash .\backup\_wtest.bin,.\backup\_wtest-read.bin -Algorithm MD5   # 必须一致
& $py @common erase_region 0xc40000 0x10000                               # 还原成空
```

### 4.6 读寄存器 / 强制软复位（排障用）

```powershell
# GPIO 输入电平：bit0=GPIO0, bit2=GPIO2, bit5=GPIO5, bit12=MTDI, bit15=MTDO
& $py @common dump_mem 0x3FF4403C 4 "$env:TEMP\gpio_in.bin"

# 读 RTC_CNTL_OPTIONS0（bit31 = SW_SYS_RST）
& $py @common dump_mem 0x3FF48000 4 "$env:TEMP\rtc_opt0.bin"

# 触发一次软件复位（不需要 DTR/RTS）
& $py @common write_mem 0x3FF48000 0x80000000
```

> `write_mem` 会立刻复位芯片，esptool 通常报 `Packet content transfer stopped` —— **这是正常的**。
> 若复位瞬间 GPIO0 仍是低电平（例如 micro-USB 还插着），芯片会再次落回下载模式。

---

### 4.7 唤醒词（"你好小智"）怎么确认生效

**它是什么**：esp-sr 2.4.7 的 WakeNet9 中文模型 `wn9_nihaoxiaozhi_tts`
（`managed_components/espressif__esp-sr/model/wakenet_model/wn9_nihaoxiaozhi_tts`，284 KB），
引擎走"无 AFE"的 lite 路径（`main/audio/engines/lite_audio_engine.cc` +
`main/audio/wake_words/esp_wake_word.cc`，2.x API：`esp_srmodel_init` /
`esp_srmodel_filter(..., ESP_WN_PREFIX, ...)` / `esp_wn_handle_from_name`）。

**它存在哪**：**不占独立分区**。构建时 `scripts/build_default_assets.py` 按 esp-sr
`pack_model.py` 的同一格式把它打成 `srmodels.bin`，作为 assets 里的一个文件
（`index.json` 的 `srmodels` 项）进 `build\generated_assets.bin` → 刷到
**assets 分区 0x800000**（8 MB）。运行时 `Assets::LoadSrmodelsFromIndex()`
（`main/assets.cc:110-131`）把这块 mmap 数据交给 `srmodel_load()`，再经
`AudioService::SetModelsList()`（`main/audio/audio_service.cc:855-861`）传给引擎。
esp-sr 自己也支持"独立 model 分区"，但那要求 `CONFIG_PARTITION_TABLE_CUSTOM` **且**
分区表里真的有 `model` 行（`managed_components/espressif__esp-sr/CMakeLists.txt:102-126`）；
本板按仓库 v2 布局的惯例（同 `espressif/esp-hi`、`m5stack/stick-s3`）用 assets，不改分区表。

**刷机清单（5 项，偏移与以前完全一样）**：

| 偏移 | 文件 | 本次变化 |
| --- | --- | --- |
| 0x1000 | `bootloader\bootloader.bin` | 否 |
| 0x8000 | `partition_table\partition-table.bin` | 否（分区表内容也没变） |
| 0xd000 | `ota_data_initial.bin` | 否 |
| 0x20000 | `xiaozhi.bin` | 内容变（唤醒词代码） |
| **0x800000** | **`generated_assets.bin`** | **内容变（多了 284 KB 模型）** |

**开机日志验收点**（COM25 @115200，`capture-serial.ps1`）：

```
I (xxxx) Assets: The assets map size is 1600 KB (partition 8192 KB)   ← 原来是 1280 KB
I (xxxx) MODEL_LOADER: Successfully load srmodels                     ← 关键：模型装上了
I (xxxx) LiteAudioEngine: Initialized, WakeNet: yes                   ← 关键：引擎起来了
I (xxxx) EspWakeWord: Wake word(wn9_nihaoxiaozhi_tts),freq: 16000, chunksize: 320
I (xxxx) xiaomodou_b220: auto-listen: DISABLED (B220_AUTO_LISTEN=0)   ← 自动监听已关
```

然后在待机状态对麦克风说**"你好小智"**，应出现：

```
I (xxxx) Application: Wake word detected: 你好小智 (state: 0)
```

**对不上时的排查表**：

| 日志现象 | 含义 / 处理 |
| --- | --- |
| `The assets map size is 1280 KB` | 刷进去的 assets 还是旧的（没带模型）→ 重建 `generated_assets.bin` 再刷 |
| `MODEL_LOADER: Can not find model in partition table` | 走到了"独立 model 分区"后备路径（本板没有该分区）。正常流程不该出现；若反复出现说明 assets 加载晚于引擎初始化，请反馈 |
| `Failed to initialize standalone WakeNet` / `No WakeNet model found` | assets 里没有 `srmodels` 或模型名不对 → 先做 §4.4 的自检 |
| 日志里根本没有 `Wake word(wn9_...)` | `CONFIG_USE_ESP_WAKE_WORD` 没生效 → 看 `build\config\sdkconfig.json` 里 `USE_ESP_WAKE_WORD=True`、`WAKE_WORD_DISABLED=False` |
| 装上了但一直唤不醒 | 先查 mic 通路（§5 的 mic 条目）与底噪/削顶；检测阈值写死 `DET_MODE_95`（`esp_wake_word.cc:47`），偏严 |

**16 kHz 是自动的，板级无需改动**：本板 codec 输入 48 kHz（`config.h` 的
`AUDIO_INPUT_SAMPLE_RATE`），`AudioService::Initialize()` 在
`input_sample_rate() != 16000` 时建 `esp_ae_rate_cvt` 重采样器
（`main/audio/audio_service.cc:73-78`），`ReadAudioData(data, 16000, samples)`
把 48 kHz 降到 16 kHz（`audio_service.cc:202-219`），再按 10 ms 一帧喂引擎
（`audio_service.cc:310-316`）。

---

## 5. 故障排查

| 现象 | 原因 / 处理 |
| --- | --- |
| `No serial data received` | 没进下载模式（§2）；或 TX/RX 接反；或 GND 没共地 |
| `Invalid head of packet (0x2B/0xFE)` | 读到的是**应用日志**而不是 ROM 响应：芯片在跑固件，不是下载模式 |
| 端口打不开 / 拒绝访问 | 别的串口工具占着 COM25（串口助手、monitor 等），先关掉 |
| 写一半失败 | 降速 `--baud 460800` 或 `115200`；缩短飞线；确认 TTL 是 3.3 V 档 |
| `Corrupt data, expected 0x1000 bytes but received 0xffd bytes` | 同上：这是串口链路误码（读到一半丢字节），**降波特率重试**即可，不是芯片问题 |
| `Packet content transfer stopped` | 芯片在命令执行中途复位了（例如 `write_mem 0x3FF48000 0x80000000` 软复位），属预期 |
| 开机第一行 `flash read err, 1000` 然后 `rst:0x10 (RTCWDT_RTC_RESET)` | **本机已知的 Flash 冷启动裕量问题**（原厂固件同样现象），靠看门狗复位自愈，不是刷坏了 |
| 刷完屏幕黑但能连上服务器 | 背光在 AW9106（I2C 0x5B）DIM0/DIM1/DIM2；若黑屏先查这一处 |
| 设备完全不启动 | 重新进下载模式（§2），再执行 §4.3 整片恢复 |

---

## 6. 两种分区布局对照（写回/切换时别弄混）

| 区域 | 原厂布局（备份） | 小智布局（`partitions/v2/16m.csv`） |
| --- | --- | --- |
| bootloader | 0x1000 | 0x1000 |
| 分区表 | 0x8000 | 0x8000 |
| nvs / otadata / phy | 0x9000 / 0xd000 / 0xf000 | 同 |
| 应用 | `wifi`(ota_0) 0x10000，3.75 MB | `ota_0` 0x20000，3.9 MB |
| 备用应用 | `wifi_1`(ota_1) 0xc40000，空 | `ota_1` 0x410000，空 |
| 音频资源 | `flashTone` 0x42d000，8 MB | `assets`(spiffs) 0x800000，8 MB |
| 厂商私有 | `Iram0`/`Dram0`/`Dram1`/`profile` | 无 |

⇒ 两套布局**不兼容**，所以从原厂切到小智（或反向）时，**整片擦除 + 整片写回**是最省心的做法。
小智固件的刷写参数与原厂镜像头一致：`--flash-mode dio --flash-freq 80m --flash-size 16MB`。

---

## 7. 实操记录表（每次刷机填一行）

| 日期 | 方向 | 命令/脚本 | 结果 | 备注 |
| --- | --- | --- | --- | --- |
| 2026-10-07 | 原厂 → 小智 | `flash-b220.ps1 -FullErase` | ✅ `Hash of data verified.` | 首次切换 |
| 2026-10-07 | 小智迭代 | `flash-b220.ps1` | ✅ 读回 MD5 一致 | 背光/分辨率修正 |
| 2026-10-07 | 小智 → 原厂 | `write_flash 0x0 backup\esp32-16MB-full.bin` | ⏳ | 本次恢复 |
| 2026-10-09 | 小智迭代（唤醒词） | 源码改动（`config.json` / `config.h`）+ `flash-b220.ps1` | ⏳ 待刷写 | 模型进 assets（偏移未变）；`B220_AUTO_LISTEN 0`；用 `--wake-word nihaoxiaozhi` 构建 |

---

## 8. 相关文档

- `PHASE1-FLASH-VERIFY.md` —— 写入能力验证的完整清单（含 10 项诊断结论）
- `flash-b220.ps1` —— 小智固件的一键刷写 + 读回校验脚本
- `capture-serial.ps1` —— 抓启动日志（默认 115200，输出到 `logs\`）
- `PORTING-FEASIBILITY.md` —— 移植可行性、引脚逆向、风险与阶段计划
- `DEVICE-ANALYSIS.md` / `GPIO-FINDINGS.md` / `ZL38063-ANALYSIS.md` / `AW9106-FINDINGS.md` —— 原厂固件逆向细节
