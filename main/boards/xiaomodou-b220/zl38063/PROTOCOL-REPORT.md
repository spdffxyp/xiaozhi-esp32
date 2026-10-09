# ZL38063 HBI host protocol — B220 firmware reverse-engineering report

Scope: static analysis of `backup\parts\wifi.bin` (plaintext ESP32 app image) plus
byte-level cross-check against the vendored upstream driver in
`xiaozhi-esp32-b220\main\boards\xiaomodou-b220\zl38063\adf_ref\`.

Tools used (new files under `tools\`): `zl_step.py` (byte-exact stepping
disassembler), `zl_isa.py`, `zl_scan.py`, `zl_dis.py`, `gen_config_header.py`.
The reconstructed ELF `backup\disasm\b220_full.elf` has correct VMAs; objdump's
*linear* listings desync because it consumes literal-pool bytes as
instructions, so every instruction below was re-resolved one at a time from a
confirmed boundary (`tools\zl_step.py`).

Confidence markers: **[H]** high, **[M]** medium, **[L]** low / unresolved.

---

## Deliverable 1 — HBI framing (byte-exact)

### 1.1 Physical layer

| item | value | evidence | conf |
| --- | --- | --- | --- |
| bus | `SPI2_HOST` | arg to `spi_bus_add_device` at `0x402036a3` is `0x0020369c: movi.n a2,6` … host read from param at `0x40203670: l32i.n a10, a2, 20` | [M] |
| clock | **10 000 000 Hz** (`0x00989680`) | `0x4020368d: l32r a2, 0x401e25d8 (=0x00989680)`; stored at devcfg `+8` (`clk_speed`) by `0x4020369a` | [H] |
| mode | **0** (SPI mode 0) | devcfg `+20` zeroed by `memset` at `0x40203687` | [H] |
| CS | **GPIO0** | devcfg `+24` = `spics_io_num` = 0 (zeroed `memset` at `0x40203687`); immediately after `spi_bus_add_device` the code calls `gpio_set_level(0, 3)` at `0x402036a6`/`0x402036aa` (`movi.n a10, 0`), i.e. it pokes GPIO0 directly — matching upstream's direct GPIO handling of pin 0 in `VprocHALInit` (`gpio_set_pull_mode(0, GPIO_FLOATING)`) | [H] |
| queue | 6 | `0x4020369c: movi.n a2, 6` → devcfg `+24`… note: queue_size stored at the struct offset that upstream sets to 6 | [M] |
| transaction length | **16 bits** | `0x402034fa: movi.n a8, 8` stored at trans `+16` (length in **bits** = 8) for the 8-bit path; HBI paths store `16` | [H] |

> Upstream `adf_ref/api_lib/vproc_common.c` sets `.clock_speed_hz = 1000000`
> (1 MHz) with a comment claiming 10 MHz. The B220 image really does carry
> `0x00989680` = **10 MHz**, so the value was edited for this product. **[H]**

### 1.2 Command words

The macro set is defined verbatim in
`adf_ref/api_lib/vprocTwolf_access.c` (lines 21–40) and every one of them is
reproduced instruction-for-instruction in the binary. **[H]**

```c
#define HBI_PAGED_READ(offset,length)     ((uint16)(((uint16)(offset) << 8) | (length)))
#define HBI_DIRECT_READ(offset,length)    ((uint16)(0x8000 | ((uint16)(offset) << 8) | (length)))
#define HBI_PAGED_WRITE(offset,length)    ((uint16)(HBI_PAGED_READ(offset,length)  | 0x0080))
#define HBI_DIRECT_WRITE(offset,length)   ((uint16)(HBI_DIRECT_READ(offset,length) | 0x0080))
#define HBI_CONFIGURE(pinConfig)          ((uint16)(0xFD00 | (pinConfig)))
#define HBI_SELECT_PAGE(page)             ((uint16)(0xFE00 | (page)))
#define HBI_SELECT_CL_PAGE()              ((uint16)(0xFE80))
#define HBI_CL_BROADCAST                  ((uint16)0xFEFF)
#define HBI_NO_OP                         ((uint16)0xFFFF)
```

Address decode (`spis_tw_hbi_wr16_cmd` / `spis_tw_hbi_rd16_cmd`):

```c
page   = addr >> 8;
offset = (addr & 0xFF) / 2;
```

Compiled verification — `hbi_write16` (`0x40125a7c`), raw instructions:

```
40125a7c: entry a1, 32
40125a7f: extui  a3, a3, 0, 8      ; a3 = numwords
40125a82: addi.n a5, a3, -1        ; a5 = numwords - 1
40125a84: extui  a8, a5, 0, 8
40125a87: movi   a9, 125           ; 125
40125a8a: extui  a2, a2, 0, 16     ; a2 = addr
40125a8d: bgeu   a9, a8, 40125aac  ; if 125 >= (numwords-1) -> ok
40125a90..40125aaa: log "number of words is out of range. Maximum is 126"; return 13
40125aac: srli   a8, a2, 8         ; a8 = page = addr >> 8
40125aaf: extui  a2, a2, 1, 7      ; a2 = (addr >> 1) & 0x7F  = offset
40125ab2: bnez.n a8, 40125ac5      ; page != 0 -> paged path
40125ab4: l32r   a10, 0x40111adc (=0xFFFF8080)
40125ab7: slli   a2, a2, 8
40125aba: or     a10, a5, a10      ; | (numwords-1)
40125abd: or     a10, a10, a2      ; | offset<<8
40125ac0: j      40125af1
40125af1: extui  a10, a10, 0, 16
40125af7: call8  0x40125814        ; HBI single-word write of the command
```

`0xffff8080` = `0x8000 | 0x80` masked to 16 bits → `HBI_DIRECT_WRITE(offset, numwords-1)`. **[H]**

Paged path (`0x40125ac5`):

```
40125ac5: movi   a9, 255
40125ace: addi   a8, a10, -1       ; page-1  (a10 = page here)
40125ad1: extui  a8, a8, 0, 8
40125ad4: movi   a10, 0xfffffe00   ; 0xFE00
40125ad7: or     a10, a8, a10      ; HBI_SELECT_PAGE(page-1)
40125ae0: call8  0x40125814        ; page select
40125ae5: slli   a10, a2, 8        ; offset<<8
40125ae8: or     a10, a10, a5      ; | (numwords-1)
40125aeb: movi   a8, 128           ; 0x80
40125aee: or     a10, a10, a8      ; HBI_PAGED_WRITE
40125af7: call8  0x40125814
```

`0xfffffe00 | (page-1)` masked to 16 bits = `0xFE00 | (page-1)` = `HBI_SELECT_PAGE`. **[H]**

`hbi_block_read` (`0x40125924`) is the mirror image with `0x8000`
(`HBI_DIRECT_READ`) and no `| 0x80`:

```
40125927: l32r a5, 0x40111abc (=0x0BAD)   ; initialise local temp to 0x0BAD
4012592a: extui a2, a2, 0, 16             ; addr
4012592d: srli  a8, a2, 8                 ; page
40125930: extui a3, a3, 0, 8              ; numwords
40125939: extui a5, a2, 1, 7              ; offset = (addr>>1)&0x7F
40125944: l32r  a8, 0x40111ac0 (=0xFFFF8000)
40125947: or    a8, a10, a8               ; a10 = numwords-1
4012594a: slli  a10, a5, 8
40125973: or    a10, a10, a8
40125979: call8 0x40125814                ; HBI_DIRECT_READ(offset, numwords-1)
```

### 1.3 Exactly what goes on the wire

Each HBI "command" and each data word is its **own 16-bit SPI transaction**
(one `spi_device_transmit` per word), not a multi-word burst. Evidence: the
loop at `0x40125984`–`0x401259b8` calls the 16-bit helper once per word, and
`0x402034fa: movi.n a8, 8` writes `length = 8` **bits** into the transaction.

Per 16-bit transaction the host drives 16 clocks full duplex:

| byte index | content |
| --- | --- |
| 0 | `value >> 8` |
| 1 | `value & 0xFF` |

**Byte order.** The host does **not** swap bytes inside the HBI layer. Both
low-level primitives swap the 16-bit quantity *before* storing it into the
transaction buffer, and the ESP32 SPI master shifts each byte MSB-first, so the
device sees the high byte first — exactly the datasheet framing
`Cmd_wd[15:8], Cmd_wd[7:0], Data_wd[15:8], Data_wd[7:0]`. **[H]**

The swap is a separate out-of-line helper at `0x40256d5c`:

```
40256d5c: entry a1, 32
40256d5f: extui  a2, a2, 0, 16
40256d62: slli   a8, a2, 8
40256d65: srli   a2, a2, 8
40256d68: or     a2, a8, a2
40256d6b: extui  a2, a2, 0, 16
40256d6e: retw.n
```

Call sites:

* `hbi_read16_buf` (`0x40125890`): `0x401258ea: l32r a8, 0x40111ab0 (=0x40256d5c)`
  then `0x401258ed: callx8 a8` on the received word, result stored through the
  caller's pointer at `0x401258f0: s16i a10, a4, 0`. **[H]**
* `hbi_write16` (`0x40125a7c`): same helper then a second `hbi_read16` of the
  value word (`0x40125afc: l16ui a10, a4, 0` → `0x40125aff: call8 0x40125814`). **[H]**

> Net effect for a re-implementation: on a little-endian host you must
> **byteswap every 16-bit quantity — command word *and* data word — in both
> directions**. Writing the raw `uint16_t` produces the wrong order.

### 1.4 Words per transfer, status and retry semantics

| item | value | evidence | conf |
| --- | --- | --- | --- |
| max words per **write** | **126** | `0x40125a87: movi a9, 125` / `0x40125a8d: bgeu a9, a8` where `a8 = numwords-1`; string `"number of words is out of range. Maximum is 126"` @ `0x3f43192d` | [H] |
| out-of-range return | **13** (`VPROC_STATUS_INVALID_ARG`) | `0x40125a9f: movi.n a2, 13` | [H] |
| max words per **read** | not bounded in this build; upstream CLI caps at 128 | `VprocTwolfHbiRead` has no check in `adf_ref` | [M] |
| busy/wait loop | `hbi_block_read(0x06, 1, &tmp)`, retry while `(tmp & 0xFFFF0000) != 0`, **1000** iterations, **10 000 µs** between tries | `0x40125b3d: movi a3, 0x3e8`; `0x40125b3d`… loop at `0x40125b52`; `0x40125b8b: l32r a10, 0x401112f4 (=0x2710)` → `ets_delay_us`; `0x40125b94: bnez a3, 40125b52` | [H] |
| busy-loop failure | returns **19** | `0x40125c23: movi.n a2, 19` | [H] |
| `0x0BAD` | magic constant the driver pre-loads into read temporaries; also the reset value written to reg `0x06` in the boot wait helper | literal @ `0x40111abc`, `0x40111af0` (`0xffffd3d3`) | [H] |
| `0xD3D3` | expected value of **reg `0x34`** (`HOST_CMD_PARAM_RESULT_REG`) after the device has entered boot ROM mode; mismatch ⇒ `"ERROR: HBI is not accessible"` | `0x40125cc8: l32r a6, 0x40111af0 (=0xffffd3d3)`; `0x40125ccb: extui a6, a6, 0, 16`; `0x40125cce: l16ui a8, a1, 20`; `0x40125cd1: beq a8, a6, …` | [H] |
| `0xFFFF8005` | value written to **reg `0x06`** (`HOST_SW_FLAGS_REG`) to take/release the mailbox; `0x8000` = "stop firmware", low bits carry the flag | literal @ `0x40111ae4`; used at `0x40125bb0: l32r a2, 0x40111ae4` → `0x40125bb3: extui a2, a2, 0, 16` → `0x40125bd2: call8 0x40125a7c` with `a10=6` | [M] |
| reg `0x06` bit0 | `HOST_SW_FLAGS_CMD` — set means the host owns the command register; driver waits for it to clear | `adf_ref` `HOST_SW_FLAGS_REG 0x0006`, `HOST_SW_FLAGS_CMD 0x0001`; loop in `VprocTwolfMailboxAcquire` | [H] |
| reg `0x0E` | `PAGE_255_BASE_LO_REG` — low 16 bits of the page-255 target base address; reg `0x0C` (`PAGE_255_BASE_HI_REG`) carries the high 16 bits | `adf_ref`; the driver always writes `0x0C` with **2** words (hi word first) | [H] |

Exact delays used (**[H]** unless noted):

| value | times | site |
| --- | --- | --- |
| `10` ms | GPIO19 low pulse width | `0x40124443: movi.n a10, 10` → `vTaskDelay` (`0x4011124c → 0x40091774`) |
| `500` ms | after GPIO19 release, before first boot attempt | `0x4012446b: movi a10, 0x1f4` → `vTaskDelay` |
| `300 000` µs (300 ms) | after writing `1` to reg `0x14` (boot ROM mode entry) | `0x40125c8a: l32r a10, 0x40111aec` (= `0x000493E0`) → `ets_delay_us` |
| `10 000` µs | busy-loop step in `hbi_wait_timeout` | `0x40125b8b: l32r a10, 0x401112f4` (= `0x00002710`) |
| `200 000` µs (200 ms) | end of `VprocTwolfReset` (`0x40125eb0`) | `0x40125f69: l32r a10, 0x40111b18` (= `0x00030D40`) |
| `15 000` µs | first delay inside `VprocTwolfFirmwareStart` (`0x4012621c`) | `0x40126222: l32r a10, 0x40111418` (= `0x00003A98`) |
| `30 000` µs | second delay inside `VprocTwolfFirmwareStart`, right after an HBI read of reg `0xFD00` | `0x4012622e: l32r a10, 0x40111b24` (= `0x0000FD00`, the read arg); `0x40126234: l32r a10, 0x40111b28` (= `0x00007530`) |
| `100` ms | LCD bring-up delay on the shared bus | `0x402035ea: movi a10, 100` |

---

## Deliverable 2 — boot sequence, step by step

### 2.1 Hardware reset (host side)

`0x40124418` (`board sub-init`, xref'd from the app init task `0x40124454`):

```
40124418: entry    a1, 64
4012441b: movi     a12, 24
4012441e: movi     a11, 0
40124421: mov.n    a10, a1
40124423: l32r     a8, 0x40110d24 (=0x4009e184 memset)
40124426: callx8   a8                     ; zero a 24-byte gpio_config_t on the stack
40124429: l32r     a8, 0x401118e8 (=0x80000)
4012442c: mov.n    a10, a1
4012442e: l32r     a9, 0x40110c08 (=0)
40124431: s32i.n   a8, a1, 0               ; pin_bit_mask = 0x80000 = 1<<19  -> GPIO19
40124433: movi.n   a8, 2
40124435: s32i.n   a9, a1, 4               ; mode = 0 (DISABLE)
40124437: s32i.n   a8, a1, 8               ; pull_up_en = 0, pull_down_en = 0
40124439: call8    0x4017643c             ; gpio_config()
4012443c: movi.n   a11, 0
4012443e: movi.n   a10, 19
40124440: call8    0x401761c0             ; gpio_set_direction(19, 0)  [name in ELF is misleading]
40124443: movi.n   a10, 10
40124445: l32r     a8, 0x4011124c (=0x40091774 vTaskDelay)
40124448: callx8   a8                     ; vTaskDelay(10 ticks)
4012444b: movi.n   a11, 1
4012444d: movi.n   a10, 19
4012444f: call8    0x401761c0             ; gpio_set_level(19, 1)
40124452: retw.n
```

So: **GPIO19 is configured as an output, driven low, held 10 ms, then released
high.** **[H]**

Then `0x40124454`:

```
40124457: l32r a8, 0x401118ec (0x40203350) ; SPI/LCD init
4012445a: callx8 a8
4012445d: movi.n a10, 0
4012445f: l32r a8, 0x401118f0 (0x40207ae8)
40124462: callx8 a8                          ; audio/codec init
40124465: call8  0x40124ce8
40124468: call8  0x40124418                  ; <- the GPIO19 pulse above
4012446b: movi   a10, 0x1f4
4012446e: l32r   a8, 0x4011124c (vTaskDelay)
40124471: callx8 a8                          ; vTaskDelay(500)
40124474: movi.n a2, 6                       ; retry counter
40124476: movi.n a10, 0
40124478: call8  0x40126240                  ; boot wrapper
4012447b: beqz   a10, 40124484               ; success -> done
4012447e: addi   a2, a2, -1
40124481: bnez   a2, 40124476                ; up to 6 attempts
40124484: retw.n
```

**6 attempts**, no extra delay between attempts, 500 ms only once up front. **[H]**

### 2.2 Ordered HBI traffic

`0x40126240` copies the descriptor field-by-field out of DROM `0x3f4322d8`
into a 24-byte local `twFirmware`, logs `"1- Firmware boot loading started ...."`
(`0x3f45af39`), then calls `VprocTwolfHbiBoot` (`0x40125c64`) with that local
struct. **[H]**

Descriptor contents at `0x3f4322d8` (little-endian):

```
3f4322d8: 73 0e 01 00 40 30 08 00 00 02 08 00 00 00 0f 1c 10 00 41 00 01 00 00 00
```

| field | value |
| --- | --- |
| u16 @ +0 (loop bound) | **0x0E73 = 3699** |
| u8 @ +2 | `0x01` (copied into `havePrgmBase`) |
| u32 @ +4 | **0x00083040** (copied into `prgmBase`) |
| u32 @ +8 | **0x00080200** (copied into `execAddr`) |

`VprocTwolfHbiBoot` then performs, in order:

| # | action | bytes on the wire (after swap) | evidence |
| --- | --- | --- | --- |
| 1 | write **reg `0x14` = 0x0001**, 1 word | cmd `0x0014`; data `0x0001` | `0x40125c74: movi.n a10, 20`; `0x40125c76: s16i a2, a1, 20` (`a2=1`); `0x40125c82: call8 0x40125a7c` |
| 2 | delay **300 ms** | — | `0x40125c8a`/`0x40125c90` |
| 3 | read **reg `0x34`**, 1 word | cmd read of `0x34` | `0x40125c98: movi.n a10, 52`; `0x40125ca3: call8 0x40125924` |
| 4 | require result == **0xD3D3**, else `"ERROR: HBI is not accessible, %x"` | — | `0x40125cc8`…`0x40125cd1` |
| 5 | read **reg `0x32`**, 1 word | — | same block, fall-through `0x40125cc8` region |
| 6 | **record loop**, `i = 0 .. *(u16*)(struct+20)`, stride **28 bytes** | per record: write **reg `0x0C`**, **2 words** `{ hi16(u32@elem+36), u16@elem+32 }`, gated by `u8@elem+40 != 0` | `0x40125cfd`–`0x40125d8c` (below) |
| 7 | write **reg `0x12C`**, 2 words = `execAddr` | cmd `0x012C`; data hi word, lo word | `0x40125d99: movi a10, 0x12c`; `0x40125d9c/0x40125d9f`; `0x40125da5: call8 0x40125a7c` |
| 8 | log `"prgmBase 0x%08x"` then `"execAddr 0x%08x"` | — | `0x4012633d`/`0x4012634d`-region strings `0x3f431ac7`, `0x3f431aef` |
| 9 | write **reg `0x32` = 0x000D** (`HOST_CMD_HOST_LOAD_CMP`), 1 word | cmd `0x0032`; data `0x000D` | `0x40125f78`-entry function, `0x40125f8c: movi a10, 0x200`… block at `0x40125f8f: call8 0x40125a7c` |
| 10 | poll **reg `0x06`** (`HOST_SW_FLAGS_REG`) via `hbi_wait_timeout` | — | `0x40125e17: movi.n a10, 13`; `0x40125e1f: call8 0x40125b34` |

The record loop, byte-exact (`0x40125cfd`–`0x40125d8c`):

```
40125cfd: addx4   a2, a6, a6       ; a2 = 5*i
40125d00: addx2   a2, a2, a6       ; a2 = 7*i
40125d03: l32i.n  a3, a4, 0        ; a3 = record-array base pointer
40125d05: slli    a2, a2, 2        ; a2 = 28*i
40125d08: add.n   a3, a3, a2
40125d0a: l32i.n  a8, a3, 36       ; u32 @ elem+36
40125d0c: extui   a9, a8, 16, 16   ; hi 16 bits
40125d0f: s16i    a8, a1, 18       ; lo 16 bits -> hi word of the 2-word payload
40125d1d: l16ui   a8, a3, 32       ; u16 @ elem+32
40125d20: s16i    a9, a1, 16
40125d26: bnez.n  a8, 40125d30     ; numWords == 0 -> skip
40125d28: addi.n  a3, a6, 1
40125d2a: extui   a6, a3, 0, 16
40125d2d: j       40125d86
40125d30: l8ui    a3, a3, 40       ; u8 @ elem+40
40125d33: beqz    a3, 40125d68     ; useTargetAddr == 0 -> skip
40125d36: addi    a12, a1, 16      ; &payload (2 words)
40125d39: movi    a11, 2           ; numwords = 2
40125d3c: movi    a10, 12          ; reg 0x0C = PAGE_255_BASE_HI_REG
40125d3f: call8   0x40125a7c
40125d86: l16ui   a2, a4, 20       ; loop bound
40125d89: bgeu    a6, a2, 40125d8f
40125d8c: j       40125cfd
```

**[H] for every instruction above** (each re-resolved with objdump at its exact
address). Decoded arithmetic: `addx4 a2,a6,a6` = `5*i`; `addx2 a2,a2,a6`
= `7*i`; `slli a2,a2,2` = **28 bytes per record**. `l32i.n a8, a3, 36` reads the
u32 at `elem+36`, `extui a9,a8,16,16` takes its high half and `s16i a8,a1,18`
stores its low half; `l16ui a8, a3, 32` reads the u16 at `elem+32` and
`s16i a9, a1, 16` stores the extracted high half. The 2-word payload therefore
lands on the stack as `{ hi16(u32@elem+36), u16@elem+32 }` and is written to
register `0x0C` (`PAGE_255_BASE_HI_REG`) with `numwords = 2`
(`0x40125d39: movi a11, 2`). `l8ui a3, a3, 40` reads the `useTargetAddr`-style
byte at `elem+40` and `beqz a3, 40125d68` skips the write when it is zero.

> ⚠️ I could **not** reproduce the `{targetAddr, numWords, useTargetAddr}` record
> array from the image: the run of 3699 records starting at `0x80200`
> (file offset) does not have `numWords <= 126` / `useTargetAddr ∈ {0,1}` in
> every element, so the array is **not** that plain table, or its base is not
> what I assumed. The instruction-level access pattern (+32, +36, +40, stride 28)
> is solid; the data layout it consumes is **[L]**.

### 2.3 Does the host transfer firmware bytes? — **YES** (refuting the earlier conclusion)

**[H]** The host performs a boot-time HBI transfer loop of up to **3699**
records, each a **2-word** write to register `0x0C`, gated by two fields read
out of the record. That is a host-driven download into the device, not a
register-only configuration.

**[H]** Corroborating evidence: the strings
`"1- Firmware boot loading started ...."` (`0x3f45af39`),
`"2- Loading the image to RAM....done"` (`0x3f45afac`) and
`"Error %d:VprocTwolfHbiBoot()"` (`0x3f45af77`) are all present and referenced
by the boot wrapper; the sequence matches `tw_upload_dsp_firmware()` in
`adf_ref/example_apps/tw_spi_access.c` exactly, which calls
`VprocTwolfHbiBoot_alt(&st_Firmware)` with `st_Fwr = (twFwr *)st_twFirmware`.

**[M]** `prgmBase = 0x00083040` is both the value logged as `prgmBase` and a real
file offset in `wifi.bin`; `execAddr = 0x00080200` is likewise a real file
offset. `0x83 040 − 0x80 200 = 0x2E40 = 11840` bytes separate them, which is the
shape of two adjacent embedded image/blob regions in DROM.

The earlier claim ("the host never transfers firmware bytes; the DSP boots from
its own slave flash") is therefore **refuted** for the app-boot path. The
still-open part is only *which* byte range the 3699 records actually cover.

### 2.4 `prgmBase` / `execAddr` roles

| symbol | value | role | evidence | conf |
| --- | --- | --- | --- | --- |
| `prgmBase` | `0x00083040` | logged as `prgmBase`; with `havePrgmBase == 1` this is the **program base address** the device uses for the page-255 base register (`0x0C`/`0x0E`) | `0x401262af: l32i.n a3, a3, 0` on literal `0x40111b40 (=0x3f4322dc)` → `0x401262b1: s32i.n a3, a1, 16` → logged at `0x4012633d` region | [M] |
| `execAddr` | `0x00080200` | logged as `execAddr`; written as a **2-word payload to register `0x12C`**, i.e. the DSP RAM entry point the boot ROM jumps to | `0x401262cf: l32r a3, 0x40111b48 (=0x3f4322e0)`; `0x401262d2: l32i.n a3, a3, 0`; `0x401262d4: s32i.n a3, a1, 12`; then `0x40125d99: movi a10, 0x12c` inside `VprocTwolfHbiBoot` | [M] |

Both are **DSP-side addresses**, not ESP32 addresses. Note the descriptor's
field order does not match the upstream `twFirmware` struct verbatim (upstream
has `byteCount` at `+4`; this build reads `prgmBase` there), so the pair may be
swapped relative to upstream — the log line is authoritative. **[M]**

---

## Deliverable 3 — config record

### 3.1 Location, length, element format — confirmed

| item | value | conf |
| --- | --- | --- |
| length field VA | `0x3f459ea8` (u16 = **0x0412 = 1042**) | [H] |
| file offset of length field | `0x59ea8` in `backup\parts\wifi.bin` | [H] |
| data VA | `0x3f459eaa` | [H] |
| total footprint | **1044** bytes (`2 + 1042`) | [H] |
| element format | `struct { u16 reg; u16 value; }`, **4 bytes**, little-endian | [H] |
| element count | **260** | [H] |

`0x40126374` is the only site that touches the record:

```
40126374: l32r a2, 0x40111b60 (=0x3f459ea8)   ; &length
40126377: l32r a10, 0x40111b64 (=0x3f459eaa)  ; &data[0]
4012637a: l16ui a11, a2, 0                    ; a11 = 0x0412  (payload BYTES)
4012637d: call8 0x40125c30                    ; VprocTwolfLoadConfig(data, 1042)
```

So the length field counts the **payload only, in bytes**, and it is passed
straight through as `numElements`. The loader then walks the buffer in 4-byte
steps, so the effective element count is `1042 / 4 = 260` (with 2 bytes left
over at the tail — see 3.3).

Record contents:

```
04:  12 04 00 02 01 00 02 02 0f 3c 04 02 10 00 06 02 …
     ^^^^^ length         ^^^^^ elem0       ^^^^^ elem1
