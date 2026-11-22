#!/usr/bin/env python3
"""tools/mkfont.py - DAY63: turn the kernel's built-in 8x8 font into a font FILE for the ISO.

    python3 tools/mkfont.py -o build/sys8x8.fnt              # from kernel/lib/font.c (the 95 ASCII glyphs 32..126)
    python3 tools/mkfont.py -o x.fnt --glyph A=FF818181818181FF   # override one glyph (8 hex bytes) - for tests
    python3 tools/mkfont.py --check x.fnt                     # validate + print the checksum

File format "LFNT" (see kernel/lib/font.c font_load()):  "LFNT", version 1, first char 32, count 95,
height 8, then 95*8 row bytes (MSB = leftmost pixel). The checksum printed is FNV-1a-32 over the 760 glyph
bytes - the kernel's `font` command prints the same number, which is how the tests prove that the glyphs in
use came from the file on the CD."""
import argparse, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRST, COUNT, HEIGHT = 32, 95, 8

def builtin_glyphs(src=None):
    src = src or os.path.join(HERE, "..", "kernel", "lib", "font.c")
    text = open(src, encoding="utf-8").read()
    start = text.index("font_bitmap[95][8]")
    body = text[start:text.index("};", start)]
    rows = re.findall(r"\{\s*((?:0x[0-9A-Fa-f]{2}\s*,?\s*){8})\}", body)
    glyphs = [bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", r)) for r in rows]
    if len(glyphs) != COUNT:
        raise SystemExit("mkfont: expected %d glyphs in %s, found %d" % (COUNT, src, len(glyphs)))
    return glyphs

def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h

def build(glyphs):
    body = b"".join(glyphs)
    return b"LFNT" + bytes([1, FIRST, COUNT, HEIGHT]) + body

def check(data):
    if len(data) != 8 + COUNT * HEIGHT: raise ValueError("bad length %d" % len(data))
    if data[:4] != b"LFNT": raise ValueError("bad magic")
    if tuple(data[4:8]) != (1, FIRST, COUNT, HEIGHT): raise ValueError("bad header %r" % data[4:8])
    return fnv1a(data[8:])

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output")
    ap.add_argument("--source", help="font.c to read (default: kernel/lib/font.c)")
    ap.add_argument("--glyph", action="append", default=[], metavar="CHAR=HEX16")
    ap.add_argument("--check", metavar="FILE")
    a = ap.parse_args()
    if a.check:
        print("0x%08X" % check(open(a.check, "rb").read())); return
    if not a.output: ap.error("-o is required")
    g = builtin_glyphs(a.source)
    for spec in a.glyph:
        ch, hx = spec.split("=", 1)
        if len(ch) != 1 or not (FIRST <= ord(ch) < FIRST + COUNT) or len(hx) != 16:
            raise SystemExit("mkfont: bad --glyph %r" % spec)
        g[ord(ch) - FIRST] = bytes.fromhex(hx)
    data = build(g)
    open(a.output, "wb").write(data)
    print("%s: %d bytes, checksum 0x%08X" % (a.output, len(data), fnv1a(data[8:])))

if __name__ == "__main__":
    main()
