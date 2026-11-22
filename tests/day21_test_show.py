#!/usr/bin/env python3
"""
tests/day21_test_show.py — one-off DAY21 interactive check.

DAY21 (Week 3, Day 21 of the 5.0 filesystem roadmap — "周复盘"):
  "添加错误处理（文件不存在、簇链断裂）。关闭文件功能（fat_close）。编写
   测试用例：更改cat命令名称为show，功能不变，一个根目录下的文本文件
   （用 Python 提前写入）。"

Boots lumaos-5.0.img headless (same approach as tests/day19_test_cat_
interactive.py / tests/day20_test_cat_full.py), then exercises the
renamed `show` command from the real running shell, via three separate
QEMU boots (one command line per boot, same "type a command, dump the
screen" technique the day19/day20 scripts already use):

  1. "show IDE0/HELLO.TXT" — HELLO.TXT is the DAY21 text-file fixture
     build.sh's gen_fat12_fixtures() writes ahead of time with Python
     (see that function's DAY21 comment) specifically so this file's
     content is legible on the VGA text screen, unlike BIGFILE.BIN/
     SMALL.BIN's synthetic binary patterns. Asserts the real fixture
     text appears verbatim and the "(whole file)" footer is printed.
  2. "show IDE0/NOPE.TXT" — a filename that does not exist. Asserts the
     shell reports "not found" (fat_strerror(FAT_ERR_NOT_FOUND), see
     fat_vfs.c) rather than crashing or hanging.
  3. "cat IDE0/HELLO.TXT" — the OLD command name. Asserts the shell
     reports "Unknown: cat", proving the rename actually removed the
     old name rather than keeping it as a silent alias.

The kmain.c boot-time [fat21a]/[fat21b]/[fat21c] self-test sections
already give byte-for-byte proof that fat_open()/fat_read() return the
specific fat_error_t codes (FAT_ERR_NOT_FOUND / FAT_ERR_CHAIN_BROKEN)
and that fat_close() actually invalidates a file_t — this script is the
shell-invoked, human-legible counterpart, the same relationship
day19_test_cat_interactive.py already has to kmain.c's own [fat19]
section.

Not part of the build; a manual verification script only.
"""
import socket
import subprocess
import sys
import time
import os

sys.path.insert(0, os.path.dirname(__file__))
from qemu_dump_console import parse_monitor_hexdump, render  # noqa: E402

IMG = "lumaos-5.0.img"
BOOT_WAIT_SECS = float(os.environ.get("LUMAOS_BOOT_WAIT", "10"))

KEYMAP = {
    "a": "a", "b": "b", "c": "c", "d": "d", "e": "e", "f": "f", "g": "g",
    "h": "h", "i": "i", "j": "j", "k": "k", "l": "l", "m": "m", "n": "n",
    "o": "o", "p": "p", "q": "q", "r": "r", "s": "s", "t": "t", "u": "u",
    "v": "v", "w": "w", "x": "x", "y": "y", "z": "z",
    "0": "0", "1": "1", "2": "2", "3": "3", "4": "4",
    "5": "5", "6": "6", "7": "7", "8": "8", "9": "9",
    ".": "dot", "/": "slash", " ": "spc", "_": "shift-minus",
}


def keys_for(s):
    out = []
    for ch in s:
        if ch.isupper():
            out.append("shift-" + KEYMAP[ch.lower()])
        else:
            out.append(KEYMAP[ch])
    return out


def run_command_get_screen(cmdline, sock_name):
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

        for key in keys_for(cmdline):
            send_monitor(f"sendkey {key}")
        send_monitor("sendkey ret")

        time.sleep(1.0)  # let the command run and redraw

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

    print("=== (1) show IDE0/HELLO.TXT — DAY21 text-file fixture ===")
    screen = run_command_get_screen("show IDE0/HELLO.TXT", "day21_show")
    print(screen)
    if ("Hello from the FAT12 root directory" in screen
            and "show command replaces cat" in screen
            and "(whole file)" in screen):
        print("PASS: show printed HELLO.TXT's real text and the whole-file footer\n")
    else:
        print("FAIL: expected HELLO.TXT text / whole-file footer not found\n",
              file=sys.stderr)
        all_ok = False

    print("=== (2) show IDE0/NOPE.TXT — missing file ===")
    screen = run_command_get_screen("show IDE0/NOPE.TXT", "day21_notfound")
    print(screen)
    if "cannot open" in screen and "not found" in screen:
        print("PASS: show reported '(not found)' for a missing file\n")
    else:
        print("FAIL: expected 'cannot open ... (not found)' diagnostic\n",
              file=sys.stderr)
        all_ok = False

    print("=== (3) cat IDE0/HELLO.TXT — old command name must be gone ===")
    screen = run_command_get_screen("cat IDE0/HELLO.TXT", "day21_catgone")
    print(screen)
    if "Unknown: cat" in screen:
        print("PASS: 'cat' is no longer a recognized command (renamed to 'show')\n")
    else:
        print("FAIL: expected 'Unknown: cat' — did the rename not take?\n",
              file=sys.stderr)
        all_ok = False

    if not all_ok:
        sys.exit(1)


if __name__ == "__main__":
    main()