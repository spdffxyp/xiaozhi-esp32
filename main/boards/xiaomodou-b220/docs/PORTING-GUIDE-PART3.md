# 小魔豆 B220 → 小智（xiaozhi-esp32）移植实战指南 · PART 3

> **把移植做成一台"真的小智"**：生产级音频 codec、联网与服务器、唤醒词，外加合并排错表、经验方法论与验收清单。
>
> 阅读前提：PART 1（可行性 + 引脚逆向 + 刷机）、PART 2（板级骨架：屏幕 / 背光 / WiFi / DSP host-boot）。
> 本部分只写**让设备成为一台可用语音助手**所需的最后一层，以及这一层的所有坑。
>
> 证据标注约定（全文统一）：
> - **[实测]** — 在本机 B220 上跑过、有串口日志或仪表读数。
> - **[逆向]** — 从原厂固件 `backup/esp32-16MB-full.bin` 反汇编/数据挖掘得到。
> - **[推断]** — 由已知事实推导，尚未单独验证。
>
> 本机基线：MAC `b4:e6:2d:fc:4c:e5`，刷机口 **COM25**（3.3 V TTL，`--before no_reset`），
> ESP-IDF **v6.1**，应用版本 `2.5.1`，ESP32-D0WD rev1.0 / 16 MB flash / 8 MB PSRAM。

---

## 0. PART 3 的边界：PART 2 结束时还缺什么

PART 2 结束时设备的真实状态是：屏幕亮、背光亮、WiFi 能连、PSRAM 与 240 MHz CPU 正常、
ZL38063 已经 host-boot 成功并能通过 `reg 0x0014 == 0x0200` 证明"应用 READY"——
但**音频还是 `DummyAudioCodec`**，说话没人听、回答没人听、也没有唤醒词。

PART 3 要补的正是这三块：

| 缺口 | 本部分章节 | 完成判据（日志） |
| --- | --- | --- |
| 没有真的音频 codec | §1 / §2 / §3 | `Zl38063AudioCodec: audio bring-up: I2S0 tx/rx handles are up` |
| 没有连上服务器 | §4 | `MQTT: Session ID: xxxxxxxx` + `StateMachine: activating -> idle` |
| 没有唤醒词（也没有按键） | §5 | `Application: Wake word detected: …` |

这三件事**互相依赖**：唤醒词要 16 kHz 单声道（由 codec 的采样率声明驱动重采样），
服务器要 24 kHz 播放（由 codec 的 48 kHz 输出重采样），功放要一次性使能（由 codec 的
`EnableOutput(true)` 触发）。所以顺序必须是 **codec → 服务器 → 唤醒词**。

---

## 1. 写一个真实的 `AudioCodec` 子类

### 1.1 要实现的接口（直接引自 `main/audio/audio_codec.h`）

`AudioCodec` 是一个只有两个纯虚函数的抽象基类，其余都可覆盖：

```cpp
class AudioCodec {
public:
    virtual void SetOutputVolume(int volume);      // 基类：存 NVS + 打日志
    virtual void SetInputGain(float gain);         // 本板不用
    virtual void EnableInput(bool enable);         // 基类：只置标志
    virtual void EnableOutput(bool enable);        // 基类：只置标志

    virtual void OutputData(std::vector<int16_t>& data);   // 基类 → Write()
    virtual bool InputData(std::vector<int16_t>& data);    // 基类 → Read()
    virtual void Start();                                   // 基类：读 NVS 音量

    // 只读属性，音频服务靠它们决定要不要重采样
    inline bool duplex() const;
    inline int  input_sample_rate() const;
    inline int  output_sample_rate() const;
    inline int  input_channels() const;
    inline int  output_channels() const;

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;
    bool duplex_          = false;
    int  input_sample_rate_  = 0;
    int  output_sample_rate_ = 0;
    int  input_channels_  = 1;
    int  output_channels_ = 1;

    virtual int Read(int16_t* dest, int samples) = 0;     // 唯一必写的两个
    virtual int Write(const int16_t* data, int samples) = 0;
};
```

关键点：**`Read` / `Write` 是 `protected` 且是纯虚**。外部永远通过
`InputData()` / `OutputData()` 调用它们（`audio_codec.cc:17-27`），而这两个包装会处理
`std::vector` 的尺寸与返回值。B220 的实现文件是
[zl38063_audio_codec.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063_audio_codec.h) 与
[zl38063_audio_codec.cc](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063_audio_codec.cc)。

本板重写了 6 个：`Read`、`Write`、`EnableOutput`、`EnableInput`、`SetOutputVolume`、`Start`。
`OutputData` / `InputData` / `SetInputGain` 用基类即可。

### 1.2 三个必须设置对的成员（否则声音一定错）

| 成员 | 值 | 理由 | 证据 |
| --- | --- | --- | --- |
| `duplex_` | `true` | ESP32 是 I2S0 **master**，BCLK/WS 由 TX 产生、RX 借用同一组时钟；声明全双工后音频服务不会去关 TX 时钟 | **[实测]** `W (1204) i2s_common: the rx channel on I2S0 is switched from master to slave for full-duplex mode` |
| `input_channels_` | `2` | ZL38063 把**同一个样本同时驱动到 L/R 两个 slot**（回环日志里 RX 十六进制是成对重复：`FFED FFED FFF2 FFF2`）；声明 2 通道后音频服务的"取左声道"路径是**无损**的 | **[实测]** [mic-loopback.log](logs/mic-loopback.log) |
| `output_channels_` | `1` | Opus 解码器按 `ESP_AUDIO_MONO` 打开，`Write()` 收到的是**单声道 PCM**，由 codec 自己复制到两个 slot | **[实测]** 1 kHz 测试音写法相同（"the same sample, no channel swap"） |

`input_sample_rate_` / `output_sample_rate_` 来自板级 `config.h` 宏，见 §2（**这一条最贵**）。

### 1.3 核心：**复用板级已经建好的 I2S 句柄，绝不 `i2s_new_channel`**

这是整个 codec 最重要的一条设计约束，理由有两条：

1. **一个 port 只能建一次 channel。** `i2s_new_channel()` 对同一个 I2S 控制器第二次调用必然失败。
   I2S0 已经被板级的 `B220_I2sUpBeforeDspBoot()`（[xiaomodou_b220.cc](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/xiaomodou_b220.cc) 第 574 行起）占用了。
2. **DSP 需要时钟先于它自己的复位。** ZL38063 是 I2S **slave**，没有自己的 MCLK，
   它从上电起就靠 ESP32 的 BCLK/WS 活着；`B220_I2sUpBeforeDspBoot()` 在 `zl38063_dsp_begin()`
   **之前**建好 48 kHz/16-bit/stereo 的全双工口并 `i2s_channel_enable()`，正是为了让 DSP 能应答 HBI。
   **[实测]** 这是 Phase 3 的关键突破：I2S 先起，host-boot 才从"读不到 `0xD3D3`"变成"上传成功"。

共享接口放在
[b220_audio_share.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/b220_audio_share.h)，
用 `extern "C"` 声明，**板级文件和 C++ codec 都 include 它**——签名漂移变成编译错误而不是链接惊喜：

```c
/* b220_audio_share.h */
#ifdef __cplusplus
extern "C" {
#endif
i2s_chan_handle_t b220_audio_i2s_tx(void);   // NULL = bring-up 没跑或失败
i2s_chan_handle_t b220_audio_i2s_rx(void);
bool b220_audio_dsp_ready(void);             // reg 0x0014 == 0x0200 才是 true
esp_err_t b220_audio_pa_enable_once(const char* tag);   // 幂等（见 §3）
#ifdef __cplusplus
}
#endif
```

实现侧（`xiaomodou_b220.cc:1298-1324`）就是"把一个静态句柄交出去"：

```cpp
extern "C" i2s_chan_handle_t b220_audio_i2s_tx(void) {
#if B220_I2S_BEFORE_DSP
    return s_i2s_tx;
#else
    return nullptr;   /* 关掉 I2S-before-DSP 时明确给 NULL，而不是野指针 */
#endif
}
extern "C" esp_err_t b220_audio_pa_enable_once(const char* tag) {
    return b220_pa_enable_one_pulse(tag);
}
```

消费侧（codec 构造函数，成员初始化列表里就取句柄）：

```cpp
Zl38063AudioCodec::Zl38063AudioCodec(int input_sample_rate, int output_sample_rate)
    : tx_handle_(b220_audio_i2s_tx()), rx_handle_(b220_audio_i2s_rx()) {
    duplex_ = true;
    input_reference_  = false;
    input_channels_   = 2;      /* 见 §1.2 */
    output_channels_  = 1;
    input_sample_rate_  = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    ReportBringupOnce();
}
```

codec 在**析构函数里什么都不做**——这是刻意的：

```cpp
Zl38063AudioCodec::~Zl38063AudioCodec() {
    /* 不 disable / 不 delete / 不 re-init：
     * DSP 是 I2S slave，时钟停了它就掉应用，而这是它直到断电都需要的时钟。 */
}
```

> ⚠️ **不要**照抄绝大多数板子的 `i2s_channel_disable(tx_handle_)` 式析构。B220 上那样做等于
> 把正在运行的 DSP 时钟掐掉。同理 `EnableInput(false)` 也**不**去 disable RX channel。

### 1.4 失败模式：句柄为 NULL 时只打一次日志，然后返回 −1

板级的 RX enable 是**非致命**的（`ESP_LOGW(TAG, "I2S: RX enable failed …; TX keeps the clock running")`），
所以"tx 有效、rx 为 NULL"是**合法状态**。codec 必须能在这个状态下不死机、不崩、不刷屏：

```cpp
void Zl38063AudioCodec::ReportBringupOnce() {
    if (reported_) return;                     /* 一辈子只打一次 */
    reported_ = true;
    if (tx_handle_ == nullptr || rx_handle_ == nullptr) {
        ESP_LOGE(TAG, "audio bring-up did NOT leave a usable I2S0 pair (tx=%p rx=%p): "
                      "B220_I2sUpBeforeDspBoot() failed or never ran.  This codec does "
                      "NOT create a channel of its own (a second i2s_new_channel() on "
                      "I2S0 would fail), so audio input returns silence (-1/0) and "
                      "playback is dropped; the rest of the firmware keeps running.",
                 (void*)tx_handle_, (void*)rx_handle_);
    } else {
        ESP_LOGI(TAG, "audio bring-up: I2S0 tx/rx handles are up (tx=%p rx=%p)", …);
    }
    if (!b220_audio_dsp_ready()) {             /* 只是警告，从不拒绝运行 */
        ESP_LOGW(TAG, "the ZL38063 application is not proven ready (reg 0x0014 != 0x0200 …)");
    }
}
```

`Read()` / `Write()` 开头的守卫：

```cpp
int Zl38063AudioCodec::Read(int16_t* dest, int samples) {
    if (dest == nullptr || samples <= 0) return 0;
    if (rx_handle_ == nullptr) { ReportBringupOnce(); return -1; }   /* 绝不碰 NULL 句柄 */
    …
}
```

三条设计原则，值得照搬到任何板子：

- **绝不在音频任务里分配内存。** `Write()` 用固定成员 `int16_t tx_block_[240*2]` 做暂存，
  不是每次 `std::vector`。音频输出路径每 60 ms 走一次，堆分配会碎。
- **单次调用有硬预算。** `Read()` 200 ms、`Write()` 1 s 上限（`esp_timer_get_time()` 判定），
  超了就"有多少交多少"。音频任务被一次卡死会让整个 `AudioService` 停摆。
- **错误只报一次。** `rx_warned_` / `tx_warned_` 两个 bool，避免 48 kHz 下每秒几千行日志。

### 1.5 帧与速率：所有尺寸都从 DMA 配置推导，不要写死魔数

| 常量 | 值 | 来源 |
| --- | --- | --- |
| `B220_CODEC_I2S_SLOTS` | 2 | 端口是 stereo |
| `B220_CODEC_FRAME_BYTES` | 4 | 2 slot × `sizeof(int16_t)` |
| `B220_CODEC_BLOCK_FRAMES` | **240** | 恰好是端口 `dma_frame_num = 240`（`B220_I2sUpBeforeDspBoot()`），即 5 ms 音频 |
| `B220_CODEC_BLOCK_BYTES` | 960 | 240 × 4 |
| `B220_CODEC_READ_TIMEOUT_MS` | 100 | 单次 `i2s_channel_read` 超时 |
| `B220_CODEC_READ_BUDGET_US` | 200000 | 一次 `Read()` 的总预算 |
| `B220_CODEC_WRITE_TIMEOUT_MS` | 200 | 单次 `i2s_channel_write` 超时 |
| `B220_CODEC_WRITE_BUDGET_US` | 1000000 | 一次 `Write()` 的总预算 |

