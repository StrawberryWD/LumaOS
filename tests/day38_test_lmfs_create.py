#!/usr/bin/env python3
"""
tests/day38_test_lmfs_create.py — DAY38 QEMU verification (kernel
boot-time self-test, lmfs_create() called directly).

DAY38: "实现 create() 与目录项。" 验收："create test.txt 成功。"

Boots the real lumaos-5.0.img headless, and asserts kmain()'s DAY38
test section (lmfs_create() against the LBA 100000 fixture's root,
creating "kday38.txt") shows:

  1. "kday38.txt" ends up existing with a valid inode number — either
     freshly created this boot ("lmfs_create() -> inode N"), or, on a
     repeat boot of an already-mutated image, idempotently reused
     ("already exists ... reusing it") rather than spuriously failing.
     Both are correct outcomes of the SAME underlying lmfs_create()/
     lmfs_readdir() code — this script accepts either message, since
     lumaos-5.0.img genuinely persists across separate boots (this
     project has no disk-reset-on-boot mechanism) and kmain()'s DAY38
     section is deliberately idempotent for exactly that reason (see
     its own comment in kmain.c).
  2. The inode, read back via lmfs_read_inode(), is correctly
     initialized: type=FILE, file_size=0, all block pointers zero.
  3. lmfs_readdir() on root includes the file, with matching name and
     inode number — proving the directory entry was actually linked,
     not just an orphan inode.
  4. Creating the same name again is rejected (duplicate) — true
     regardless of whether step 1 took the fresh-create or the
     idempotent-reuse path.

The interactive-shell counterpart (actually typing `create
IDE3/test.txt` at a live prompt, per the roadmap's own literal
acceptance example) is tests/day38_test_create_shell.py.
"""
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker

IMG = "lumaos-5.0.img"

screen = wait_for_marker(IMG, "day38",
                          marker="duplicate create correctly rejected")

print(screen)
print("=" * 70)

checks = [
    ("DAY38 section ran",
     "DAY38: lmfs_create()" in screen),
    ("kday38.txt exists with a valid inode (fresh create or "
     "idempotent reuse)",
     "lmfs_create() -> inode" in screen or "reusing it" in screen),
    ("new inode correctly initialized message",
     "new inode correctly initialized (type=FILE, empty)" in screen),
    ("root now has entries after create() (3: '.', '..', new file)",
     "root now has 3 entries after create()" in screen),
    ("new file visible in root directory listing",
     "new file visible in root directory listing" in screen),
    ("duplicate create correctly rejected",
     "duplicate create correctly rejected" in screen),
    ("no FAIL lines anywhere in the DAY38 section",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)