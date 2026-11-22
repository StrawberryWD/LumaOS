#!/usr/bin/env python3
"""
tests/day39_test_lmfs_write.py — DAY39 QEMU verification.

DAY39: "实现 write()，支持直接块与间接块。" 验收："写入 10KB 文件并验证。"

Boots the real lumaos-5.0.img headless, and asserts kmain()'s DAY39
test section (lmfs_write() of a 10240-byte, byte-pattern-verifiable
buffer against the LBA 100000 fixture) shows:

  1. lmfs_write() succeeds.
  2. The resulting inode's shape is correct: file_size=10240, all 8
     direct block pointers set, indirect1 also set — proving the
     single-indirect path was genuinely exercised, not just direct
     blocks.
  3. All 10240 bytes, read back by kmain.c's own test code by walking
     direct[]/indirect1 directly (lmfs_read() doesn't exist until
     Day 40), are byte-for-byte identical to what was written.
  4. A write exceeding LMFS_MAX_FILE_SIZE is correctly rejected.
"""
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker

IMG = "lumaos-5.0.img"

screen = wait_for_marker(
    IMG, "day39",
    marker="write exceeding LMFS_MAX_FILE_SIZE correctly rejected")

print(screen)
print("=" * 70)

checks = [
    ("DAY39 section ran",
     "DAY39: lmfs_write()" in screen),
    ("lmfs_write() returned 0",
     "lmfs_write() returned 0" in screen),
    ("inode shape correct message",
     "inode shape correct (file_size=10240, 8 direct blocks + "
     "indirect1 used)" in screen),
    ("all 10240 bytes byte-for-byte identical",
     "all 10240 bytes read back byte-for-byte identical to what was "
     "written" in screen),
    ("oversized write correctly rejected",
     "write exceeding LMFS_MAX_FILE_SIZE correctly rejected" in screen),
    ("no FAIL lines anywhere in the DAY39 section",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)