**按 DMA block 分块**的原因：`i2s_channel_read/write` 只会按整描述符推进，
一次给 960 字节正好一个描述符，短传输永远不会切裂一个 stereo frame。
`Read()` 里另有一道保护——**奇数样本请求直接丢掉尾巴一个**（`if (samples & 1) --samples;`），
否则 L/R 从此整体旋转。音频服务总是请求整数帧，所以这是防御而非正常路径。

`Read()` 的正常返回是"写入 `dest` 的 int16 个数"（交错 L/R），`Write()` 返回"消费的**单声道**样本数"。
`ESP_ERR_TIMEOUT` 在 `Read()` 里**不是错误**（RX 由 TX 时钟驱动，没数据往往只是端口没跑），
收集到什么返回什么。

### 1.6 `Start()` 与 `SetOutputVolume()`：诚实地"什么都做不了"

```cpp
void Zl38063AudioCodec::Start() {
    AudioCodec::Start();      /* 基类：读 NVS 音量 + 打 "Audio codec started" */
    ReportBringupOnce();
    ESP_LOGI(TAG, "ZL38063 codec started: %d Hz / 16-bit / 2 slot(s); …");
}
```

`SetOutputVolume()` 是这块板子最反直觉的一个：**数值被存进 NVS（重启后保留、UI 可见），
但完全不影响音量**——真正的输出电平在 DSP 内部（`reg 0x238/0x23A`，`-90 dB + vol`），
由 boot 时灌入的配置记录 / `zl38063_dsp_set_volume()` 写死。所以实现是
"调基类 + 一条 `ESP_LOGW` 说明它不生效"。**不要**在这里做软件增益：那要给每个播放样本做乘法，
而已经验证过的通路并不那么做。

---

## 2. ⭐ 采样率这一坑：硬件真的是 48 kHz，而且改不了

### 2.1 结论与证据

`config.h` 里必须是这样（这是**唯一正确**的写法）：

```c
/* Phase 4 (production audio): these are the rates the ONE I2S0 link to the
 * ZL38063 really runs at. */
#define AUDIO_INPUT_SAMPLE_RATE  48000
#define AUDIO_OUTPUT_SAMPLE_RATE 48000
```

证据链（三条独立证据，互相印证）：

| # | 事实 | 证据 |
| --- | --- | --- |
| 1 | 板级初始化就写死 48 kHz，且在 DSP 复位**之前** | **[实测]** `I (1214) XiaoMoDouB220: I2S up before DSP boot: 48 kHz, 16-bit, stereo (bck=5 ws=25 dout=26 din=35)` |
| 2 | 往这个 48 kHz 端口写 **1 kHz 正弦**，喇叭里听到的就是 **1 kHz** | **[实测]** 耳朵判定；软件侧同时证明数据正确：`tx_peak=32759 tx_mean~20855 write_err=0`（`tx_mean/peak = 0.6366 ≈ 2/π`，正是满幅正弦的均值） |
| 3 | 原厂机示波器实测 **I2S_WSA = 48 kHz**，BCLK 1.54 MHz（32 BCLK/frame = 16-bit slot） | **[实测]** 用户示波器，见 [PHASE3-DSP-DEBUG-STATUS.md](PHASE3-DSP-DEBUG-STATUS.md) §2；BCLK 与 slot 宽度在 `i2s-tx-sweep` 期间复核 |

**为什么"改不了"**：`B220_I2sUpBeforeDspBoot()` 在 DSP 复位前就把端口配好，之后**从不重配**。
DSP 是 I2S slave，上电起就必须有 BCLK/WS；把端口停掉去改采样率 = 把 DSP 的应用打断。
而 ZL38063 自己**不做任何采样率转换**（证据 #2：写什么频率就出什么频率）。

### 2.2 如果误写 16000 会怎样（这是真踩过的坑）

`AudioCodec::input_sample_rate()` 是**给音频服务看的声明**，不是"去设置硬件"。
音频服务的逻辑是：**声明值 ≠ 16000 就建重采样器**。

```cpp
/* main/audio/audio_service.cc:73-80 */
if (codec->input_sample_rate() != 16000) {
    esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
        codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
    esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
}
```

所以：

| 声明 | 实际 | 后果 |
| --- | --- | --- |
| 48000 | 48 kHz | 建 48k→16k 重采样器，一切正确 |
| **16000** | **48 kHz** | **不建重采样器**，服务以为拿到的就是 16 kHz；于是采集/播放全部以 **3 倍速度**跑——音调升三个八度都不止，且 Opus 编码器/解码器的时间戳全错 |

同样地，`output_sample_rate()` 决定 Opus 解码器的打开速率（`audio_service.cc:53`），
写错会让播放变调。codec 里因此放了一条**自检警告**：

```cpp
if (input_sample_rate_ != 48000 || output_sample_rate_ != 48000) {
    ESP_LOGW(TAG, "the codec was told %d/%d Hz but the I2S0 port runs at %d Hz "
                  "(B220_I2S_SAMPLE_RATE_HZ) - the audio service will resample, and "
                  "playback/capture would be pitch-shifted if it did not", …);
}
```

看到这条警告 = `config.h` 被人改错了。

### 2.3 小智内部的重采样发生在哪、怎么发生的

三处，全在 [audio_service.cc](xiaozhi-esp32-b220/main/audio/audio_service.cc)：

**(a) 上行 48k → 16k（给唤醒词 / Opus 编码器 / ASR）**
`ReadAudioData(data, 16000, samples)` 是唯一入口：

```cpp
/* audio_service.cc:195-225 */
bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) { … codec_->EnableInput(true); }
    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) return false;
        if (input_resampler_ != nullptr) {
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            esp_ae_rate_cvt_process(input_resampler_, data.data(), in_sample_num, resampled.data(), …);
        }
    } else { … }
    last_input_time_ = std::chrono::steady_clock::now();   /* ← 15 s 掉电计时基准 */
}
```

调用点有两类（`audio_service.cc:294` 与 `:313`）：编码器/ASR 每帧 60 ms、唤醒词 **10 ms（160 样本）**。
**[实测]** 启动日志：`I (7884) AudioService: Resampling audio from 16000 to 48000`（播放侧）与
`I (103764) AudioService: Resampling audio from 16000 to 48000`（唤醒词侧首次分配）。

**(b) 下行 服务器 24k → 设备 48k（播放）**
Opus 解码器以 `codec->output_sample_rate()`（48000）打开，于是 `decoder_sample_rate_ = 48000`；
而服务器音频包自带 24000：

```cpp
/* audio_service.cc:553-563 */
if (decoder_sample_rate_ != codec->output_sample_rate()) { … }
/* 实际生效的是解码后按包内 rate 判断（:430-437） */
```

**[实测]** `I (21074) AudioService: Resampling audio from 24000 to 48000`。

**(c) 那条"无害警告"是怎么来的**

服务器在音频通道打开的 hello 里说 24 kHz，设备的输出声明是 48 kHz，于是：

```cpp
/* main/application.cc:560-568 */
protocol_->OnAudioChannelOpened([this, codec, &board]() {
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
        ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, "
                      "resampling may cause distortion",
                 protocol_->server_sample_rate(), codec->output_sample_rate());
    }
});
```

**[实测]** `W (13924) Application: Server sample rate 24000 does not match device output sample rate 48000, resampling may cause distortion`

> ✅ **这条警告在本板是正常的，不要试图消除它。** 它只是说"服务器 24 k、设备 48 k，中间要重采样"。
> 重采样确实发生了（`decoder` 输出 24 k → `esp_ae_rate_cvt` → 48 k），而且**音质实测可接受**
> （唤醒词对话全程 60 s、多轮问答清晰，见 [wakeword.log](logs/wakeword.log)）。
> 唯一"消除"它的办法是把设备声明成 24 kHz——那会直接违反 §2.1，绝不要做。

---

## 3. 功放使能与"锁存"设计：**整个 boot 只允许一个上升沿**

### 3.1 硬件为什么这么脆

功放是 **AW8737S**。它的 SHDN 引脚（A4）**低有效**，由 AW9106B（I2C `0x5B`）的
group-A 输出 `OUT_A1` = **寄存器 `0x02` 的 bit 1** 驱动。它不是普通 enable：
SHDN 先被拉低超过 TOFF（数据手册建议 1 ms）之后，芯片按**随后的上升沿个数**选工作模式。

| 上升沿数 | 模式 | 输出功率 |
| --- | --- | --- |
| **1** | mode 1 | **1.2 W ← 本板要的** |
| 2 | mode 2 | 1.0 W |
| 3 | mode 3 | 0.8 W |
| 4 | mode 4 | NCN 功能关 = **完全没有输出** |
| >4 | 不允许 | — |

**[实测]** 症状与结论完全吻合：boot 时曾在**三个地方**写 `reg 0x02`（原厂背光初始化序列、
ZL38063 group-A 诊断、回环的 PA-on），每次 `0 → 1` 都算一个沿。"DAC1_P/DAC1_M 上有 1 kHz 方波、
喇叭一点声音没有"就是 **mode 4**。

另一个致命细节 **[实测]**：`InitializeBacklight()` 回放原厂尾部时写 `0x11(CTL)<-0x1B` +
`0x12(GPMD_A)<-0x08`，如果这发生在 PA 使能**之后**，`GPMD_A = 0x08` 会让 group-A 输出**完全不驱动**
——1 kHz 音在 `0x02 <- 0xFF` 时都听不到，直到把 `0x12 <- 0x02` + `0x04(GPIO_CFG_A) <- 0x00` 写对才出声。

### 3.2 正确的五步序列（板级唯一实现）

实现在 `b220_pa_enable_one_pulse()`（[xiaomodou_b220.cc:1136](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/xiaomodou_b220.cc) 起）：

| 步 | 动作 | 为什么必须在这一步 |
| --- | --- | --- |
| 1/5 | `0x12 (GPMD_A) <- 0x02` | group-A 进入能驱动 SHDN 的模式；`0x08` 会让它失效 |
| 2/5 | `0x04 (GPIO_CFG_A) <- 0x00` | 所有 group-A 引脚设为普通输出 |
| 3/5 | `0x02 <- 0x00`，保持 **≥ 2 ms** | 登记 shutdown（TOFF）；用 `esp_rom_delay_us()` 忙等而不是 `vTaskDelay(1)`（tick 粒度只能给 1 ms，不够余量） |
| 4/5 | `0x02 <- 0x02` | **唯一一个上升沿** → mode 1 / 1.2 W |
| 5/5 | 锁存 `0x12` / `0x04` / `0x02` | 之后任何改动都被 `b220_aw9106_frozen_refuses()` 拒绝 |

### 3.3 单例 / 锁存：第二次调用必须是 no-op

```cpp
static bool s_pa_enable_done;                                  /* 一生只跑一次 */
static esp_err_t s_pa_enable_result = ESP_ERR_INVALID_STATE;   /* 第一次的结果，转发给后来者 */

static esp_err_t b220_pa_enable_one_pulse(const char* tag) {
    if (s_pa_enable_done) {
        ESP_LOGW(TAG, "pa: enable sequence already ran this boot (requested again by "
                      "%s) - IGNORED on purpose: a second 0 -> 1 on reg 0x02 bit 1 "
                      "would be SHDN rising edge #%lu and would move the AW8737S out "
                      "of mode 1 (1.2 W)", tag ? tag : "?", (unsigned long)(s_aw9106_reg02_rising_edges + 1u));
        return s_pa_enable_result;      /* 返回第一次的结果，不是"失败" */
    }
    s_pa_enable_done = true;
    …
}
```

同时 `b220_aw9106_write_reg02()` 是 `reg 0x02` 的**唯一写者**，它统计
`0x02` 写次数与 bit 1 的**上升沿次数**，超过 1 就大声报警：

```
pa: !!! AW8737S SHDN RISING EDGE #2 (bit 1 of reg 0x02 went 0 -> 1) - only 1 edge is
allowed, the amplifier is now in a HIGHER mode (2 edges = mode 2 1.0 W, 3 = mode 3
0.8 W, 4 = mode 4 NCN OFF = NO OUTPUT); offending write #N by <caller> !!!
```

**证明锁存生效的那两行日志**（同一次 boot，先板级后 codec）：

