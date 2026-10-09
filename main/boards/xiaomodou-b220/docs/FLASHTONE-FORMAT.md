# flashTone container format (小魔豆 B220 stock firmware) — independent re-derivation

Source image: `backup\esp32-16MB-full.bin` (unit A stock, 16 MiB full dump).
Region: partition `flashTone`, flash offset **0x42d000**, length **0x800000** (8 MiB).

* region MD5 (unit A) = `afde056c2d1b703a22dd4368c4a8b1f7`
* region MD5 (unit B, `esp32-16MB-unitB-stock.bin`) = `afde056c2d1b703a22dd4368c4a8b1f7` → **byte-identical**

## Layout

```
partition offset 0x000000  .. 0x001280   index table, 74 records x 64 bytes (0x1280 bytes)
partition offset 0x001280  .. 0x001288   8 zero bytes of padding
partition offset 0x001288  .. 0x775120   payloads, one after another (7,814,720 bytes)
partition offset 0x775120  .. 0x800000   569,056 bytes of erased 0xFF (no entry, no footer)
```

### Record format (64 bytes, little-endian)

| off | type | meaning | evidence |
| --- | --- | --- | --- |
| +0  | u32 | magic; `"SJ\0"` (bytes `53 20 4a 00`) on record 0 only, `0` on all others | raw record 0 begins `53 20 4a 00 00 00 00 00` |
| +4  | u32 | always 0 (reserved) | all 74 records |
| +8  | u32 | **index token** = `0x28 + (index << 8)` | 74/74 records: `0x28, 0x128, 0x228, … 0x4928` |
| +12 | u32 | **payload offset** inside the partition | record 0 = `0x1288`; every payload begins with `ID3`/MPEG sync/`<?xml` |
| +16 | u32 | **payload length in bytes** | see proof below |
| +20..+63 | 44 bytes | always `00` | raw hex dump, all records |

So index 32/33/34 carry token `0x2028/0x2128/0x2228` — exactly the index-like fields a
previous pass noticed. The firmware most likely keys a clip as `0x28 + (n << 8)`.

### Proof that the table stride is 0x40

Probe: count how many consecutive records satisfy `u32@+8 == 0x28 + 0x100*n`.
stride 0x20 → 1 hit, **stride 0x40 → 74 hits**, stride 0x80 → 1 hit, stride 0x100 → 1 hit.

### Proof that +16 is a length (not a CRC)

* For all **74/74** records, `offset + length` lands 0–3 bytes before the next record's
  offset, and those filler bytes are `0xFF` (88 bytes of padding in total across 40 gaps).
* The accounting closes exactly:
  `table 4736 + bump 8 + payloads 7814720 + padding 88 + erased tail 569056 = 8388608` = partition size.
* `crc32(payload) == stored(+16)` for **0/74** records → the field is not a checksum.
  (An earlier script in `tools\unpack_tones.py` mis-read +16 as a CRC and sliced payloads by
  "distance to the next offset" instead; that only worked because of the 0–3 byte padding.)

Raw record 0, for reference:

```
53 20 4a 00 | 00 00 00 00 | 28 00 00 00 | 88 12 00 00 | 4e ab 01 00 | 00 x44
"SJ\0"        reserved      0x28          0x1288         109390         zeros
```

## Inventory

| metric | value |
| --- | --- |
| entries the table claims | 74 |
| extracted | 74 (all), 7,814,720 bytes |
| audio (decodable) | 69 → 69 WAVs |
| non-audio payloads | 5 — Audacity 2.3.2 project XML (indices 041, 044, 045, 046, 047) |
| duplicates (identical MD5) | 0 — 74/74 distinct |
| empty / all-0xFF entries | 0 |
| skipped | nothing; the 569,056 trailing bytes are erased flash, not an entry |
| gaps / overlaps between payloads | none; only 0–3 byte `0xFF` alignment padding (88 B total) |
| unit A vs unit B | identical (whole 8 MiB region, and all 74 payloads) |

XML payloads are Audacity `.aup` projects for a vehicle-sound pack
(`42Police`, `45Truck`, `46Racing`, `47Fire_engine`, `48Helicopter`, all 22050 Hz) — the
audio files themselves are indices 042/043/048 in the same table.

## Wake-prompt shortlist (`请吩咐`)

The phrase does **not** appear as text anywhere in the 16 MiB image (searched UTF-8,
UTF-16LE and GBK, whole image — not only the app partition), so the prompt exists only as
audio and cannot be named from metadata. The vendor-TTS-shaped family is:

| idx | file | bytes | rate | ch | dur | note |
| --- | --- | --- | --- | --- | --- | --- |
| **032** | `032.mp3` | 9504 | 16000 | 1 | 1.188 s | **no ID3 tag** (raw MPEG frames) |
| **033** | `033.mp3` | 12384 | 16000 | 1 | 1.548 s | **no ID3 tag** |
| **034** | `034.mp3` | 10368 | 16000 | 1 | 1.296 s | **no ID3 tag** |
| **065** | `065.mp3` | 11629 | 16000 | 1 | 2.556 s | has ID3; `TYER=2019 TDAT=0603 TIME=1057` |
| 052 | `052.mp3` | 133747 | 44100 | 2 | 3.344 s | no ID3, but music-rate → not TTS |
| 066 | `066.mp3` | 119745 | 22050 | 2 | 29.936 s | no ID3, 30 s → not TTS |

All four 16 kHz mono clips show 2 pause regions under `silencedetect -30dB/0.15s`
(0.49–1.27 s of silence total), the utterance/pause structure typical of TTS speech.
032/033/034 are the only tag-less 16 kHz mono clips; 065 is the same rate/channel layout
but carries an ID3 tag dated 2019-06-03 — i.e. it was written during the product's own
2018–2019 build window. **032, 033, 034 and 065 are the files to listen to first.**

## Files in this directory

| path | content |
| --- | --- |
| `NNN.mp3` / `NNN.xml` | raw payloads exactly as stored (69 mp3 + 5 xml) |
| `wav\NNN.wav` | 16-bit PCM (`pcm_s16le`), original sample rate and channel count |
| `tone-report.csv` | per-file: index, offset, size, container, codec, rate, channels, bitrate, duration, ID3 tags, MD5 |
| `extract-manifest.csv` | parser output: index, offset, length, token, gap, MD5, CRC32 |
| `parse-flashtone.py` … `final_verify.py` | the scripts used (reproducible) |
| `parse-log.txt`, `tail-log.txt`, `report-log.txt`, `wakephrase-log.txt`, `deepchecks-log.txt`, `verify.txt`, `table-hex.txt` | raw logs / evidence |

## Not determined

* Which clip actually speaks `请吩咐` — that needs a human listen; no textual mapping exists.
* The consumer of the `0x28 + (n << 8)` token (no symbol names in the stock firmware), so
  the token's exact role is inferred from the table alone.
* Whether the 5 Audacity XML files were referenced by the firmware at all, or were simply
  packed by mistake.
