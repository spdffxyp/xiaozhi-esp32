"""Locate, validate and extract the ZL38063 DSP firmware image from the stock
ESP32 flash dump (backup/parts/wifi.bin).

Evidence chain (see zl38063_firmware_image.md):
  boot wrapper        0x40126240 : struct+0 = 0x3f4322e4  (l32r of a DROM addr)
  VprocTwolfHbiBoot   0x40125c64 : record loop at 0x40125cfd..0x40125d8f
                                   elem = st_Fwr + i*44, fields +32/+36/+40

Record layout recovered from the loop and from the upstream reference
api_lib/vproc_data_types.h "twFwr":

    typedef struct {          /* 44 bytes, as compiled here            */
        uint16 buf[16];       /* +0  .. +31  data words (little-endian) */
        uint16 numWords;      /* +32                                       */
        uint32 targetAddr;    /* +36                                       */
        uint8  useTargetAddr; /* +40  (0 or 1)                             */
    } twFwr;                  /* +41..43 = tail padding to 4-byte align    */

Run:  python zl38063_extract_fw.py
Writes (all next to this script):
    zl38063_firmware_image.bin      concatenated payload words (DSP RAM image)
    zl38063_firmware_records.bin    byte-exact 3699*44 record array copy
    zl38063_firmware_image.h        C array in the twFwr layout above
"""
import os
import struct
import sys

APP = r"<workspace>\backup\parts\wifi.bin"
HERE = os.path.dirname(os.path.abspath(__file__))

# segment: name, load VA, file offset, size
SEGMENTS = [
    ("DROM",  0x3f400020, 0x000020, 0x1ce1c8),
    ("DRAM",  0x3ffbdb60, 0x1ce1f0, 0x001e20),
    ("IROM",  0x400d0018, 0x1d0018, 0x1884e0),
    ("DRAM2", 0x3ffbf980, 0x358500, 0x002cf0),
    ("IRAM",  0x40080000, 0x35b1f8, 0x000400),
    ("IRAM2", 0x40080400, 0x35b600, 0x01eb60),
    ("RTC",   0x400c0000, 0x37a168, 0x00006c),
]

ST_FWR = 0x3f4322e4        # struct+0, loaded by 0x4012628b..0x4012628e
HDR = 0x3f4322d8           # packed scalars just before the array
STREAM_LEN = 3699          # 0x0E73, u16 at ST_FWR-12
STRIDE = 44
EXEC_ADDR = 0x00083040     # u32 at ST_FWR-8
PRGM_BASE = 0x00080200     # u32 at ST_FWR-4
HAVE_BASE = 1              # u8  at ST_FWR-10

MAX_WORDS = 16             # sizeof(twFwr.buf)/2


def load():
    with open(APP, "rb") as fh:
        return fh.read()


IMG = load()


def seg_for(va):
    for name, ld, off, size in SEGMENTS:
        if ld <= va < ld + size:
            return name, ld, off, size
    return None


def rd(va, n):
    s = seg_for(va)
    if not s:
        return None
    _n, ld, off, size = s
    return IMG[off + (va - ld):off + (va - ld) + n]


def u16(va):
    return struct.unpack("<H", rd(va, 2))[0]


def u32(va):
    return struct.unpack("<I", rd(va, 4))[0]


