# WakeNet5 "魔豆精灵" model image — extraction + esp-sr compatibility

Sources: stock `backup/parts/wifi.bin` (ESP32-D0WD, plaintext app image, verified against
`backup/disasm/b220_full.elf`), local esp-sr `2.4.7`
(`xiaozhi-esp32-b220/managed_components/espressif__esp-sr/`), and esp-sr tags
`v1.0` / `v1.9.5` / `v2.0.0` fetched through `api.github.com`.
Analysis scripts live in `ref/analysis/wakenet/`.

## 1. The extracted image

| item | value |
|---|---|
| file | `wakenet/wakenet5_modoujingling.bin` |
| size | **84 160 bytes** (`0x148c0`) |
| sha256 | `fb786b2ae14479141f6124b91dfdc93cba8a9e9bca889332d52a441bdc4aa910` |
| DROM extent | `0x3f4131b0 .. 0x3f427a70` (app-image file offsets `0x131b0..0x27a70`) |
| ends exactly at | `model_info_t` @`0x3f427a70` (word_num=3, word_list=`0x3ffbeee0`, win_list=`{6,6,6}`, thresh_list=`{0.90,0.85}×3`, info_str=the model-name string) |

The extent is *proven*, not guessed: the image is a chain of self-describing
`dl_matrix2dq_t` records and the chain terminates precisely where the model-info
struct starts. Independent cross-check: the name→matrix table baked into the
stock's `get_coeff_q` (IROM `0x4010070c`) references exactly the same 20 matrix
addresses and no others (`ref/analysis/wakenet/pairs.py`).

### Header (offset 0 = `0x3f4131b0`)

`model_coeff_getter_t` — 6 × 32-bit slots (24 bytes) at the head of the image:

| off | field | value |
|---|---|---|
| +0x00 | getter_f | `NULL` |
| +0x04 | getter_q | `0x4010070c` (IROM, name→matrix lookup) |
| +0x08 | getter_q8 | `NULL` |
| +0x0c | free_f | `NULL` |
| +0x10 | getter_info | `0x40100704` (IROM; `entry a1,32; l32r a2,[0x3f427a70]; retw.n`) |
| +0x14 | free_q/free_q8 | `NULL` |

Note: esp-sr v1.0's `include/esp32/dl_lib_coefgetter_if.h` defines the same struct
with **9** slots and `getter_info` at `+0x18`. Our instance is the earlier
**6-slot** revision (slot 4 = info getter), i.e. the stock predates esp-sr v1.0's
header. Any re-emitted descriptor must use the *target* release's layout.

### Object table (24-byte header + int16 items, back to back)

`dl_matrix2dq_t = { int w; int h; int stride; int flags; int exponent; qtp_t *itemq; }`
(`managed_components/espressif__esp-sr/include/esp32/dl_lib_matrixq.h:28-35`),
items = `w*stride` int16. Names recovered from `get_coeff_q`.

| name | image off | stock VA | w | h | flags | exp | int16 |
|---|---|---|---|---|---|---|---|
| kernel_9 | 0x00018 | 0x3f4131c8 | 4 | 64 | 1 | -14 | 256 |
| kernel_8 | 0x00230 | 0x3f4133e0 | 32 | 160 | 1 | -15 | 5120 |
| kernel_7 | 0x02a48 | 0x3f415bf8 | 32 | 160 | 1 | -15 | 5120 |
| kernel_6 | 0x05260 | 0x3f418410 | 32 | 160 | 1 | -15 | 5120 |
| kernel_5 | 0x07a78 | 0x3f41ac28 | 32 | 160 | 1 | -15 | 5120 |
| kernel_4 | 0x0a290 | 0x3f41d440 | 32 | 160 | 1 | -15 | 5120 |
| kernel_3 | 0x0caa8 | 0x3f41fc58 | 32 | 160 | 1 | -15 | 5120 |
| kernel_2 | 0x0f2c0 | 0x3f422470 | 32 | 160 | 1 | -15 | 5120 |
| kernel_1 | 0x11ad8 | 0x3f424c88 | 32 | 160 | 1 | -15 | 5120 |
| kernel_0 | 0x142f0 | 0x3f4274a0 | 32 | 10 | 1 | -16 | 320 |
| bias_9 | 0x14588 | 0x3f427738 | 4 | 1 | 1 | -18 | 4 |
| bias_8..bias_1 | 0x145a8.. | 0x3f427758.. | 32 | 1 | 1 | -17/-16 | 32 each |
| bias_0 | 0x14868 | 0x3f427a18 | 32 | 1 | 1 | -14 | 32 |

