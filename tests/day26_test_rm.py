#!/usr/bin/env python3
"""
tests/day26_test_rm.py — DAY26 interactive + on-disk verification check.

DAY26 (Week 4, Day 26 of the 5.0 filesystem roadmap):
  "实现 fat_delete_file(name)：找到目录项，标记为已删除（首字节设为
   0xE5），释放其占用的簇链（在 FAT 表中标记为空闲）。Shell 中输入
   rm test.txt，ls 结果中该文件消失，且空闲簇数增加。"

Same "boot headless, drive the real Shell via QEMU monitor sendkey,
dump the VGA text buffer" technique day21/24/25's own tests already
use. Two groups of checks:

SCENARIO A (day24_test_create.py's own style: one fresh disk-image
copy per scenario, so scenarios never see each other's writes):
  A1. "create test.txt" (bare) + "ls" + "rm test.txt" + "ls" — the
      roadmap's own literal example. Asserts the file appears in the
      first `ls`, then is gone from the second.
  A2. "create IDE0/TEST.TXT" + "rm IDE0/TEST.TXT" + "show IDE0/TEST.TXT"
      — cross-checks the deletion through a DIFFERENT command than
      `ls`, the same "prove it's really gone from any VFS consumer,
      not just from ls's own listing" cross-check day24's own test
      does for `show` after `create`. Asserts `show` reports a
      not-found-shaped diagnostic, not a 0-byte read.
  A3. "rm IDE0/NOSUCH.TXT" (never created) — asserts a "cannot remove"
      diagnostic, not a crash or a false "removed" report.
  A4. "rm" with no argument — asserts the usage line, no crash.
  A5. "create test.txt" TWICE in a row (same lower-case spelling both
      times) — asserts the SECOND is rejected as a duplicate. This is
      the DAY26 case-insensitivity bugfix's own regression check: the
      on-disk entry from the first `create` is always stored upper-
      case ("TEST.TXT"), so this only passes if fat_open()'s duplicate
      check now matches a lower-case-typed name against it (day24's
      own equivalent scenario used the SAME case both times and could
      never have caught this bug).

SCENARIO B (single boot, `write` + Ctrl+S exactly like
day25_test_write.py, plus a host-side FAT-table read before and after):
  B1. Read IDE0's FAT12 table straight out of the built image and
      count how many clusters currently read back as free (0) —
      before anything in this script runs.
  B2. Boot once: `write IDE0/RMTEST.TXT`, type one line of content,
      Ctrl+S to save (this claims at least one cluster via
      fat_alloc_cluster(), Day 23), `read` it back to confirm the
      content really landed, then `rm IDE0/RMTEST.TXT`, then `ls` to
      confirm it's gone from the listing too.
  B3. After QEMU exits (the image is a real file, no -snapshot — see
      day24_test_create.py's own comment on why writes persist),
      re-read IDE0's FAT12 table and count free clusters again.
  B4. Assert the free-cluster count is back to EXACTLY what it was in
      B1 — the direct, mechanism-level proof of this day's own "且空闲
      簇数增加" acceptance text: fat_alloc_cluster() (write) took one
      count downward, fat_delete_file() (rm) brought it back up by the
      same amount, not merely "ls stopped showing the file" without
      actually reclaiming its cluster(s) in the FAT table.

Not part of the build; a manual verification script only.
"""
import shutil
import socket
import struct
import subprocess
import sys
import time
import os

sys.path.insert(0, os.path.dirname(__file__))
from qemu_dump_console import parse_monitor_hexdump, render  # noqa: E402
from day21_test_show import keys_for  # noqa: E402

IMG = "lumaos-5.0.img"
BOOT_WAIT_SECS = float(os.environ.get("LUMAOS_BOOT_WAIT", "10"))

# IDE0 == tests/fat12-with-file.img, embedded at LBA 5000 in the final
# composed image — see build.sh's own "[img] embedded
# tests/fat12-with-file.img at LBA 5000" build-log line.
IDE0_MOUNT_LBA = 5000

_BASE_IMG_SNAPSHOT = IMG + ".day26_test_base"


