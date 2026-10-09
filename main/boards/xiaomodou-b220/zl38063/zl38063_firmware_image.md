# ZL38063 DSP firmware image — found in the stock flash dump

Status: **CONFIRMED**. The stock B220 app image carries the complete DSP
host-boot payload (firmware records + config record). Nothing here is inferred
from a linear disassembly: every step below was re-decoded one instruction at a
time from raw bytes (objdump queried per-address from confirmed boundaries).

Dump: `backup/parts/wifi.bin`, DROM segment load `0x3f400020`, file offset
`0x000020`, size `0x1ce1c8`.

## 1. Record-array base = DROM `0x3f4322e4` (file offset `0x322e4`)

App boot wrapper `0x40126240` (`tw_upload_dsp_firmware` in
`adf_ref/example_apps/tw_spi_access.c`), verified region `0x40126285..`:

```
40126285: bltui a2, 2, 4012628b      ; mode < 2 -> load firmware (mode 0 or 1)
40126288: j 40126355                 ; mode == 2 -> config only
4012628b: l32r a3, 40111b38 (3f4322e4)   ; literal-pool entry holds the DROM address
4012628e: s32i.n a3, a1, 0           ; twFirmware.st_Fwr = 0x3f4322e4
40126298: l32r a3, 40111b3c (3f4322d8) ; l16ui a3,a3,0 -> 0x0E73 = 3699
4012629e: s16i a3, a1, 20            ; twFirmware.twFirmwareStreamLen = 3699
401262ac: l32r a3, 40111b40 (3f4322dc) ; l32i.n -> 0x00083040
401262b1: s32i.n a3, a1, 16          ; twFirmware.execAddr  = 0x00083040
401262bb: l32r a3, 40111b44 (3f4322da) ; l8ui -> 1
401262c1: s8i a3, a1, 8              ; twFirmware.havePrgmBase = 1
401262cf: l32r a3, 40111b48 (3f4322e0) ; l32i.n -> 0x00080200
401262d4: s32i.n a3, a1, 12          ; twFirmware.prgmBase  = 0x00080200
401262ed: mov.n a10, a1
401262ef: call8 40125c64             ; VprocTwolfHbiBoot(&st_Firmware)
```

The five literal-pool words at `0x40111b38..0x40111b48` were read directly:
they hold `3f4322e4, 3f4322d8, 3f4322dc, 3f4322da, 3f4322e0`. `st_Fwr` is a
**literal DROM address** (hypothesis (a)), not a DRAM global.

`VprocTwolfHbiBoot` `0x40125c64` (loop top `0x40125cfd`, verified):

```
40125cfd: addx4 a2, a6, a6        ; a2 = 5*i
40125d00: addx2 a2, a2, a6        ; a2 = 11*i
40125d03: l32i.n a3, a4, 0        ; a3 = struct->st_Fwr          (= 0x3f4322e4)
40125d05: slli a2, a2, 2          ; a2 = 44*i          <-- STRIDE IS 44, not 28
40125d08: add.n a3, a3, a2        ; a3 = &record[i]
40125d0a: l32i.n a8, a3, 36       ; u32 @+36 = targetAddr
40125d0d: extui/s16i -> tmp[0]=hi16, tmp[1]=lo16
40125d1d: l16ui a8, a3, 32        ; u16 @+32 = numWords
40125d26: bnez.n a8, ...          ; numWords == 0 -> skip record
40125d30: l8ui a3, a3, 40         ; u8  @+40 = useTargetAddr
40125d36: (useTargetAddr) hbi_write16(0x00C, 2, {hi16,lo16})   ; set target
40125d68: hbi_write16(0xFF, lowbyte(targetAddr), numWords, buf); page-255 data
40125d8f: hbi_write16(0x12C, 2, execAddr)                      ; entry point
```
This is `HbiSrecBoot_alt()` from `adf_ref/api_lib/vprocTwolf_access.c`, and the
referenced log strings match it line for line (`prgmBase 0x%08x` @`3f431ac7`
printed from `a4+12`, `execAddr 0x%08x` @`3f431aef` printed from `a4+16`, so
`prgmBase = 0x00080200`, `execAddr = 0x00083040` — note PROTOCOL-REPORT.md
§2.4 has these two swapped).

