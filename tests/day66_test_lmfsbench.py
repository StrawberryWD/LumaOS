"""tests/day66_test_lmfsbench.py - Day 66: LMFS bitmap allocator optimisation (`lmfsbench`).
    python3 tests/day66_test_lmfsbench.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
Old (Day 36 first-fit) and new (free counters + hint + 32-bit word scan + bitmap sector cache) allocators must
produce the identical allocation order; counters match the bitmap; the new one does >= 4x fewer bitmap reads;
fsck clean. Also checked on a nearly-full volume (after `fstress`) and on both VGA backends.
"""
import shutil, sys, os, time, re
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)
def fresh(src, tag):
    dst = "build-text/test-%s.img" % tag
    shutil.copyfile(src, dst); return dst
def wait_for_any(screen, marks, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = screen()
        for k in marks:
            if k in s: return s
        time.sleep(1)
    return screen()

def bench(m, screen, tag):
    m.line("clear", 0.5); m.line("lmfsbench", 0.5)
    s = wait_for_any(screen, ["lmfsbench: PASS", "lmfsbench: FAIL", "not LMFS", "too full"])
    check("%s: lmfsbench PASS" % tag, "lmfsbench: PASS" in s, s[-900:])
    check("%s: allocation order identical" % tag, "allocation order identical" in s, s[-600:])
    mo = re.search(r"bitmap sector reads (\d+) -> (\d+)", s)
    if mo: print("   ", tag, "bitmap reads", mo.group(1), "->", mo.group(2))
    return s

m = Machine(None, "d66a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    bench(m, m.text_screen, "CD/RAM0")
    m.line("clear", 0.5); m.line("fstress 60", 0.5)
    wait_for_any(m.text_screen, ["fstress: PASS", "fstress: FAIL"])
    m.line("create keep1.txt", 1.0); m.line("create keep2.txt", 1.0)
    bench(m, m.text_screen, "CD/RAM0 after fstress")
    m.line("clear", 0.5); m.line("cd CD0", 0.8); m.line("lmfsbench", 1.0)
    check("refuses non-LMFS volume", "not LMFS" in m.text_screen(), m.text_screen()[-300:])
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-text.img", "d66b"), "d66b", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    bench(m, m.text_screen, "HD/IDE3")
    m.line("cd IDE0", 0.8); m.line("clear", 0.5); m.line("lmfsbench", 1.0)
    check("HD FAT volume refused", "not LMFS" in m.text_screen())
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(None, "d66g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        bench(g, g.gui_screen, "[%s] GUI" % vga)
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
