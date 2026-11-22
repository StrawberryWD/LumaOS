#!/usr/bin/env python3
"""
tests/day41_test_rm_shell.py — DAY41 interactive-Shell check.

DAY41: "实现 delete() 与空间回收。" 验收："rm 后空间恢复。"

kmain.c's own DAY41 boot-time section (and tests/day41_test_lmfs_delete.py,
which asserts on it) calls lmfs_delete() directly and independently
re-reads raw bitmap bytes to prove space reclaim. This script instead
drives the REAL interactive Shell: create a file (same LMFS `create`
path tests/day38_test_create_shell.py already proved works), confirm it
shows up in `ls`, `rm` it, and confirm `ls` no longer shows it — the
roadmap's own literal acceptance phrasing, "rm 后空间恢复", demonstrated
end to end through the actual commands a user would type.

Uses the same fresh-image-copy isolation technique as
day38_test_create_shell.py, for the same reason: `create`/`rm` genuinely
write to lumaos-5.0.img, and that persists across separate QEMU boots.

Not part of the build; a manual verification script only.
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(__file__))
from day24_test_create import run_commands_get_screen  # noqa: E402

IMG = "lumaos-5.0.img"
_BASE_IMG_SNAPSHOT = IMG + ".day41_test_base"


def _fresh_image_copy():
    if not os.path.exists(_BASE_IMG_SNAPSHOT):
        shutil.copyfile(IMG, _BASE_IMG_SNAPSHOT)
    shutil.copyfile(_BASE_IMG_SNAPSHOT, IMG)


def main():
    _fresh_image_copy()
    print("=== create IDE3/rmtest.txt, ls, rm IDE3/rmtest.txt, ls ===")
    screen = run_commands_get_screen(
        ["create IDE3/rmtest.txt", "ls", "rm IDE3/rmtest.txt", "ls"],
        "day41_rm")
    print(screen)

    ok = True

    if "created IDE3/rmtest.txt" in screen:
        print("PASS: file created\n")
    else:
        print("FAIL: expected 'created IDE3/rmtest.txt'\n", file=sys.stderr)
        ok = False

    # Check the specific `ls` LISTING LINE ("rmtest.txt      0 bytes"),
    # not just the bare filename substring (which also appears in the
    # create/rm command echoes and their success messages). That exact
    # listing line should appear exactly once — from the first `ls`,
    # right after create — and must NOT appear a second time after `rm`.
    listing_line = "rmtest.txt      0 bytes"
    listing_occurrences = screen.count(listing_line)
    if ("removed IDE3/rmtest.txt" in screen and listing_occurrences == 1):
        print("PASS: rm succeeded and the file no longer appears in "
              "the second 'ls'\n")
    else:
        print("FAIL: expected 'removed IDE3/rmtest.txt' and exactly "
              "one 'ls' listing line (got %d)\n" % listing_occurrences,
              file=sys.stderr)
        ok = False

    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()