#!/usr/bin/env python3
"""
tests/day20_test_cat_full.py — one-off DAY20 interactive check.

DAY20 (Week 3, Day 20 of the 5.0 filesystem roadmap):
  "增加文件偏移指针 file->pos，支持多次读取。连续调用两次 read，第二次
   能接着上次位置读。"

Boots lumaos-5.0.img headless (same approach as
tests/day19_test_cat_interactive.py), types "cat IDE0/SMALL.BIN" into
the running text-mode shell via the QEMU monitor's `sendkey`, waits for
cmd_cat to run, and dumps the VGA text screen.

SMALL.BIN is 1536 bytes (3 clusters on this project's 512-byte-cluster
fixtures — see build.sh's gen_fat12_fixtures()). Day 19's cmd_cat capped
every read at 512 bytes (one cluster) and always printed the trailing
"-- read 512 bytes (truncated preview) --" footer for this file. Day
20's cmd_cat instead loops fs->read() until EOF, so it must now read
and print all 1536 bytes across multiple fs->read() calls (file->pos
carrying forward between them, crossing two cluster boundaries) and
print "-- read 1536 bytes (whole file) --" instead.

This script asserts exactly that footer text appears (and the old
"truncated preview" footer does NOT), which is a strong, easy-to-check,
screen-scrapeable proxy for "multiple fs->read() calls actually ran and
file->pos correctly carried the offset across all of them" — the exact
byte-for-byte content check already lives in kmain.c's own DAY20
boot-time self-test section (which the boot sequence prior to the shell
prompt already ran and printed [fat20]/[fat20b] OK/FAIL for).

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
SOCK = "/tmp/lumaos_qemu_mon_day20.sock"
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


def main():
    if os.path.exists(SOCK):
        os.remove(SOCK)

    proc = subprocess.Popen(
        [
            "qemu-system-i386",
            "-machine", "type=pc",
            "-m", "256",
            "-drive", f"format=raw,file={IMG},if=ide",
            "-display", "none",
            "-monitor", f"unix:{SOCK},server,nowait",
            "-no-reboot",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        for _ in range(50):
            if os.path.exists(SOCK):
                break
            time.sleep(0.1)
        time.sleep(BOOT_WAIT_SECS)

        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK)
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

        # Type: cat IDE0/SMALL.BIN <Enter>
        cmdline = "cat IDE0/SMALL.BIN"
        for key in keys_for(cmdline):
            send_monitor(f"sendkey {key}")
        send_monitor("sendkey ret")

        time.sleep(1.5)  # let cmd_cat run (multiple fs->read() calls) and redraw

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

    screen = render(vals)
    print(screen)

    if "whole file" in screen and "truncated preview" not in screen:
        print("\nPASS: cat printed the whole 1536-byte file via multiple "
              "fs->read() calls (file->pos carried forward correctly)")
    else:
        print("\nFAIL: expected '... (whole file) --' footer, "
              "did not find it (or still saw 'truncated preview')",
              file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()