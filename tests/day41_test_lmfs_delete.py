#!/usr/bin/env python3
"""
tests/day41_test_lmfs_delete.py — DAY41 QEMU verification (kernel
boot-time self-test, lmfs_delete() called directly).

DAY41: "实现 delete() 与空间回收。" 验收："rm 后空间恢复。"

Boots the real lumaos-5.0.img headless, and asserts kmain()'s DAY41
test section (writes "kday41.txt" with 10240+ bytes spanning direct
AND indirect1 blocks, deletes it, independently re-reads the raw data
and inode bitmap bytes before/after) shows:

  1. Before delete: all 11 blocks the file uses (8 direct + 2 indirect
     -referenced data blocks + the indirect1 index block itself) and
     the file's own inode are all shown allocated on disk.
  2. lmfs_delete() succeeds.
  3. The file is no longer found via lmfs_lookup().
  4. After delete: all those same 11 block bits AND the inode bit are
     independently re-read off disk and confirmed freed — this is the
     literal "space reclaimed" proof, not just a function return value.
  5. Deleting a directory (lmfs_delete() on ".") is correctly rejected.

The interactive-shell counterpart (actually typing `rm` at a live
prompt and confirming `ls` no longer shows the file, per the roadmap's
own literal acceptance example) is tests/day41_test_rm_shell.py.
"""
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker

IMG = "lumaos-5.0.img"

screen = wait_for_marker(
    IMG, "day41", marker="deleting a directory correctly rejected")

print(screen)
print("=" * 70)

checks = [
    ("DAY41 section ran",
     "DAY41: lmfs_delete()" in screen),
    ("before delete: all 12 bits allocated",
     "before delete: 11 data block bits + 1 inode bit all "
     "allocated=1" in screen),
    ("lmfs_delete() returned 0",
     "lmfs_delete() returned 0" in screen),
    ("file no longer found via lmfs_lookup()",
     "file no longer found via lmfs_lookup()" in screen),
    ("after delete: all 12 bits freed",
     "after delete: 11 data block bits + 1 inode bit all freed=1"
     in screen),
    ("space correctly reclaimed on disk",
     "space correctly reclaimed on disk" in screen),
    ("deleting a directory correctly rejected",
     "deleting a directory correctly rejected" in screen),
    ("no FAIL lines anywhere in the DAY41 section",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)