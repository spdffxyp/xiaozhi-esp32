#!/usr/bin/env python3
"""Generate zl38063_config_record_full.h - the COMPLETE 1042-entry ZL38063
configuration record recovered from the stock B220 firmware image.

Why this file exists
--------------------
The stock image stores the record at wifi.bin file offset 0x059EA8 as

    u16  length            = 0x0412 = 1042      <- an ELEMENT count
    dataArr[1042]          = 1042 x {u16 reg; u16 value}, reg stepping +2
                             from 0x0200 to 0x0A22

and the stock host loader VprocTwolfLoadConfig() (VA 0x40125c30) walks it with

    40125c33: extui a3, a3, 0, 16     ; a3 = numElements (the length field)
    40125c36: addx4 a3, a3, a2        ; a3 = data + 4 * numElements   <- 1042!
    40125c56: bne   a2, a3, 40125c3c  ; one 4-byte step per iteration

so the device is programmed with 1042 register pairs, not 260.  Upstream agrees:
api_lib/vprocTwolf_access.c:457 loops `for (i = 0; i < numElements; i++)` over
dataArr[], and the esp-adf object zl38063_config.o carries configStreamLen = 1042
next to a 1042-element st_twConfig.

The earlier extraction (zl38063_config_record.h, COUNT 260) read the length field
as a BYTE count and therefore stopped after 260 pairs + 2 stray bytes.  Registers
0x0408..0x0A22 - 782 entries, 157 of them non-zero - were never written.

Usage
-----
    python gen_full_config_record.py            # regenerate the header
    python gen_full_config_record.py --check    # verify only, exit 1 on drift
"""

import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD_DIR = os.path.dirname(HERE)                      # .../boards/xiaomodou-b220
# .../boards/xiaomodou-b220 -> boards -> main -> xiaozhi-esp32-b220 -> repo root
REPO = os.path.abspath(os.path.join(BOARD_DIR, "..", "..", "..", ".."))
WIFI_BIN = os.path.join(REPO, "backup", "parts", "wifi.bin")
OUT = os.path.join(HERE, "zl38063_config_record_full.h")

# Stock image, verified: u16 length field at this file offset, data right after.
RECORD_OFFSET = 0x59EA8
RECORD_VA = 0x3F459EA8
STRIDE = 2        # register step
PAIR = 4          # bytes per {u16 reg; u16 value;}


def load_pairs():
    with open(WIFI_BIN, "rb") as fh:
        blob = fh.read()
    count = struct.unpack_from("<H", blob, RECORD_OFFSET)[0]
    data = blob[RECORD_OFFSET + 2: RECORD_OFFSET + 2 + count * PAIR]
    if len(data) != count * PAIR:
        raise SystemExit("record truncated in the image: %d < %d bytes"
                         % (len(data), count * PAIR))
    pairs = [struct.unpack_from("<HH", data, i * PAIR) for i in range(count)]
    regs = [p[0] for p in pairs]
    if any(regs[i + 1] - regs[i] != STRIDE for i in range(len(regs) - 1)):
        raise SystemExit("register numbers are not a +2 ramp - wrong offset?")
    return pairs