```
I (5844) XiaoMoDouB220: pa: SHDN enable sequence: 0x12(GPMD_A)<-0x02, 0x04(GPIO_CFG_A)<-0x00, 0x02<-0x00, hold 2 ms, 0x02<-0x02 (exactly 1 rising edge -> AW8737S mode 1, 1.2 W)
I (5924) XiaoMoDouB220: pa: AW9106B 0x5B reg 0x02 <- 0x02 (PA enable step 4/5: the one rising edge) -> ESP_OK | write #3, SHDN bit1=1, rising edges so far=1
I (5944) XiaoMoDouB220: pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02 | 0x02 writes=3, SHDN rising edges=1 (AW8737S mode 1)
        ↓ 3240 ms 后，音频服务进入 idle/使能输出
W (7844) XiaoMoDouB220: pa: enable sequence already ran this boot (requested again by Zl38063AudioCodec::EnableOutput(true)) - IGNORED on purpose: a second 0 -> 1 on reg 0x02 bit 1 would be SHDN rising edge #2 and would move the AW8737S out of mode 1 (1.2 W)
I (7874) Zl38063AudioCodec: Set output enable to true: the ZL38063 -> AW8737S PA -> speaker leg is live (PA enable returned ESP_OK)
```

最后两行就是**验收证据**：`SHDN rising edges=1` 且第二次请求被 IGNORED。
（这一对日志在 [production-codec.log](logs/production-codec.log) 与 [wakeword.log](logs/wakeword.log) 里都有。）

### 3.4 codec 侧怎么写 `EnableOutput`

```cpp
void Zl38063AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) return;      /* 幂等：状态没变直接返回 */
    AudioCodec::EnableOutput(enable);

    if (!enable) {
        /* 关不了。唯一的关断手段是再写一次 0x02，而每个 0->1 都是新的 SHDN 上升沿。
         * 静音来自"DSP 不再被喂音频"，不来自 PA。 */
        ESP_LOGI(TAG, "Set output enable to false: the AW8737S PA is left ON …");
        return;
    }

    /* 唯一被授权的入口：五步序列，第一次调用后锁存；板级 boot 通常已经先跑过，
     * 所以这里是一个打了日志的 no-op。两个调用点都安全。 */
    const esp_err_t pa = b220_audio_pa_enable_once("Zl38063AudioCodec::EnableOutput(true)");
    if (pa == ESP_OK) {
        ESP_LOGI(TAG, "Set output enable to true: the ZL38063 -> AW8737S PA -> speaker leg is live (PA enable returned ESP_OK)");
    } else {
        ESP_LOGW(TAG, "Set output enable to true, but the AW8737S PA enable did not complete cleanly (%s); …", esp_err_to_name(pa));
    }
}
```

谁先调用？**板级构造函数**（`xiaomodou_b220.cc:3746`）：

```cpp
s_b220_dsp_ready = B220_AudioProbeDspReady();
{
    const esp_err_t pa_err = b220_pa_enable_one_pulse("board init (production codec)");
    …
}
```

这样做的原因：`EnableOutput(true)` 只在音频服务第一次真的要出声时才被调用——可能是启动后
几十秒；而板级在 boot 就使能，能保证**任何时候出声都来得及**。而且两条路径共用同一个锁存，
所以无论谁先到，`0x02` bit 1 的上升沿总数永远是 1。

> ⚠️ **迁移警告**：`B220_BACKLIGHT_SWEEP=1` 会调用 `ProbeAw9106()`，它对 AW9106B 发 `0x7F`
> 软件复位，会**清掉 `0x02` 和 `GPMD_A`**。如果确实要用，必须在探针**之后**重新跑一次完整的
> PA 使能序列（或去掉 `0x7F` 复位），**不要只是把开关打开**。

### 3.5 与音频服务 15 秒空闲掉电的交互

`main/audio/audio_service.h:48`：

```c
#define AUDIO_POWER_TIMEOUT_MS 15000
```

`AudioService::CheckAndUpdateAudioPowerState()`（`audio_service.cc:833-853`）每秒检查一次：

```cpp
if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);  /* 让输入任务自己关 */
}
if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
    if (!(codec_->duplex() && codec_->input_enabled())) {   /* ← 本板命中的分支 */
        codec_->EnableOutput(false);
    }
}
```

对本板的具体含义（**实测**，见 [production-codec.log:153](logs/production-codec.log)）：

- 空闲 15 s 后 `EnableOutput(false)` 被调用 → 我们的实现**只打一行日志、不动硬件**，
  PA 依然在 mode 1、DSP 时钟依然在跑。所以"静音"仅仅是因为没人喂音频。
- 因为 `duplex_ == true`，所以当输入还使能时**连 `EnableOutput(false)` 都不会被调用**。
- 下一次要出声时 `EnableOutput(true)` 会再次被调用 → 走到锁存的 no-op 分支 →
  **不会产生第二个上升沿**。这正是 §3.3 那个设计要保护的东西。
- 输入侧是另一条路：15 s 无音频输入 → `AS_EVENT_AUDIO_INPUT_STOP_REQUEST` →
  输入任务调 `codec_->EnableInput(false)`。我们的 `EnableInput()` 只置标志，
  **不 disable RX channel**（BCLK/WS 必须继续给 DSP）。唤醒词要重新开始听时，
  `ReadAudioData()` 会自己 `EnableInput(true)`（`audio_service.cc:196-200`）。

> 💡 **这就是为什么本板不能"靠关功放省电"**：功放一旦使能就锁死在 mode 1 直到断电。
> 想做省电只能在 DSP 侧或整机电源侧动手，不在这个 codec 的职责内。

---

## 4. 联网与服务器

### 4.1 两条传输路径

小智固件在 OTA 之后从服务器下发的配置里选择传输（`main/application.cc:540-545`）：

```cpp
} else if (ota_->HasWebsocketConfig()) {
    protocol_ = std::make_unique<WebsocketProtocol>();
} else {
    ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
    protocol_ = std::make_unique<MqttProtocol>();
}
```

| | **MQTT + UDP（官方默认）** | **WebSocket** |
| --- | --- | --- |
| 控制通道 | MQTT over TLS，端口 **8883** | WSS |
| 音频通道 | **UDP**（AES-CTR 加密，密钥经 MQTT 下发） | WebSocket 二进制帧 |
| 延迟 | 低 | 中 |
| NAT 友好性 | **差**（UDP 打洞/端口可达性依赖网络） | **好**（走 443/TCP） |
| 本板状态 | **[实测] 控制通道通、音频通道不出声** | 推荐的回退路径 |

OTA 地址在 `sdkconfig`：`CONFIG_OTA_URL="https://api.tenclass.net/xiaozhi/ota/"`。
`MqttProtocol::GetHelloMessage()` 明确申请 UDP 传输：

```cpp
cJSON_AddStringToObject(root, "type", "hello");
cJSON_AddNumberToObject(root, "version", 3);
cJSON_AddStringToObject(root, "transport", "udp");   /* ← 只接受 udp */
```

### 4.2 成功时的日志长什么样（**[实测]**，逐字摘自 [wakeword.log](logs/wakeword.log)）

```
I (8354) WifiBoard: Connected to WiFi: dlink
I (8354) WifiBoard: Connected to WiFi: … ; sta ip: 192.168.x.x
I (8354) Application: Network connected
I (8354) StateMachine: State: starting -> activating
I (8364) Assets: The assets map size is 1600 KB (partition 8192 KB)
I (8594) MODEL_LOADER: Successfully load srmodels
I (8614) Ota: Current version: 2.5.1
I (9444) HttpClient: Established new connection to api.tenclass.net:443 protocol=https cost=830
I (9664) Ota: Current is the latest version
I (9674) Ota: Running partition: ota_0
I (9684) MQTT: Connecting to endpoint api.tenclass.net:8883
I (9684) MQTT: Resolved MQTT host api.tenclass.net -> 47.112.110.152
I (10514) MQTT: Connected to endpoint
I (10514) Application: Activation done
I (10514) StateMachine: State: activating -> idle
I (10524) wifi:Set ps type: 2, coexist: 0        ← 空闲省电模式
   … 唤醒词触发后 …
I (13924) MQTT: Session ID: 4c74af7f
W (13924) Application: Server sample rate 24000 does not match device output sample rate 48000, …
I (13944) StateMachine: State: connecting -> listening
I (17544) Application: >> 你好…                    ← 上行：设备把识别文本发过去
I (17554) StateMachine: State: listening -> speaking
I (19554) Application: << % get_weather...        ← 下行：服务器调 MCP 工具
I (21074) AudioService: Resampling audio from 24000 to 48000
I (25754) StateMachine: State: speaking -> listening
   … 多轮问答 …
I (99734) MQTT: Received goodbye message, session_id: 4c74af7f
I (99754) MQTT: Closing audio channel, send_goodbye: 0
I (99754) StateMachine: State: listening -> idle
```

要认的三个句号：

| 日志 | 含义 |
| --- | --- |
| `MQTT: Connected to endpoint` | 控制通道建立（TLS 到 8883 成功） |
| `MQTT: Session ID: <8 hex>` | 服务器接受了 `hello`，分配了一次会话 |
| `StateMachine: activating -> idle` | **激活完成，设备进入待命**——这是"这台机器能用了"的分界线 |

反向状态机也要会读：`idle -> connecting -> listening -> speaking -> listening` 是一次完整对话。

### 4.3 观察到的失败模式：控制通道通、**UDP 音频通道一个字节都没有**

**[实测]** 在**没有唤醒词、靠 auto-listen 主动开麦**的那轮（[auto-listen.log](logs/auto-listen.log)）里：

```
I (10144) StateMachine: State: activating -> idle
I (10984) XiaoMoDouB220: auto-listen: arm #1 (state idle -> listening)
I (10984) StateMachine: State: idle -> connecting
I (11154) MQTT: Session ID: fc729ef9
I (11154) Application: Server sample rate 24000 does not match device output sample rate 48000, …
I (11164) StateMachine: State: connecting -> listening
I (11314) AudioCodec: Set input enable to true
        … 整整 68 秒，没有任何 >> / << / 重采样日志 …
I (79464) MQTT: Received goodbye message, session_id: fc729ef9
I (79464) MQTT: Closing audio channel, send_goodbye: 0
I (79474) StateMachine: State: listening -> idle
I (80284) XiaoMoDouB220: auto-listen: arm #2 (state idle -> listening)
```

读法：

- **控制通道完全正常**：`Session ID` 拿到了，状态机走到 `listening`。
- **音频通道完全静默**：68 s 里没有任何 `Application: >>`（服务器没收到可识别语音）、
  没有任何 `Application: <<`（服务器没下发言论），也没有 `Resampling audio from 24000 to 48000`
  （服务器没发过一个音频包）。
- 服务器大约 **65 秒**后主动发 `goodbye` —— **这是服务端的会话超时**，不是崩溃：
  它按"这个会话从没收到过音频"判定为空会话并回收。

**判别方法**（一条命令的差别）：

```powershell
# 在设备处于 listening 状态时抓 10 s 日志，看有没有这两类行
Select-String -Path .\logs\xxx.log -Pattern 'Application: >>|Resampling audio from 24000'
```

| 现象 | 结论 |
| --- | --- |
| 有 `>>`、有 24000→48000 | 双向音频都通，会话正常（[wakeword.log](logs/wakeword.log) 就是这样） |
| 无 `>>`、65 s 后 `goodbye` | 上行 UDP 到不了服务器（**本条**） |
| 有 `>>`、无 24000→48000 | 上行通、下行丢 → 回程 UDP 被挡或服务器没回 |
| 连 `Session ID` 都没有 | 控制通道问题（不是本条的范畴） |

**推荐缓解手段**（按成本从低到高）：

1. **换 WebSocket 传输**——只要 OTA 响应里带 `websocket` 段，`HasWebsocketConfig()` 就为真，
   固件自动切到 `WebsocketProtocol`（`application.cc:540`）。走 443/TCP，UDP 完全不需要打洞。
   在服务器侧（xiaozhi-server 的 `data/.config.yaml` / 管理后台）启用 websocket 段即可，**固件不用改**。
2. **验证 NAT / 路由器**：把设备放到路由器 DMZ 或对 UDP 做放行；或换一台 AP 测试，
   排除运营商 CGNAT（**[推断]** 这是最常见的成因）。
3. **对照实验**：同一台设备在同一 WiFi 下用官方 App 或另一台成品小智连同一服务器。
   如果它也静默 → 网络/服务器问题；如果它正常 → 本机 NAT 会话或固件端问题。
4. 代码级确认：`mqtt_protocol.cc:371` 的 `udp->Connect(udp_server_, udp_port_)` 若失败会打
   `Failed to connect UDP audio channel: …`。**我们的日志里没有这一行**，
   说明 UDP socket 打开了、`connect()` "成功"了，但服务器从未收到包——
   所以问题在**包的投递**（NAT/端口可达性），不在 socket 建立。

