"""tests/day58b_test_resolution.py - Day 58b: runtime resolutions + blend pool.

    python3 tests/day58b_test_resolution.py [outdir]
For BOTH display backends (QEMU -vga std = Bochs BGA 8bpp, -vga vmware = SVGA II 32bpp):
  * `resolution` lists the modes and the pool; every mode can be set; the QEMU screendump has exactly that size
  * after every switch the desktop is intact (menu bar, rail drawn, shell still answers)
  * wallpaper stress (theme toggles x modes x translucent windows): no reddish wallpaper pixel, pool never over capacity
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d58b"
os.makedirs(out, exist_ok=True)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)

MODES = ["640x360", "800x450", "800x600", "1024x576", "1024x768", "1280x720"]

def reddish(path):
    im = Image.open(path).convert("RGB"); w, h = im.size; px = im.load()
    n = 0
    for y in range(44, h, 3):                      # below the title bar (its close button is red)
        for x in range(52, w, 3):
            r, g, b = px[x, y]
            if r > g + 25 and r > b + 25: n += 1
    return n, im.size

for vga in ("std", "vmware"):
    g = Machine("lumaos-5.0.img", "d58b" + vga, gui=True, elf="build/kernel.elf", vga=vga)
    try:
        g.wait_for("Type 'help'", timeout=150); time.sleep(2)
        g.line("resolution", 1.5)
        s = g.gui_screen()
        check("[%s] resolution lists modes + pool" % vga, "Modes" in s and "1280x720" in s and "Blend pool" in s, s)
        check("[%s] pool capacity is 199" % vga, "/ 199" in s, s)
        for m in MODES:
            g.line("resolution " + m, 2.0)
            png = g.png("%s/%s_%s.png" % (out, vga, m))
            r, size = reddish(png)
            check("[%s] %s: screen is %s" % (vga, m, m), "%dx%d" % size == m, size)
            check("[%s] %s: no reddish wallpaper" % (vga, m), r == 0, r)
            g.line("version", 1.2)
            check("[%s] %s: shell still answers" % (vga, m), "LumaOS 5.0" in g.gui_screen())
        g.line("resolution 12x7", 1.0)
        check("[%s] unknown mode rejected" % vga, "unknown mode" in g.gui_screen())
        g.line("resolution 800x450", 2.0)
        g.line("resolution", 1.5)
        s = g.gui_screen()
        check("[%s] back to 800x450" % vga, "800x450" in s)
        # ---- wallpaper stress with the mouse: minimise the terminal, toggle the theme many times ----
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
        mv(762, 31); click()                       # minimise the main terminal -> wallpaper visible
        mv(40, 10); click(); mv(60, 44); click()   # open Settings
        bad = 0
        for i in range(8):
            mv(375 if i % 2 == 0 else 310, 70); click()
            png = g.png("%s/%s_stress%d.png" % (out, vga, i))
            im = Image.open(png).convert("RGB"); px = im.load(); n = 0
            for y in range(24, 450, 3):
                for x in range(410, 800, 3):
                    r, gg, b = px[x, y]
                    if r > gg + 25 and r > b + 25: n += 1
            bad += n
        check("[%s] wallpaper never reddish through 8 theme switches" % vga, bad == 0, bad)
        g.line("resolution", 1.5)
        import re
        mo = re.findall(r"(\d+) / 199", s)
        check("[%s] pool usage within capacity" % vga, mo and int(mo[-1]) <= 199, s)
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
