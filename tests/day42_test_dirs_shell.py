#!/usr/bin/env python3
"""
tests/day42_test_dirs_shell.py — DAY42 interactive-Shell check.

DAY42: "多级目录与路径解析。" 验收："可以创建并访问多级目录。"

tests/day42_test_lmfs_mkdir.py proves the library layer (lmfs_mkdir/
lmfs_resolve_path) from kmain()'s boot self-test. This script drives
the REAL interactive Shell, the way a user would: mkdir, cd (into
subdirectories, up with "..", across volumes), ls (hiding "./" and
"../", taking a path argument), create/read/rm with relative and
multi-level paths, and the error messages for bad input.

The VGA text console is 80x25 with no scrollback and every command adds
3-4 lines, so the scenario is split into several QEMU boots of at most
~4 commands each. The disk image is copied fresh ONCE at the start and
then deliberately carried across boots, so each boot sees what the
previous one wrote to disk — which is also the strongest persistence
proof available (directories survive a full reboot). The cwd is
in-memory only, so every boot starts at "IDE3/ >" and uses paths
relative to the volume root.

Not part of the build; a manual verification script only.
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(__file__))
from day24_test_create import run_commands_get_screen  # noqa: E402

IMG = "lumaos-5.0.img"
_BASE = IMG + ".day42_test_base"

ok = True


def check(name, cond, screen=None):
    global ok
    print(("PASS  " if cond else "FAIL  ") + name)
    if not cond:
        ok = False
        if screen is not None:
            print("----- screen -----\n" + screen + "\n------------------",
                  file=sys.stderr)


def listing_has(screen, name, size=None, is_dir=False):
    """True if `ls` printed a row for `name` ("name/" for directories,
    "name<pad>N bytes" for files)."""
    for line in screen.splitlines():
        line = line.rstrip()
        if is_dir and line.strip() == name + "/":
            return True
        if (not is_dir) and line.startswith(name) and line.endswith(
                " bytes") and (size is None or
                               (" %d bytes" % size) in line):
            return True
    return False


def no_dot_rows(screen):
    return not any(l.strip() in ("./", "../") for l in screen.splitlines())


def main():
    if not os.path.exists(_BASE):
        shutil.copyfile(IMG, _BASE)
    shutil.copyfile(_BASE, IMG)

    # ---- boot 1: build a three-level tree, ls the root ----
    print("=== boot 1: mkdir docs / docs/notes / docs/notes/deep, ls ===")
    s = run_commands_get_screen(
        ["mkdir docs", "mkdir docs/notes", "mkdir docs/notes/deep", "ls"],
        "d42_b1")
    print(s)
    check("mkdir docs", "created directory IDE3/docs" in s, s)
    check("mkdir docs/notes (two levels)",
          "created directory IDE3/docs/notes" in s, s)
    check("mkdir docs/notes/deep (three levels)",
          "created directory IDE3/docs/notes/deep" in s, s)
    check("ls root shows docs/ as a directory row",
          listing_has(s, "docs", is_dir=True), s)
    check("ls hides './' and '../' rows", no_dot_rows(s), s)

    # ---- boot 2: directories persisted; cd in, create, ls, cd .. ----
    print("=== boot 2: cd docs/notes, create a.txt, ls, cd .. ===")
    s = run_commands_get_screen(
        ["cd docs/notes", "create a.txt", "ls", "cd .."], "d42_b2")
    print(s)
    check("cd docs/notes works after a reboot (persisted on disk)",
          " - Changed directory to IDE3/docs/notes/" in s, s)
    check("prompt shows the cwd", "IDE3/docs/notes/ >" in s, s)
    check("create a.txt lands in the cwd",
          "created IDE3/docs/notes/a.txt" in s, s)
    check("ls lists a.txt (0 bytes) in the subdirectory",
          listing_has(s, "a.txt", 0), s)
    check("ls lists the nested directory deep/",
          listing_has(s, "deep", is_dir=True), s)
    check("ls hides './' and '../' inside a subdirectory",
          no_dot_rows(s), s)
    check("cd .. goes up one level",
          " - Changed directory to IDE3/docs/" in s, s)
    check("prompt after cd .. is IDE3/docs/ >",
          s.rstrip().splitlines()[-1].strip().startswith("IDE3/docs/ >"), s)

    # ---- boot 3: read content written by the kernel self-test ----
    print("=== boot 3: read nested file by full path and by relative path ===")
    s = run_commands_get_screen(
        ["read kday42/sub/deep/leaf.txt", "cd kday42/sub",
         "read deep/leaf.txt", "cd ../.."], "d42_b3")
    print(s)
    check("read kday42/sub/deep/leaf.txt prints the content",
          s.count("Hello from a nested directory!") >= 2, s)
    check("read shows the resolved full path in its header",
          "IDE3/kday42/sub/deep/leaf.txt (31 bytes)" in s, s)
    check("cd kday42/sub then read deep/leaf.txt (relative path)",
          "IDE3/kday42/sub/deep/leaf.txt" in s and
          " - Changed directory to IDE3/kday42/sub/" in s, s)
    check("cd ../.. climbs two levels back to the volume root",
          s.rstrip().splitlines()[-1].strip().startswith("IDE3/ >"), s)

    # ---- boot 4: error handling ----
    print("=== boot 4: errors ===")
    s = run_commands_get_screen(
        ["mkdir nosuch/x", "cd docs/notes/a.txt", "cd nosuch", "mkdir docs"],
        "d42_b4")
    print(s)
    check("mkdir with a missing parent is refused",
          "mkdir: cannot create directory 'IDE3/nosuch/x'" in s, s)
    check("cd into a file is refused",
          "not a directory" in s, s)
    check("cd into a missing directory is refused",
          "cd: 'IDE3/nosuch': no such directory" in s, s)
    check("mkdir of an existing name is refused",
          "mkdir: cannot create directory 'IDE3/docs'" in s, s)
    check("a refused cd leaves the prompt at the volume root",
          s.rstrip().splitlines()[-1].strip().startswith("IDE3/ >"), s)

    # ---- boot 5: FAT refusal, cross-volume cd, rm with a relative path ----
    print("=== boot 5: FAT volume, cross-volume cd, rm in a subdirectory ===")
    s = run_commands_get_screen(
        ["cd IDE0", "mkdir x", "cd IDE3/docs/notes", "rm a.txt"], "d42_b5")
    print(s)
    check("cd IDE0 still switches volume (prompt IDE0/ >)",
          "IDE0/ >" in s, s)
    check("mkdir on a FAT volume is refused clearly",
          "has no subdirectory support" in s, s)
    check("cd IDE3/docs/notes switches volume AND directory at once",
          " - Changed directory to IDE3/docs/notes/" in s, s)
    check("rm a.txt (relative) removes it from the subdirectory",
          "removed IDE3/docs/notes/a.txt" in s, s)

    # ---- boot 6: ls with a path argument, rm refuses a directory ----
    print("=== boot 6: ls <path>, rm on a directory ===")
    s = run_commands_get_screen(
        ["ls docs/notes", "rm docs/notes/deep", "ls IDE3/docs"], "d42_b6")
    print(s)
    check("ls docs/notes: a.txt is gone, deep/ remains",
          (not listing_has(s, "a.txt", 0)) and
          listing_has(s, "deep", is_dir=True), s)
    check("rm of a directory is refused",
          "rm: cannot remove 'IDE3/docs/notes/deep'" in s, s)
    check("ls IDE3/docs (fully qualified) lists notes/",
          listing_has(s, "notes", is_dir=True), s)

    print("=" * 70)
    print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
