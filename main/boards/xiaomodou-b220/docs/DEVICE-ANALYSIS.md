# 小魔豆（B220）设备分析报告

分析对象：`backup\esp32-16MB-full.bin`（16,777,216 字节，整片完整）
全文件 MD5：`a6a27d2b4b7a8de36636e9ef635bea40`
日期：2026-10-07

---

## 1. 设备身份

| 项目 | 结论 | 依据 |
| --- | --- | --- |
| 产品 | **小魔豆 AI 百变机器人**（配套手机 App 同名） | 网络检索 |
| 板号 / 固件版本 | **B220_V01B97**（另有 `B220_V01B100` 字符串） | 应用内 `AT+Ver:` 后的版本串 |
| 固件工程名 | **SmartAlarmDevice**（智能闹钟类设备） | 编译路径 `/mnt/2018/dev/SmartAlarmDevice/` |
| 音频框架 | **ESP-ADF**（LyraT-Mini / LyraTD-MSC 板级支持）+ 自研 `esp-audio-app` | 应用内日志 `[APP] This board is %s` 的板名为 `ESP_LYRATD_MSC_V2_1`；播放器路径 `/home/donglianghao/esp/audio_app/esp-audio-app/` |
| 芯片 | ESP32-D0WD（双核，经典 ESP32） | 引导日志 |
| Flash / PSRAM | 16 MB / 8 MB | 引导日志 |
| MAC | `b4:e6:2d:fc:4c:e5` | NVS（Espressif OUI） |
| ESP-IDF | **3.x 时代的定制分支** `62aaac8b-dirty` | 应用镜像内**不含** v4.0 才引入的 `esp_app_desc_t`（0xABCD5432），且使用 `tcpip_adapter`/`bluedroid` 等 3.x 组件 |

**云端与 AI 服务**

- `http://xiaop.onegohome.com/api/api/devlogin` —— 厂商云登录（Onego）
- `smartdevice.ai.tuling123.com` —— **图灵机器人** AI 语音服务
- `cn.ntp.org.cn` —— NTP

---

## 2. 系统架构（从固件内源码路径还原）

编译路径把整个软件结构暴露了出来，`TuLing/components/` 下是自研组件：

```
SmartAlarmDevice/
├── TuLing/components/
│   ├── AppService/AppControlService.c          应用总控
│   ├── DeviceController/
│   │   ├── DeviceController.c
│   │   ├── WifiManager/{WifiManager,wifismartconfig}.c   WiFi + SmartConfig 配网
│   │   ├── TouchManager/{TouchManager,ble}.c             触摸 + BLE 配网
│   │   ├── SDCardManager/SDCardManager.c                 SD 卡
│   │   ├── DspManager/DspManager.c                       音频 DSP
│   │   ├── AuxManager/AuxManager.c                        AUX 输入
│   │   └── PowerManager/PowerManager.c                    电源管理
│   ├── MediaHal/DSP/zl38063/vproc_common.c               Microsemi ZL38063 语音 DSP
│   ├── OnegoAlarmService/{OnegoAlarmService,OnegoAlarmparser}.c   云端闹钟
│   ├── OnegoOtaService/OnegoOtaService.c                 云端 OTA
│   ├── SleepModeService/SleepService.c                   休眠
│   ├── TouchService/TouchControlService.c
│   ├── TuringRobotService/{AiwifiController,Turing_wakeup}.c     图灵 AI 模块 + 唤醒
│   ├── UartControlService/{BaseBoardManager,CmdPackedService,UartControlService}.c  底座控制
│   ├── AlarmService/AlarmService.c
│   └── lcd/{lcdService,spi_lcd}.c                         SPI 屏
```

**关键结论：USB 口为什么不能刷机**

固件里存在一整套 `{AT+...}` 串口协议（`UartControlService` / `CmdPackedService`），用于控制**底座（BaseBoard）**，同时 `AiwifiController.c` 负责与**图灵 AI WiFi 模组**通信。也就是说：

