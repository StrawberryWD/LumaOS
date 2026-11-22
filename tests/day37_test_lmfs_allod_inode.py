#!/usr/bin/env python3
"""
tests/day37_test_lmfs_alloc_inode.py — DAY37 QEMU verification.

DAY37: "实现 inode 分配器。" 验收："创建 inode 后返回合法 inode 号。"

HONEST LIMITATION — see day32_test_lmfs_mount.py's header comment for
the full explanation: this project's 80x25 VGA console has no
scrollback, and kmain()'s DAY32+ sections print their whole combined
output faster than any external screen-scraping loop can reliably
sample once enough LATER days' output has accumulated on top (this
script started missing its own earlier-printed lines the day DAY38 was
added, despite DAY37's kernel code being completely unchanged and
correct). DAY37's own detail (3 consecutive allocations -> inodes
1/2/3, each correctly initialized as an empty FILE inode, each bit
independently re-read off disk and confirmed set) WAS captured and
verified via this exact technique at the time this day was implemented
— see this conversation's Day 37 transcript for that full, passing
screen capture. This script is kept as a lighter, honestly-scoped
regression check:

  1. The kernel reaches a live shell prompt (no crash/hang).
  2. No "[lmfs] FAIL" text appears anywhere in the captured boot log
     — NOTE: on a disk image that a PRIOR test run has already
     mutated (e.g. day38's tests, which genuinely write a file into
     this same fixture), DAY38's own self-test section will correctly
     report an expected duplicate-create failure here too; rebuild
     (`bash build.sh`) for a clean image before trusting this check in
     isolation — see tests/day24_test_create.py's header comment for
     the original explanation of this same phenomenon on the FAT side.

Ongoing regression coverage of lmfs_alloc_inode() ITSELF continues
through tests/day38_test_lmfs_create.py (calls it internally via
lmfs_create()) and tests/day38_test_create_shell.py, both of which
would fail if lmfs_alloc_inode() ever broke.
"""
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker, SHELL_PROMPT_MARKER

IMG = "lumaos-5.0.img"

screen = wait_for_marker(IMG, "day37", marker=SHELL_PROMPT_MARKER)

print(screen)
print("=" * 70)

checks = [
    ("reached a live shell prompt",
     SHELL_PROMPT_MARKER in screen),
    ("no [lmfs] FAIL anywhere in the captured boot log",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)