## 2. Record layout (this build) and array extent

```c
typedef struct {           /* sizeof == 44, 41 bytes rounded up to 4 */
    uint16_t buf[16];      /* +0  .. +31 */
    uint16_t numWords;     /* +32 .. +33, pad +34..+35 */
    uint32_t targetAddr;   /* +36 .. +39 */
    uint8_t  useTargetAddr;/* +40, pad +41..+43 */
} twFwr;                   /* identical to adf_ref vproc_data_types.h */
```
Extent `3699 * 44 = 0x27BC4` from `0x3f4322e4` -> ends exactly at `0x3f459ea8`,
which is where the config record begins (u16 length 1042 at `0x3f459ea8`,
`dataArr[260]` at `0x3f459eaa`, already extracted as `zl38063_config_record.h`;
the wrapper loads it two instructions later with `l32r a10, 0x40111b64
(3f459eaa)` + `call8 40125c30`). Independent confirmation of the array length.

Validation: 3699/3699 records non-empty; numWords histogram {16:3692, 12:2,
10:1, 6:2, 2:2}; `useTargetAddr == 1` for all 3699; 118244 payload bytes
(59122 words) mapped with **zero overlaps**; covered DSP word range
`0x00080128..0x0009f4e6`; single hole `0x80a24..0x82fff` (9694 B). 8.6% zero
words, 0.1% `0xFFFF`, 10659 distinct words — real code/coefficients, no padding
pattern. Record 0 targets `0x00080200` (= prgmBase) and execAddr `0x00083040`
sits 0x40 into the block that starts at record 66 (`0x83000`): a reset-vector +
entry layout, exactly what a DSP image should look like.

## 3. It is live code, not dead code

`0x40124454` (raw-byte-verified: every `l32r` pool read back matches) is the
power-up loader:

```
40124418: gpio_config(pin 19) / set 0 / delay 10 ms / set 1   ; DSP RESET pulse
40124454: entry a1, 32
40124464: call8 40124ce8 (bus init) ; call8 40124418 (reset pulse)
4012446b: callx8 40091774 (vTaskDelay(500))
40124474: movi.n a2, 6               ; 6 attempts
40124476: movi.n a10, 0              ; mode = 0 (firmware + config)
40124478: call8 40126240             ; tw_upload_dsp_firmware(0)
4012447b: beqz a10, done / retry
```

## 4. Verdict / what to do

* DSP firmware image **exists** in this dump -> high confidence (raw-byte
  instruction chain + literal-pool cross-check + independent array-boundary
  cross-check against the config record + live call site).
* Our new firmware should **host-boot the DSP on every power-up** over HBI, the
  same way the stock app does: reset GPIO19 pulse, HBI init, reg 0x14 = 1,
  300 ms, reg 0x34 == 0xD3D3, 3699 record writes (reg 0x0C target + page-255
  data), reg 0x12C = 0x00083040, reg 0x32 = 0x000D, poll reg 0x06, then the
  1042-byte config record. Do **not** rely on DSP self-boot.

## 5. Files

| file | content |
| --- | --- |
| `zl38063_firmware_image.bin` | 118244 B, payload words concatenated in record order (sha256 `c8267e3c…30ac`) |
| `zl38063_firmware_records.bin` | 162756 B, byte-exact copy of the DROM record array (sha256 `ce9d8e83…a344`) |
| `zl38063_firmware_image.h` | 3699 `twFwr` entries + scalars; static by default, `-DZL38063_FWIMAGE_EXTERNAL` emits the strong `st_twFirmware`/`firmwareStreamLen`/`executionAddress`/`haveProgramBaseAddress`/`programBaseAddress` symbols |
| `zl38063_extract_fw.py` | regenerates all three and re-runs every check above |

Uncertainty: the exact Xtensa CALL8 immediate bit-field is ambiguous by ±1/±2 by
hand, so call sites were resolved with objdump on regions whose `l32r` pool
values were independently verified from raw bytes; the caller *of* the retry
function `0x40124454` was not established (no direct `call8` or function-pointer
literal found in ±128 KB), so "runs at every power-up" rests on the loader
shape, the log-string set and the earlier runtime-log evidence.