- ESP32 是主控，通过 UART 与「底座」和「AI 模组」通信；
- 我们先前看到的 `{AT+Pon}` 刷屏，正是 ESP32 在向底座/AI 模组发 `AT+Pon`（上电握手），并不是它在等主机命令；
- 那条 micro-USB 属于另一颗芯片（USB CDC 虚拟串口 `\Device\VCP0`），它的 TX 方向没接到 ESP32 的 UART0，所以 PC 的命令永远到不了 ESP32 —— 与实测的 10 项证据完全吻合。

**底座 AT 协议命令表**（从固件提取）

| 命令 | 推测含义 |
| --- | --- |
| `AT+Pon` / `AT+PowerOn` | 上电握手 |
| `AT+StartUp` / `AT+Run` / `AT+Stop` / `AT+Rst` / `AT+Reboot` | 启停/复位 |
| `AT+LMY` / `AT+LMN` | 左电机 开/关 |
| `AT+RMY` / `AT+RMN` | 右电机 开/关 |
| `AT+HLY` / `AT+HLN` | 头部左 开/关 |
| `AT+TLY` / `AT+TLN` | 尾部左 开/关 |
| `AT+MTO` / `AT+CMD:MTOY` / `AT+CMD:MTON` | 动作/电机动作 |
| `AT+FTOID=` / `AT+OID=` | 下发 OID（对象 ID，用于 LED 效果/曲目） |
| `AT+Ver` / `AT+Ver:` | 版本查询 → `B220_V01B97` |
| `AT+CMD=(%d,%d,%d,%d),(%d,%d,%d,%d,%d)` | 参数化动作指令 |
| `AT+CMD:OK` / `AT+OK` | 应答 |

这说明「百变机器人」是靠底座上的多路电机/头部/尾部动作来实现变形与行走的。

---

## 3. Flash 分区逐个解析

| # | 分区 | 类型 | 偏移 | 大小 | 实际内容 |
| --- | --- | --- | --- | --- | --- |
| 0 | nvs | data/nvs | 0x009000 | 16 KB | WiFi 凭据、BLE 密钥、PHY 校准（见 §5） |
| 1 | otadata | data/otadata | 0x00d000 | 8 KB | **全空** → 引导器默认启动 ota_0 |
| 2 | phy_init | data/phy | 0x00f000 | 4 KB | **全空** → RF 校准运行时生成 |
| 3 | **wifi** | app/ota_0 | 0x010000 | 3.75 MB | **主固件**（明文，未加密，见 §6） |
| 4 | Iram0 | data/0x20 | 0x3d0000 | 128 KB | 87% 空白；尾部有一小段**残缺的同款音频归档**（含 `LAME3.99r`/`Xing` MP3 数据） |
| 5 | Dram0 | data/0x21 | 0x3f0000 | 128 KB | **高熵不透明数据**（熵 7.96，无任何已知文件头） |
| 6 | Dram1 | data/0x22 | 0x410000 | 112 KB | 同上（熵 7.95） |
| 7 | profile | data/0xff | 0x42c000 | 4 KB | 同上（熵 7.93） |
| 8 | **flashTone** | data/nvs_keys | 0x42d000 | **8 MB** | **打包的 74 个音频文件**（见 §4），占据 93% |
| 9 | coredump | data/coredump | 0xc2d000 | 64 KB | 一次崩溃的现场（见 §6） |
| 10 | wifi_1 | app/ota_1 | 0xc40000 | 3.75 MB | **全空 `0xFF`** → 备用 OTA 槽位从未写写过 |

要点：分区的**类型/子类型是厂商乱填的**（flashTone 用了 `nvs_keys`，Iram0/Dram0/Dram1/profile 用了未定义类型 0x20/0x21/0x22/0xff），只有名字有意义。

---

## 4. flashTone：8 MB 音频库（已完整解出）

### 归档格式（逆向结果）

分区开头是一张索引表，**每条 64 字节**：

```
偏移 +0  u32  magic（第 0 条为 "SJ\0"/"SF\0"，其余为 0）
偏移 +4  u32  0
偏移 +8  u32  索引字段（0x28 + 0x100×序号）
偏移 +12 u32  文件在分区内的偏移
偏移 +16 u32  文件长度
其余 44 字节为 0
```

