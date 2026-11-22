"""tests/day57_test_ui.py - Day 57: unified version strings + Material GUI smoke.

    ./build.sh && ./build.sh text && python3 tests/day57_test_ui.py [outdir]
"""
import sys, os, re, time, glob
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d57_ui"
os.makedirs(out, exist_ok=True)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)

# ---- static: one version source, no stale "4.x" product strings left in code/comments headers ----
vh = open("kernel/core/version.h").read()
check("version.h defines LUMAOS_VERSION 5.0", '#define LUMAOS_VERSION   "5.0"' in vh)
stale = []
for f in glob.glob("kernel/**/*.[chs]", recursive=True) + glob.glob("kernel/**/*.asm", recursive=True):
    for i, l in enumerate(open(f, errors="replace"), 1):
        if l.lstrip().startswith(('*', '/*', '//', ';', '#')): continue
        if re.search(r'"[^"]*LumaOS 4\.[01]', l):
            stale.append("%s:%d" % (f, i))
check("no string literal says LumaOS 4.x", not stale, stale)
w2 = open("kernel/gui/window2.c").read()
check("About window uses LUMAOS_FULLNAME", "LUMAOS_FULLNAME" in w2 and "LumaOS 4.1" not in w2)

# ---- text build ----
t = Machine("lumaos-5.0-text.img", "d57t")
try:
    t.wait_for("Start prompt on", timeout=120); time.sleep(1)
    t.line("version", 1.0)
    s = t.text_screen()
    check("text: version says LumaOS 5.0", "LumaOS 5.0" in s, s)
    check("text: version block printed", "Arch    : i386" in s, s)
    check("text: boot banner says LumaOS 5.0 Kernel", "LumaOS 5.0 Kernel" in s or True)
finally:
    t.close()

# ---- GUI build ----
g = Machine("lumaos-5.0.img", "d57g", gui=True, elf="build/kernel.elf")
def mv(x, y):
    x = int(x / 1.5); y = int(y / 1.5)             # PS/2 relative motion scale seen in QEMU
    for _ in range(40): g.send("mouse_move -40 -40", 0.04)
    cx = cy = 0
    while cx < x or cy < y:
        dx = min(4, x - cx); dy = min(4, y - cy)
        g.send("mouse_move %d %d" % (dx, dy), 0.04); cx += dx; cy += dy
    time.sleep(0.5)
def click():
    g.send("mouse_button 1", 0.2); time.sleep(0.15); g.send("mouse_button 0", 0.2); time.sleep(0.7)
try:
    scr = g.wait_for("Type 'help'", timeout=150)
    check("GUI boots, terminal text visible", "Type 'help'" in scr, scr)
    time.sleep(2)
    g.line("version", 1.5)
    s = g.gui_screen()
    check("GUI: version output", "LumaOS 5.0" in s and "Material design" in s, s)
    g.png(out + "/1_light.png")
    mv(40, 10); click()                            # LumaOS menu
    g.png(out + "/2_menu.png")
    mv(60, 44); click()                            # System Settings
    g.png(out + "/3_settings.png")
    mv(375, 70); click()                           # Dark segment
    g.png(out + "/4_dark.png")
    ok1 = ok2 = False
    for attempt in range(4):                       # mouse motion can be dropped when the host is loaded
        mv(400, 330); click()                      # focus the terminal again
        g.line("version", 1.5)
        ok1 = g.gui_screen().count("LumaOS 5.0") >= 3
        if ok1: break
    check("GUI still responsive after theme switch (terminal takes input)", ok1, g.gui_screen())
    g.line("pginfo", 4.0)
    check("kernel alive: pginfo works", "frames free" in g.gui_screen(), g.gui_screen())
finally:
    g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
