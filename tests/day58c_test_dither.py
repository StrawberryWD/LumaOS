"""tests/day58c_test_dither.py - Day 58c: the wallpaper is ONE flat colour (no gradient, no bands).
    python3 tests/day58c_test_dither.py [outdir]
Minimise the terminal, then every wallpaper pixel in the free right-hand strip must be identical (both
display backends, both themes)."""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d58c"
os.makedirs(out, exist_ok=True)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)

def colours(path):
    im = Image.open(path).convert("RGB"); w, h = im.size; px = im.load()
    return {px[x, y] for y in range(28, h - 2, 2) for x in range(w - 24, w - 4, 2)}

for vga in ("std", "vmware"):
    g = Machine("lumaos-5.0.img", "d58c" + vga, gui=True, elf="build/kernel.elf", vga=vga)
    try:
        g.wait_for("Type 'help'", timeout=150); time.sleep(2)
        g.line("resolution 800x450", 2.0)
        def mv(x, y):
            x = int(x / 1.5); y = int(y / 1.5)
            for _ in range(40): g.send("mouse_move -40 -40", 0.04)
            cx = cy = 0
            while cx < x or cy < y:
                dx = min(4, x - cx); dy = min(4, y - cy)
                g.send("mouse_move %d %d" % (dx, dy), 0.04); cx += dx; cy += dy
            time.sleep(0.4)
        def click():
            g.send("mouse_button 1", 0.2); time.sleep(0.15); g.send("mouse_button 0", 0.2); time.sleep(0.6)
        for _ in range(4):                         # minimise the terminal (pointer clicks can miss)
            mv(762, 31); click()
            if len(colours(g.png("%s/%s_probe.png" % (out, vga)))) <= 2: break
        mv(40, 10); click(); mv(60, 44); click()
        for i, name in enumerate(("themeA", "themeB")):
            mv(375 if i == 0 else 310, 70); click()
            cs = colours(g.png("%s/%s_%s.png" % (out, vga, name)))
            check("[%s/%s] wallpaper is one flat colour (%d seen)" % (vga, name, len(cs)), len(cs) == 1, cs)
    finally:
        g.close()
print("FAILED: %s" % fails if fails else "ALL PASS")
sys.exit(1 if fails else 0)
