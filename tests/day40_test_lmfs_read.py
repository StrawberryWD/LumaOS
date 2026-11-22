#!/usr/bin/env python3
"""
tests/day40_test_lmfs_read.py — DAY40 QEMU verification (kernel
boot-time self-test, lmfs_read() called directly).

DAY40: "实现 read()。" 验收："read bigfile.txt 正常。"

Boots the real lumaos-5.0.img headless, and asserts kmain()'s DAY40
test section (writes "bigfile.txt" with a readable repeated phrase,
10240 bytes, then reads it back via lmfs_read() in three misaligned
chunks: 3000+3000+4240) shows:

  1. bigfile.txt written successfully.
  2. Each of the three chunked lmfs_read() calls returns exactly the
     number of bytes requested.
  3. The three chunks, reassembled, are byte-for-byte identical to
     what was written.
  4. A read exactly at EOF (offset == file_size) returns 0, not a
     negative error.

The interactive-shell counterpart (actually typing `read bigfile.txt`
at a live prompt, per the roadmap's own literal acceptance example) is
tests/day40_test_read_shell.py.
"""
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker

IMG = "lumaos-5.0.img"

screen = wait_for_marker(
    IMG, "day40",
    marker="read at EOF correctly returned 0 (not an error)")

print(screen)
print("=" * 70)

checks = [
    ("DAY40 section ran",
     "DAY40: lmfs_read()" in screen),
    ("bigfile.txt written (10240 bytes)",
     "bigfile.txt written (10240 bytes)" in screen),
    ("chunk #1 at offset=0 requested=3000 -> 3000",
     "chunk #1 at offset=0 requested=3000 -> 3000" in screen),
    ("chunk #2 at offset=3000 requested=3000 -> 3000",
     "chunk #2 at offset=3000 requested=3000 -> 3000" in screen),
    ("chunk #3 at offset=6000 requested=4240 -> 4240",
     "chunk #3 at offset=6000 requested=4240 -> 4240" in screen),
    ("chunked reads reassembled byte-for-byte match",
     "chunked reads reassembled byte-for-byte match" in screen),
    ("read at EOF correctly returned 0",
     "read at EOF correctly returned 0 (not an error)" in screen),
    ("no FAIL lines anywhere in the DAY40 section",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)