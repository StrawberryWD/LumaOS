#!/usr/bin/env python3
"""
tests/day19_test_cat_interactive.py — one-off DAY19 interactive check.

Boots lumaos-5.0.img headless, waits for the boot-time diagnostics to
finish and the text-mode shell prompt to appear, then uses the QEMU
monitor's `sendkey` command to type "cat IDE0/BIGFILE.BIN" + Enter into
the running kernel exactly as a real keyboard would, waits for cmd_cat
to run, and dumps the VGA text screen (reusing qemu_dump_console.py's
monitor-hexdump parser) so the actual shell-invoked `cat` output can be
read back, not just the kmain.c DAY19 boot-time self-test.

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
SOCK = "/tmp/lumaos_qemu_mon_day19.sock"
BOOT_WAIT_SECS = float(os.environ.get("LUMAOS_BOOT_WAIT", "10"))

# QEMU monitor `sendkey` names for the characters we need to type.
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

        # Type: cat IDE0/BIGFILE.BIN <Enter>
        cmdline = "cat IDE0/BIGFILE.BIN"
        for key in keys_for(cmdline):
            send_monitor(f"sendkey {key}")
        send_monitor("sendkey ret")

        time.sleep(1.5)  # let cmd_cat run and redraw the terminal

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
    print(render(vals))


if __name__ == "__main__":
    main()