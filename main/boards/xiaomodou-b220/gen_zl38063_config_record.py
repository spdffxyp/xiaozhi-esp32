#!/usr/bin/env python3
"""Generate zl38063_config_record.h from the stock B220 flash dump.

Run from anywhere:

    python gen_zl38063_config_record.py

Input  : D:\\workspaces\\EspIdf\\xiaomodou\\backup\\parts\\wifi.bin
         (the plaintext 4 MB ESP32 application image pulled from the toy)

The ZL38063 "configuration record" (Microsemi calls the file *.cr2) that the
stock firmware hands to VprocTwolfLoadConfig() is stored verbatim in the
image's DROM segment:

    file offset 0x59eaa   == DROM address 0x3f459eaa
    length      1042 bytes == 260 x {uint16_t reg; uint16_t value;}
    registers   0x0200, 0x0202, ... 0x0406  (little-endian, step +2)

Note the two bytes at file offset 0x59ea8..0x59eaa are 12 04, i.e. a u16
"0x0412" = 1042 that the stock loader uses as the *payload byte count*; it is
NOT part of the record itself (sizeof(dataArr) * 260 == 1042).

Emitted as

    static const uint16_t zl38063_config_record[260][2]

which is bit-identical in layout to Microsemi's

    typedef struct { uint16_t reg; uint16_t value; } dataArr;

so it can be cast to dataArr * and passed to VprocTwolfLoadConfig().

A second, byte-verified copy of this record already exists at
`zl38063/zl38063_config_record.h` (an alternative byte-array form produced by
another agent).  The board CMake globs only the top-level *.c/*.cc of the board
directory, so that sibling header is never compiled and there is no duplicate
definition.  This script writes to a *different* path (this directory) and
refuses to overwrite anything unless --force is given, so the verified sibling
file can never be clobbered.
"""
import os
import struct
import sys

RAW = r"<workspace>\backup\parts\wifi.bin"
OFFSET = 0x59EAA
COUNT = 260
FIRST_REG = 0x0200
REG_STEP = 2

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "zl38063_config_record.h")
SIBLING = os.path.join(HERE, "zl38063", "zl38063_config_record.h")


def main():
    force = "--force" in sys.argv[1:]

    if os.path.exists(OUT) and not force:
        print("REFUSING to overwrite the existing %s" % OUT)
        print("  (pass --force only if you are certain that file is stale)")
        return 0
    if os.path.exists(SIBLING):
        print("NOTE: verified sibling header present at %s (left untouched)" % SIBLING)

    if not os.path.isfile(RAW):
        sys.exit("input image not found: %s" % RAW)
    with open(RAW, "rb") as fh:
        fh.seek(OFFSET)
        blob = fh.read(COUNT * 4)
    if len(blob) != COUNT * 4:
        sys.exit("short read: got %d of %d bytes" % (len(blob), COUNT * 4))

    words = struct.unpack("<%dH" % (COUNT * 2), blob)
    entries = [(words[2 * i], words[2 * i + 1]) for i in range(COUNT)]

    # sanity: contiguous register ramp, as documented in the analysis
    for i, (reg, _val) in enumerate(entries):
        want = (FIRST_REG + REG_STEP * i) & 0xFFFF
        if reg != want:
            sys.exit("entry %d: reg 0x%04x, expected 0x%04x" % (i, reg, want))

    lines = []
    w = lines.append
    w("/*")
    w(" * zl38063_config_record.h - ZL38063 configuration record (*.cr2) for the")
    w(" * xiaomodou B220 board.")
    w(" *")
    w(" * GENERATED FILE - do not edit by hand.")
    w(" * Regenerate with:  python gen_zl38063_config_record.py")
    w(" *")
    w(" * Source")
    w(" *   image      : backup/parts/wifi.bin (plaintext ESP32 app image)")
    w(" *   file offset: 0x%05x" % OFFSET)
    w(" *   DROM VA    : 0x3f459eaa" )
    w(" *   size       : 1042 bytes = %d x {uint16_t reg, uint16_t value}" % COUNT)
    w(" *   byte order : little-endian, exactly as stored in flash")
    w(" *")
    w(" * The 1042-byte payload is preceded in flash by the u16 byte count")
    w(" * 0x0412 = 1042; that counter is not part of the record and is not")
    w(" * emitted here.")
    w(" *")
    w(" * Registers step +2 and run contiguously:")
    w(" *   entry 0x0000: reg=0x%04x value=0x%04x   (first)"
      % (entries[0][0], entries[0][1]))
    w(" *   entry 0x%04x: reg=0x%04x value=0x%04x   (last)"
      % (COUNT - 1, entries[-1][0], entries[-1][1]))
    w(" *")
    w(" * Binary layout is identical to Microsemi's")
    w(" *   typedef struct { uint16_t reg; uint16_t value; } dataArr;")
    w(" * so the array can be handed straight to the ported API:")
    w(" *")
    w(" *   VprocTwolfLoadConfig((dataArr *)zl38063_config_record,")
    w(" *                        ZL38063_CONFIG_RECORD_COUNT);")
    w(" */")
    w("")
    w("#ifndef ZL38063_CONFIG_RECORD_H")
    w("#define ZL38063_CONFIG_RECORD_H")
    w("")
    w("#include <stdint.h>")
    w("")
    w("/* Number of {reg, value} entries in the record. */")
    w("#define ZL38063_CONFIG_RECORD_COUNT %d" % COUNT)
    w("/* Register of entry 0 and register of the last entry. */")
    w("#define ZL38063_CONFIG_RECORD_FIRST_REG 0x%04X" % entries[0][0])
    w("#define ZL38063_CONFIG_RECORD_LAST_REG  0x%04X" % entries[-1][0])
    w("")
    w("static const uint16_t zl38063_config_record[ZL38063_CONFIG_RECORD_COUNT][2] = {")
    for i, (reg, val) in enumerate(entries):
        if i == 0:
            note = "  /* first */"
        elif i == COUNT - 1:
            note = "  /* last  */"
        else:
            note = ""
        w("    {0x%04X, 0x%04X},%s" % (reg, val, note))
    w("};")
    w("")
    w("#endif /* ZL38063_CONFIG_RECORD_H */")
    w("")

    with open(OUT, "w", encoding="ascii", newline="\n") as fh:
        fh.write("\n".join(lines))
    print("wrote %s (%d entries, %d bytes of record = 0x%x..0x%x)"
          % (OUT, COUNT, COUNT * 4, OFFSET, OFFSET + COUNT * 4 - 1))


if __name__ == "__main__":
    main()