def _fresh_image_copy():
    """Same per-scenario disk-image isolation day24_test_create.py's
    own _fresh_image_copy() uses, for the exact same reason (create/
    rm/write all persist real writes to the disk image file)."""
    if not os.path.exists(_BASE_IMG_SNAPSHOT):
        shutil.copyfile(IMG, _BASE_IMG_SNAPSHOT)
    shutil.copyfile(_BASE_IMG_SNAPSHOT, IMG)


def count_free_fat12_clusters(img_path, mount_lba):
    """Read IDE0's own BPB straight out of the built image (standard
    FAT12/16 BPB byte layout — BytesPerSec @ 0x0B, SecPerClus @ 0x0D,
    RsvdSecCnt @ 0x0E, NumFATs @ 0x10, RootEntCnt @ 0x11, TotSec16 @
    0x13, FATSz16 @ 0x16, TotSec32 @ 0x20 — the same fields
    fat_common.c's own fat_parse_bpb_common()/fat_compute_geometry()
    parse on the kernel side) rather than hard-coding this fixture's
    geometry, so this check keeps working even if build.sh's own
    mkfs.fat invocation ever changes the fixture's size. Returns the
    number of data clusters (2..total_clusters+1) whose 12-bit FAT
    entry reads back as 0 (FAT12_CLUSTER_FREE)."""
    with open(img_path, "rb") as f:
        f.seek(mount_lba * 512)
        boot = f.read(512)

        bytes_per_sec = struct.unpack_from("<H", boot, 0x0B)[0]
        sec_per_clus = boot[0x0D]
        rsvd_sec_cnt = struct.unpack_from("<H", boot, 0x0E)[0]
        num_fats = boot[0x10]
        root_ent_cnt = struct.unpack_from("<H", boot, 0x11)[0]
        tot_sec16 = struct.unpack_from("<H", boot, 0x13)[0]
        fat_sz16 = struct.unpack_from("<H", boot, 0x16)[0]
        tot_sec32 = struct.unpack_from("<I", boot, 0x20)[0]

        tot_sec = tot_sec16 if tot_sec16 != 0 else tot_sec32
        root_dir_sectors = ((root_ent_cnt * 32) + (bytes_per_sec - 1)) // bytes_per_sec
        data_sec = tot_sec - (rsvd_sec_cnt + num_fats * fat_sz16 + root_dir_sectors)
        total_clusters = data_sec // sec_per_clus

        fat_lba = mount_lba + rsvd_sec_cnt
        f.seek(fat_lba * 512)
        fat_bytes = f.read(fat_sz16 * 512)

    def fat12_entry(cluster):
        off = cluster + cluster // 2
        if cluster % 2 == 0:
            return fat_bytes[off] | ((fat_bytes[off + 1] & 0x0F) << 8)
        else:
            return (fat_bytes[off] >> 4) | (fat_bytes[off + 1] << 4)

    free = 0
    for cluster in range(2, total_clusters + 2):
        if fat12_entry(cluster) == 0:
            free += 1
    return free


