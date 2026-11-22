#!/usr/bin/env python3
"""
tests/day25_test_write_extra.py — DAY25 supplementary interactive check.

Covers what day25_test_write.py doesn't:
  1. Answering N (cancel) at the overwrite-confirmation prompt — the
     file's content must be UNCHANGED afterwards.
  2. `write`/`read` against a bare filename with NO mount-slot prefix
     at all (relies on shell_resolve_path() defaulting onto vfs_root's
     current mount — exercised while vfs_root is IDE1/FAT16).
  3. `write`/`read` on IDE2 (FAT32) directly, to confirm fat_write()'s
     three-way variant dispatch also works for the FAT32 branch (only
     IDE0/FAT12 was exercised by the main day25_test_write.py script).

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
SOCK = "/tmp/lumaos_qemu_mon_day25b.sock"
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

    dumps = {}

    try:
        for _ in range(50):
            if os.path.exists(SOCK):
                break
            time.sleep(0.1)
        time.sleep(BOOT_WAIT_SECS)

        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK)
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

        def type_line(text):
            for key in keys_for(text):
                send_monitor(f"sendkey {key}")

        def press_enter():
            send_monitor("sendkey ret")

        def press_ctrl_s():
            send_monitor("sendkey ctrl-s")

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
            dumps[tag] = render(vals) if vals else "(no data)"

        # ---- Setup: create+write a file on IDE0 first ----
        type_line("write IDE0/CANCELTS.TXT")
        press_enter()
        time.sleep(0.5)
        type_line("Original content")
        press_ctrl_s()
        time.sleep(0.7)

        # ---- Test 1: write again, answer N -> must stay unchanged ----
        type_line("write IDE0/CANCELTS.TXT")
        press_enter()
        time.sleep(0.5)
        send_monitor("sendkey shift-n")  # 'N'
        time.sleep(0.5)
        dump_screen("after_cancel")

        type_line("read IDE0/CANCELTS.TXT")
        press_enter()
        dump_screen("after_cancel_readback")

        # ---- Test 2: cd IDE1, then bare-filename write/read ----
        type_line("cd IDE1")
        press_enter()
        time.sleep(0.3)

        type_line("write BAREFILE.TXT")
        press_enter()
        time.sleep(0.5)
        type_line("No mount slot needed")
        press_ctrl_s()
        time.sleep(0.7)
        dump_screen("after_bare_write")

        type_line("read BAREFILE.TXT")
        press_enter()
        dump_screen("after_bare_read")

        # ---- Test 3: write/read directly on IDE2 (FAT32) ----
        type_line("cd IDE2")
        press_enter()
        time.sleep(0.3)

        type_line("write IDE2/FAT32NEW.TXT")
        press_enter()
        time.sleep(0.5)
        type_line("FAT32 write test")
        press_ctrl_s()
        time.sleep(0.7)
        dump_screen("after_fat32_write")

        type_line("read IDE2/FAT32NEW.TXT")
        press_enter()
        dump_screen("after_fat32_read")

        s.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    for tag in [
        "after_cancel", "after_cancel_readback",
        "after_bare_write", "after_bare_read",
        "after_fat32_write", "after_fat32_read",
    ]:
        print(f"===== {tag} =====")
        print(dumps.get(tag, "(missing)"))
        print()


if __name__ == "__main__":
    main()
