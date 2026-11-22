#!/usr/bin/env python3
"""
tests/day38_test_create_shell.py — DAY38 interactive-Shell check, the
LMFS counterpart to tests/day24_test_create.py.

DAY38: "实现 create() 与目录项。" 验收："create test.txt 成功。"

kmain.c's own DAY38 boot-time section (and tests/day38_test_lmfs_create.py,
which asserts on it) calls lmfs_create() directly — this script instead
drives the REAL interactive Shell, the same way the roadmap's own
acceptance text is phrased ("在 Shell 中输入 create test.txt"), against
the LMFS mount (IDE3) specifically. Mirrors day24_test_create.py's own
structure and isolation technique almost line for line — see that
file's header comment for why each scenario needs its own fresh copy
of the disk image: `create` genuinely writes to lumaos-5.0.img, and
that write persists across separate QEMU boots (this project has no
snapshot/rollback of its own), so without per-scenario isolation, a
later scenario could see an earlier scenario's leftover "test.txt" and
report a false duplicate.

  1. "create IDE3/test.txt" then "ls" — the roadmap's own literal
     example, pointed at the LMFS mount instead of a FAT one. Asserts
     the file appears in `ls` at "0 bytes" (proving the write is
     immediately visible through the same generic VFS readdir path
     `ls` already uses for every other filesystem).
  2. "create IDE3/test.txt" a SECOND time (same boot continuing on) —
     asserts the shell reports the duplicate-name rejection rather
     than silently creating a second entry or crashing.
  3. bare "create test2.txt" (no mount-slot) then "ls" — LMFS is the
     last-registered mount (vfs_root), so create.c's existing DAY24-
     FOLLOWUP bare-filename fallback (shell_resolve_path() onto
     vfs_root) should land this on LMFS too, exactly the same as it
     already does for FAT — this scenario is really checking that
     Day 38 didn't have to special-case that fallback logic at all.

Not part of the build; a manual verification script only.
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(__file__))
from day24_test_create import run_commands_get_screen  # noqa: E402

IMG = "lumaos-5.0.img"
_BASE_IMG_SNAPSHOT = IMG + ".day38_test_base"


def _fresh_image_copy():
    if not os.path.exists(_BASE_IMG_SNAPSHOT):
        shutil.copyfile(IMG, _BASE_IMG_SNAPSHOT)
    shutil.copyfile(_BASE_IMG_SNAPSHOT, IMG)


def main():
    all_ok = True

    _fresh_image_copy()
    print("=== (1) create IDE3/test.txt + ls (LMFS mount) ===")
    screen = run_commands_get_screen(
        ["create IDE3/test.txt", "ls"], "day38_create")
    print(screen)
    if ("created IDE3/test.txt" in screen
            and "test.txt" in screen.lower()
            and "0 bytes" in screen):
        print("PASS: created IDE3/test.txt and it shows up in 'ls' "
              "at 0 bytes\n")
    else:
        print("FAIL: expected 'created IDE3/test.txt' and a 0-byte "
              "'ls' listing\n", file=sys.stderr)
        all_ok = False

    _fresh_image_copy()
    print("=== (2) create IDE3/test.txt twice — duplicate must be "
          "rejected ===")
    screen = run_commands_get_screen(
        ["create IDE3/test.txt", "create IDE3/test.txt"], "day38_dup")
    print(screen)
    if (screen.count("created IDE3/test.txt") == 1
            and "cannot create" in screen):
        print("PASS: second create of the same path was rejected, "
              "not silently duplicated\n")
    else:
        print("FAIL: expected exactly one 'created' line and one "
              "'cannot create' diagnostic\n", file=sys.stderr)
        all_ok = False

    _fresh_image_copy()
    print("=== (3) bare 'create test2.txt' (no mount-slot, LMFS is "
          "vfs_root) + ls ===")
    screen = run_commands_get_screen(
        ["create test2.txt", "ls"], "day38_bare")
    print(screen)
    if ("created" in screen and "test2.txt" in screen.lower()
            and "0 bytes" in screen):
        print("PASS: bare 'create test2.txt' resolved onto LMFS "
              "(vfs_root) and is visible in 'ls'\n")
    else:
        print("FAIL: expected 'created ...test2.txt' and a 0-byte "
              "'ls' listing\n", file=sys.stderr)
        all_ok = False

    if not all_ok:
        sys.exit(1)


if __name__ == "__main__":
    main()