`wakenet5_modoujingling_layout.txt` is the machine-generated dump.
`wakenet5_tool.py info|emit <dir>` re-derives the table and emits relocatable C
(`get_coeff_q` strcmp chain + `get_info` + 20 `dl_matrix2dq_t`) so the absolute
stock pointers are replaced by linker symbols.

## 2. esp-sr compatibility

* Which esp-sr supports **ESP32 classic + WakeNet5**: the wn5 engine is
  `esp_sr_wakenet5_quantized` in `lib/esp32/libwakenet.a`.
  * esp-sr **v1.0**: present — `wakenet5_quantized_mono.c.obj`, log string
    `Quantized wakeNet5: %s, mode:%d (%s %s)` (exact match for the stock's
    `Quantized wakeNet5: %s, mode:%d`), `include/esp32/esp_wn_iface.h`:
    `typedef model_iface_data_t* (*esp_wn_iface_op_create_t)(const model_coeff_getter_t *model_coeff, det_mode_t det_mode);`
  * esp-sr **v1.9.5**: still present for esp32 —
    `lib/esp32/libwakenet.a :: wakenet5_quantized_mono.c.obj` defines
    `esp_sr_wakenet5_quantized`, `model_create`, `model_detect`, UND
    `srmodel_get_model_coeff` only; `Deprecated wakenet5: %s, mode:%d`; Kconfig
    still offers `SR_WN_WN5_HILEXIN`, `SR_WN_WN5_NIHAOXIAOZHI`, `SR_WN_WN5X3_*`.
    Since v1.2.0 the create signature is name-based:
    `create(const void *model_name, det_mode_t)`.
  * esp-sr **2.4.7** (the version this repo pins): **no wn5 engine for any
    target.** `lib/esp32/libwakenet.a` contains only
    `wakenet9_quantized/l/s.c.obj` + `esp_wn_models.c.obj` whose UND list is only
    the three wn9 ifaces; the strings `Quantized wakeNet5` / `wakenet5_create` do
    not exist anywhere under `lib/esp32/`. `src/model_path.c` no longer defines
    `srmodel_get_model_coeff`, yet `src/include/model_path.h:166-174` still
    declares it: *"@warning Just support ESP32 to load old wakenet"* — a stale
    declaration for exactly the capability we need.
* **Can esp-sr be fed our blob? Yes, on the v1.x line — but as a *compile-time*
  descriptor, not a runtime file.** The mechanism is quoted from esp-sr v1.9.5:
  * `include/esp32/esp_wn_models.h:90-96`
    ```c
    #include "customized_word_wn5.h"
    #define WAKENET_MODEL_NAME "wn5_customizedword"
    #define WAKENET_COEFF get_coeff_customizedword_wn5
    ```
  * `src/model_path.c:313-317`
    ```c
    model_coeff_getter_t *srmodel_get_model_coeff(char *model_name)
    { model_coeff_getter_t *gettercb = (model_coeff_getter_t *)&WAKENET_COEFF; return gettercb; }
    ```
    i.e. the ESP32 wn5 engine asks for *a descriptor*, the model name is ignored,
    and the descriptor comes from a build-time macro. Defining our own
    `get_coeff_modoujingling_wn5` and pointing `WAKENET_COEFF` at it is supported
    usage, not a hack. There is **no** external-blob/partition loader for wn5
    (`srmodel_load`/`movemodel.py` cover wn7/wn8/wn9 models; the wn5 model
    archives `libhilexin_wn5.a`, `libcustomized_word_wn5.a`, … are compiled-in C
    in every release from v0.7.0 through 2.4.7).