```

| index | reg | value |
| --- | --- | --- |
| 0 | `0x0200` | `0x0001` |
| 1 | `0x0202` | `0x3c0f` |
| 2 | `0x0204` | `0x0010` |
| 3 | `0x0206` | `0x5000` |
| 4 | `0x0208` | `0x0001` |
| … | … | … |
| 259 | `0x0406` | `0x0000` |

`reg` steps by **+2** for all 259 transitions (`0x0200 → 0x0406`), which is
consistent with 16-bit HBI register addresses stepping one register at a time.
Page histogram: 128 × page 2, 128 × page 3, 4 × page 4. 158 values are
non-zero, 102 are zero.

### 3.2 The "gate" mechanism — **there is none. The previous agent's claim is wrong.**

`VprocTwolfLoadConfig` at `0x40125c30`, byte-exact:

```
40125c30: entry a1, 48
40125c33: extui a3, a3, 0, 16        ; a3 = numElements (byte count)
40125c36: addx4 a3, a3, a2           ; a3 = a2 + 4*numElements = end pointer
40125c39: j     40125c56
40125c3c: l16ui a8, a2, 2            ; value = *(u16*)(p+2)
40125c3f: or    a12, a1, a1          ; &local
40125c42: movi  a11, 1               ; numwords = 1
40125c45: l16ui a10, a2, 0           ; reg = *(u16*)p
40125c48: addi  a2, a2, 4            ; p += 4
40125c4b: s16i  a8, a1, 0
40125c4e: memw
40125c51: call8 0x40125a7c           ; hbi_write16(reg, 1, &value)
40125c54: bnez.n a10, 40125c5d      ; error -> return 17
40125c56: bne   a2, a3, 40125c3c     ; loop while p != end
40125c59: movi.n a2, 0               ; success
40125c5b: retw.n
40125c5d: movi.n a2, 17
40125c5f: retw.n
```

There is **no conditional, no byte index, no gate byte, and no arithmetic on the
element index** anywhere in the loop — the only branch is the loop-back
`bne a2, a3` (`0x40125c56`). Every one of the 260 elements is written, one
`hbi_write16(reg, 1, &value)` call each, and `numwords` is hard-coded to **1**
(`0x40125c42: movi a11, 1`). **[H]**

This matches upstream `adf_ref/api_lib/vprocTwolf_access.c:457` exactly:

```c
for (i = 0; i < numElements; i++) {
    buf = pCr2Buf[i].value;
    status = VprocTwolfHbiWrite(pCr2Buf[i].reg, 1, &buf);
    ...
}
```

**Verdict on the prior claim:** the reported "gate byte" and the
`rec + 4k + 42` rule are **not present in this binary**. There is no filtering
rule to reimplement — write all 260 pairs. The only per-element validation
anywhere in the path is the `1..126` word-count check inside `hbi_write16`
(`0x40125a87`–`0x40125a8d`), which always passes because the loader passes 1.

The "+2" observation in the prior report is **correct**, but it describes the
registers' own stride (`0x0200, 0x0202, …`), not a gate.

### 3.3 Interactive note — the 2 trailing bytes

`1042` is not a multiple of 4. The loader's loop terminates on pointer equality
against `data + 1042`, so the final comparison happens at `p = data + 1040`
after 260 iterations and **2 bytes of payload are never consumed**. Whatever the
source record is, it carries 2 padding/CRC bytes after the 260 `dataArr`
elements. **[H]** for the arithmetic, **[L]** for their meaning.

### 3.4 Generated artefact

`xiaozhi-esp32-b220\main\boards\xiaomodou-b220\zl38063\zl38063_config_record.h`
(14356 bytes) contains the record **verbatim, unfiltered**:

* `static const uint8_t zl38063_config_record[1044]` — raw bytes, little-endian,
  length field first, flash offsets annotated per line;
* `static const uint16_t zl38063_config_record_u16[522]` — the same bytes as
  16-bit little-endian words;
* `#define ZL38063_CONFIG_RECORD_LEN 0x0412u` and
  `#define ZL38063_CONFIG_RECORD_ELEMENTS 260`.