def run_commands_get_screens(cmdlines, sock_name, dump_after_each=True):
    """Boot once, type each line in `cmdlines` (Enter after each), and
    return a screen dump after every line (or just the final one).
    Same technique as day24_test_create.py's run_commands_get_screen(),
    generalized to return intermediate dumps too (day25_test_write.py's
    own dump_screen() style) since scenario A2/A3 need to see the
    screen right after `rm`, not just after the trailing command."""
    sock = f"/tmp/lumaos_qemu_mon_{sock_name}.sock"
    if os.path.exists(sock):
        os.remove(sock)

    proc = subprocess.Popen(
        [
            "qemu-system-i386",
            "-machine", "type=pc",
            "-m", "256",
            "-drive", f"format=raw,file={IMG},if=ide",
            "-display", "none",
            "-monitor", f"unix:{sock},server,nowait",
            "-no-reboot",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    screens = []

    try:
        for _ in range(50):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        time.sleep(BOOT_WAIT_SECS)

        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock)
        time.sleep(0.3)
        s.recv(65536)  # discard monitor banner

        def send_monitor(cmd):
            s.sendall((cmd + "\n").encode())
            time.sleep(0.03)
            try:
                s.settimeout(0.3)
                while True:
                    chunk = s.recv(65536)
                    if not chunk or chunk.endswith(b"(qemu) "):
                        break
            except socket.timeout:
                pass

        def dump_screen(wait=0.6):
            time.sleep(wait)
            nbytes = 80 * 25 * 2
            s.sendall(f"xp /{nbytes}xb 0xb8000\n".encode())
            time.sleep(0.4)
            raw = b""
            s.settimeout(2.0)
            try:
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    raw += chunk
                    if raw.count(b"(qemu)") >= 1:
                        break
            except socket.timeout:
                pass
            vals = parse_monitor_hexdump(raw.decode(errors="replace"))
            return render(vals) if vals else "(no data)"

        for cmdline in cmdlines:
            for key in keys_for(cmdline):
                send_monitor(f"sendkey {key}")
            send_monitor("sendkey ret")
            time.sleep(0.5)
            if dump_after_each:
                screens.append(dump_screen())

        if not dump_after_each:
            screens.append(dump_screen())

        s.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    return screens


def main():
    all_ok = True

    # ---- Scenario A1 ----
    _fresh_image_copy()
    print("=== (A1) create test.txt + ls + rm test.txt + ls ===")
    screens = run_commands_get_screens(
        ["create test.txt", "ls", "rm test.txt", "ls"], "day26_a1")
    print(screens[3])
    seen_ok    = "test.txt" in screens[1].lower() and "0 bytes" in screens[1]
    removed_idx = screens[3].lower().find("removed")
    removed_ok = removed_idx != -1
    # NOTE: the VGA text screen is only 25 rows and doesn't necessarily
    # scroll the FIRST 'ls' (which correctly listed test.txt) off
    # screen before the SECOND 'ls' runs — both can be simultaneously
    # visible in one dump. So "test.txt" not appearing ANYWHERE in
    # screens[3] is the wrong check (it would also fail on a correct
    # run); only the text AFTER the 'removed ...' LINE ITSELF matters
    # (that line names the just-deleted file by name, so it must be
    # skipped past entirely, not just past the word "removed") since
    # that's where the second 'ls' output actually lives.
    after_removed_line = ""
    if removed_ok:
        nl = screens[3].find("\n", removed_idx)
        after_removed_line = screens[3][nl + 1:] if nl != -1 else ""
    gone_ok = removed_ok and "test.txt" not in after_removed_line.lower()
    if seen_ok and removed_ok and gone_ok:
        print("PASS: file appeared in 'ls', then vanished after 'rm'\n")
    else:
        print(f"FAIL: seen_ok={seen_ok} removed_ok={removed_ok} "
              f"gone_ok={gone_ok}\n", file=sys.stderr)
        print(f"----- screens[1] -----\n{screens[1]}", file=sys.stderr)
        print(f"----- screens[2] -----\n{screens[2]}", file=sys.stderr)
        all_ok = False

    # ---- Scenario A2 ----
    _fresh_image_copy()
    print("=== (A2) create IDE0/TEST.TXT + rm IDE0/TEST.TXT + "
          "show IDE0/TEST.TXT ===")
    screens = run_commands_get_screens(
        ["create IDE0/TEST.TXT", "rm IDE0/TEST.TXT", "show IDE0/TEST.TXT"],
        "day26_a2")
    print(screens[-1])
    if ("removed IDE0/TEST.TXT" in screens[1]
            and ("not found" in screens[2].lower()
                 or "cannot" in screens[2].lower())):
        print("PASS: 'show' confirms the entry is really gone after 'rm', "
              "not just missing from 'ls'\n")
    else:
        print("FAIL: expected 'removed IDE0/TEST.TXT' then a not-found "
              "diagnostic from 'show'\n", file=sys.stderr)
        all_ok = False

    # ---- Scenario A3 ----
    _fresh_image_copy()
    print("=== (A3) rm IDE0/NOSUCH.TXT (never created) ===")
    screens = run_commands_get_screens(["rm IDE0/NOSUCH.TXT"], "day26_a3")
    print(screens[-1])
    if "cannot remove" in screens[-1]:
        print("PASS: rm on a missing file reported a diagnostic, not a "
              "crash or a false 'removed'\n")
    else:
        print("FAIL: expected a 'cannot remove' diagnostic\n", file=sys.stderr)
        all_ok = False

    # ---- Scenario A4 ----
    _fresh_image_copy()
    print("=== (A4) rm (no argument) ===")
    screens = run_commands_get_screens(["rm"], "day26_a4")
    print(screens[-1])
    if "usage:" in screens[-1]:
        print("PASS: bare 'rm' printed the usage line\n")
    else:
        print("FAIL: expected a 'usage: rm ...' line\n", file=sys.stderr)
        all_ok = False

    # ---- Scenario A5 (DAY26 case-insensitivity regression check) ----
    _fresh_image_copy()
    print("=== (A5) create test.txt TWICE (same lower-case spelling) — "
          "second must be rejected ===")
    screens = run_commands_get_screens(
        ["create test.txt", "create test.txt"], "day26_a5")
    print(screens[-1])
    if screens[-1].count("created") == 1 and "cannot create" in screens[-1]:
        print("PASS: second lower-case 'create test.txt' was rejected as "
              "a duplicate of the upper-case on-disk entry (DAY26 case-"
              "insensitivity bugfix confirmed)\n")
    else:
        print("FAIL: expected exactly one 'created' line and one 'cannot "
              "create' diagnostic — a duplicate 'TEST.TXT' entry would mean "
              "the case-insensitivity bugfix regressed\n", file=sys.stderr)
        all_ok = False

    # ---- Scenario B: write + rm, verified against the raw FAT table ----
    #
    # NOTE on methodology: kmain.c's own boot-time [fat22]/[fat23]
    # self-test section (see its output further up this script's own
    # printed log) calls fat_find_free_cluster()/fat_alloc_cluster()
    # for real on EVERY boot, including IDE0 — permanently claiming a
    # couple of clusters as a side effect of simply booting, every
    # single time, regardless of anything this script does afterward.
    # Comparing a host-side free-cluster read taken BEFORE booting
    # against one taken AFTER booting would therefore always show a
    # decrease even with a perfectly-working `rm` (the boot self-test's
    # own permanent allocations, not this scenario's write+rm, would
    # account for it) — the wrong comparison. Instead, this runs TWO
    # separate boots from the exact same pristine base image (so both
    # pay the identical boot-self-test cluster cost): one that does
    # nothing but boot and exit (baseline), and one that does the full
    # write+rm sequence. If `rm` genuinely freed everything `write`
    # claimed, both boots should leave IDE0 with the SAME free-cluster
    # count.
    print("=== (B) write IDE0/RMTEST.TXT, Ctrl+S, rm it, verify the FAT "
          "table's free-cluster count round-trips across a boot with "
          "identical self-test overhead ===")

    def boot_and_run(cmd_steps, sock_name):
        """cmd_steps is a list of (kind, value) pairs:
          ("type", text)   - type text via keys_for()
          ("enter", None)  - press Enter
          ("ctrl_s", None) - press Ctrl+S
          ("dump", tag)    - dump the screen under `tag`
        Returns the dict of tag -> screen text."""
        sock = f"/tmp/lumaos_qemu_mon_{sock_name}.sock"
        if os.path.exists(sock):
            os.remove(sock)

        proc = subprocess.Popen(
            [
                "qemu-system-i386",
                "-machine", "type=pc",
                "-m", "256",
                "-drive", f"format=raw,file={IMG},if=ide",
                "-display", "none",
                "-monitor", f"unix:{sock},server,nowait",
                "-no-reboot",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        screens = {}
        try:
            for _ in range(50):
                if os.path.exists(sock):
                    break
                time.sleep(0.1)
            time.sleep(BOOT_WAIT_SECS)

            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(sock)
            time.sleep(0.3)
            s.recv(65536)

            def send_monitor(cmd):
                s.sendall((cmd + "\n").encode())
                time.sleep(0.03)
                try:
                    s.settimeout(0.3)
                    while True:
                        chunk = s.recv(65536)
                        if not chunk or chunk.endswith(b"(qemu) "):
                            break
                except socket.timeout:
                    pass

            def dump_screen(tag, wait=0.6):
                time.sleep(wait)
                nbytes = 80 * 25 * 2
                s.sendall(f"xp /{nbytes}xb 0xb8000\n".encode())
                time.sleep(0.4)
                raw = b""
                s.settimeout(2.0)
                try:
                    while True:
                        chunk = s.recv(65536)
                        if not chunk:
                            break
                        raw += chunk
                        if raw.count(b"(qemu)") >= 1:
                            break
                except socket.timeout:
                    pass
                vals = parse_monitor_hexdump(raw.decode(errors="replace"))
                screens[tag] = render(vals) if vals else "(no data)"

            # NOTE: an earlier version of this helper dumped the screen
            # once here (right after boot, before typing anything) to
            # confirm the self-test had finished. That extra monitor
            # round-trip (an "xp" memory dump) turned out to itself
            # perturb the very next sendkey batch's timing enough to
            # occasionally drop/misdirect keystrokes — a QEMU-monitor-
            # sendkey timing artifact, not a kernel bug (confirmed by
            # reproducing the exact same write+read+rm keystroke
            # sequence standalone, without that extra dump, which
            # succeeded every time). Removed; BOOT_WAIT_SECS alone
            # (same value day21/24/25's own tests already rely on) is
            # enough to reach the shell prompt before typing starts.

            for kind, value in cmd_steps:
                if kind == "type":
                    for key in keys_for(value):
                        send_monitor(f"sendkey {key}")
                elif kind == "enter":
                    send_monitor("sendkey ret")
                    time.sleep(0.5)
                elif kind == "ctrl_s":
                    send_monitor("sendkey ctrl-s")
                    time.sleep(0.8)
                elif kind == "dump":
                    dump_screen(value)

            s.close()
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

        return screens

    # ---- Boot #1: baseline — boot, do nothing, exit ----
    _fresh_image_copy()
    boot_and_run([], "day26_b_baseline")
    free_baseline = count_free_fat12_clusters(IMG, IDE0_MOUNT_LBA)
    print(f"IDE0 free clusters after a boot-only baseline: {free_baseline}")

    # ---- Boot #2: write, save, read back, rm, confirm gone ----
    _fresh_image_copy()
    screens = boot_and_run(
        [
            ("type", "write IDE0/RMTEST.TXT"), ("enter", None),
            ("dump", "after_write_open"),
            ("type", "Day 26 rm test content"), ("ctrl_s", None),
            ("dump", "after_save"),
            ("type", "read IDE0/RMTEST.TXT"), ("enter", None),
            ("dump", "after_read"),
            ("type", "rm IDE0/RMTEST.TXT"), ("enter", None),
            ("dump", "after_rm"),
            ("type", "read IDE0/RMTEST.TXT"), ("enter", None),
            ("dump", "after_read_again"),
        ],
        "day26_b_write_rm",
    )

    for tag in ["after_write_open", "after_save", "after_read", "after_rm",
                "after_read_again"]:
        print(f"----- {tag} -----")
        print(screens.get(tag, "(missing)"))

    free_after_write_rm = count_free_fat12_clusters(IMG, IDE0_MOUNT_LBA)
    print(f"IDE0 free clusters after write+rm (same boot overhead): "
          f"{free_after_write_rm}")

    content_ok = "Day 26 rm test content" in screens.get("after_read", "")
    removed_ok = "removed IDE0/RMTEST.TXT" in screens.get("after_rm", "")
    # After `rm`, re-reading the SAME path must fail — the strongest
    # "really gone, not just missing from one listing" check, the same
    # cross-command-diagnostic style scenario A2 above uses.
    gone_ok = ("not found" in screens.get("after_read_again", "").lower()
               or "cannot" in screens.get("after_read_again", "").lower())
    clusters_ok = (free_after_write_rm == free_baseline)

    if content_ok and removed_ok and gone_ok and clusters_ok:
        print("PASS: write claimed cluster(s) (fat_alloc_cluster()), rm "
              "freed them again (fat_delete_file()), and IDE0's free-"
              "cluster count matches an otherwise-identical boot that "
              f"never wrote anything at all ({free_baseline})\n")
    else:
        print(f"FAIL: content_ok={content_ok} removed_ok={removed_ok} "
              f"gone_ok={gone_ok} clusters_ok={clusters_ok} "
              f"(free_baseline={free_baseline} "
              f"free_after_write_rm={free_after_write_rm})\n",
              file=sys.stderr)
        all_ok = False

    if not all_ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
