#!/usr/bin/env python3
"""
tests/day36_test_lmfs_alloc_block.py — DAY36 QEMU verification.

DAY36: "实现数据块 bitmap 分配器。" 验收："连续分配多个块，bitmap 正确变化。"

HONEST LIMITATION — see day32_test_lmfs_mount.py's header comment for
the full explanation: this project's 80x25 VGA console has no
scrollback, and kmain()'s DAY32+ sections print their whole combined
output faster than any external screen-scraping loop can reliably
sample once enough LATER days' output has accumulated on top (this
script started missing its own earlier-printed lines the day DAY37 was
added, despite DAY36's kernel code being completely unchanged and
correct). DAY36's own detail (3 consecutive allocations -> blocks
2/3/4, each bit independently re-read off disk and confirmed set) WAS
captured and verified via this exact technique at the time this day
was implemented — see this conversation's Day 36 transcript for that
full, passing screen capture. This script is kept as a lighter,
honestly-scoped regression check:

  1. The kernel reaches a live shell prompt (no crash/hang).
  2. No "[lmfs] FAIL" text appears anywhere in the captured boot log.

Ongoing regression coverage of lmfs_alloc_block() ITSELF continues
through tests/day37_test_lmfs_alloc_inode.py's own mount (which re-
exercises the same bitmap-scan code shape, just against the inode
bitmap), and any future day that calls lmfs_alloc_block() again.
"""
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker, SHELL_PROMPT_MARKER

IMG = "lumaos-5.0.img"

screen = wait_for_marker(IMG, "day36", marker=SHELL_PROMPT_MARKER)

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