* **Format match**: ours *is* an Espressif-era wn5 model in exactly that form —
  `model_coeff_getter_t` + `dl_matrix2dq_t` rodata, same `kernel_%d`/`bias_%d`
  coefficient names as `libhilexin_wn5.a` / `libwakeword_model.a`, and its
  name string `wakeNet5_v3_modoujingling_6_0.90_0.85_…` follows the same grammar
  as `wakeNet5_v1_hilexin_5_0.95_0.90`, `wakeNet5X3_v5_hilexin_5_0.97_0.90`.
  The coefficient archives in 2.4.7 were even rebuilt with this project's
  toolchain (`GNU C17 14.2.0 … -mdynconfig=xtensa_esp32.so`), while
  `libcustomized_word_wn5.a` still carries the *stock's own* build path
  (`/home/wangwangwang/workspace/idf-version/v3_3/...`), confirming a common
  ancestry.

## 3. What this repo already has

* `main/idf_component.yml:43` → `espressif/esp-sr: ==2.4.7`; the component is
  already unpacked in `managed_components/espressif__esp-sr` (also `lib/esp32`
  and `include/esp32`, so ESP32 is a supported target:
  `CMakeLists.txt:1,90`).
* Wake-word front end exists: `main/audio/wake_words/esp_wake_word.cc` uses
  `esp_srmodel_init` / `esp_srmodel_filter(…, ESP_WN_PREFIX, NULL)` /
  `esp_wn_handle_from_name` / `create(model_name, DET_MODE_95)`, and
  `main/Kconfig.projbuild:908-912`:
  ```
  config USE_ESP_WAKE_WORD
      bool "Wakenet model without AFE"
      depends on IDF_TARGET_ESP32C3 || IDF_TARGET_ESP32C5 || IDF_TARGET_ESP32C6 || (IDF_TARGET_ESP32 && SPIRAM)
  ```
  The B220 board satisfies that gate: `boards/xiaomodou-b220/config.json` sets
  `CONFIG_SPIRAM=y` / `CONFIG_SPIRAM_SPEED_80M=y` and the board is
  `"target": "esp32"`; `config.h` documents "ESP32-D0WD rev1.0, 16 MB flash,
  8 MB PSRAM".
* `sdkconfig.defaults.esp32:3` already selects `CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y`,
  i.e. on ESP32 this repo expects a **WakeNet9** model ("你好小智") — the only
  wakenet family 2.4.7 can still run on a classic ESP32. There is **no** wn5
  option left in 2.4.7's `Kconfig.projbuild`.

## 4. Options (ranked)

**Recommendation: (b) for the exact keyword, (a) if any keyword will do.**

1. **(b) esp-sr v1.x + our model** — *reproduces 魔豆精灵 on the ESP32 exactly*,
   no retraining. Keep esp-sr 2.4.7 for AFE/other models but take the wn5 engine
   from `v1.9.5 lib/esp32/libwakenet.a` (calls `srmodel_get_model_coeff`, which
   2.4.7 no longer defines — supply it, or vendor v1.9.5's `model_path.c`
   additively), drop in the generated `modoujingling_wn5.c`, and set the wn5
   `WAKENET_COEFF`/`WAKENET_MODEL_NAME`. Effort ≈ 1–2 days; risk: medium
   (duplicate `esp_wn_handle_from_name` / `model_create` symbols vs 2.4.7's wn9;
   prebuilt v1.9.5 archive is GCC-13/IDF-5.x era — must be link-tested against
   IDF 6.1; no runtime blob loading so the model costs ~84 KB of DROM/flash).
   Fallback if mixing two esp-sr generations proves messy: pin the whole
   component to **v1.9.5** and use its own `esp_wake_word.cc`-compatible API.
