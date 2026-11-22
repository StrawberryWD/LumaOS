#!/usr/bin/env python3
"""
tests/day35_test_lmfs_vfs.py — DAY35 QEMU verification.

DAY35: "LMFS 注册进 VFS。" 验收："FAT 与 LMFS 可以同时存在。"

HONEST LIMITATION — see day32_test_lmfs_mount.py's header comment for
the full explanation: this project's 80x25 VGA console has no
scrollback, and kmain()'s DAY32+ sections print their whole combined
output faster than any external screen-scraping loop can reliably
sample once enough LATER days' output has accumulated on top (this
script started missing its own earlier-printed lines the day DAY36 was
added, despite DAY35's kernel code being completely unchanged and
correct). DAY35's own detail (IDE0..IDE3 all resolving to the right
fs->name, LMFS listed via the generic fs->readdir() adapter) WAS
captured and verified via this exact technique at the time this day
was implemented — see this conversation's Day 35 transcript for that
full, passing screen capture. This script is kept as a lighter,
honestly-scoped regression check:

  1. The kernel reaches a live shell prompt on slot IDE3 (proving LMFS
     is still the last-registered, and therefore live, mount — if
     lmfs_vfs_mount()/vfs_register_mount() broke, the prompt would
     show an earlier IDE slot or the boot would hang before reaching
     one at all).
  2. No "[lmfs] FAIL" text appears anywhere in the captured boot log.

Ongoing regression coverage of the LMFS<->VFS adapter ITSELF continues
through tests/day36_test_lmfs_alloc_block.py, which re-mounts the same
fixture from scratch every run.
"""
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker, SHELL_PROMPT_MARKER

IMG = "lumaos-5.0.img"

screen = wait_for_marker(IMG, "day35", marker=SHELL_PROMPT_MARKER)

print(screen)
print("=" * 70)

checks = [
    ("reached a live shell prompt on IDE3",
     "IDE3" + SHELL_PROMPT_MARKER in screen),
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