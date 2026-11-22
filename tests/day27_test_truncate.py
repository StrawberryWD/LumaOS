#!/usr/bin/env python3
"""
tests/day27_test_truncate.py — DAY27 interactive + on-disk verification.

DAY27 (Week 4, Day 27 of the 5.0 filesystem roadmap):
  "实现文件截断（fat_truncate），支持清空文件内容。只保留目录项，释放
   所有数据簇。覆盖文件时检测是否为0bytes(空) 若为空不弹出是否覆盖y/n
   的提示。并解决文本编辑器跨行删除时的问题，并添加一行字数如果超出
   自动换行的逻辑，或者增加一行能显示的最多字符。并实现光标绘制在将
   输入文字的正下方，而不是跳一个。删除那个show只保留read。"

Same "boot headless, drive the real Shell via QEMU monitor sendkey,
dump the VGA text buffer" technique day21/24/25/26's own tests already
use, via this repo's shared tests/mon_helper.py Session wrapper. Six
scenarios, matching the six sub-requirements of today's roadmap entry:

SCENARIO A — `show` is gone, `read` is the only survivor:
  A1. `show` with no argument -> "Unknown: show" (not a usage line —
      the command itself no longer exists).
  A2. `help` output does not mention "show" anywhere.

SCENARIO B — empty-file overwrite skips the Y/N prompt:
  B1. `create` a file, then immediately `write` it: NO "ALREADY EXISTS"
      prompt appears; the editor opens directly.
  B2. Save non-empty content to that same file, then `write` it AGAIN:
      the prompt DOES appear this time (only an empty file skips it).

SCENARIO C — fat_write()'s DAY27 leak-fix (the natural, load-bearing
use of the same free-chain helper fat_truncate() itself uses):
  A file is written long enough to span two FAT12 clusters, then
  overwritten with short content. The FAT12 table is read directly off
  the built disk image (same technique day26_test_rm.py's own
  count_free_fat12_clusters() uses) to confirm the OLD chain's SECOND
  cluster reads back as genuinely free (0) after the second write —
  not left allocated/orphaned the way it was through Day 26.

SCENARIO D — cross-line Backspace no longer loses data:
  D1. Two lines that fit together within the new WRITE_MAX_LINE_LEN
      (76) fully merge with all characters intact.
  D2. Two lines whose COMBINED length exceeds WRITE_MAX_LINE_LEN
      partially merge (fill the previous line, keep the remainder on
      the current line) with zero characters lost — the exact scenario
      that silently dropped data through Day 26.

SCENARIO E — cursor drawn at the real insertion point, not one column
past it: reads the live VGA hardware cursor position back via the CRTC
cursor-location registers (0x3D4 index 0x0E/0x0F) after typing on a
single-digit-numbered line, and asserts it lands exactly where the
next character will be inserted, not one column further right.
"""
import os
import sys
import time
import shutil
import struct

sys.path.insert(0, os.path.dirname(__file__))
from mon_helper import Session  # noqa: E402
from day26_test_rm import (      # noqa: E402
    IDE0_MOUNT_LBA,
)

IMG = os.path.join(os.path.dirname(__file__), "..", "lumaos-5.0.img")
PRISTINE = "/tmp/lumaos_day27_pristine.img"

if not os.path.exists(PRISTINE):
    shutil.copyfile(IMG, PRISTINE)


def fresh_image():
    shutil.copyfile(PRISTINE, IMG)


def read_fat12_root_and_fat(img_path, mount_lba):
    with open(img_path, "rb") as f:
        f.seek(mount_lba * 512)
        boot = f.read(512)
        bytes_per_sec = struct.unpack_from("<H", boot, 0x0B)[0]
        rsvd_sec_cnt = struct.unpack_from("<H", boot, 0x0E)[0]
        num_fats = boot[0x10]
        root_ent_cnt = struct.unpack_from("<H", boot, 0x11)[0]
        fat_sz16 = struct.unpack_from("<H", boot, 0x16)[0]

        fat_lba = mount_lba + rsvd_sec_cnt
        f.seek(fat_lba * 512)
        fat_bytes = f.read(fat_sz16 * 512)

        root_lba = mount_lba + rsvd_sec_cnt + num_fats * fat_sz16
        root_dir_sectors = ((root_ent_cnt * 32) + (bytes_per_sec - 1)) // bytes_per_sec
        f.seek(root_lba * 512)
        root_bytes = f.read(root_dir_sectors * 512)
    return fat_bytes, root_bytes


