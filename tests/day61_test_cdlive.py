"""tests/day61_test_cdlive.py - Day 61: QEMU CD-ROM boot test - the ISO enters LumaOS.

    python3 tests/day61_test_cdlive.py        (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
A machine with ONLY the CD (no hard disk, `-boot d`):
  text build : boots to the shell, prints the CD boot lines, skipped the hard-disk self-tests (no FAIL
               lines at all), `version` reports "Boot : CD-ROM (El Torito)", commands work
  GUI build  : (both display backends) reaches the desktop, `version` in the terminal says CD-ROM, the
               screenshot is a real desktop (menu bar + rail + terminal window), mouse + keyboard work
Control: the same kernel from the hard-disk image reports "Hard disk (BIOS)" and still runs the self-tests.
"""
import sys, os, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d61"
os.makedirs(out, exist_ok=True)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)

# ---- text build, CD only ----
m = Machine(None, "d61t", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    s = m.text_screen()
    check("text/CD: 'Booted from CD-ROM' line", "Booted from CD-ROM" in s, s)
    check("text/CD: live mode line, self-tests skipped", "hard-disk self-tests skipped" in s, s)
    check("text/CD: no FAIL lines on the whole boot screen", "FAIL" not in s, s)
    m.line("version", 1.0)
    s = m.text_screen()
    check("text/CD: version says CD-ROM (El Torito)", "Boot    : CD-ROM (El Torito)" in s, s[-500:])
    m.line("echo cdlive42", 1.0)
    check("text/CD: commands work (echo)", m.text_screen().count("cdlive42") >= 2, m.text_screen()[-300:])
finally:
    m.close()

# ---- control: hard-disk boot of the same kernel ----
m = Machine("lumaos-5.0-text.img", "d61h", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=120)
    m.line("version", 1.0)
    s = m.text_screen()
    check("text/HDD: version says Hard disk (BIOS)", "Boot    : Hard disk (BIOS)" in s, s[-400:])
finally:
    m.close()

# ---- GUI build, CD only, both backends ----
def distinct(png):
    im = Image.open(png).convert("RGB"); w, h = im.size; px = im.load()
    return len({px[x, y] for y in range(0, h, 4) for x in range(0, w, 4)})

for vga in ("std", "vmware"):
    g = Machine(None, "d61g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("version", 1.5)
        s = g.gui_screen()
        check("[%s] GUI/CD: terminal shows Boot : CD-ROM (El Torito)" % vga, "CD-ROM (El Torito)" in s, s[-400:])
        png = g.png("%s/%s_cd.png" % (out, vga))
        im = Image.open(png).convert("RGB")
        check("[%s] GUI/CD: screenshot is %dx%d desktop" % (vga, im.size[0], im.size[1]), im.size == (800, 450), im.size)
        check("[%s] GUI/CD: real desktop (>= 8 distinct colours)" % vga, distinct(png) >= 8, distinct(png))
        g.line("help", 1.2)
        check("[%s] GUI/CD: keyboard input still works" % vga, "help" in g.gui_screen().lower())
        g.line("resolution 1024x576", 2.0)
        im2 = Image.open(g.png("%s/%s_cd_1024.png" % (out, vga)))
        check("[%s] GUI/CD: runtime resolution switch works from the CD" % vga, im2.size == (1024, 576), im2.size)
    finally:
        g.close()

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