### 4.4 WiFi 凭据放在哪、重新刷机要不要重配

- 凭据在 **NVS 分区（`0x9000`，长 `0x4000`）**，命名空间 `wifi`（见 `WifiManager` / `SsidManager`）。
- `flash-b220.ps1`（不带 `-FullErase`）**只写 5 个区域**：`0x1000` / `0x8000` / `0xd000` /
  `0x20000`（app）/ `0x800000`（assets）——**都不碰 NVS**。
  所以"改代码 → 重新刷 → 不用重配 WiFi"是成立的，这是日常迭代的主要省事点。
- 只有 `flash-b220.ps1 -FullErase` 或 `esptool erase_flash` 会清掉凭据。

**没有凭据时的配网流程（config AP）**——**[实测]** [production-codec.log:132-160](logs/production-codec.log)：

```
W (6204) SsidManager: NVS namespace wifi doesn't exist
I (7714) StateMachine: State: starting -> wifi_configuring
I (7714) WifiManager: Starting config AP
I (7784) wifi:mode : sta (b4:e6:2d:fc:4c:e5) + softAP (b4:e6:2d:fc:4c:e6)
I (7794) WifiConfigurationAp: Access Point started with SSID Xiaozhi-4CE6
I (7804) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 192.168.4.1
I (7814) WifiConfigurationAp: Web server started
W (7844) Application: Alert [gear] … Xiaozhi-4CE6 … http://192.168.4.1
        ↓ 手机连上之后
I (34244) WifiConfigurationAp: Station <手机MAC> joined, AID=1
I (36354) esp_netif_lwip: DHCP server assigned IP to a client, IP is: 192.168.4.2
I (40374) WifiConfigurationAp: SSID: dlink, RSSI: -78, Authmode: 3
I (56344) WifiConfigurationAp: Connecting to WiFi dlink (attempt 1/2)
```

| 项 | 值 |
| --- | --- |
| AP SSID | `Xiaozhi-4CE6`（`Xiaozhi-` + MAC 后 4 位；本机 MAC 尾 `4c:e5` → softAP 尾 `4c:e6`） |
| 配网页面 | `http://192.168.4.1` |
| DHCP 给手机 | `192.168.4.2` |
| 重试 | `Connecting to WiFi <ssid> (attempt 1/2)`，失败后 3 s 再试一次 |

> ⚠️ 本板**没有按键**（`InitializeButtons()` 是空实现），所以"进配网模式"只有自动一条路：
> 无凭据时自己进。想强制重配，清 NVS：`esptool erase_region 0x9000 0x4000`。

---

## 5. ⭐ 唤醒词（本项目最后一块拼图）

### 5.1 逆向结论：原厂的关键词是 **ESP32 软件识别**，而且**没有任何外部触发线**

来源：[WAKE-WORD-PATH-REVERSE.md](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063/WAKE-WORD-PATH-REVERSE.md)（对 `backup/parts/wifi.bin` + `backup/disasm/b220_full.elf` 的反汇编）。

```
3 个麦克风 → ZL38063（AEC/NR/波束成形）→ I2S 16 kHz 单声道（ESP32 是 master）
  → ESP32: rec_engine_task (0x400fac04) 读定长块 → VAD（WebRTC vad_filterbank.c/vad_sp.c）
  → 本地关键词识别（Turing/esp32-sr DNN + MFCC，"### spot keyword ###" @0x3f4122e1）
  → 命中（0x400fb500 处 bnei a10,1）: flag 0x3ffc5830<-1, state 0x3ffc5838<-2,
     启动 tm_wakeup 0x3ffc3850（0xb40 tick = 4000 ms），经 0x3ffc3864 推 wake 事件
```

| 结论 | 内容 | 置信度 |
| --- | --- | --- |
| 检测位置 | **ESP32 内部软件**，不是 DSP、不是 AI 模块、不是底座 | **[逆向] H** |
| 模型 | `wakeNet5_v3_modoujingling_6_0.90_0.85_kuaisuqianjin_6_0.90_0.85_tiaogewu_6_0.90_0.85` @DROM `0x3f413134` | **[逆向] H** |
| 关键词名 | `modoujingling` / `kuaisuqianjin` / `tiaogewu` | **[逆向] H** |
| 引擎输入 | 配置结构 @DRAM `0x3ffbeeb0`：`+0x34` 之后是 1, **`0x3e80` = 16000 Hz**, 30, 1, 0, 1 → **16 kHz 单声道** | **[逆向] H** |
| 阈值 | 0.90 / 0.85，窗口 6 帧，3 个关键词槽 | **[逆向] H** |
| 触发传输 | **只有内部状态机事件**。DSP 侧唯一 literals-verified 的 HBI 流量是 boot（3699 条固件记录 + 1042 条配置）+ reg 0x06 邮箱轮询 → **没有运行期 arm/trigger 命令**，也没有能携带触发的 HBI 读 | **[逆向] H**（完整性 M） |
| GPIO / HBI / UART 触发 | **不存在**。`gpio_set_intr_type(38, INTR_DISABLE)` @`0x40124ce8` 只是 disable，而且 **ESP32-D0WD 没有 pin 38** | **[逆向] H** |

> 📌 **这一条决定了整个方案**：既然原厂就是"ESP32 上跑软件唤醒词"，
> 我们就不需要任何 DSP 触发协议、不需要底座握手、不需要 GPIO 中断——
> 只需要一条 **16 kHz 单声道**的麦克风数据流和一个能在 ESP32 上跑的模型。

### 5.2 模型文件长什么样（**[逆向]**）

| 项 | 值 |
| --- | --- |
| 文件 | `…\zl38063\wakenet\wakenet5_modoujingling.bin` |
| 大小 | **84 160 字节**（`0x148c0`） |
| sha256 | `fb786b2ae14479141f6124b91dfdc93cba8a9e9bca889332d52a441bdc4aa910` |
| DROM 范围 | `0x3f4131b0 .. 0x3f427a70`（app 镜像内文件偏移 `0x131b0..0x27a70`） |
| 结尾 | 正好等于 `model_info_t` @`0x3f427a70` |
| 头 | 24 字节 `model_coeff_getter_t`：`getter_q = 0x4010070c`（IROM，名字→矩阵查表），`getter_info = 0x40100704` |
| 载荷 | 头之后是 **20 个 `dl_matrix2dq_t` 记录**（10 个 `kernel_i` + 10 个 `bias_i`），`{int w; int h; int stride; int flags; int exponent; qtp_t *itemq;}`，int16 项紧随其后，首尾相接 |

范围不是猜的：镜像是自描述的记录链，链**正好终止在 `model_info_t` 起始处**；
独立交叉验证是原厂 `get_coeff_q`（IROM `0x4010070c`）里的名字→矩阵表恰好引用这 20 个地址、不多不少。
完整表在 `wakenet5_modoujingling_layout.txt`，工具是
[wakenet5_tool.py](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063/wakenet/wakenet5_tool.py)：

```powershell
python wakenet5_tool.py info                                # 打印头 + 20 条记录 + sha256
python wakenet5_tool.py emit <outdir> modoujingling_wn5     # 输出可重定位 C
```

### 5.3 为什么不用原模型：esp-sr 2.4.7 **没有 wn5 引擎**

仓库 `main/idf_component.yml` 钉的是 `espressif/esp-sr: ==2.4.7`。逐版本核对结果
（[wakenet/README.md](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063/wakenet/README.md) §2）：

| esp-sr 版本 | ESP32 classic 上的 wn5 引擎 | 证据 |
| --- | --- | --- |
| v1.0 | ✅ 有 | `lib/esp32/libwakenet.a` 含 `wakenet5_quantized_mono.c.obj`，日志串 `Quantized wakeNet5: %s, mode:%d (%s %s)` |
| v1.9.5 | ✅ 仍有 | 同目标 `.obj` 定义 `esp_sr_wakenet5_quantized` / `model_create` / `model_detect`，UND 里有 `srmodel_get_model_coeff` |
| **2.4.7（本仓库）** | ❌ **任何目标都没有** | `lib/esp32/libwakenet.a` 里只有 `wakenet9_quantized/l/s.c.obj` + `esp_wn_models.c.obj`；字符串 `Quantized wakeNet5` / `wakenet5_create` 在整个 `lib/esp32/` 下**不存在**。`src/model_path.c` 不再定义 `srmodel_get_model_coeff`，但 `src/include/model_path.h:166-174` **还留着它的声明**，注释写着 *"@warning Just support ESP32 to load old wakenet"* |

同时 **2.4.7 的 Kconfig 里没有任何 wn5 选项**，而 `sdkconfig.defaults.esp32:3` 已经写死：

```
CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y
```

**即：2.4.7 在经典 ESP32 上唯一还能跑的 wakenet 家族就是 WakeNet9。**

### 5.4 采用的方案：用 esp-sr 现成的 wn9 中文模型（**你好小智**）

**决策**：唤醒短语从"魔豆精灵"改成 **"你好小智"**（模型 `wn9_nihaoxiaozhi_tts`，284 KB）。
理由：**零逆向、零训练、当天可验证**；而"用原关键词"要走 §5.6 那条数小时到数天的路。

#### 5.4.1 构建命令（以及为什么 `idf.py build` 是错的）

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd <repo>
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi
```

> ⚠️ **为什么不能直接 `idf.py build`：`sdkconfig` 是"粘"的。**
> 工作目录里的 `sdkconfig` 会保留上一次配置；如果它带着 `CONFIG_WAKE_WORD_DISABLED=y`，
> 那么直接 `idf.py build` 会**沿用**这个值，唤醒词代码整段编译不进去，
> 而你会在日志里找不到任何 `EspWakeWord` 行，却怎么改 `config.json` 都没用。
> `scripts/build.py` 是仓库的规范入口，它负责把 board `config.json` + variant 的
> `sdkconfig_append` + `--wake-word` 展开的值一起重新生成 `sdkconfig`——
> **构建一定要走它**。

`--wake-word nihaoxiaozhi` 是仓库的官方开关（`docs/custom-board.md:376-388`）。
对 `esp32` 目标它展开成（`scripts/build.py:338-385`）：

```
CONFIG_USE_ESP_WAKE_WORD=y
CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y
CONFIG_WAKE_WORD_DISABLED=n
```

> 📌 **仓库规则，别踩**：`CONFIG_SR_WN_*`（选模型）**不能**写进板级 `config.json` ——
> `scripts/tests/test_build.py:172-184` 专门断言这一点，写了会让 host 测试挂掉。
> 模型选择只有两个合法来源：`sdkconfig.defaults.esp32:3`（已存在）或命令行 `--wake-word`。
>
> 补充：`esp32` **不在** `_LITE_WAKE_WORD_TARGETS` 里，所以选到的是 `wn9_*` **大模型**
> 而不是 `wn9s_*` 小模型——这也是为什么 assets 会长 320 KB 而不是更少。

#### 5.4.2 `config.json` 需要加的三行

[xiaomodou-b220/config.json](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/config.json)：

```json
{
    "type": "xiaomodou-b220",
    "target": "esp32",
    "builds": [
        {
            "name": "xiaomodou-b220",
            "sdkconfig_append": [
                "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y",
                "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions/v2/16m.csv\"",
                "CONFIG_ESPTOOLPY_FLASHFREQ_80M=y",
                "CONFIG_SPIRAM=y",
                "CONFIG_SPIRAM_SPEED_80M=y",
                "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y",
                "CONFIG_USE_ESP_WAKE_WORD=y",
                "CONFIG_WAKE_WORD_DISABLED=n",
                "CONFIG_USE_AFE_WAKE_WORD=n"
            ]
        }
    ]
}
```

三行的作用与理由：

| 行 | 作用 |
| --- | --- |
| `CONFIG_USE_ESP_WAKE_WORD=y` | 启用**无 AFE** 的唤醒词路径（`LiteAudioEngine` + `EspWakeWord`）。`main/Kconfig.projbuild:908-912` 的依赖是 `IDF_TARGET_ESP32 && SPIRAM`——**本板正好满足**（`CONFIG_SPIRAM=y` + `target: esp32`） |
| `CONFIG_USE_AFE_WAKE_WORD=n` | **经典 ESP32 没有 AFE**（AFE 走 S3/P4 路径，见 `audio_service.cc:82-86` 的 `#if CONFIG_IDF_TARGET_ESP32S3 …`）。开着只会浪费内存并可能让引擎初始化失败 |
| `CONFIG_WAKE_WORD_DISABLED=n` | 明确关掉"禁用唤醒词" |