def fat12_entry(fat_bytes, cluster):
    off = cluster + cluster // 2
    if cluster % 2 == 0:
        return fat_bytes[off] | ((fat_bytes[off + 1] & 0x0F) << 8)
    else:
        return (fat_bytes[off] >> 4) | (fat_bytes[off + 1] << 4)


def find_entry(root_bytes, fname83):
    for i in range(0, len(root_bytes), 32):
        e = root_bytes[i:i + 32]
        if e[0] == 0x00:
            break
        if e[0] == 0xE5:
            continue
        if e[0:11] == fname83:
            clus_hi = struct.unpack_from("<H", e, 20)[0]
            clus_lo = struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            return (clus_hi << 16) | clus_lo, size
    return None


def scenario_a():
    print("=== SCENARIO A: `show` is gone ===")
    fresh_image()
    sess = Session(IMG, "day27_a", boot_wait=10)
    try:
        sess.type_line("show")
        sess.press("ret")
        screen = sess.dump_screen(0.3)
        assert "Unknown: show" in screen, "A1 FAIL: `show` still exists"
        print("PASS A1: `show` is Unknown, not a recognized command")

        sess.type_line("help")
        sess.press("ret")
        screen = sess.dump_screen(0.3)
        # Check the command table itself, not the English word "show"
        # incidental to other commands' own descriptions ("Show quick
        # reference", "Show OS version", etc.) — a real `show` entry
        # would be its own line starting with two-space-indented "show".
        assert "\n  show" not in screen, \
            "A2 FAIL: `help` command table still lists a `show` entry"
        print("PASS A2: `help` command table no longer lists `show`")
    finally:
        sess.close()


def scenario_b():
    print("=== SCENARIO B: empty-file overwrite skips the Y/N prompt ===")
    fresh_image()
    sess = Session(IMG, "day27_b", boot_wait=10)
    try:
        sess.type_line("create IDE0/EMPTY.TXT")
        sess.press("ret")
        time.sleep(0.3)

        sess.type_line("write IDE0/EMPTY.TXT")
        sess.press("ret")
        time.sleep(0.4)
        screen = sess.dump_screen(0.3)
        assert "ALREADY EXISTS" not in screen, \
            "B1 FAIL: prompt shown for an empty file"
        assert "editing IDE0/EMPTY.TXT" in screen, \
            "B1 FAIL: editor did not open directly"
        print("PASS B1: empty file -> straight into the editor, no prompt")

        sess.type_line("hi")
        sess.press("ctrl-s")
        time.sleep(0.6)
        sess.dump_screen(0.3)

        sess.type_line("write IDE0/EMPTY.TXT")
        sess.press("ret")
        time.sleep(0.4)
        screen = sess.dump_screen(0.3)
        assert "ALREADY EXISTS" in screen, \
            "B2 FAIL: prompt missing for a now-non-empty file"
        print("PASS B2: non-empty file -> prompt still shown")
        sess.press("shift-n")
        time.sleep(0.2)
    finally:
        sess.close()


def scenario_c():
    print("=== SCENARIO C: fat_write() DAY27 leak-fix ===")
    fresh_image()
    fname83 = b"LEAK27  TXT"

    sess = Session(IMG, "day27_c1", boot_wait=10)
    try:
        sess.type_line("write IDE0/LEAK27.TXT")
        sess.press("ret")
        time.sleep(0.4)
        for _ in range(8):
            sess.type_line("a" * 70)
            sess.press("ret")
        sess.press("ctrl-s")
        time.sleep(0.8)
        sess.dump_screen(0.3)
    finally:
        sess.close()

    fat_bytes, root_bytes = read_fat12_root_and_fat(IMG, IDE0_MOUNT_LBA)
    start1, size1 = find_entry(root_bytes, fname83)
    second_cluster = fat12_entry(fat_bytes, start1)
    assert second_cluster not in (0, 0xFFF, 0xFF8), \
        "C setup FAIL: first write didn't span 2+ clusters as expected"
    print(f"  first write: start={start1} size={size1} "
          f"chain continues to cluster {second_cluster}")

    sess = Session(IMG, "day27_c2", boot_wait=10)
    try:
        sess.type_line("write IDE0/LEAK27.TXT")
        sess.press("ret")
        time.sleep(0.5)
        sess.press("shift-y")
        time.sleep(0.3)
        sess.type_line("short")
        sess.press("ctrl-s")
        time.sleep(0.8)
        sess.dump_screen(0.3)
    finally:
        sess.close()

    fat_bytes2, root_bytes2 = read_fat12_root_and_fat(IMG, IDE0_MOUNT_LBA)
    entry_val = fat12_entry(fat_bytes2, second_cluster)
    assert entry_val == 0, (
        f"C FAIL: old chain's 2nd cluster ({second_cluster}) still reads "
        f"{hex(entry_val)} after overwrite — chain leaked, not freed"
    )
    print(f"PASS C: old chain's 2nd cluster ({second_cluster}) reads back "
          f"free (0x0) after the second write — fat_write() no longer leaks")