2. **(a) esp-sr 2.4.7 + existing wn9 model** — *zero reverse-engineering, wrong
   keyword.* Enable `CONFIG_USE_ESP_WAKE_WORD=y` for the b220 variant and pick a
   Chinese wn9 model already in `managed_components/.../model/wakenet_model`
   (`wn9_nihaoxiaozhi`, `wn9_nihaoxiaozhi_tts`, `wn9_xiaolongxiaolong_tts`,
   `wn9_xiaomeitongxue_tts`, `wn9_nihaoxiaoxin_tts`, …). Effort hours, risk low;
   needs the `model` partition wired for the ESP32 load path. Useful as a
   bring-up/fallback so the wake path is proven before touching wn5.
3. **(c) train/obtain an esp-sr-compatible wn5 model for 魔豆精灵** — *not
   available by normal means.* Espressif's wake-word customization now produces
   **WakeNet9 / wn9s / wn9l** (and MultiNet) models; the only "custom" artefact in
   current esp-sr is `wn9_customword`, and 2.4.7's Kconfig exposes no wn5 model
   option at all. We do not need this route anyway: the stock already contains a
   trained wn5 model for exactly this keyword.
4. **(d) port the stock's own IROM engine** — *not recommended.* The code exists
   (loader `0x400fbf0c`→`0x400fa198`, inference `0x400fc7ec`, int16 MAC
   `0x400fce30`, `free`/`get_info` `0x40100704`, `get_coeff_q` `0x4010070c`), but
   it is position-dependent machine code whose calls into `dl_lib`, cJSON,
   FreeRTOS and newlib resolve to *stock* addresses; lifting it would mean
   relocating thousands of literals and re-binding every ROM/newlib symbol
   (`strcmp` is `0x4009e394`). Far more work, far more risk, and it duplicates
   what esp-sr v1.x already ships as a linkable archive.
5. **(e) another trigger** — ask the user in Chinese with a wn9 model, or
   button/soft trigger, as a temporary stand-in.

### Concrete next step

1. Prove the plumbing with (2): build the b220 variant with
   `CONFIG_USE_ESP_WAKE_WORD=y` + a wn9 Chinese model and confirm the wake path
   end-to-end (the recorder already runs at 16 kHz, which is what wn5/wn9 want).
2. In parallel, prepare (1): `python wakenet/wakenet5_tool.py emit <dir>` (done
   once, output in `ref/analysis/wakenet/gen/modoujingling_wn5.c`, 262 KB,
   84 KB of rodata), vendor the v1.9.5 esp32 wn5 engine archive plus
   `srmodel_get_model_coeff`, and set `WAKENET_MODEL_NAME` to the stock info
   string so `esp_wn_wakeword_from_name` still reports `modoujingling`.
3. Verify per `AGENTS.md`: host tests for the Kconfig/CMake change, then a real
   flash + "魔豆精灵" utterance test; a successful build is not hardware
   validation.

## 5. Not determined

* Whether the stock's 6-slot `model_coeff_getter_t` field order matches any
  published header revision (esp-sr v1.0's is 9 slots); the *contents* are known,
  only the historical field names are not.
* Which esp-sr release is the last with the esp32 wn5 engine — v1.9.5 has it and
  2.4.7 does not; the exact tag between them was not bisected.
* Whether `esp_sr_wakenet5_quantized` + our descriptor actually yields correct
  detections (thresholds 0.90/0.85, 6-frame window, 3 keyword slots) — this needs
  a build and a microphone test.
* Whether the wake-word list in `model_info_t` is used at all by the v1.x wn5
  engine (esp-sr's `get_wake_words_from_info` parses `_`-separated tokens at
  index 3/7/11 of this info string and would yield "6;6;6", which suggests the
  parser expects a different info layout than the stock's).