放在板级 `config.json` 里的额外好处：**即使漏掉 `--wake-word`，唤醒词仍然有**
（模型由 `sdkconfig.defaults.esp32` 提供，两处覆盖值一致、不冲突）。

#### 5.4.3 不需要 `model` 分区：模型进的是 `generated_assets.bin`

| 项 | 值 |
| --- | --- |
| 模型 | `managed_components/espressif__esp-sr/model/wakenet_model/wn9_nihaoxiaozhi_tts`（284 KB） |
| 打包 | `scripts/build_default_assets.py` 按 esp-sr `pack_model.py` 的同一格式打成 `srmodels.bin`，作为 assets 里的一个文件（`index.json` 的 `srmodels` 项） |
| 产物 | `build\generated_assets.bin` |
| 刷写位置 | **assets 分区 `0x800000`**（8 MB） |
| 大小变化 | **[实测]** assets 映射 **1280 KB → 1600 KB**；`generated_assets.bin` 约 **1,578,891 字节** |
| 分区表 | **不变**（仍是 `partitions/v2/16m.csv`，没有 `model` 行） |

运行时链路：

```
Assets::LoadSrmodelsFromIndex()           main/assets.cc:110-131
    ↓ 把 mmap 到的那块数据交给
srmodel_load(...)                          esp-sr
    ↓ 经
AudioService::SetModelsList()              main/audio/audio_service.cc:855-861
    ↓
LiteAudioEngine::Initialize(codec, frame_ms, models_list)
    ↓
EspWakeWord::Initialize(codec, models_list)  → esp_srmodel_filter(…, ESP_WN_PREFIX, NULL)
                                             → esp_wn_handle_from_name(model_name)
                                             → create(model_name, DET_MODE_95)
```

> 📌 **为什么不用独立 `model` 分区**：esp-sr 自己支持它，但要求
> `CONFIG_PARTITION_TABLE_CUSTOM` **且**分区表里真的有 `model` 行
> （`managed_components/espressif__esp-sr/CMakeLists.txt:102-126`）。
> 本仓库 v2 布局的惯例（`espressif/esp-hi`、`m5stack/stick-s3`）是走 assets，
> 于是**分区表不用动、刷机偏移一个都没变**——迁移成本最低。
>
> `flash-b220.ps1` 也做了对应处理：assets 里没有 `srmodels` 时黄字警告"语音唤醒不会工作"
> 并给出重建命令；**如果**将来分区表真出现 `model` 行，它会自动把
> `build\srmodels\srmodels.bin` 按分区表偏移一起写。

#### 5.4.4 构建期与运行期的验收日志（**[实测]**）

构建日志（证明模型真的被打进去了）：

```
  wakenet models: wn9_nihaoxiaozhi_tts (will be packaged)
Generated: .../srmodels/srmodels.bin
```

不刷机也能自检 assets 里有没有模型：

```powershell
$B = '<repo>\build'
$txt = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$B\generated_assets.bin"))
"assets 含 srmodels: " + $txt.Contains('srmodels')      # 必须 True
(Get-Item "$B\generated_assets.bin").Length             # 约 157 万（带模型）/ 约 129 万（无模型）
```

开机日志（COM25 @115200，`capture-serial.ps1`）——**逐字摘自 [wakeword.log](logs/wakeword.log)**：

```
I (8364) Assets: The assets map size is 1600 KB (partition 8192 KB)   ← 无模型时是 1280 KB
I (8594) MODEL_LOADER: Successfully load srmodels                     ← 关键：模型装上了
I (10524) wifi:Set ps type: 2, coexist: 0
MC Quantized wakenet9: wakenet9l_tts1h8_…_3_0.631_0.635, tigger:v4, mode:1, p:0, (Apr 17 2026 17:25:50)
I (10634) EspWakeWord: Wake word(wn9_nihaoxiaozhi_tts),freq: 16000, chunksize: 512
I (10634) LiteAudioEngine: Initialized, WakeNet: yes                  ← 关键：引擎起来了
I (10644) AudioCodec: Set input enable to true
W (10664) XiaoMoDouB220: pa: enable sequence already ran this boot …  ← PA 锁存在工作
I (10704) AudioService: Resampling audio from 16000 to 48000
```

对着麦克风说 **"你好小智"**，应出现：

```
I (13814) Application: Wake word detected: 你好小智 (state: 3)
I (13814) StateMachine: State: idle -> connecting
I (13924) MQTT: Session ID: 4c74af7f
I (13944) StateMachine: State: connecting -> listening
```

四个验收点的含义：

| 行 | 说明 |
| --- | --- |
| `MODEL_LOADER: Successfully load srmodels` | `srmodels.bin` 从 assets 加载成功 |
| `LiteAudioEngine: Initialized, WakeNet: yes` | 引擎初始化成功、唤醒词开启 |
| `EspWakeWord: Wake word(wn9_nihaoxiaozhi_tts),freq: 16000, chunksize: 512` | **[实测]** 引擎自报 16 kHz / 512 样本（32 ms）一块 |
| `Application: Wake word detected: …` | 真的识别出来了 |

> 💡 注意 **[实测]** 的 `chunksize: 512` 与 [FLASH-MANUAL.md](FLASH-MANUAL.md) §4.7 里写的
> `chunksize: 320` 不同——那是早期文档的笔误/预估值。**以日志为准**（512 样本 @16 kHz = 32 ms）。
> 而 `EspWakeWord::Feed` 无论 chunksize 是多少都能工作（它内部按
> `get_samp_chunksize()` 切块，且多通道时只取左声道，见 `esp_wake_word.cc:86-92`）。
>
> 💡 `MC Quantized wakenet9: …_0.631_0.635, tigger:v4` 这行是 **esp-sr 自己的模型自述**
> （注意它把 trigger 拼成了 `tigger`，是上游拼写）。两个 0.631/0.635 是模型的内部参数，
> **不是**我们能调的检测阈值：阈值是 `esp_wake_word.cc:47` 写死的 `DET_MODE_95`。

#### 5.4.5 装上了但唤不醒 / 唤醒了但对话不通

| 日志现象 | 含义 / 处理 |
| --- | --- |
| `The assets map size is 1280 KB` | 刷进去的 assets 还是旧的（不带模型）→ 重建 `generated_assets.bin` 再刷 |
| `MODEL_LOADER: Can not find model in partition table` | 走到了"独立 model 分区"后备路径（本板没有该分区）。正常流程不该出现 |
| `Failed to initialize wakenet model` / `No model found` / `No WakeNet model found` | assets 里没有 `srmodels` 或模型名不对 → 先做 §5.4.4 的 assets 自检 |
| 日志里根本没有 `Wake word(wn9_…)` | `CONFIG_USE_ESP_WAKE_WORD` 没生效 → 查 `build\config\sdkconfig.json` 里 `USE_ESP_WAKE_WORD=True`、`WAKE_WORD_DISABLED=False`。**99% 是漏了 `--wake-word` 而 `sdkconfig` 是旧的**（§5.4.1） |
| `LiteAudioEngine: Initialized, WakeNet: no` | 引擎起来了但没加载唤醒词——典型是 assets 无模型（[auto-listen.log:178](logs/auto-listen.log) 就是这一版：`assets map size is 1280 KB` + `WakeNet: no`） |
| 装上了但一直唤不醒 | 先查 mic 通路与底噪/削顶（§1.5 的 `peak`/`mean` 日志）；`DET_MODE_95` 偏严，说话要清楚、距离要近（**实测** 1 m 内正常音量即可） |
| 唤醒了但服务器侧静默 | §4.3（UDP 音频通道） |

#### 5.4.6 16 kHz 是自动的，板级无需改动

本板 codec 输入 48 kHz，`AudioService::Initialize()` 在 `input_sample_rate() != 16000` 时
建 `esp_ae_rate_cvt`（`audio_service.cc:73-80`），`ReadAudioData(data, 16000, samples)`
把 48 kHz 降到 16 kHz（`:202-219`），再按 10 ms 一帧喂引擎（`:310-316`）。
**[实测]** `I (10704) AudioService: Resampling audio from 16000 to 48000` 与后续的唤醒成功，
证明这条链路端到端可用。

> ⚠️ 千万不要为了"省掉一次重采样"把 `AUDIO_INPUT_SAMPLE_RATE` 改成 16000——见 §2.2。

### 5.5 auto-listen（已弃用）与唤醒词的关系

`B220_AUTO_LISTEN` 是"这台设备没有按键、又没有唤醒词"那个阶段的临时拐杖
（[config.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/config.h) 的 Phase 5 块，
`xiaomodou_b220.cc:2762` 起）。它的做法是：监督任务轮询 `Application::GetDeviceState()`，
**每次进入 `kDeviceStateIdle` 就 800 ms 后调 `Application::StartListening()`**——
用的还是按键/唤醒词同一个事件位 API，不碰 core。

2026-10-09 **唤醒词验证成功之后，它被置 0**：

```c
/* Why it is 0 now (2026-10-09): voice wake-up does work on this chip. … 
 * The phrase "ni hao xiao zhi" is what starts a conversation now.
 * Auto-listen would be a second, competing way to open the microphone, hence this is 0. */
#define B220_AUTO_LISTEN 0
```

**[实测]** `I (5974) XiaoMoDouB220: auto-listen: DISABLED (B220_AUTO_LISTEN=0 in config.h): the esp-sr WakeNet9 wake word (CONFIG_USE_ESP_WAKE_WORD) starts conversations now`

置 0 时整个监督任务、状态与日志**完全编译掉**（`B220_AutoListenStart()` 退化成一个只打日志的 stub），
没有运行期开销。**这是"有了唤醒词之后必须做的一步清理"**：两个开麦来源会互相打架。

### 5.6 可选升级：换回原关键词「魔豆精灵」（**未实现**）

**[逆向]** 模型已经提取并重生成为可重定位 C：

| 项 | 值 |
| --- | --- |
| 二进制 | `…\zl38063\wakenet\wakenet5_modoujingling.bin`，84 160 B，sha256 `fb786b2a…` |
| 布局 | 24 字节 `model_coeff_getter_t` + **20 个 `dl_matrix2dq_t` 记录** + `model_info_t` |
| 生成的 C | [modoujingling_wn5.c](ref/analysis/wakenet/gen/modoujingling_wn5.c)（262 436 B，其中 rodata 约 84 KB）+ `modoujingling_wn5.h` |
| 生成方式 | `python wakenet5_tool.py emit <dir> modoujingling_wn5` —— 把原厂绝对指针换成链接符号，并把 `get_coeff_q` 换成 `strcmp` 链 |

它需要的引擎是 **esp-sr v1.x 的 wn5**，而 v1.x 的加载机制是**编译期描述符、不是运行期文件**
（引自 v1.9.5 源码）：

```c
/* include/esp32/esp_wn_models.h:90-96 */
#include "customized_word_wn5.h"
#define WAKENET_MODEL_NAME "wn5_customizedword"
#define WAKENET_COEFF get_coeff_customizedword_wn5

/* src/model_path.c:313-317 */
model_coeff_getter_t *srmodel_get_model_coeff(char *model_name)
{ model_coeff_getter_t *gettercb = (model_coeff_getter_t *)&WAKENET_COEFF; return gettercb; }
```

也就是说：**wn5 引擎根本不看运行时给的名字，它要一个编译期宏指定的描述符**。
定义自己的 `get_coeff_modoujingling_wn5` 并让 `WAKENET_COEFF` 指向它是**受支持的用法**，不是 hack。
（wn5 **没有**外部 blob/分区加载器——`srmodel_load`/`movemodel.py` 只覆盖 wn7/wn8/wn9。）

落地步骤（按可行性排序）：

1. 从 `v1.9.5 lib/esp32/libwakenet.a` 取 wn5 引擎（`ref\analysis\wakenet\v1.9.5_libwakenet_esp32.a` 已下载备用）；
2. 补上 2.4.7 已删除的 `srmodel_get_model_coeff`，或**加性**引入 v1.9.5 的 `model_path.c`；
3. 放入生成的 `modoujingling_wn5.c`，设好 `WAKENET_COEFF` / `WAKENET_MODEL_NAME`；
4. `WAKENET_MODEL_NAME` 用原厂 info 串，这样 `get_word_name` 仍能报出 `modoujingling`。

**工作量与风险（明确评估）：**