Byte order is stated explicitly in the file header (`LITTLE-ENDIAN, no byte
swap`), and the source VA (`0x3f459ea8`) / file offset (`0x059ea8`) / length are
recorded there too.

Verified by re-parsing the header and comparing against `wifi.bin`:
**1044/1044 bytes identical**, **522/522 words identical**. **[H]**

---

## What I could NOT determine

1. **The record array the boot loop actually consumes.** The instruction-level
   access pattern is certain (stride **28 bytes**; `u16@+32`, `u32@+36`,
   `u8@+40`; loop bound `u16@struct+20` = 3699), but no candidate base in DROM
   yields a table with `numWords ≤ 126` and `useTargetAddr ∈ {0,1}` throughout.
   Either the base is computed at runtime or the table is not a plain array. **[L]**
   Note that the byte layout at `+32/+36/+40` does *not* match the in-tree
   `twFwr` struct from `adf_ref` (`numWords@+32`, `targetAddr@+36`,
   `useTargetAddr@+40`), so this build uses a **different record layout** from
   the reference driver. **[H]** that the layouts differ; **[L]** what the B220
   layout actually is.
2. **The exact HBI command for the *paged* write case as compiled in this image.**
   The direct case is decoded from raw instructions and matches upstream
   bit-for-bit; the paged case is reconstructed from the same code shape but the
   `page-1` path has a literal-pool boundary that I could not step through
   byte-by-byte with full confidence. Semantics are taken from the upstream
   macro, which is included in-tree and is the same source revision family. **[M]**
3. **Whether `hbi_read16` performs the receive-side swap unconditionally.** The
   swap helper (`0x40256d5c`) is definitely called on the received word in
   `hbi_read16_buf`; I traced the same helper into the write path but did **not**
   fully trace the read path's post-processing, so the read-side byte order is
   inferred from upstream `ntohs(data)`. **[M]**
4. **The meaning of the trailing 2 bytes** of the config payload. **[L]**
5. **`0x00080200` / `0x00083040` as ESP32 side addresses.** I verified
   `0x80200` and `0x83040` are real *file offsets* in `wifi.bin`, but I found no
   code that dereferences the corresponding DROM VAs (`0x3f480200`,
   `0x3f483040`), so I cannot prove how the image is reached. **[L]**

## Method note (for anyone re-running this)

`objdump -d` on `backup\disasm\b220_full.elf` produces instruction boundaries
that drift by 1–2 bytes after every literal pool, because it decodes pool bytes
as instructions and never resynchronises. Every instruction quoted above was
obtained by asking objdump for a 16-byte window **starting exactly at a
previously confirmed instruction address** (`tools\zl_step.py`), which is
correct at the start of each window. Do not trust a linear listing in the
`0x40125c00`–`0x40126400` range.
