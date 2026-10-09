# ADF reference ZL38063 image vs. our flash-dump image — comparison report

Analysis only. No board files modified. All new files under
`ref/analysis/zl38063_adf/`.

## 1. Extracted ADF data

`libfirmware.a` holds two objects; `xtensa-esp32-elf-nm` symbols:

| object | symbol | size |
| --- | --- | --- |
| `zl38063_firmware.o` | `st_twFirmware` | `0x1a59c` = 107932 B = **2453** × 44 |
| `zl38063_firmware.o` | `programBaseAddress` / `executionAddress` | `0x00080200` / `0x00084040` |
| `zl38063_firmware.o` | `haveProgramBaseAddress` / `firmwareStreamLen` | `1` / **2453** |
| `zl38063_config.o` | `st_twConfig` | `0x1048` = 4168 B = **260** × 4 |
| `zl38063_config.o` | `configStreamLen` | **1042** |

Note: `nm -n` ordering puts `haveProgramBaseAddress` @`0x1a5a4` and
`firmwareStreamLen` @`0x1a5a6` (declaration order in the header is reversed).

| file | size | sha256 |
| --- | --- | --- |
| `adf_firmware_rodata.bin` | 107944 | `295914a4…2289` |
| `adf_firmware_records.bin` (2453 × 44) | 107932 | `01fba81f…afa1` |
| `adf_config_rodata.bin` | 4170 | `c7038a4d…5e1e` |
| `adf_config_260.bin` | 1040 | `9725787a…36a9` |
| `ours_config_260.bin` | 1040 | `1038a34a…e013` |

## 2. Record layout — identical, no packing difference

Both use `twFwr` = 44 B exactly as `vproc_data_types.h` (`buf[16]` +0, `numWords`
+32, pad, `targetAddr` +36, `useTargetAddr` +40, pad to 44). ADF record 0..63
headers equal ours position-for-position (same `numWords`, same `targetAddr`,
`useTargetAddr == 1` everywhere). **No realignment was needed.**

| metric | ADF | ours |
| --- | --- | --- |
| records | 2453 | 3699 |
| array bytes | 107932 | 162756 |
| payload words | 39204 | 59122 |
| `numWords` histogram | {2:1, 6:2, 12:2, 14:1, 16:2447} | {2:2, 6:2, 10:1, 12:2, 16:3692} |
| DSP addr span | 0x80128–0x9696e | 0x80128–0x9f4e6 |
| entry point | 0x84040 | 0x83040 |

Position-wise payload comparison is misleading: both arrays end with
overlay records re-targeting 0x801xx, so the *record order* differs. On the
**address axis** the result is clean and decisive:

* **ADF-only addresses: 0.** Every ADF word address is present in ours.
* **ours-only addresses: 19918** (0x80a00–0x9f4e6, 78.9 % non-zero — real code,
  not padding). → the ADF address set is a **strict subset** of ours.
* Over the 39204 shared addresses, **38011 values differ (96.96 %)**; only 1193
  match. Not a byte-order artefact (byte-swapped matches are *fewer*: 854).
  XOR-popcount: 19178 words differ by 3–7 bits, 16893 by 8–11 bits, 1126 by
  12+ — i.e. systematically different data/coefficients, not a shifted copy.

First differences (address axis, final applied value):

| addr | ADF | ours |
| --- | --- | --- |
| 0x80128 | `fdc6` (rec 2446) | `fbf9` (rec 3692) |
| 0x8012e | `4040` | `3040` |
| 0x80206 | `1000` (rec 0) | `0041` (rec 0) |
| 0x80318 | `0000` (rec 8) | `000f` |
| 0x8031c | `7fff` | `000f` |
| 0x84000 | `3144` (rec 64) | `a000` (rec 194) |

Hex, first mismatch region (0x80128, ADF / ours):

```
0x80128 ADF  fdc6 0000 0008 4040 913d 0000 0000 0000 0000 0000 0000 0001 0101 2970 0008 4000
0x80128 ours fbf9 0000 0008 3040 f4b4 0000 0000 0000 0000 0000 0000 0001 0101 c4e8 0008 3000
```

The reset-vector/entry word differs in a structured way (`…0008 4040` vs
`…0008 3040` = entry 0x84040 vs 0x83040), and each entry address is the first
word of its block (ADF rec 66 @0x84000, ours rec 196 @0x83000) — same design,
different build.

## 3. Config record

`configStreamLen` = 1042 both sides → **260 entries, same length**. The dump's
stored length word (`0x59ea8`) also reads 1042.

| metric | value |
| --- | --- |
| entries | 260 / 260 |
| differing pairs | **21 / 260** |
| distinct pairs | 260 / 260, common 239, ADF-only 21, ours-only 21 |

Full diff list (reg, ADF value, ours value):
`0x0200` 0/1 · `0x0212` 0x0605/0 · `0x0224` 1/2 · `0x0228` 2/3 · `0x022a` 3/1 ·
`0x0232` 6/0 · `0x0238` 0x0202/0xf1f1 · `0x023a` 0x0202/0xf1f1 · `0x0262`
0x03f6/0x01f6 · `0x0264` 0xc0c0/0xc040 · `0x02a0` 0xc000/0xc005 · `0x02b2` 0/1 ·
`0x02b6` 0x0140/0 · `0x02d8` 0x0500/0 · `0x02e0` 0x0ff7/0x0200 · `0x02e2`
0xffff/0xffdf · `0x02e4` 0/0x1d40 · `0x02e6` 0/0x1d40 · `0x030c` 0x0a10/0x0810 ·
`0x0310` 0x0064/0x0190 · `0x0312` 0/0x000e

## 4. Verdict

* **Firmware:** *different revision*, and ours is a **superset on the address
  axis** — the ADF image writes a strict subset of our addresses, with
  systematically different content and a different entry point. Same `twFwr`
  layout, same file/version banner (`Microsemi_ZLS38063_1_P1_4_0`, Sep 2018),
  but a different ZLS38063 build/configuration. Confidence: **high** (symbol
  sizes, entry-point structure, subset relation with zero ADF-only addresses all
  agree).
* **Config:** same length, **different configuration** (21/260 values), i.e. a
  different product profile. Confidence: **high**.

## 5. Fallback-experiment assessment (not run — no hardware here)

Structurally loadable: same 44-byte records, same `prgmBase` 0x80200, same
`numWords` distribution shape, entry point aligned to the first word of a record.
The host-side loader is version-agnostic (`twFirmwareStreamLen` + `execAddr`
come from the object), so swapping in the ADF object is a *configuration* change
only, not a protocol change. The ADF image is a legitimate ESPRESSIF-licensed
copy for ESPRESSIF products on the same ZL38063.

Caveat: ADF's `executionAddress` is 0x84040 and its multi-record overlay tail
(records ~2446–2452 at 0x801xx) is tied to that build. Trying it is worthwhile
**only as a paired firmware+config experiment**, and the practical value is
limited because ours is the stock shipping image for this exact product — the
ADF set is *older/smaller*, not a newer replacement. Expect it to load, not to
improve anything.

## 6. Uncertainty

* Whether the ADF `firmwareStreamLen` = 2453 vs. its symbol-size-derived record
  count is a real inconsistency: **no** — both are 2453, they agree.
* `0x8636e`-region and the 85 differing runs were not disassembled, so the
  characterisation "different build" rests on statistics (popcount/zero
  distribution, entry-point layout), not on decoded code.
* Field-level meaning of the 21 config registers is not established here.
