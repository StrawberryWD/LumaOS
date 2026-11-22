#!/usr/bin/env python3
"""
tests/day24_test_create.py — one-off DAY24 interactive check.

DAY24 (Week 4, Day 24 of the 5.0 filesystem roadmap):
  "实现 fat_create_file(name)：在根目录区找一个空条目（首字节 0x00 或
   0xE5），填入文件名、起始簇 0、大小 0。在 Shell 中输入 create test.txt，
   重新 ls 能看到它。"

Boots lumaos-5.0.img headless (same approach as tests/day21_test_show.py),
then exercises the new `create` command from the real running shell, via
several separate QEMU boots (one command sequence per boot, same "type a
command, dump the screen" technique day21_test_show.py already uses —
extended here to send TWO commands per boot where the check needs a
`create` immediately followed by `ls`, since `ls` only ever lists
`vfs_root`, the last-mounted volume — see kmain.c's own DAY24 comment
for why the kmain.c boot-time [fat24] self-test section is what
independently verifies all three FAT12/16/32 volumes, not just
vfs_root's — this script is the human-legible, Shell-invoked companion
to that section, the same relationship day21_test_show.py already has
to kmain.c's own [fat21] section):

  1. "create test.txt" then "ls" — the roadmap's own literal example
     (no mount-slot given). Asserts the file appears in `ls` with
     "0 bytes" (proving create.c's DAY24-FOLLOWUP bare-filename
     fallback resolved onto vfs_root, and the write is immediately
     visible through the ordinary VFS readdir path `ls` already uses).
  2. "create IDE0/TEST.TXT" then "show IDE0/TEST.TXT" — an explicit
     mount-slot path, cross-checked through the DIFFERENT existing
     `show` command instead of `ls`, proving the new entry is a
     genuinely well-formed directory entry any VFS consumer can open,
     not just something `ls` happens to render. Asserts `show` reports
     a 0-byte file rather than "not found" or a read error.
  3. "create IDE0/TEST.TXT" a SECOND time (same path as case 2, same
     boot continuing on) — asserts the shell reports the duplicate-name
     rejection ("already exists"-shaped diagnostic) rather than
     silently creating a second entry or crashing.
  4. "create" with no argument at all — asserts the shell prints the
     command's usage line rather than crashing on a NULL/empty path.

Not part of the build; a manual verification script only.
"""
import shutil
import socket
import subprocess
import sys
import time
import os

sys.path.insert(0, os.path.dirname(__file__))
from qemu_dump_console import parse_monitor_hexdump, render  # noqa: E402
from day21_test_show import keys_for  # noqa: E402

IMG = "lumaos-5.0.img"
BOOT_WAIT_SECS = float(os.environ.get("LUMAOS_BOOT_WAIT", "10"))

# DAY24: unlike every earlier day's tests, `create` WRITES to the disk
# image, and that write genuinely persists on lumaos-5.0.img between
# separate QEMU boots (the whole point of Day 24 — see kmain.c's own
# [fat24] self-test comment for why THAT section's own repeat-boot
# runs correctly report a "duplicate" failure on a second run against
# the same un-reset image, which is the image behaving exactly right,
# not a bug). Each scenario below therefore runs against its own
# private COPY of lumaos-5.0.img, taken fresh from the build's output
# right before that scenario boots, so one scenario's writes never
# leak into the next scenario's expectations — the same isolation a
# real disk-image-based test suite needs, just done at the file level
# since this project has no snapshot/rollback tooling of its own yet.
_BASE_IMG_SNAPSHOT = IMG + ".day24_test_base"


def _fresh_image_copy():
    if not os.path.exists(_BASE_IMG_SNAPSHOT):
        shutil.copyfile(IMG, _BASE_IMG_SNAPSHOT)
    shutil.copyfile(_BASE_IMG_SNAPSHOT, IMG)


def run_commands_get_screen(cmdlines, sock_name):
    """Boot once, type each line in `cmdlines` (pressing Enter after
    each), then dump the final screen. Mirrors day21_test_show.py's
    run_command_get_screen(), generalized to more than one command per
    boot."""
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

        for cmdline in cmdlines:
            for key in keys_for(cmdline):
                send_monitor(f"sendkey {key}")
            send_monitor("sendkey ret")
            time.sleep(0.5)

        time.sleep(1.0)  # let the last command run and redraw

        nbytes = 80 * 25 * 2
        s.sendall(f"xp /{nbytes}xb 0xb8000\n".encode())
        time.sleep(0.5)

        raw = b""
        s.settimeout(2.0)
        try:
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                raw += chunk
                if raw.count(b"(qemu)") >= 2:
                    break
        except socket.timeout:
            pass
        s.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    text = raw.decode(errors="replace")
    vals = parse_monitor_hexdump(text)
    if not vals:
        print("ERROR: got no hex bytes back from QEMU monitor", file=sys.stderr)
        print(text, file=sys.stderr)
        sys.exit(1)
    return render(vals)


def main():
    all_ok = True

    _fresh_image_copy()
    print("=== (1) create test.txt (bare, roadmap's own example) + ls ===")
    screen = run_commands_get_screen(["create test.txt", "ls"], "day24_bare")
    print(screen)
    if "created" in screen and "test.txt" in screen.lower() and "0 bytes" in screen:
        print("PASS: bare 'create test.txt' created a file visible in 'ls' "
              "at 0 bytes\n")
    else:
        print("FAIL: expected 'created ...test.txt' and a 0-byte 'ls' "
              "listing\n", file=sys.stderr)
        all_ok = False

    _fresh_image_copy()
    print("=== (2) create IDE0/TEST.TXT + show IDE0/TEST.TXT ===")
    screen = run_commands_get_screen(
        ["create IDE0/TEST.TXT", "show IDE0/TEST.TXT"], "day24_show")
    print(screen)
    if ("created IDE0/TEST.TXT" in screen
            and "(0 bytes)" in screen
            and "(whole file)" in screen):
        print("PASS: created IDE0/TEST.TXT and 'show' confirmed a 0-byte "
              "whole-file read\n")
    else:
        print("FAIL: expected 'created IDE0/TEST.TXT' and a 0-byte 'show' "
              "readout\n", file=sys.stderr)
        all_ok = False

    _fresh_image_copy()
    print("=== (3) create IDE0/TEST.TXT twice — duplicate must be rejected ===")
    screen = run_commands_get_screen(
        ["create IDE0/TEST.TXT", "create IDE0/TEST.TXT"], "day24_dup")
    print(screen)
    if screen.count("created IDE0/TEST.TXT") == 1 and "cannot create" in screen:
        print("PASS: second create of the same path was rejected, not "
              "silently duplicated\n")
    else:
        print("FAIL: expected exactly one 'created' line and one 'cannot "
              "create' diagnostic\n", file=sys.stderr)
        all_ok = False

    _fresh_image_copy()
    print("=== (4) create (no argument) — usage line, no crash ===")
    screen = run_commands_get_screen(["create"], "day24_usage")
    print(screen)
    if "usage:" in screen:
        print("PASS: bare 'create' printed the usage line\n")
    else:
        print("FAIL: expected a 'usage: create ...' line\n", file=sys.stderr)
        all_ok = False

    if not all_ok:
        sys.exit(1)


if __name__ == "__main__":
    main()