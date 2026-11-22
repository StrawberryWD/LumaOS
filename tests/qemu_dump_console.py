#!/usr/bin/env python3
"""
tests/qemu_dump_console.py — LumaOS 5.0 headless text-mode debug helper

DAY12 (Week 2, Day 12 of the 5.0 filesystem roadmap):
  Boots lumaos-5.0.img in QEMU with -display none (no window needed — this
  project stays in VGA *text* mode for the whole first month, see kmain.c's
  "非常重要" header block), waits for the kernel to reach its final "hlt"
  loop, then reads the raw 0xB8000 VGA text-buffer bytes back out through
  the QEMU monitor (`xp /Nxb 0xb8000`) and renders them as plain text
  (attribute bytes dropped, trailing blank cells on each 80-column row
  trimmed) so kmain.c's DAY9-DAY12 console output can be read/diffed like
  any other command's stdout instead of eyeballed from a screenshot.

  This is purely a dev/test convenience script — it is not part of the
  build (build.sh does not invoke it) and is not linked into the kernel
  image in any way. It exists because this roadmap's Day 1 through Day 28
  acceptance criteria are all "read text off the QEMU console and compare
  against an expected value", and a screenshot can't be grepped/diffed.
"""
import socket
import subprocess
import sys
import time
import os

IMG = "lumaos-5.0.img"
SOCK = "/tmp/lumaos_qemu_mon.sock"
COLS = 80
ROWS = 25
BOOT_WAIT_SECS = float(os.environ.get("LUMAOS_BOOT_WAIT", "6"))


def dump_vga_text():
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
        # Wait for the monitor socket to appear, then for the kernel's
        # boot sequence (GDT/IDT/.../fat12 tests) to finish and settle
        # into its final `for(;;) hlt;` loop.
        for _ in range(50):
            if os.path.exists(SOCK):
                break
            time.sleep(0.1)
        time.sleep(BOOT_WAIT_SECS)

        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK)
        time.sleep(0.3)
        s.recv(65536)  # discard the monitor banner/prompt

        nbytes = COLS * ROWS * 2
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

    return raw.decode(errors="replace")


def parse_monitor_hexdump(text):
    """Turn `xp` output lines like:
    00000000000b8000: 0x5b 0x0a 0x66 0x0a 0x61 0x0a 0x74 0x0a
    into a flat list of byte values, in address order.
    """
    byte_vals = []
    for line in text.splitlines():
        line = line.strip()
        if ":" not in line or not line.startswith("0000"):
            continue
        _, _, rest = line.partition(":")
        for tok in rest.strip().split():
            if tok.startswith("0x"):
                byte_vals.append(int(tok, 16))
    return byte_vals


def render(byte_vals):
    lines = []
    cell_count = COLS * ROWS
    n = min(cell_count, len(byte_vals) // 2)
    for row in range(ROWS):
        chars = []
        for col in range(COLS):
            idx = row * COLS + col
            if idx >= n:
                break
            ch = byte_vals[idx * 2]
            chars.append(chr(ch) if 0x20 <= ch < 0x7F else " ")
        lines.append("".join(chars).rstrip())
    # Trim trailing all-blank lines for readability.
    while lines and lines[-1] == "":
        lines.pop()
    return "\n".join(lines)


if __name__ == "__main__":
    raw = dump_vga_text()
    vals = parse_monitor_hexdump(raw)
    if not vals:
        print("ERROR: got no hex bytes back from QEMU monitor", file=sys.stderr)
        print(raw, file=sys.stderr)
        sys.exit(1)
    print(render(vals))