| 项 | 评估 |
| --- | --- |
| 工作量 | **数小时到数天** |
| 风险 | **中** |
| 具体风险 1 | 与 2.4.7 的 wn9 引擎存在 `esp_wn_handle_from_name` / `model_create` **符号重复**，要在链接层解决 |
| 具体风险 2 | v1.9.5 的预编译归档是 GCC-13 / IDF-5.x 时代产物，**必须**对 IDF 6.1 做链接测试 |
| 具体风险 3 | 没有运行期 blob 加载，模型常驻 ~84 KB DROM/flash |
| 兜底 | 若混两代 esp-sr 太乱，就把整个组件钉到 **v1.9.5**，用它的 API |
| 未验证 | `esp_sr_wakenet5_quantized` + 我们的描述符是否真的给出正确识别——**这需要构建 + 麦克风实测**，不是看代码能定的 |

> ❗ **状态：未实现（NOT implemented）。** 本文档记录的是**已完成的分析与准备**：
> 模型已提取、已重生成 C、目标归档已取得、机制已从上游源码确认。
> 实际设备上跑的是 §5.4 的 `wn9_nihaoxiaozhi_tts`（"你好小智"）。
> 另外仍未确定的一点（[逆向] L）：原厂 6 槽 `model_coeff_getter_t` 的字段名与任何已发布头文件都不完全一致
> （v1.0 的是 9 槽、`getter_info` 在 +0x18；我们的是更早的 6 槽、槽 4 是 info getter），
> 所以**任何重发的描述符都必须按目标版本的头文件布局来**。

---

## 6. 不要去做的方案（以及原因）

这一节是"省下几天时间"的清单。每条都写清**为什么不做**，避免后来者重走。

| 方案 | 状态 | 为什么不做 |
| --- | --- | --- |
| **auto-listen / 常听 watch** | **试过、不喜欢、已关**（`B220_AUTO_LISTEN 0`） | 它在**每一轮**对话结束后自动重新开麦（`arm #N`，见 [auto-listen.log](logs/auto-listen.log)），设备变成"永远竖着耳朵"；隐私与体验都差，而且会和唤醒词抢麦。功能够用后被明确退役 |
| **移植原厂 IROM 里的 wn5 引擎** | **不做** | 它是**位置相关**的机器码，对 `dl_lib`、cJSON、FreeRTOS、newlib 的调用全部解析到**原厂地址**。搬过来要重定位成千上万个 literal、逐个重新绑定 ROM/newlib 符号（例如 `strcmp` 在 `0x4009e394`）。工作量 >> 收益，而且 esp-sr v1.x 已经**以可链接归档的形式**提供了同一个引擎（§5.6） |
| **自己训练一个新的 wn5 模型** | **拿不到** | Espressif 的唤醒词定制现在只产出 **WakeNet9 / wn9s / wn9l**（和 MultiNet）；当前 esp-sr 里唯一的 "custom" 产物是 `wn9_customword`。2.4.7 的 Kconfig 里**连 wn5 选项都没有**。而且我们**不需要**：原厂固件里已经带着这个关键词训练好的 wn5 模型 |
| **在 ESP32-S3 专属路径上做软件唤醒词（AFE）** | **本芯片不可用** | AFE 引擎只在 S3/P4/S3.1 上构造（`audio_service.cc:82-86` 的 `#if CONFIG_IDF_TARGET_ESP32S3 \|\| …`）。经典 ESP32 走的是 `LiteAudioEngine`。开 `CONFIG_USE_AFE_WAKE_WORD=y` 只会失败或白占内存 |
| **给唤醒词加 HBI 轮询 / GPIO 中断 / 底座 UART 触发** | **不存在这条路** | **[逆向] H**：原厂的检测全在 ESP32 内部软件里，"没有 GPIO 沿、没有 HBI 轮询、没有 UART 消息参与唤醒"。自己造一条只会引入新的失败面 |
| **为了"省一次重采样"把声明改成 16 kHz** | **绝不要** | §2.2：端口实际是 48 kHz，声明 16 kHz 会去掉重采样器 → 采集/播放 3 倍速 |
| **靠关功放来省电 / 做静音** | **做不到** | AW8737S 的模式由**上升沿个数**决定，锁存后不能再动；关功放 = 一次新的 0→1 = 换模式（第 2 次 mode 2，第 4 次直接没输出）。静音只能靠"不喂音频" |
| **在 `SetOutputVolume()` 里做软件增益** | **不做** | 要给每个播放样本做乘法，而已经验证的通路不那么做；DSP 侧 `reg 0x238/0x23A` 才是真的音量，由 boot 配置记录负责 |
| **`idf.py build` 直接编译**（不走 `scripts/build.py`） | **不要** | §5.4.1：陈旧 `sdkconfig` 会把 `WAKE_WORD_DISABLED` 粘住，症状是"改了配置却没反应" |
| **`CONFIG_SR_WN_*` 写进板级 `config.json`** | **不允许** | `scripts/tests/test_build.py:172-184` 专门断言，写了 host 测试挂 |

---

## 7. 合并排错表（symptom → 根因 → 精确处置）

> 用法：先按"现象"定位行，再看"先看哪行日志"——**不要凭感觉改配置**。
> 每行都给了文件路径或确切日志串，便于直接搜索。

| # | 现象 | 最可能根因 | 精确处置 / 要看的那行日志 |
| --- | --- | --- | --- |
| 1 | 串口**一个字节都没有** | ① 没共地 ② TX/RX 接反 ③ TTL 不是 3.3 V 档 ④ 设备没开机（电池） | `capture-serial.ps1`；万用表确认 TTL GND ↔ 模组 GND 通断、TTL TX 通到 GPIO3（模组第 34 脚）。运行固件时电脑只靠 TTL 供电**不够**，要靠玩具电池 |
| 2 | `No serial data received` / `Invalid head of packet (0x2B/0xFE)` | 没进下载模式（`0xFE` 那类其实是**读到应用日志**，说明芯片在跑固件） | 进下载模式：① 按启动键**关机** ② **插 micro-USB** ③ 按启动键**开机** → 横幅须为 `boot:0x27 (DOWNLOAD_BOOT…)` + `waiting for download`。退出：**拔 micro-USB** 再开关机 → `boot:0x3f (SPI_FAST_FLASH_BOOT)` |
| 3 | `PermissionError` / 端口打不开 / 拒绝访问 | 别的串口工具占着 COM25（串口助手、`idf.py monitor`、上一个没退的 esptool） | 关掉所有占用者；`Get-Process` 查残留 python。**不要**改用 `--before default_reset`（本机 TTL 没接 DTR/RTS，一律 `--before no_reset`） |
| 4 | `Corrupt data, expected 0x1000 bytes but received 0xffd bytes` | **串口链路误码**（读到一半丢字节），不是芯片坏 | **降波特率重试**：`--baud 460800` 或 `115200`；缩短飞线；确认 TTL 是 3.3 V 档。稳定后 921600 下有效约 490 kbit/s |
| 5 | 部分刷写之后**反复重启 / boot loop** | 混用了两套**不兼容**的分区布局：原厂 app 在 `0x10000`（3.75 MB，`wifi`/`wifi_1`），小智 app 在 `0x20000`（3.9 MB，`ota_0`/`ota_1`）；原厂音频区 `flashTone` 在 **`0x42d000`**（8 MB），小智 `assets` 在 **`0x800000`** | **整片擦除 + 整片写回**是最省心的：切到小智用 `.\flash-b220.ps1 -FullErase`；回原厂用 `write_flash 0x0 .\backup\esp32-16MB-full.bin`。原始布局见 [FLASH-MANUAL.md](FLASH-MANUAL.md) §6 |
| 6 | 屏幕**全黑**（但能连服务器） | 背光不在 GPIO 上，在 **AW9106B（I2C `0x5B`）** 的 DIM0/DIM1/DIM2 | 查日志 `XiaoMoDouB220: AW9106B backlight on (DIM0/1/2 = 0x3F)`；同时确认 I2C 在 SDA 18 / SCL 23、地址 `0x5B` |
| 7 | 屏幕**颜色反了**（红蓝互换） | 裸 `esp_lcd_panel_draw_bitmap()` 需要**手动字节交换**（panel 要高字节在前）；LVGL 那条路靠 `SpiLcdDisplay` 的 `swap_bytes=1` 已处理 | 自绘缓冲要 `__builtin_bswap16()`。**[实测]** 方向自检卡就踩过这个坑（`config.h` 的 `B220_ORIENT_CARD` 注释里记着） |
| 8 | 屏幕**分辨率/几何不对** | 初始化表的窗口让人误判成 ILI9341 **240×320**；实测物理是 **128×160** | `config.h`：`DISPLAY_WIDTH 128` / `DISPLAY_HEIGHT 160`，`DISPLAY_MIRROR_X/Y false`、`DISPLAY_SWAP_XY false`、`DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB`（原厂自身发 `0x21 INVON`、`MADCTL(0x36)=0x00`，驱动**不得**再反相/重映射） |
| 9 | ZL38063 所有寄存器读回 **`0x0000`**（HBI 全零） | ① DSP 完全没被驱动（MISO 恒低） ② BCLK/WS 没起（DSP 是 I2S slave，没时钟就不应答） ③ 片选/引脚不通 | 先确认 I2S 已起：`I (1214) XiaoMoDouB220: I2S up before DSP boot: 48 kHz, 16-bit, stereo …`；再按 [PHASE3-DSP-DEBUG-STATUS.md](PHASE3-DSP-DEBUG-STATUS.md) §4 量 GPIO19（应"约 1.2 s 处低 10 ms → 随后保持高"）、GPIO0（空闲应高、仅通信瞬间拉低）、GPIO34(MISO)（通信时应有跳变）。字节序不是问题：`0xD3D3` 是回文，交换写反也会读出它 |
| 10 | `reg 0x34` 读不到 **`0xD3D3`**（host-boot 中止） | ① I2S 时钟未先于 DSP 复位建立 ② 复位脉冲没真正释放 ③ DSP 没通电 | 关键顺序：**I2S 先起 → GPIO19 复位脉冲 → host-boot**（`B220_I2S_BEFORE_DSP 1`）。**[实测]** 加了"I2S before DSP"之后 host-boot 才从"读不到 `0xD3D3`"变成"上传成功" |
| 11 | host-boot 上传成功但**应用一直不报 running**（`reg 0x0030 & 0x0F == 0`） | **本产品的应用从不更新 `reg 0x0030`**，这个寄存器在这里没有意义 | **改用 `reg 0x0014 == 0x0200`**：`I (5834) XiaoMoDouB220: codec: reg 0x0014 = 0x0200 -> the ZL38063 application is READY`。这也是原厂自己的音量/路由 routine 的 gate 条件。若 `0x0014` 也读不到 → 回到 #9/#10 |
| 12 | DAC1_P/DAC1_M 上有波形，**喇叭没声音** | AW8737S 被切成 **mode 4**（NCN OFF）或 **`GPMD_A != 0x02`** 导致 group-A 根本不驱动 SHDN | 查三行：`pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02 \| 0x02 writes=N, SHDN rising edges=1 (AW8737S mode 1)`；出现 `!!! AW8737S SHDN RISING EDGE #2 …` 就是有第二个写者在写 `0x02`（§3）。也要确认背光初始化没有在 PA 之后写 `0x12 <- 0x08` |
| 13 | 唤醒词**一直不触发** | ① assets 里没模型 ② `CONFIG_USE_ESP_WAKE_WORD` 没生效 ③ 漏了 `--wake-word` 而 `sdkconfig` 陈旧 ④ mic 通路问题/阈值偏严 | 依次看：`Assets: The assets map size is 1600 KB`（1280 = 旧 assets）→ `MODEL_LOADER: Successfully load srmodels` → `LiteAudioEngine: Initialized, WakeNet: yes` → `EspWakeWord: Wake word(wn9_nihaoxiaozhi_tts),freq: 16000, chunksize: 512`；再确认 mic 有信号（回环日志的 `peak`/`mean`）；`DET_MODE_95` 偏严，靠近点说清楚 |
| 14 | `WakeNet: no` / `Failed to initialize wakenet model` / `No WakeNet model found` | assets 无 `srmodels`，或模型名不对 | 先做 §5.4.4 的 assets 自检（`$txt.Contains('srmodels')` 必须 `True`），再重建 `generated_assets.bin` 并刷 `0x800000` |
| 15 | 服务器 **65 s 静默后发 `goodbye`** | **UDP 音频通道一个字节都没送到服务器**（NAT / 端口可达性），控制通道本身是好的 | 判据：`Session ID` 有，但 68 s 内**没有** `Application: >>` 也**没有** `Resampling audio from 24000 to 48000`（[auto-listen.log](logs/auto-listen.log) 的 arm #1 段）。处置：换 WebSocket 传输（OTA 响应里带 `websocket` 段即可，固件不改）→ 或排查 NAT/CGNAT → 或对照实验。注意 `mqtt_protocol.cc:371` 的 `Failed to connect UDP audio channel` **没有**出现，说明 socket 建起来了、是包没到 |
| 16 | `MODEL_LOADER` 失败 | ① assets 分区没刷/刷旧 ② 偏移写错 ③ 分区表与构建不一致 | 确认写机清单：`0x1000` bootloader、`0x8000` partition-table、`0xd000` ota_data_initial、`0x20000` xiaozhi.bin、`0x800000` generated_assets.bin；`partition-table.bin` 与 `partitions/v2/16m.csv` 必须同源 |
| 17 | 开机第一行 `flash read err, 1000` 然后 `rst:0x10 (RTCWDT_RTC_RESET)` | **本机已知的 Flash 冷启动裕量问题**（原厂固件同样现象），靠看门狗复位自愈 | **不是刷坏了，不用处理。** 后续 `boot:0x3f (SPI_FAST_FLASH_BOOT)` 正常启动即可（每份日志开头都有这两行） |
| 18 | `SetOutputVolume` 调了但音量没变 | **设计如此**：音量在 DSP 内部（`reg 0x238/0x23A`），由 boot 配置记录写死；codec 只存 NVS | 日志里会有 `SetOutputVolume(N): stored, but NOT applied by this codec …`。这是**预期行为**，不是 bug |
| 19 | `Server sample rate 24000 does not match device output sample rate 48000` | **正常**：服务器 24 k、设备 48 k，中间有重采样 | **不要消除它**（消除的唯一办法是把设备声明成 24 kHz，那会违反 §2.1）。确认后续有 `Resampling audio from 24000 to 48000` 且对话可听即可 |
| 20 | `write_mem` 后 esptool 报 `Packet content transfer stopped` | 芯片在命令执行中途复位了（如 `write_mem 0x3FF48000 0x80000000` 软复位） | **预期行为**。注意：复位瞬间若 GPIO0 仍为低（micro-USB 还插着），芯片会**再次落回下载模式** |
| 21 | 跨机恢复后两台设备**用同一个 MAC**、互相干扰 | 把甲机整片备份写到乙机时，NVS 里的 `nvs.net80211:sta.mac` 一起搬过去了 | 写完后清 NVS：`esptool erase_region 0x9000 0x4000`（eFuse 里的真实 MAC 无法被 flash 写入改变，清完即恢复），代价是**要重新配网** |
| 22 | 整片重新读出来的 MD5 与旧备份**不同** | 设备只要运行过，**NVS 分区（`0x9000–0xd000`）就会变**（WiFi 状态、日志游标） | 不是接线问题。要确认零误码就**按 64 KB 分块比较**，只应看到 NVS 所在的那一块有差异 |