音频数据从 0x1288 开始。这与固件里的 `FlashTracker.c`（播放列表可来自 flash 或 SD 卡）对应。已按「偏移+长度」**无损解包全部 74 个文件**，长度与索引完全吻合。

### 内容清单（`backup\tones\`，共 7,814,720 字节 = 分区的 93%）

| 类别 | 数量 | 说明 |
| --- | --- | --- |
| 带 ID3 标签的 MP3 | 64 | 儿歌、音效、语音提示 |
| 无标签的 MP3 | 5 | 厂商自制 TTS 提示音 |
| XML（不是音频） | 5 | **制作时误打包的 Audacity 工程文件** |

能读出的曲名/来源：

- `最美的光` —— 尚琪祺（儿歌）
- `小宝贝（私奔版）` —— 专辑《私奔丽江》，夏天播放
- `站长素材(sc.chinaz.com)` —— 多条，来自免费素材站「站长素材」
- `卡通特色音效-87.mp3` —— artist `flash8`（闪吧素材站）
- 5 个 Audacity 工程：`42Police`、`45Truck`、`46Racing`、`47Fire_engine`、`48Helicopter`
  → 一套**交通工具音效包**（警车/卡车/赛车/消防车/直升机），22050 Hz

结论：这是一个典型的「免费素材 + 厂商 TTS 提示音」拼装产品，连音频制作时的中间文件都一起烧进固件了。

完整索引见 `backup\tones-index.csv`（序号、偏移、长度、类型、标题、艺人、专辑）。

---

## 5. NVS：设备里存了什么

用 ESP-IDF 自带 `nvs_partition_tool` 解析：

| 命名空间 / 键 | 值 |
| --- | --- |
| `nvs.net80211:sta.ssid[0]` | **`yangsibei`** |
| `nvs.net80211:sta.pswd[0]` | **`hyx19980513`** |
| `storage:flash_op[0]` | 厂商配置块，同样内含上面这组 SSID/密码 |
| `nvs.net80211:sta.mac[0]` | `b4:e6:2d:fc:4c:e5` |
| `nvs.net80211:ap.mac[0]` | `b4:e6:2d:fc:4c:e6` |
| `bt_config.conf:bt_cfg_key0[0]` | BLE 的 IRK / IR / DHK / ER 四把密钥（明文） |
| `phy:cal_data[0]` / `phy:cal_version` | RF 校准数据（version 4009） |
| `misc:log[0]` | 运行日志游标 |

配网方式为 **SmartConfig**（`esp_smartconfig_start`）+ BLE 辅助，所以设备里直接落地了明文 WiFi 密码。

⚠️ 提醒：备份文件里包含**明文 WiFi 密码和蓝牙密钥**。如果这台设备不是你自己一直用的，建议换掉对应网络的密码；分享备份前也记得先删掉这一块（`dd`/`flash_erase` 掉 `0x9000` 开始 16 KB，或先恢复出厂）。

---

## 6. 崩溃转储与安全性

### coredump（0xc2d000，64 KB，非空）

里面保存了一次崩溃时的任务现场，能读出任务名清单：

```
rec_engine_task      turing_wakeup_task   SDCardTrackerSc(an)
main                 task_player_mid      IDLE0 / IDLE1
buttonTask           Tmr Svc              OnegoOtaService
eventTask            MediaCtrlTask        WifiManagerEvtT(ask)
PlaylistManager      ipc1                 hciHostT / hciH4T
btuT                 Btc_task             wifi
esp_timer            btController         rec_engine_task
```

（任务名首字符被格式截断成 `?`。）这份清单等于把固件的运行时结构又确认了一遍：录音引擎、图灵唤醒、播放列表、蓝牙协议栈、OTA、按键、WiFi 事件。没有 ELF 符号表，无法还原调用栈；如果需要精确回溯，得拿原厂 `.elf`。

### 安全姿态

| 项目 | 结论 |
| --- | --- |
| Flash 加密 | **未启用** —— 应用镜像在 0x10000 处是明文 `0xE9` 镜像，esptool 校验 checksum 与 SHA256 均通过 |
| Secure Boot | **未见启用迹象** —— 引导器内没有签名校验相关字符串；分区表也没有 secure 专用布局 |
| 应用描述符 | 不存在（因为 IDF 3.x 时代没有这个结构） |
| OTA | 双槽 OTA + 云端 `OnegoOtaService`；`ota_1` 空槽可用 |

