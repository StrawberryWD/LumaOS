"""tests/day65_test_fatcache.py - Day 65: FAT table cache.
    python3 tests/day65_test_fatcache.py   (needs LUMAOS_TEXT=1 ./build.sh and ./build.sh)
`fatbench` on IDE0/IDE1/IDE2 (FAT12/16/32): cache off vs on, contents identical, FAT device reads cut >= 8x.
Also: cache coherence after writes (fstress on FAT passes with the cache on), on/off/reset commands.
"""
import shutil, sys, os, time, re
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
fails = []
def fresh(src, tag):   # 每次从干净镜像开始：上一次被中断的测试不会把垃圾留给下一次
    dst = "build-text/test-%s.img" % tag
    shutil.copyfile(src, dst); return dst
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)

def wait_for_any(screen, marks, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = screen()
        for k in marks:
            if k in s: return s
        time.sleep(1)
    return screen()

m = Machine(fresh("lumaos-5.0-text.img", "d65"), "d65", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    m.line("fatcache", 1.0)
    s = m.text_screen()
    check("fatcache shows ON by default", "FAT cache: ON" in s, s[-400:])
    for v in ("IDE0", "IDE1", "IDE2"):
        m.line("cd " + v, 0.8)
        m.line("clear", 0.5); m.line("fatbench", 0.5)
        s = wait_for_any(m.text_screen, ["fatbench: PASS", "fatbench: FAIL", "not FAT"])
        check("%s fatbench PASS" % v, "fatbench: PASS" in s, s[-700:])
        mo = re.search(r"FAT device reads (\d+) -> (\d+)", s)
        if mo: print("   ", v, "device reads", mo.group(1), "->", mo.group(2))
        m.line("clear", 0.5); m.line("fstress 16", 0.5)
        s = wait_for_any(m.text_screen, ["fstress: PASS", "fstress: FAIL"])
        check("%s fstress (cache coherence under writes)" % v, "fstress: PASS" in s, s[-700:])
    m.line("fatcache off", 1.0)
    check("fatcache off", "FAT cache: OFF" in m.text_screen())
    m.line("clear", 0.5); m.line("fstress 16", 0.5)
    s = wait_for_any(m.text_screen, ["fstress: PASS", "fstress: FAIL"])
    check("fstress passes with cache OFF too", "fstress: PASS" in s, s[-500:])
    m.line("fatcache on", 1.0)
    m.line("clear", 0.5); m.line("fatcache reset", 1.0)
    check("fatcache on/reset", "FAT cache: ON" in m.text_screen() and re.search(r"lookups 0:", m.text_screen()), m.text_screen()[-400:])
    m.line("cd IDE3", 0.8); m.line("clear", 0.5); m.line("fatbench", 1.0)
    check("fatbench refuses non-FAT", "not FAT" in m.text_screen())
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(fresh("lumaos-5.0.img", "d65g" + vga), "d65g" + vga, gui=True, elf="build/kernel.elf", vga=vga)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("cd IDE2", 1.0); g.line("fatbench", 0.5)
        s = wait_for_any(g.gui_screen, ["fatbench: PASS", "fatbench: FAIL"])
        check("[%s] GUI fatbench on FAT32" % vga, "fatbench: PASS" in s, s[-500:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
