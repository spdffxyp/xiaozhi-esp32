# Wake word ("魔豆精灵") + wake flow — reverse-engineering result (B220 stock `backup/parts/wifi.bin`)

Method: string locate → byte-decoded L32R xref index → pointer/table dumps → `objdump` on
`backup/disasm/b220_full.elf` (correct VMAs). App VAs. Confidence [H]/[M]/[L]. Linear listings in
`0x400fa000..0x400fc000` desync on literal pools, so every claim below rests on decoded L32R targets,
data dumps, or byte-verified instruction windows.

## 1. Mechanism

```
3 mics -> ZL38063 (AEC/NR/beamform ASR-assist front end)
        |  I2S, 16 kHz mono (ESP32 = I2S master)
  ESP32: task rec_engine_task (0x400fac04) reads fixed blocks, runs VAD
         (WebRTC vad_filterbank.c / vad_sp.c are linked) and the local keyword spotter
         (Turing/esp32-sr DNN + MFCC). "### spot keyword ###" @0x3f4122e1, log site 0x400fb0f7.
         On hit (test `bnei a10,1` @0x400fb500): flag 0x3ffc5830<-1, state 0x3ffc5838<-2,
         arm tm_wakeup 0x3ffc3850 with 0xb40 ticks (4000 ms) and 0x3ffc384c, push a wake
         event into the AI/wakeup service via the descriptor at 0x3ffc3864 (site 0x400fb594).
```

**Where the wake word is detected: the ESP32, in software** — not the DSP, AI module or baseboard.
* `wakeNet5_v3_modoujingling_6_0.90_0.85_kuaisuqianjin_6_0.90_0.85_tiaogewu_6_0.90_0.85` @DROM `0x3f413134`; keyword names `modoujingling/kuaisuqianjin/tiaogewu` @`0x3f413189/97/a5`, pointed to from DRAM `0x3ffbeee0/e4/e8` [H]
* dispatch table @DROM `0x3f427a74`: `{0x3ffbeee0, 0x3f427a9c, {0.90f,0.85f}×3, 6,6,6}` — thresholds 0.90/0.85, 6 frames, 3 keyword slots [H]
* wakenet config @DRAM `0x3ffbeeb0`: `+0x00`=0x4000bf18, `+0x34`=0x3f40d5a8, then 1, **0x3e80 = 16000 Hz**, 30, 1, 0, 1 → **KWS input is 16 kHz mono** (not our 48 kHz loopback) [H]
* loader `0x400fbf0c` → `0x400fa198(cfg, blobs)`; `0x400fbefc` reload path; engine struct = **0x2e0 bytes** (malloc @`0x400fbd77`) [H]
* inference `0x400fc7ec` (frame 0xfd80) and `0x400fce30` = int16 MAC, `srai 12`, saturate to 16 bit; Turing DL strings `/home/wangwangwang/workspace/AI/20190601_turing/Turing/components/dl_lib/…`, `Quantized wakeNet5: %s, mode:%d`, `csf_mfcc_HTK`, `MFCC_LOGFBANK`, `kernel_%d/bias_%d` [H]
* DSP side: the only literals-verified HBI traffic is boot (3699 firmware records + 1042 config pairs @`0x40126240`/`0x40125c64`/`0x40125c30`) and the reg-0x06 mailbox poll — **no runtime arm-trigger command and no HBI read that could carry a trigger** [H for decoded code, M for completeness]. `gpio_set_intr_type(38, INTR_DISABLE)` @`0x40124ce8` only disables (pin 38 does not exist on ESP32-D0WD) → not a trigger path [H]
* local-spotting side effects: sibling handlers log `### go offline ###` `0x3f41230d` and `### dance offline ###` `0x3f412337`; flashTone holds 16 kHz mono 1.2–1.5 s prompts as entries 032/033/034 (`0x2028/0x2128/0x2228`, `mp3-no-tag`, ffmpeg: 1.19/1.55/1.30 s) [H]

## 2. Trigger transport into the ESP32 — no external trigger exists