**这意味着固件可以被自由修改并回写。**

---

## 7. 仍未解开的三个分区

`Dram0`(128 KB)、`Dram1`(112 KB)、`profile`(4 KB) 是**整片 Flash 里唯一的谜**：

- 熵值 7.93–7.96，接近理论上限；
- 没有任何已知文件头（非 gzip/zlib/lz4/xz/zip/ELF/ESP 镜像）；
- 无重复 16 字节块（排除 AES-ECB）；
- 对 1/2/4/8/16/32 字节位移做异或后熵值依旧接近 8（**排除重复密钥异或混淆**）；
- 应用里也搜不到按名字引用这些分区的代码。

最可能的解释：**AES 加密的云端下发内容**（故事/专辑，配合 `story_player`、`albumUid/songUid`、`Downloaded %d bytes` 等字符串）或厂商加密数据。

好消息：**这三个分区已在整片备份里原样保留**，不影响恢复；只是暂时读不懂内容。若日后想破解，切入点是在应用里定位 AES 密钥（明文固件可反汇编）。

---

## 8. 后续可以做什么

1. **恢复出厂**：`esptool write_flash 0x0 backup\esp32-16MB-full.bin`（整片回写，含 NVS/音频/OTA 状态）。
2. **换掉开机音频**：重打包 flashTone 归档（保持「64 字节索引 + 偏移 + 长度」格式），或走 `/sdcard/story` 与 `/playlist_*.ini` 的 SD 卡路径，不动固件。
3. **改固件**：镜像明文且无 Secure Boot，可直接 patch 后写入 `ota_0`（0x10000）。反汇编时按段加载：
   - DROM `0x3f400020`、DRAM `0x3ffbdb60`、IROM `0x400d0018`、IRAM `0x40080000`
   - 入口 `0x40081814`，共 7 段（`esptool image_info` 可复现）
4. **继续逆 AT 协议**：底座动作指令表已在 §2，配合 `CmdPackedService.c` 的字符串可还原完整指令集，做成自己的底座控制器。
5. **隐私清理**：先清 NVS 再分享备份。

---

## 9. 本次分析产物

| 路径 | 内容 |
| --- | --- |
| `backup\esp32-16MB-full.bin` | 整片原始备份（16 MB，MD5 `a6a27d2b4b7a8de36636e9ef635bea40`） |
| `backup\partitions.csv` | 分区表 |
| `backup\parts\*.bin` | 11 个分区分别切出，便于单独分析 |
| `backup\tones\*.mp3` | **74 个音频文件（已解包）** |
| `backup\tones-index.csv` | 音频索引 + ID3 标签 |
| `backup\app-strings.txt` | 应用镜像全部 ASCII 字符串（17,066 条） |
| `backup\app-cn-strings.txt` | GBK 中文串尝试提取结果（多为二进制误判，参考价值有限） |
| `tools\analyze_dump.py` | 分区切分 + 熵值/格式画像 |
| `tools\unpack_tones.py` | flashTone 归档解包（含长度校验） |
| `tools\list_mp3.py` / `tones_index.py` | ID3 解析（GBK 兼容） |
| `tools\strings_report.py` / `cn_strings.py` | 字符串分类提取 |
| `tools\verify_backup.py` | 备份校验 / 分区表解析 |

---

## 10. 一句话总结

**小魔豆 B220_V01B97 是一台 2018–2019 年基于定制 ESP-IDF 3.x 的 AI 变形机器人玩具**：ESP32-D0WD 主控 + 图灵 AI WiFi 模组 + Microsemi ZL38063 语音 DSP + SPI 屏/触摸/BLE/云闹钟，底座通过 `{AT+...}` 协议驱动多路电机完成变形；固件未加密未签名，8 MB 音频区里躺着 74 个免费素材拼成的儿歌与音效（连 Audacity 工程文件都没删），另有约 240 KB 加密内容尚未解开。