---

## 8. 经验方法论：真正让这个项目推进的调试纪律

这一节的每一条都是本项目**实际起过作用**的做法，不是泛泛之谈。

### 8.1 一次只改一个变量（per flash）

这个项目里同时打开过 10+ 个 `B220_*` 诊断开关（`B220_ZL_DIAG`、`B220_DSP_VERIFY_UPLOAD`、
`B220_DSP_PAGE255_CALIB`、`B220_DSP_VERIFY_BEFORE_GO`、`B220_DSP_RELEASE_MATRIX`、
`B220_DSP_ADDR_MATRIX`、`B220_DSP_HANDS_OFF`、`B220_AUDIO_LOOPBACK`、`B220_AUDIO_TEST_MODE`、
`B220_I2S_TX_VARIANT_SWEEP`、`B220_PA_BIT_FINDER`、`B220_AUTO_LISTEN`）。

它们存在的意义是**每一个都对应一个单独的假设**，而且典型做法是：

- **单行开关**：翻一个 0/1 就换一个实验，其余全部保持上一版的已知状态；
- **每次实验只测一个变量**，并把"其他开关的值"写进该开关的注释里（`config.h` 里每个块结尾都有
  "everything else is left exactly as the ADF build had it: `B220_ZL_DIAG 0`, …"）；
- **关掉时完全编译掉**："0 means the switch, the table, the record array and the call site are all
  compiled out and the build is byte-for-byte the previous one."

反例就是踩过的坑：`B220_I2S_TX_VARIANT_SWEEP` 开着的时候每 3 s 重配一次 I2S 格式，
而那一轮要回答的是"功放到底有没有出声"——**两个变量同时在动，结论就不可信**。
所以那一版把它明确置 0，并在注释里写清"this run needs one STABLE 1 kHz sine"。

### 8.2 让设备在日志里**自己诊断**

不要靠"看起来不对"。这个项目的每一条结论背后，都是设备自己报出来的数字或十六进制：

| 做法 | 实例（日志串） |
| --- | --- |
| **峰/均值**区分"静音"与"有信号" | `loopback t=+4s: … last_block=240 frames peak=12656 mean=5937` vs `peak=121 mean=43`（空闲底噪） |
| **十六进制 dump** 首块样本 | `loopback: first non-empty block after start - first 16 of 480 sample(s), hex, L/R interleaved: FFFA FFFA FFF3 FFF3 …`（一眼看出 L/R 成对重复 = DSP 双 slot 同数据） |
| **写后读回**验证 | `page255: calib enc=b preselect=0 base=hi_lo -> matched 16/16`；`verify: mismatching_words=0` |
| **仪表数值写进日志** | `pa: final AW9106 state 0x12(GPMD_A)=0x02 0x04(GPIO_CFG_A)=0x00 0x02=0x02 \| 0x02 writes=3, SHDN rising edges=1` |
| **计数器**防漂移 | `write #3`、`SHDN bit1=1, rising edges so far=1`、`frames_in=96000 frames_out=96000 read_err=0 write_err=0 empty_reads=0` |
| **一次性的 bring-up 裁决** | `ReportBringupOnce()` 只在第一次报告句柄与 DSP 就绪状态 |
| **正弦表的理论值自检** | `tone: expected mean = peak * 2/pi = 0.63662 * peak (peak 32760 -> mean 20856)` —— 之后 `tx_mean~20855` 正好对上，同时证明了表、DMA 与计数都对 |

一个很好的反例教训：早期只看 `reg 0x0030`（"app status"）就得出结论"应用没跑起来"，
浪费了大量时间。后来发现**这个产品的应用从不更新 `0x0030`**，
真正的就绪指标是 `reg 0x0014 == 0x0200`（原厂自己的音量/路由 routine 也用它做 gate）。
**教训：要确认你读的那个寄存器在这个产品上真的有意义。**

### 8.3 优先测量（示波器 / 万用表），而不是猜

硬件层的问题在软件侧是**不可判定**的，早点拿表就早点结案：

| 问题 | 猜的代价 | 测量给出的答案 |
| --- | --- | --- |
| "DSP 是不是没在跑？" | 反复改 HBI 时序 | **量 I2S_WSA = 48 kHz**（原厂机）→ DSP 确实在跑，问题不在 DSP 有没有启动 |
| "复位脚是不是被按住？" | 反复改复位时序 | **量 GPIO19**：开关关时低、按下后拉高并保持高 → 复位网络正常释放，不是被按住 |
| "功放为什么不出声？" | 反复改 I2S 格式 | **量 SHDN 电平**：固件在跑时读 LOW → 功放确实被关；再量 DAC1_P/DAC1_M → 有 100 mV 方波说明 DSP 侧没问题，问题在 SHDN → 定位到 AW9106B `GPMD_A`/`0x02` |
| "BCLK 对不对？" | 改 `mclk_multiple`、试各种 slot 配置 | 原厂机 BCLK **1.54 MHz = 48 k × 32**，即 16-bit slot；与我们的配置一致 → ESP32 侧不是问题 |

**[实测]** 这一条直接把"我猜是软件"变成了"就是 AW9106B 的驱动模式"：
`0x12(GPMD_A)` 必须写 `0x02`，`0x08` 会让 group-A 输出完全不驱动 SHDN——
这个结论是从"用表量着一步步走、写一个值听一次"（`B220_PA_BIT_FINDER`）里得到的。

### 8.4 保留原厂固件，把它当 A/B 参照

`backup/esp32-16MB-full.bin`（A 机，MD5 `A6A27D2B…`）与
`backup/esp32-16MB-unitB-stock.bin`（B 机，MD5 `9F9FABA6…A49D`，**版本与 A 不同，不可互刷**）
是整个项目最有价值的两个文件。它们解决的问题是**"我们的固件做不到，是硬件不行还是代码不行"**：

| 悬而未决的问题 | 用原厂机得到的答案 |
| --- | --- |
| I2S 到底该跑多少采样率？ | 原厂机 **I2S_WSA = 48 kHz** → 我们的 48 kHz 是对的 |
| BCLK 与 slot 宽度？ | 原厂机 BCLK **1.54 MHz = 32 BCLK/frame** → 16-bit slot，和我们一致 |
| DAC 有没有输出？ | 原厂机播放时 **DAC1_P/DAC1_M 有 100 mV 方波** → 说明"有波形"是正常状态，我们的"没有波形"才是异常 |
| 唤醒词是软件还是硬件触发？ | 反汇编原厂 app → **[逆向] ESP32 软件 WakeNet5**，无 GPIO/HBI/UART 触发 |
| `reg 0x0030` 为什么不更新？ | 原厂也从不用它做判定，用的是 `reg 0x0014` |

操作纪律：

- **备份先做，而且读两遍比 MD5**（见 [FLASH-MANUAL.md](FLASH-MANUAL.md) §4.2）；
- **还原时各用各的备份**（两台原厂版本不同，应用分区约 20% 差异；音频素材区完全一致）；
- **跨机恢复记得清 NVS**（否则两台同 MAC，见排错表 #21）。

### 8.5 把每条结论和它的证据一起写下来

本项目所有 `*.md` 的写法都是同一个模式：**结论 + 证据 + 置信度 + "已经排除的假设"**。
最有价值的部分往往是"已经排除"那张表——它防止后来的自己（或别人）重走一遍：

> `| 假设 | 结论 | 证据 |`
> `| HBI 帧格式/字节序错 | ❌ 排除 | 逐位对照 ADF 参考实现：5 个代表性操作…全部一致；且 0xD3D3 是字节序回文 |`

同样地，本指南每一处判断都标了 **[实测] / [逆向] / [推断]**，
`config.h` 里每个开关的注释都写着"State of the bring-up when this was added"。
**没有证据的结论不准写进代码注释**，因为下一个读它的人会当成事实。

### 8.6 诊断写入**绝不允许是破坏性的**

这是硬规则。观察这个项目里所有"取证"型开关的写入目标：

| 诊断 | 写了什么 | 为什么安全 |
| --- | --- | --- |
| `B220_DSP_VERIFY_UPLOAD` | 只重指 page-255 窗口基址（upload 本来每记录都写），**只读** DSP RAM | 读完 DSP 与 boot attempt 留下的一模一样 |
| `B220_DSP_PAGE255_CALIB` | 往 **scratch RAM `0x00080A24`** 写 16 个字（镜像范围内唯一有文档的空洞） | 不碰任何有效镜像区域 |
| `B220_DSP_VERIFY_BEFORE_GO` | 只读采样记录 + 读 `reg 0x0034/0x0030/0x0032/0x0006` | 在**任何 host 命令之前**取证，不改变后续 |
| `B220_DSP_RELEASE_MATRIX` | 每个变体各做**一次完整全新** reset + upload | 不做增量修改，失败也不留半状态；且"应用已在跑时**拒绝运行**" |
| `B220_ZL_DIAG` 的 `hbi_sweep` | 每个组合**一次只读**寄存器读，之后**恢复**正常设备 | 明确写出"one read-only register read per combination, then the normal device is restored" |
| `B220_PA_BIT_FINDER` | 会写 `0x12/0x04/0x02`（故意） | 这是**唯一**被允许"解除冻结"的诊断，而且解锁时大声报警 |
| 写能力验证（§4.5 of FLASH-MANUAL） | 只往**空的 OTA 槽 `0xc40000`** 写 64 KB 再读回、然后擦除 | 不影响启动、不碰任何出厂数据 |

反过来，`B220_DSP_HANDS_OFF` 那次的教训值得记：
"我们的固件在 reset + `reg 0x14 <- 1` + 重灌镜像，这可能**毁掉一个本来正常运行的 DSP 应用**"——
于是一个专门的"hands off"开关被造出来，**完全不动 GPIO19、不写任何设备寄存器**，只做只读轮询。
**当你不确定某个动作是否安全时，先造一个不写它的版本。**

### 8.7 一条副作用：永远给"关掉诊断"留一条**一行**的路