def main():
    # ---- 0. verify the literal pool entries the wrapper loads -------------
    print("== literal pool (IROM) used by wrapper 0x40126240 ==")
    for lit, want in [(0x40111b38, ST_FWR), (0x40111b3c, HDR),
                      (0x40111b40, HDR + 4), (0x40111b44, HDR + 2),
                      (0x40111b48, HDR + 8)]:
        got = u32(lit)
        print("   0x%08x -> 0x%08x %s" % (lit, got, "OK" if got == want else "MISMATCH"))

    print("\n== header scalars in DROM (packed, not the C struct) ==")
    print("   streamLen  u16@0x%08x = %d" % (HDR, u16(HDR)))
    print("   haveBase   u8 @0x%08x = %d" % (HDR + 2, rd(HDR + 2, 1)[0]))
    print("   execAddr   u32@0x%08x = 0x%08x" % (HDR + 4, u32(HDR + 4)))
    print("   prgmBase   u32@0x%08x = 0x%08x" % (HDR + 8, u32(HDR + 8)))

    n = u16(HDR)
    # ---- 1. walk the records -------------------------------------------
    recs = []
    bad_numwords = []
    bad_use = []
    empty = 0
    payload = bytearray()
    for i in range(n):
        e = ST_FWR + i * STRIDE
        num = u16(e + 32)
        tgt = u32(e + 36)
        use = rd(e + 40, 1)[0]
        buf = rd(e, 2 * MAX_WORDS) if num <= MAX_WORDS else None
        recs.append((e, num, tgt, use))
        if num == 0:
            empty += 1
            continue
        if num > MAX_WORDS:
            bad_numwords.append((i, num))
        if use not in (0, 1):
            bad_use.append((i, use))
        payload += buf[:2 * num]

    print("\n== record array 0x%08x, %d records x %d bytes = 0x%x bytes =="
          % (ST_FWR, n, STRIDE, n * STRIDE))
    print("   array end      0x%08x" % (ST_FWR + n * STRIDE))
    print("   non-empty recs %d  (empty numWords==0: %d)" % (n - empty, empty))
    print("   max numWords   %d  (buf[] capacity %d)" % (max(r[1] for r in recs), MAX_WORDS))
    print("   numWords>cap   %d  %s" % (len(bad_numwords), bad_numwords[:5]))
    print("   useTargetAddr not in {0,1}: %d %s" % (len(bad_use), bad_use[:5]))
    print("   payload bytes  %d (0x%x)  words %d" % (len(payload), len(payload), len(payload) // 2))

    nonzero = [r for r in recs if r[1]]
    tgts = [r[2] for r in nonzero]
    print("   targetAddr min 0x%08x  max 0x%08x" % (min(tgts), max(tgts)))
    # contiguity check: each record should follow the previous
    gaps = 0
    for a, b in zip(nonzero, nonzero[1:]):
        if b[2] != a[2] + a[1] * 2:
            gaps += 1
    print("   contiguous (tgt[i+1] == tgt[i]+2*numWords[i]): %s (%d breaks)"
          % (gaps == 0, gaps))
    print("   first 4: " + " | ".join("0x%08x/%dw/use=%d" % (r[2], r[1], r[3]) for r in nonzero[:4]))
    print("   last  4: " + " | ".join("0x%08x/%dw/use=%d" % (r[2], r[1], r[3]) for r in nonzero[-4:]))

    # padding / 0xFF check
    n_ff = sum(1 for i in range(0, len(payload) - 1, 2)
               if payload[i] == 0xFF and payload[i + 1] == 0xFF)
    n_00 = sum(1 for i in range(0, len(payload) - 1, 2)
               if payload[i] == 0 and payload[i + 1] == 0)
    words = len(payload) // 2
    print("   payload words: 0xFFFF=%d (%.1f%%)  0x0000=%d (%.1f%%)  distinct=%d"
          % (n_ff, 100.0 * n_ff / words, n_00, 100.0 * n_00 / words,
             len({payload[i] | (payload[i + 1] << 8) for i in range(0, len(payload) - 1, 2)})))

    # ---- 2. write outputs ----------------------------------------------
    p_bin = os.path.join(HERE, "zl38063_firmware_image.bin")
    with open(p_bin, "wb") as fh:
        fh.write(payload)
    p_rec = os.path.join(HERE, "zl38063_firmware_records.bin")
    with open(p_rec, "wb") as fh:
        fh.write(rd(ST_FWR, n * STRIDE))

    p_h = os.path.join(HERE, "zl38063_firmware_image.h")
    with open(p_h, "w") as fh:
        fh.write(HEADER_TMPL % (n, PRGM_BASE, EXEC_ADDR, HAVE_BASE, n,
                                len(payload), PRGM_BASE,
                                ST_FWR, n, PRGM_BASE, EXEC_ADDR, HAVE_BASE,
                                n, EXEC_ADDR, HAVE_BASE, PRGM_BASE))
        for i, (e, num, tgt, use) in enumerate(recs):
            data = rd(e, 2 * num) if num <= MAX_WORDS else b""
            ws = ", ".join("0x%04x" % struct.unpack_from("<H", data, 2 * k)[0]
                           for k in range(num))
            pad = ", ".join(["0x0000"] * (MAX_WORDS - num)) if num <= MAX_WORDS else ""
            body = (ws + (", " + pad if pad else "")) if num else ", ".join(["0x0000"] * MAX_WORDS)
            fh.write("    { { %s }, %2d, 0x%08x, %d },\n" % (body, num, tgt, use))
        fh.write("};\n\n#endif /* ZL38063_FIRMWARE_IMAGE_H */\n")
    print("\nwrote %s (%d bytes)" % (p_bin, len(payload)))
    print("wrote %s (%d bytes)" % (p_rec, n * STRIDE))
    print("wrote %s (%d bytes)" % (p_h, os.path.getsize(p_h)))
    return 0


HEADER_TMPL = """/* Generated by zl38063_extract_fw.py - DO NOT EDIT.
 *
 * ZL38063 DSP firmware image pulled out of the stock ESP32 flash dump
 * backup/parts/wifi.bin (DROM 0x3f400020 / file 0x20).
 *
 * Source of truth: the twFwr record array at DROM 0x3f4322e4, length
 * %d records, copied byte-exact from the dump.  It is the exact array that
 * stock firmware's VprocTwolfHbiBoot (0x40125c64) feeds to the DSP through
 * HBI register 0x0C / page-255 writes, and the layout below is the layout
 * that HbiSrecBoot_alt() in adf_ref/api_lib/vprocTwolf_access.c consumes.
 *
 * struct layout on this build (stride 44, confirmed from the loop at
 * 0x40125cfd: elem = st_Fwr + i*44 reading +32, +36, +40):
 *
 *     typedef struct {           size 44
 *         uint16 buf[16];        +0   .. +31  little-endian data words
 *         uint16 numWords;       +32
 *         uint32 targetAddr;     +36
 *         uint8  useTargetAddr;  +40  (0 or 1)
 *     } twFwr;                   +41..43 tail padding to 4-byte alignment
 *
 * The upstream header declares the fields in the order
 * { buf[16]; numWords; targetAddr; useTargetAddr; } - identical here, but the
 * stock build keeps numWords on a 4-byte boundary (2 pad bytes at +34) and
 * pads the tail to 44; a compiler emitting the upstream declaration with
 * uint32 alignment reproduces exactly this: sizeof(twFwr) == 44.
 *
 * Scalars (read by wrapper 0x40126240 from the packed DROM header just before
 * the array):
 *     programBaseAddress = 0x%08x   (first record's targetAddr)
 *     executionAddress   = 0x%08x
 *     haveProgramBaseAddress = %d
 *     firmwareStreamLength   = %d records
 * Concatenated payload: %d bytes, DSP word addresses 0x%08x..+payload.
 */
#ifndef ZL38063_FIRMWARE_IMAGE_H
#define ZL38063_FIRMWARE_IMAGE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t buf[16];
    uint16_t numWords;
    uint32_t targetAddr;
    uint8_t  useTargetAddr;
} twFwr;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(twFwr) == 44, "twFwr must be 44 bytes to match flash");
_Static_assert(offsetof(twFwr, numWords) == 32, "layout mismatch");
_Static_assert(offsetof(twFwr, targetAddr) == 36, "layout mismatch");
_Static_assert(offsetof(twFwr, useTargetAddr) == 40, "layout mismatch");
#endif

#define ZL38063_FW_RECORD_BASE     0x%08xu
#define ZL38063_FW_RECORDS         %du
#define ZL38063_FW_PROGRAM_BASE    0x%08xu
#define ZL38063_FW_EXEC_ADDR       0x%08xu
#define ZL38063_FW_HAVE_PROGRAM_BASE %du

/* Linkage: by default everything is static and the array is named
 * zl38063_st_twFirmware, so this header can be included anywhere without
 * clashing with the extern declarations in zl38063_vproc.h.  Compile ONE .c
 * with -DZL38063_FWIMAGE_EXTERNAL to emit the strong upstream symbols
 * (st_twFirmware, firmwareStreamLen, executionAddress, haveProgramBaseAddress,
 * programBaseAddress) that override the weak placeholders in zl38063_fwimg.c.
 */
#if defined(ZL38063_FWIMAGE_EXTERNAL)
#define ZL38063_FW_SYMBOL
#define ZL38063_FW_NAME(n) n
#else
#define ZL38063_FW_SYMBOL static
#define ZL38063_FW_NAME(n) zl38063_##n
#endif

ZL38063_FW_SYMBOL const uint16_t ZL38063_FW_NAME(firmwareStreamLen)     = %du;
ZL38063_FW_SYMBOL const uint32_t ZL38063_FW_NAME(executionAddress)      = 0x%08xu;
ZL38063_FW_SYMBOL const uint8_t  ZL38063_FW_NAME(haveProgramBaseAddress) = %du;
ZL38063_FW_SYMBOL const uint32_t ZL38063_FW_NAME(programBaseAddress)    = 0x%08xu;

ZL38063_FW_SYMBOL const twFwr ZL38063_FW_NAME(st_twFirmware)[ZL38063_FW_RECORDS] = {
"""


if __name__ == "__main__":
    sys.exit(main())
