#!/usr/bin/env python3
"""
tests/day40_test_read_shell.py — DAY40 interactive-Shell check.

DAY40: "实现 read()。" 验收："read bigfile.txt 正常。"

kmain.c's own DAY40 boot-time section (and tests/day40_test_lmfs_read.py,
which asserts on it) calls lmfs_read() directly and, before that, writes
"bigfile.txt" into the LMFS root via lmfs_write() every boot (idempotently
— see that section's own kmain.c comment). This script instead drives the
REAL interactive Shell, typing exactly the roadmap's own literal
acceptance example — `read bigfile.txt`, no mount-slot prefix, relying on
the same Day24-FOLLOWUP "bare filename defaults onto vfs_root" fallback
already proven for LMFS in tests/day38_test_create_shell.py's scenario 3
(LMFS being the last-registered mount / vfs_root, per Day 35).

Read-only — unlike tests/day38_test_create_shell.py this doesn't need a
fresh-image-copy/snapshot technique, since `read` never mutates the disk.

Asserts the Shell's output:
  1. Contains the readable phrase kmain.c wrote ("The quick brown fox
     jumps over the lazy dog.") — proving real content came back, not
     garbage.
  2. Reports "-- read 10240 bytes (whole file) --", matching cmd_read()'s
     own summary-line format (kernel/shell/cmd/read.c) and the exact size
     kmain.c's DAY40 section wrote.

Not part of the build; a manual verification script only.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from day24_test_create import run_commands_get_screen  # noqa: E402


def main():
    print("=== read bigfile.txt (bare, no mount-slot; LMFS is vfs_root) ===")
    screen = run_commands_get_screen(["read bigfile.txt"], "day40_read")
    print(screen)

    ok = True
    if "The quick brown fox jumps over the lazy dog." in screen:
        print("PASS: readable phrase content came back correctly\n")
    else:
        print("FAIL: expected the repeated phrase in the output\n",
              file=sys.stderr)
        ok = False

    if "read 10240 bytes (whole file)" in screen:
        print("PASS: cmd_read()'s summary line reports the correct "
              "byte count\n")
    else:
        print("FAIL: expected '-- read 10240 bytes (whole file) --'\n",
              file=sys.stderr)
        ok = False

    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()