每个诊断开关的注释里都有 "One-line kill switch" 或 "Set to 0 to get the previous boot
behaviour back"。这条纪律的收益在这个项目里非常具体：生产构建（`B220_AUDIO_LOOPBACK 0` +
全部诊断 0）与实验构建之间只差**几个 0/1**，所以"演示用的稳定版本"和"正在做的实验版本"
永远不会分叉成两条代码线。

---

## 9. 检查清单

### 9.1 最终验收清单（按 boot 顺序，逐项打勾）

串口在 COM25 @115200（`capture-serial.ps1`），往下逐条对：

| # | 项目 | 期望证据（日志 / 现象） | ✅ |
| --- | --- | --- | --- |
| 1 | **屏幕** | `I (704) ili9341: LCD panel create success, version: 2.1.0` + `LcdDisplay: Turning display on`；物理 128×160、颜色正确（不反红蓝） | ☐ |
| 2 | **背光** | `XiaoMoDouB220: AW9106B backlight on (DIM0/1/2 = 0x3F)`（不是 GPIO，是 I2C `0x5B`） | ☐ |
| 3 | **PSRAM / CPU** | `esp_psram: Found 8MB PSRAM device` / `Speed: 80MHz` / `SPI SRAM memory test OK` / `cpu freq: 240000000 Hz` | ☐ |
| 4 | **WiFi** | 首次：`Access Point started with SSID Xiaozhi-4CE6` + `192.168.4.1`；已配网：`Connected to WiFi: <ssid>` + `Got IP: 192.168.x.x` | ☐ |
| 5 | **DSP host-boot** | `I2S up before DSP boot: 48 kHz, 16-bit, stereo` → `reset: GPIO19 low 10 ms` → `HBI attached to SPI2 (cs=0, 10000000 Hz, mode 0)` → `firmware image transferred (3699 records)` → `codec: reg 0x0014 = 0x0200 -> the ZL38063 application is READY` | ☐ |
| 6 | **mic 回环可听** | 回环诊断构建（`B220_AUDIO_LOOPBACK 1` + `B220_AUDIO_TEST_MODE 1`）下 1 kHz 音可听；速率行 `tx_peak=32759 tx_mean~20855 write_err=0` | ☐ |
| 7 | **PA 单一边沿** | `pa: final AW9106 state … \| 0x02 writes=N, SHDN rising edges=1 (AW8737S mode 1)`，且 codec 的 `EnableOutput(true)` 只打 `already ran this boot … IGNORED on purpose` | ☐ |
| 8 | **生产 codec 48 kHz** | `Zl38063AudioCodec: ZL38063 audio codec: 48000 Hz in / 48000 Hz out, 16-bit, 2 slot(s), I2S0 borrowed from the board bring-up` + `audio bring-up: I2S0 tx/rx handles are up` | ☐ |
| 9 | **服务器会话** | `MQTT: Connected to endpoint` + `MQTT: Session ID: xxxxxxxx` + `StateMachine: activating -> idle` | ☐ |
| 10 | **唤醒词识别** | `MODEL_LOADER: Successfully load srmodels` + `LiteAudioEngine: Initialized, WakeNet: yes` + `EspWakeWord: Wake word(wn9_nihaoxiaozhi_tts),freq: 16000, chunksize: 512`；说"你好小智"出现 `Application: Wake word detected: 你好小智 (state: 3)` | ☐ |
| 11 | **完整对话往返** | `connecting -> listening` → `Application: >> …` → `listening -> speaking` → `AudioService: Resampling audio from 24000 to 48000` → `<< …` → `speaking -> listening`；**喇叭能听到回答** | ☐ |
| 12 | **空闲回落** | 空闲后 `Application: Wake word detected` 仍可再次唤醒（第二次会话拿到新的 `Session ID`） | ☐ |

**收尾核对（每次改动都做）**：

```powershell
# 1) 是否走了规范构建入口（而不是裸 idf.py build）
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi

# 2) 生成结果的配置自检
Select-String -Path .\xiaozhi-esp32-b220\build\config\sdkconfig.json `
              -Pattern 'USE_ESP_WAKE_WORD|WAKE_WORD_DISABLED|SR_WN_WN9_NIHAOXIAOZHI_TTS'
#    期望：USE_ESP_WAKE_WORD=True / WAKE_WORD_DISABLED=False / SR_WN_WN9_NIHAOXIAOZHI_TTS=True

# 3) assets 里到底有没有模型
$B='.\xiaozhi-esp32-b220\build'
[System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$B\generated_assets.bin")).Contains('srmodels')  # True

# 4) 刷写 + 读回校验（不带 -FullErase 时保留 NVS/WiFi 凭据）
.\flash-b220.ps1
#    判定标准：Hash of data verified. 并且读回 MD5 与 build\xiaozhi.bin 一致
```

### 9.2 本部分产出/涉及的文件与脚本清单

**生产代码（PART 3 新增或定稿）**：

| 文件 | 作用 |
| --- | --- |
| [zl38063_audio_codec.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063_audio_codec.h) | 生产 codec 的类声明与设计说明 |
| [zl38063_audio_codec.cc](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063_audio_codec.cc) | `Read`/`Write`/`EnableInput`/`EnableOutput`/`SetOutputVolume`/`Start` 的实现（48 kHz 借用 I2S0） |
| [b220_audio_share.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/b220_audio_share.h) | 板级 ↔ codec 的**唯一**接口（`extern "C"`，5 个访问器） |
| [xiaomodou_b220.cc](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/xiaomodou_b220.cc) | `B220_I2sUpBeforeDspBoot()`、PA 五步单例 `b220_pa_enable_one_pulse()`、访问器实现、`GetAudioCodec()`、`InitializeButtons()` 空实现 |
| [config.h](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/config.h) | `AUDIO_INPUT/OUTPUT_SAMPLE_RATE 48000`、全部 `B220_*` 开关（含 `B220_AUTO_LISTEN 0`） |
| [config.json](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/config.json) | 板级 sdkconfig_append（含三行唤醒词配置） |
| [README.md](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/README.md) | 板级硬件表、引脚表、构建方式、已知待测项 |
| `main/audio/audio_codec.h` / `audio_codec.cc` | 基类（**未改**）：`Read`/`Write` 纯虚、`duplex_`/rate/channels 约定 |
| `main/audio/audio_service.cc` / `audio_service.h` | **未改**：48k↔16k 与 24k↔48k 重采样、`AUDIO_POWER_TIMEOUT_MS 15000` 空闲掉电、`SetModelsList()` |
| `main/audio/wake_words/esp_wake_word.cc` | **未改**：`esp_srmodel_filter(ESP_WN_PREFIX)` → `esp_wn_handle_from_name` → `create(name, DET_MODE_95)`，`Feed()` 多通道取左 |
| `main/application.cc` | **未改**：传输选择（`HasWebsocketConfig()` → WS，否则 MQTT）、24 k/48 k 警告 |

**逆向与素材产物**：

| 路径 | 内容 |
| --- | --- |
| `…\zl38063\WAKE-WORD-PATH-REVERSE.md` | 唤醒词路径逆向报告（软件检测、16 kHz 单声道、无外部触发） |
| `…\zl38063\wakenet\README.md` | wn5 模型提取 + esp-sr 兼容性/选项排序的完整分析 |
| `…\zl38063\wakenet\wakenet5_modoujingling.bin` | 提取出的原厂 wn5 模型（84 160 B，sha256 `fb786b2a…`） |
| `…\zl38063\wakenet\wakenet5_modoujingling_layout.txt` | 机器生成的布局 dump（头 + 20 条 `dl_matrix2dq_t` + `model_info_t`） |
| `…\zl38063\wakenet\wakenet5_tool.py` | `info` / `emit` 工具 |
| [ref/analysis/wakenet/gen/modoujingling_wn5.c](ref/analysis/wakenet/gen/modoujingling_wn5.c) | 重新生成的可重定位 C（262 436 B；**尚未接入构建**） |
| [ref/analysis/wakenet/gen/modoujingling_wn5.h](ref/analysis/wakenet/gen/modoujingling_wn5.h) | 对应头文件 |
| `ref\analysis\wakenet\v1.9.5_libwakenet_esp32.a` / `v1.0_libwakenet_esp32.a` | 备用的 wn5 引擎归档（供 §5.6 使用） |
| `ref\analysis\wakenet\v1.9.5_model_path.c` | v1.9.5 的 `model_path.c`（含 `srmodel_get_model_coeff`） |
| [ref/analysis/flashtone/README.md](ref/analysis/flashtone/README.md) | 原厂 `flashTone` 容器格式的独立重推导（74 条记录 × 64 B，token = `0x28 + (n<<8)`） |
| [ref/analysis/flashtone/tone-report.csv](ref/analysis/flashtone/tone-report.csv) | 74 条素材的属性表；唤醒提示音候选是 **032/033/034**（16 kHz 单声道、1.19/1.55/1.30 s、无 ID3） |
| `logs\production-codec.log` | 生产 codec 启动 + PA 锁存（[排错表](#7-合并排错表symptom--根因--精确处置) #7 #8 的证据） |
| `logs\wakeword.log` | **唤醒词成功的完整证据链**（模型加载 → 唤醒 → 多轮对话 → goodbye） |
| `logs\auto-listen.log` | auto-listen 版本 + **UDP 音频通道静默 65 s** 的实证（排错表 #15） |
| `logs\mic-loopback.log` | mic 回环 + peak/mean + L/R 成对样本 hex（§1.2 / §8.2 的证据） |
| `logs\listen-diagnose.log` | 无唤醒词时期"进入 listening 但服务器静默"的对照日志 |
| `logs\i2s-before-dsp.log` / `logs\i2s-tx-sweep*.log` | "I2S 先起"带来的变化 + I2S 变体扫描（48 kHz/16-bit slot 的实测依据） |
| `logs\pa-pulse.log` | PA 单一边沿与 1 kHz 满幅音的成功实验（`tx_peak=32759 tx_mean~20855`） |

**脚本与手册**：

| 文件 | 作用 |
| --- | --- |
| `scripts\build.py` | **唯一规范的构建入口**：`python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi` |
| `scripts\tests\test_build.py` | host 测试（断言 `CONFIG_SR_WN_*` 不能进板级 `config.json`） |
| `scripts\build_default_assets.py` | 把 `srmodels.bin` 打进 `generated_assets.bin` |
| [flash-b220.ps1](flash-b220.ps1) | 一键刷写 + 读回校验；`-FullErase` 首次切换用；自动自检 assets 里有没有 `srmodels` |
| [capture-serial.ps1](capture-serial.ps1) | 抓启动日志到 `logs\`（默认 115200） |
| [FLASH-MANUAL.md](FLASH-MANUAL.md) | 刷机指导书：接线、进/退下载模式、备份/恢复、唤醒词验收（§4.7） |
| [PHASE3-DSP-DEBUG-STATUS.md](PHASE3-DSP-DEBUG-STATUS.md) | ZL38063 调试状态与"已排除的假设"表 |
| [backup/esp32-16MB-full.bin](backup) / `backup/esp32-16MB-unitB-stock.bin` | 两台原厂整片备份（**版本不同，不可互刷**） |

---

## 10. 一句话总结（PART 3）

> 把 B220 变成一台真小智，靠的不是更聪明的代码，而是**三条诚实**：
> **对硬件诚实**（I2S 就是 48 kHz，声明成 16 kHz 只会跑 3 倍速）；
> **对时序诚实**（功放一个 boot 只允许一个上升沿，第二次必须锁存成 no-op）；
> **对证据诚实**（读那个真的会被更新的 `reg 0x0014`，不是看起来更"标准"的 `reg 0x0030`）。
>
> 唤醒词的答案则来自一个更早的逆向结论：**原厂本来就是在 ESP32 上用软件识别关键词**，
> 所以不需要 GPIO、不需要 HBI 触发、也不需要底座。
> 于是"最后一块拼图"落到了一句务实的取舍上——
> 用 2.4.7 现成的 `wn9_nihaoxiaozhi_tts`（**你好小智**，当天可验证），
> 而不是为了原关键词去接 esp-sr v1.x 的 wn5 引擎（**模型已备好、机制已确认、数小时到数天、中等风险，
> 明确未实现**）。

---

*PART 3 完。相关文档：PART 1（可行性/引脚/刷机）、PART 2（板级骨架）、
[FLASH-MANUAL.md](FLASH-MANUAL.md)、[PHASE3-DSP-DEBUG-STATUS.md](PHASE3-DSP-DEBUG-STATUS.md)、
[zl38063/WAKE-WORD-PATH-REVERSE.md](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063/WAKE-WORD-PATH-REVERSE.md)、
[zl38063/wakenet/README.md](xiaozhi-esp32-b220/main/boards/xiaomodou-b220/zl38063/wakenet/README.md)。*