def render(pairs):
    n = len(pairs)
    nz = sum(1 for _, v in pairs if v)
    out = []
    w = out.append
    w("/*")
    w(" * zl38063_config_record_full.h - the COMPLETE ZL38063 configuration")
    w(" * record of the xiaomodou B220, extracted byte-for-byte from the stock")
    w(" * firmware image.")
    w(" *")
    w(" * GENERATED FILE - do not edit by hand.")
    w(" * Regenerate with:  python zl38063/gen_full_config_record.py")
    w(" *")
    w(" * Source")
    w(" *   image      : backup/parts/wifi.bin (plaintext ESP32 app image)")
    w(" *   file offset: 0x%05X   (the u16 length field)" % RECORD_OFFSET)
    w(" *   DROM VA    : 0x%08X" % RECORD_VA)
    w(" *   length field: 0x%04X = %d   <- an ELEMENT count, NOT a byte count"
      % (n, n))
    w(" *   payload    : %d x {u16 reg; u16 value} = %d bytes" % (n, n * PAIR))
    w(" *   registers  : 0x%04X .. 0x%04X, step +2, contiguous"
      % (pairs[0][0], pairs[-1][0]))
    w(" *   non-zero   : %d of %d values" % (nz, n))
    w(" *")
    w(" * The stock host loader writes ALL %d pairs.  Its loop, byte-exact" % n)
    w(" * (see zl38063/PROTOCOL-REPORT.md and zl38063_dsp.c for the context):")
    w(" *")
    w(" *   40125c33: extui a3, a3, 0, 16     ; a3 = numElements")
    w(" *   40125c36: addx4 a3, a3, a2        ; end = data + 4 * numElements")
    w(" *   40125c3c: l16ui a8, a2, 2         ; value = p[1]")
    w(" *   40125c45: l16ui a10, a2, 0        ; reg   = p[0]")
    w(" *   40125c48: addi  a2, a2, 4         ; p += 4")
    w(" *   40125c51: call8 hbi_write16       ; one HBI write per pair")
    w(" *   40125c56: bne   a2, a3, 40125c3c  ; %d iterations" % n)
    w(" *")
    w(" * addx4 is a3 = a2 + (a3 << 2), so numElements = the length field = %d" % n)
    w(" * iterations of a 4-byte step: the whole %d-pair record." % n)
    w(" *")
    w(" * Binary layout is identical to Microsemi's")
    w(" *   typedef struct { uint16_t reg; uint16_t value; } dataArr;")
    w(" *")
    w(" *   VprocTwolfLoadConfig((dataArr *)zl38063_config_record_full,")
    w(" *                        ZL38063_CONFIG_RECORD_FULL_COUNT);")
    w(" *")
    w(" * The tail entries beyond index %d (registers 0x%04X..0x%04X) are the"
      % (260, pairs[260][0], pairs[-1][0]))
    w(" * ones the 260-entry header zl38063_config_record.h never wrote.")
    w(" */")
    w("")
    w("#ifndef ZL38063_CONFIG_RECORD_FULL_H")
    w("#define ZL38063_CONFIG_RECORD_FULL_H")
    w("")
    w("#include <stdint.h>")
    w("")
    w("/* Number of {reg, value} entries in the complete record. */")
    w("#define ZL38063_CONFIG_RECORD_FULL_COUNT %d" % n)
    w("/* Register of the first and of the last entry. */")
    w("#define ZL38063_CONFIG_RECORD_FULL_FIRST_REG 0x%04X" % pairs[0][0])
    w("#define ZL38063_CONFIG_RECORD_FULL_LAST_REG  0x%04X" % pairs[-1][0])
    w("/* Index of the first entry the 260-element record omitted. */")
    w("#define ZL38063_CONFIG_RECORD_TAIL_FIRST_INDEX 260")
    w("#define ZL38063_CONFIG_RECORD_TAIL_FIRST_REG   0x%04X" % pairs[260][0])
    w("")
    w("/* The record, little-endian {reg, value} pairs, exactly as stored.")
    w(" * `static const` because this header is included by exactly one")
    w(" * translation unit (zl38063_dsp.c); that keeps the array in .rodata")
    w(" * without a linker-visible symbol. */")
    w("static const uint16_t zl38063_config_record_full[%d][2] = {" % n)
    for i, (reg, val) in enumerate(pairs):
        w("    {0x%04X, 0x%04X},%s" % (reg, val,
                                      ("  /* 0x%04X */" % reg) if i % 8 == 0 else ""))
    w("};")
    w("")
    w("#endif /* ZL38063_CONFIG_RECORD_FULL_H */")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify the header on disk matches the image")
    args = ap.parse_args()

    text = render(load_pairs())
    if args.check:
        try:
            with open(OUT, "rb") as fh:
                on_disk = fh.read().decode("latin-1")
        except OSError as exc:
            raise SystemExit("cannot read %s: %s" % (OUT, exc))
        if on_disk != text:
            raise SystemExit("DRIFT: %s does not match wifi.bin" % OUT)
        print("OK: %s matches wifi.bin" % OUT)
        return 0

    with open(OUT, "wb") as fh:
        fh.write(text.encode("latin-1"))
    print("wrote %s (%d bytes)" % (OUT, len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
