#!/usr/bin/env python3
"""
tests/day25_test_write.py — DAY25 interactive check.

Boots lumaos-5.0.img headless, waits for the text-mode shell prompt,
then drives the running kernel through the QEMU monitor's `sendkey`
exactly like day19/20/21/24's own interactive tests — this time
exercising:

  1. `write IDE0/HELLO.TXT` on a file that does NOT exist yet (auto-
     creates it, drops into the multi-line editor).
  2. Typing two lines of content, Enter between them.
  3. Ctrl+S to save (QEMU monitor `sendkey ctrl-s` — a combo-key
     `sendkey` invocation, the first this project's own tests/ use).
  4. `read IDE0/HELLO.TXT` to confirm fat_write() + fat_update_entry_
     fixed_root() actually persisted the content (fat_open()/fat_read()
     see it immediately, no remount needed).
  5. `write IDE0/HELLO.TXT` a SECOND time (file now exists) to exercise
     the "! HELLO.TXT ALREADY EXISTS / DO YOU WANT TO COVER THE FILE?"
     prompt, answering Y, typing new (shorter) content, Ctrl+S again.
  6. `read IDE0/HELLO.TXT` again to confirm the overwrite really
     replaced the old content (not appended).
  7. `cd IDE0` / `cd IDE1/` to exercise the prompt switching mounts.

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
SOCK = "/tmp/lumaos_qemu_mon_day25.sock"
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

        def type_line(text):
            for key in keys_for(text):
                send_monitor(f"sendkey {key}")

        def press_enter():
            send_monitor("sendkey ret")

        def press_ctrl_s():
            send_monitor("sendkey ctrl-s")

        def press_backspace():
            send_monitor("sendkey backspace")

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

        # ---- Step 1: write a brand-new file ----
        type_line("write IDE0/NEWDAY25.TXT")
        press_enter()
        time.sleep(0.6)
        dump_screen("after_write_open")

        # ---- Step 2: type two lines of content ----
        type_line("Hello LumaOS")
        press_enter()
        type_line("Day 25 write works")
        time.sleep(0.3)
        dump_screen("after_typing")

        # ---- Step 3: Ctrl+S to save ----
        press_ctrl_s()
        time.sleep(0.8)
        dump_screen("after_save")

        # ---- Step 4: read it back ----
        type_line("read IDE0/NEWDAY25.TXT")
        press_enter()
        dump_screen("after_read_1")

        # ---- Step 5: write again -> overwrite prompt ----
        type_line("write IDE0/NEWDAY25.TXT")
        press_enter()
        time.sleep(0.5)
        dump_screen("overwrite_prompt")

        send_monitor("sendkey shift-y")  # 'Y'
        time.sleep(0.5)
        dump_screen("after_confirm_yes")

        type_line("Overwritten")
        press_ctrl_s()
        time.sleep(0.8)
        dump_screen("after_second_save")

        # ---- Step 6: read again, confirm overwrite ----
        type_line("read IDE0/NEWDAY25.TXT")
        press_enter()
        dump_screen("after_read_2")

        # ---- Step 7: cd between mounts ----
        type_line("cd IDE0")
        press_enter()
        dump_screen("after_cd_ide0")

        type_line("cd IDE1/")
        press_enter()
        dump_screen("after_cd_ide1")

        s.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    for tag in [
        "after_write_open", "after_typing", "after_save", "after_read_1",
        "overwrite_prompt", "after_confirm_yes", "after_second_save",
        "after_read_2", "after_cd_ide0", "after_cd_ide1",
    ]:
        print(f"===== {tag} =====")
        print(dumps.get(tag, "(missing)"))
        print()


if __name__ == "__main__":
    main()