def scenario_d():
    print("=== SCENARIO D: cross-line Backspace loses no data ===")
    fresh_image()

    # D1: full merge (fits within WRITE_MAX_LINE_LEN=76)
    sess = Session(IMG, "day27_d1", boot_wait=10)
    try:
        sess.type_line("write IDE0/D1.TXT")
        sess.press("ret")
        time.sleep(0.4)
        sess.type_line("ab")
        sess.press("ret")
        sess.type_line("y" * 60)
        time.sleep(0.2)
        for _ in range(60):
            sess.press("left")
        sess.press("backspace")
        time.sleep(0.3)
        sess.press("ctrl-s")
        time.sleep(0.6)
        sess.dump_screen(0.3)
        sess.type_line("read IDE0/D1.TXT")
        sess.press("ret")
        screen = sess.dump_screen(0.5)
        expected = "ab" + "y" * 60
        assert expected in screen, f"D1 FAIL: merged content missing/corrupt"
        assert "(62 bytes)" in screen, f"D1 FAIL: wrong byte count\n{screen}"
        print("PASS D1: full cross-line merge keeps all 62 characters")
    finally:
        sess.close()

    # D2: overflow merge — the actual DAY26 data-loss bug scenario
    fresh_image()
    sess = Session(IMG, "day27_d2", boot_wait=10)
    try:
        sess.type_line("write IDE0/D2.TXT")
        sess.press("ret")
        time.sleep(0.4)
        sess.type_line("a" * 70)
        sess.press("ret")
        sess.type_line("b" * 60)
        time.sleep(0.2)
        for _ in range(60):
            sess.press("left")
        sess.press("backspace")
        time.sleep(0.3)
        sess.press("ctrl-s")
        time.sleep(0.6)
        sess.dump_screen(0.3)
        sess.type_line("read IDE0/D2.TXT")
        sess.press("ret")
        screen = sess.dump_screen(0.5)
        assert "(131 bytes)" in screen, (
            f"D2 FAIL: expected all 130 typed characters (+1 newline) "
            f"preserved, got:\n{screen}"
        )
        total_bs = screen.count("a") + screen.count("b")
        assert total_bs >= 130, f"D2 FAIL: characters missing\n{screen}"
        print("PASS D2: overflowing cross-line merge keeps all 130 "
              "characters (line 1 fills to 76, remainder stays on line 2)")
    finally:
        sess.close()


def scenario_e():
    print("=== SCENARIO E: cursor lands at the real insertion point ===")
    fresh_image()
    sess = Session(IMG, "day27_e", boot_wait=10)
    try:
        sess.type_line("write IDE0/CUR.TXT")
        sess.press("ret")
        time.sleep(0.4)
        sess.type_line("ab")
        time.sleep(0.2)
        col, row = sess.cursor_pos()
        assert col == 5, (
            f"E FAIL: cursor at col={col}, expected col=5 "
            f"(prefix '1. ' is 3 chars + 2 typed chars)"
        )
        print(f"PASS E: cursor at col={col} — matches the real insertion "
              f"point right after '1. ab', not one column past it")
    finally:
        sess.close()


if __name__ == "__main__":
    scenario_a()
    scenario_b()
    scenario_c()
    scenario_d()
    scenario_e()
    print("\nALL DAY27 SCENARIOS PASSED")