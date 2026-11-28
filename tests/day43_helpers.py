"""
tests/day43_helpers.py — shared QEMU-monitor driver for the Day 43 tests.

Day 42's helpers (mon_helper.py / lmfs_test_helpers.py) only know how to
read the VGA TEXT buffer at 0xB8000. Day 43 turns the GUI on, so this one
can drive BOTH builds of the same kernel:

  text build  (LUMAOS_TEXT=1 ./build.sh -> lumaos-5.0-text.img)
      screen() = the 80x25 text buffer, exactly like the older tests.
  GUI build   (./build.sh -> lumaos-5.0.img)
      screen() = the GUI terminal's own scrollback buffer (g_terminal),
      read straight out of guest RAM through the monitor (`xp`), the
      address coming from `nm build/kernel.elf`. That is the same data
      terminal_render() paints into the window, so it is a faithful,
      OCR-free way to assert on what the desktop is showing.
      png(path) takes a real framebuffer screenshot (`screendump`) for
      eyeballing.

Not part of the build; a manual verification helper only.
"""
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from qemu_dump_console import parse_monitor_hexdump, render  # noqa: E402

KEYMAP = {c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"}
KEYMAP.update({".": "dot", "/": "slash", " ": "spc", "_": "shift-minus",
               "-": "minus", "=": "equal", '"': "shift-apostrophe", ",": "comma", "\\": "backslash",
               ";": "semicolon", "'": "apostrophe", ">": "shift-dot",
               "$": "shift-4", "{": "shift-bracket_left", "}": "shift-bracket_right", "[": "bracket_left",
               "]": "bracket_right", ":": "shift-semicolon"})
KEYMAP.update({c.upper(): "shift-" + c for c in "abcdefghijklmnopqrstuvwxyz"})   # DAY77c: 大写字母（环境变量名）

# terminal_t layout (kernel/gui/gui.h): lines[400] of
#   { char text[120]; unsigned char color[120]; int len; }  -> 244 bytes
TERM_ROWS, TERM_COLS, LINE_SZ = 400, 120, 244


def keys_for(s):
    out = []
    for ch in s:
        if ch.isupper():
            out.append("shift-" + KEYMAP[ch.lower()])
        else:
            out.append(KEYMAP[ch])
    return out


def nm_symbol(elf, name):
    out = subprocess.check_output(["nm", elf], text=True)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise KeyError(name)


class Machine:
    def __init__(self, img, name, gui=None, elf=None, extra=None, vga="std", serial=None, cdrom=None, hd=True):
        """gui=None: guess from the image name (…-text.img = text build)."""
        if gui is None:
            gui = "text" not in os.path.basename(img)
        self.gui = gui
        self.img = img
        self.sock_path = f"/tmp/lumaos_d43_{name}.sock"
        if os.path.exists(self.sock_path):
            os.remove(self.sock_path)
        args = ["qemu-system-i386", "-machine", "type=pc", "-m", "256",
                "-vga", vga,
                "-display", "none",
                "-monitor", f"unix:{self.sock_path},server,nowait",
                "-no-reboot"]
        # DAY59: img = hard disk (hd=True). cdrom=<iso> adds an IDE CD-ROM and boots from it
        # (-boot d); with hd=False the machine has NO hard disk at all (pure live CD).
        if hd and img:
            args += ["-drive", f"format=raw,file={img},if=ide,index=0"]
        if cdrom:
            args += ["-drive", f"format=raw,file={cdrom},if=ide,media=cdrom,index=2", "-boot", "d"]
        if serial:
            args += ["-serial", "file:" + serial]
        if extra:
            args += extra
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(self.sock_path):
                break
            time.sleep(0.1)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(self.sock_path)
        time.sleep(0.3)
        self._drain()
        self.term_addr = None
        if gui:
            elf = elf or "build/kernel.elf"
            self.term_addr = nm_symbol(elf, "g_terminal")

    # ---- monitor plumbing -------------------------------------------
    def _drain(self, timeout=0.3):
        out = b""
        self.s.settimeout(timeout)
        try:
            while True:
                chunk = self.s.recv(65536)
                if not chunk:
                    break
                out += chunk
                if out.endswith(b"(qemu) "):
                    break
        except socket.timeout:
            pass
        return out.decode(errors="replace")

    def send(self, cmd, timeout=0.5):
        self.s.sendall((cmd + "\n").encode())
        return self._drain(timeout)

    def key(self, k):
        self.send(f"sendkey {k}", 0.2)
        time.sleep(0.03)

    def type(self, text):
        for k in keys_for(text):
            self.key(k)

    def line(self, text, settle=0.6):
        self.type(text)
        self.key("ret")
        time.sleep(settle)

    def ctrl_s(self):
        self.key("ctrl-s")

    # ---- reading the screen -----------------------------------------
    def _read_bytes(self, addr, n):
        raw = ""
        got = []
        # `xp /Nxb` output is "ADDR: 0x.. 0x.. ..." lines
        text = self.send(f"xp /{n}xb 0x{addr:x}", 3.0)
        for ln in text.splitlines():
            if ":" not in ln:
                continue
            _, rest = ln.split(":", 1)
            for tok in rest.split():
                if tok.startswith("0x"):
                    got.append(int(tok, 16))
        return bytes(got)

    def text_screen(self):
        text = self.send(f"xp /{80 * 25 * 2}xb 0xb8000", 3.0)
        vals = parse_monitor_hexdump(text)
        return render(vals) if vals else ""

    def gui_screen(self):
        """The GUI terminal's scrollback, as plain text (one line per row)."""
        hdr = self._read_bytes(self.term_addr + TERM_ROWS * LINE_SZ, 4)
        total = int.from_bytes(hdr[:4], "little") if len(hdr) == 4 else 0
        total = max(1, min(total, TERM_ROWS))
        first = max(0, total - 60)
        base = self.term_addr + first * LINE_SZ
        data = self._read_bytes(base, (total - first) * LINE_SZ)
        rows = []
        for i in range(total - first):
            rec = data[i * LINE_SZ:(i + 1) * LINE_SZ]
            if len(rec) < LINE_SZ:
                break
            n = int.from_bytes(rec[240:244], "little")
            n = max(0, min(n, TERM_COLS))
            rows.append(rec[:n].decode("latin-1").rstrip())
        return "\n".join(rows)

    def screen(self):
        return self.gui_screen() if self.gui else self.text_screen()

    def png(self, path):
        ppm = path + ".ppm"
        self.send(f"screendump {ppm}", 3.0)
        for _ in range(30):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                break
            time.sleep(0.1)
        time.sleep(0.2)
        from PIL import Image
        Image.open(ppm).save(path)
        os.remove(ppm)
        return path

    def hash_screen(self):
        """Cheap 'did the display change' probe for hang detection."""
        ppm = f"/tmp/lumaos_d43_{os.getpid()}_probe.ppm"
        self.send(f"screendump {ppm}", 3.0)
        time.sleep(0.3)
        try:
            with open(ppm, "rb") as f:
                return hash(f.read())
        finally:
            if os.path.exists(ppm):
                os.remove(ppm)

    # ---- waiting ----------------------------------------------------
    def wait_for(self, marker, timeout=60, poll=0.5):
        t0 = time.time()
        last = ""
        while time.time() - t0 < timeout:
            last = self.screen()
            if marker in last:
                return last
            time.sleep(poll)
        return last

    def close(self):
        try:
            self.s.close()
        finally:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
