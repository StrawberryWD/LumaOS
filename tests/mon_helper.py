import socket, subprocess, time, os, sys
sys.path.insert(0, "tests")
from qemu_dump_console import parse_monitor_hexdump, render

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

class Session:
    def __init__(self, img, sockname, boot_wait=10):
        self.sock = f"/tmp/lumaos_mon_{sockname}.sock"
        if os.path.exists(self.sock):
            os.remove(self.sock)
        self.proc = subprocess.Popen(
            ["qemu-system-i386", "-machine", "type=pc", "-m", "256",
             "-drive", f"format=raw,file={img},if=ide",
             "-display", "none",
             "-monitor", f"unix:{self.sock},server,nowait",
             "-no-reboot"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            if os.path.exists(self.sock):
                break
            time.sleep(0.1)
        time.sleep(boot_wait)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(self.sock)
        time.sleep(0.3)
        self.s.recv(65536)

    def send(self, cmd):
        self.s.sendall((cmd + "\n").encode())
        time.sleep(0.03)
        out = b""
        try:
            self.s.settimeout(0.5)
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

    def type_line(self, text):
        for key in keys_for(text):
            self.send(f"sendkey {key}")

    def press(self, key):
        self.send(f"sendkey {key}")

    def dump_screen(self, wait=0.6):
        time.sleep(wait)
        nbytes = 80 * 25 * 2
        self.s.sendall(f"xp /{nbytes}xb 0xb8000\n".encode())
        time.sleep(0.4)
        raw = b""
        self.s.settimeout(2.0)
        try:
            while True:
                chunk = self.s.recv(65536)
                if not chunk:
                    break
                raw += chunk
                if raw.count(b"(qemu)") >= 1:
                    break
        except socket.timeout:
            pass
        vals = parse_monitor_hexdump(raw.decode(errors="replace"))
        return render(vals) if vals else "(no data)"

    def cursor_pos(self):
        """Read the VGA CRTC hardware cursor location (0x3D4 index
        0x0E/0x0F) and decode it into (col, row) on an 80-col screen."""
        self.send("o /1b 0x3d4 0x0f")
        r = self.send("i /1xb 0x3d5")
        lo = int(r.rsplit("0x", 1)[-1].split("\r")[0], 16)
        self.send("o /1b 0x3d4 0x0e")
        r = self.send("i /1xb 0x3d5")
        hi = int(r.rsplit("0x", 1)[-1].split("\r")[0], 16)
        addr = (hi << 8) | lo
        return addr % 80, addr // 80

    def close(self):
        self.s.close()
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()