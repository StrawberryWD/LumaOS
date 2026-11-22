#!/usr/bin/env python3
"""
tests/day42_test_lmfs_mkdir.py — DAY42 QEMU verification (kernel
boot-time self-test: lmfs_mkdir()/lmfs_resolve_path()/
lmfs_resolve_parent() called directly).

DAY42: "多级目录与路径解析。" 验收："可以创建并访问多级目录。"

Boots the real lumaos-5.0.img headless (QEMU text mode, no GUI) and
asserts kmain()'s DAY42 section — which builds kday42/sub/deep/leaf.txt,
then checks "." / ".." entries, multi-level + relative + absolute path
resolution, rejection of bad paths, resolve_parent() splitting, readdir
of a subdirectory and the leaf's content — printed "N/N passed" with
no FAIL line.

Run it TWICE in a row on the same image to see the idempotent-setup
path (second boot finds the directories already there and must reach
the same verdict):

    python3 tests/day42_test_lmfs_mkdir.py
    python3 tests/day42_test_lmfs_mkdir.py

The interactive-shell counterpart (typing mkdir/cd/ls/create/read/rm at
a live prompt) is tests/day42_test_dirs_shell.py.
"""
import re
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker

IMG = "lumaos-5.0.img"

screen = wait_for_marker(
    IMG, "day42", marker="multi-level directories: mkdir + path resolution verified")

print(screen)
print("=" * 70)

m = re.search(r"DAY42 checks: (\d+)/(\d+) passed", screen)
n_pass, n_total = (int(m.group(1)), int(m.group(2))) if m else (0, -1)

checks = [
    ("DAY42 section ran",
     "DAY42: lmfs_mkdir()" in screen),
    ("mkdir chain kday42/sub/deep reported inodes",
     "mkdir chain kday42/sub/deep -> inodes" in screen),
    ("all in-kernel checks passed (N/N, N >= 20)",
     m is not None and n_pass == n_total and n_total >= 20),
    ("success line printed",
     "multi-level directories: mkdir + path resolution verified" in screen),
    ("no FAIL lines anywhere in the DAY42 section",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)
