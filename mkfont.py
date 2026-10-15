#!/usr/bin/env python3
"""mkfont.py generate a tint font (a .c file) from GNU Unifont.

    mkfont.py NAME [--ranges HEX-HEX,...] [--codec CODEC[:LO-HI]]... [--text FILE] [-o OUT.c]

NAME      the font is emitted as `const TintFont tint_font_NAME` (default output: font_NAME.c next to this script)
--ranges  Unicode ranges in hex, e.g.  20-7e,a0-17f,400-4ff
--codec   every character a legacy codec can decode, optionally limited to a 2-byte code range, e.g.
              gb2312:b0a1-d7f9   (3755 level-1 hanzi)     gb2312:d8a1-f7fe  (3008 level-2 hanzi)
              big5:a440-c67e     (Traditional, common)    shift_jis:889f-9ffc (JIS level-1 kanji)
              euc-kr:b0a1-c8fe   (2350 Hangul syllables)
--text    every character that occurs in FILE (UTF-8): "font for exactly the strings my program shows"
--font    path to unifont.otf (default: the Debian/Ubuntu fonts-unifont location)

Output is a 16 pixel-high font: 8 wide cells, with double-width (16 px) glyphs for CJK and friends.
Needs Pillow. The generated data is derived from GNU Unifont and therefore stays under Unifont's
licence (SIL OFL 1.1, or GPL-2+ with the Font Embedding Exception); see OFL.md.
Only the Basic Multilingual Plane is supported, and there is no shaping: scripts that need it
(Arabic, Devanagari, ...) or combining marks will not render correctly."""
import argparse, os, sys
from PIL import Image, ImageDraw, ImageFont

K = 4                                   # Unifont's outline grid: 64 units/em = 16 px * 4 units
ap = argparse.ArgumentParser()
ap.add_argument("name"); ap.add_argument("--ranges", default=""); ap.add_argument("--codec", action="append", default=[])
ap.add_argument("--text"); ap.add_argument("-o"); ap.add_argument("--font", default="/usr/share/fonts/opentype/unifont/unifont.otf")
a = ap.parse_args()

cps = set()
for r in filter(None, a.ranges.split(",")):
    lo, _, hi = r.partition("-"); cps.update(range(int(lo, 16), int(hi or lo, 16) + 1))
for spec in a.codec:
    name, _, rng = spec.partition(":")
    lo, hi = (int(x, 16) for x in rng.split("-")) if rng else (0x8140, 0xfefe)
    for hb in range(lo >> 8, (hi >> 8) + 1):
        for lb in range(0x40, 0xff):
            if lo <= hb << 8 | lb <= hi:
                try: cps.add(ord(bytes([hb, lb]).decode(name)))
                except (UnicodeDecodeError, TypeError): pass
if a.text: cps.update(ord(c) for c in open(a.text, encoding="utf-8").read() if c >= " ")

font = ImageFont.truetype(a.font, 16 * K)           # 1 render pixel = 1 font unit
glyphs = {}                                         # cp -> (cells, bytes)
for cp in sorted(cps):
    if cp > 0xFFFF: continue
    adv = font.getlength(chr(cp)) / (8 * K)
    if adv not in (1, 2): continue                  # missing from the font, or a zero-width combining mark
    cells = int(adv)
    im = Image.new("L", (cells * 8 * K, 16 * K), 0)
    ImageDraw.Draw(im).text((0, 14 * K), chr(cp), font=font, fill=255, anchor="ls")   # baseline = row 14
    out = bytearray()
    for y in range(16):
        for byte in range(cells):
            v = 0
            for b in range(8):                      # sample each pixel's centre; bit 0 = leftmost
                if im.getpixel(((byte * 8 + b) * K + K // 2, y * K + K // 2)) >= 128: v |= 1 << b
            out.append(v)
    glyphs[cp] = (cells, bytes(out))

cp_list = sorted(glyphs); offs = [0]
for cp in cp_list: offs.append(offs[-1] + glyphs[cp][0])          # offsets in 16-byte units
assert offs[-1] < 65536
cmd = "mkfont.py " + " ".join(sys.argv[1:])
o = [f"""/* font_{a.name}.c: successfully generated, do not edit:
 *     {cmd}
 * {len(cp_list)} glyphs. Derived from GNU Unifont 15.1 (https://unifoundry.com/unifont/).
 * Copyright 1998-2023 Roman Czyborra, Paul Hardy, Qianqian Fang, Andrew Miller, Johnnie Weaver, David Corbett,
 * Nils Moskopp, Rebecca Bettencourt, Minseo Lee, Ho-Seok Ee, et al.
 * This file is licensed under the SIL Open Font License 1.1 (or GPL-2+ with the GNU Font Embedding
 * Exception), as upstream: it is NOT covered by tint's LGPL. See OFL.md. */
#include "fonts.h"
#include <stddef.h>

static const uint16_t cps[{len(cp_list)}] = {{"""]
o.append("".join(("\n" if i and i % 16 == 0 else "") + f"{c:#x}," for i, c in enumerate(cp_list)))
o.append(f"}};\nstatic const uint16_t offs[{len(cp_list) + 1}] = {{")
o.append("".join(("\n" if i and i % 16 == 0 else "") + f"{x}," for i, x in enumerate(offs)))
o.append(f"}};\nstatic const uint8_t data[{offs[-1] * 16}] = {{")
for cp in cp_list: o.append(",".join(f"{b:#04x}" for b in glyphs[cp][1]) + ",")
o.append("""};

static const uint8_t *glyph(uint32_t cp, int *cells) {
    size_t lo = 0, hi = sizeof cps / sizeof *cps;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (cps[m] < cp) lo = m + 1; else hi = m; }
    if (lo >= sizeof cps / sizeof *cps || cps[lo] != cp) return NULL;
    *cells = offs[lo + 1] - offs[lo];
    return data + offs[lo] * 16;
}
""" + f"const TintFont tint_font_{a.name} = {{ 8, 16, glyph }};\n")
path = a.o or os.path.join(os.path.dirname(os.path.abspath(__file__)), f"font_{a.name}.c")
open(path, "w", encoding="utf-8").write("\n".join(o))
print(f"{path}: {len(cp_list)} glyphs, {offs[-1] * 16} bytes of bitmap data", file=sys.stderr)