Internal state-machine event only: keyword hit inside `rec_engine_task` (FETCH_DATA handler `0x400fb4d3`) →
flags `0x3ffc5830`/`0x3ffc5838` → `xTimerGenericCommand` on `0x3ffc3850` ("Wakeup time is out" handler `0x400fa5d8` clears
`0x3ffc5830`) and `0x3ffc384c` → indirect wake-event push through the descriptor at `0x3ffc3864` (site `0x400fb594`).
No GPIO edge, no HBI poll, no UART message is involved in waking [H].

## 3. Wake response "请吩咐"

* **"请吩咐" is not in the 16 MB dump in any encoding** — UTF-8, GBK, GB18030, Big5, UTF-16LE/BE, searched whole-image and per-string in the app image [H]. It is not a firmware string.
* Most likely a **pre-rendered local prompt** in the 8 MB `flashTone` partition; the only files with the right shape (16 kHz mono, 1.2–1.5 s) are entries 032/033/034. Cloud TTS (Turing) or the AI module's own prompt remain possible [L] — needs one listen/transcription.
* Anchor instead of the string: the `voice_work_mode` three-way branch @`0x400fb25c`/`0x400fb2a5`/`0x400fb313` — mode 1 stays in the offline wake loop; mode 2 logs `### go offline ###` + calls `0x4012b820`; mode 3 logs `### dance offline ###` + calls `0x4012b848`; both post to the Turing AI task (`0x4012e79c`) and release the player mutex `0x3ffc3864` (`xSemaphoreGive` `0x40090814`).

## 4. What the stock does right after waking

1. `wakeup_flag=1`, 4000 ms wake timeout armed, recorder enters the wake/dialog loop (`======1/2 wake up loop======`).
2. `voice_work_mode` selects **online** (Aiwifi `TCP` session to `smartdevice.ai.tuling123.com`: `AiwifiConTask`, `AiwifiDatatTask`, `AiwifiThroughTask`, `AiwifiPlayerTask`, `AiwifiTvt_Voice_Send`; TTS returns in `AiwifiPlayerTask`, playback enables AW8737S via AW9106 reg 0x02<-0x02 @`0x401253ed`) or **offline** (local prompt / dance motion).
3. Nothing in the ZL38063 is re-configured per wake; the boot-time config stays in force [H].

## 5. Shortest path to reproduce in our firmware (ESP32-only, no DSP trigger needed)

1. Keep the DSP host-boot + 1042-entry config as-is, but give the recorder/KWS path **16 kHz mono** (set the cross-point/I2S output to 16 kHz, or decimate 48 kHz→16 kHz in the capture task).
2. Port the in-tree stock engine: load `0x400fbf0c`→`0x400fa198` with the record @`0x3ffbeeb0`; alloc `0x400fbd74` (0x2e0); inference `0x400fc7ec` + `0x400fce30`; features `csf_mfcc_HTK`; VAD from `vad_filterbank.c`/`vad_sp.c`. Expected values: 16 000 Hz, 30-frame window, thresholds 0.90/0.85, 6 frames, 3 keyword slots; blob base DROM `0x3f4131b0` (64-byte payload after the model-name string).
3. Decision: `detect(block)==1` → reproduce `0x400fb500`: set flag, arm 4000 ms timeout, log `======1 wake up loop======`, then open **our** ASR session instead of the Turing TCP session.
4. Prompt: play flashTone index 32/33/34 (0x2028/0x2128/0x2228) and confirm by ear which says "请吩咐".
5. **Do not** add "poll HBI reg 0xNN" or a HINT/GPIO interrupt — the stock has neither.

## 6. Not determined

* Which file is "请吩咐" (not a string anywhere; needs a listen/ASR of entries 032/033/034) [L].
* Exact byte extent of the wakenet model blob and the names of the engine function-pointer table @DROM `0x3f4125…` / vtable `0x3ffc583c..0x3ffc5854` (code located, blob bounds unproven) [L].
* What sets `voice_work_mode` (bss/RAM byte read by the recorder; only AI-module event-handler writes decoded) [M].
