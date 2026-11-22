"""tests/day64_test_fstress.py - Day 64: file system stress test (`fstress`).
    python3 tests/day64_test_fstress.py     (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
Runs `fstress` on: live CD RAM0 (LMFS), HD IDE3 (LMFS, from the HD boot), plus the GUI build on std/vmware VGA.
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

def wait_result(m, screen, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = screen()
        if "fstress: PASS" in s or "fstress: FAIL" in s: return s
        time.sleep(1)
    return screen()

def run(m, screen, tag, cmd="fstress"):
    m.line(cmd, 0.5)
    s = wait_result(m, screen)
    check("%s: %s PASS" % (tag, cmd), "fstress: PASS" in s, s[-900:])
    return s

m = Machine(None, "d64a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    s = run(m, m.text_screen, "CD/RAM0 lmfs")
    check("all 8 phases ok", s.count("[ok]") >= 8, s[-900:])
    run(m, m.text_screen, "CD/RAM0 lmfs", "fstress 60")
    m.line("fstress 0", 0.8)
    check("bad count rejected", "usage: fstress" in m.text_screen())
    m.line("cd CD0", 0.8); m.line("fstress", 1.0)
    check("read-only volume refused", "read-only" in m.text_screen(), m.text_screen()[-300:])
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-text.img", "d64b"), "d64b", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    m.line("mounts", 1.0); print(m.text_screen()[-500:])
    run(m, m.text_screen, "HD default volume")
    m.line("cd IDE3", 0.8)
    run(m, m.text_screen, "HD/IDE3 lmfs", "fstress 40")
    for v in ("IDE0", "IDE1", "IDE2"):
        m.line("cd " + v, 0.8)
        if v + "/" in m.text_screen()[-200:]:
            s = run(m, m.text_screen, "HD/" + v + " FAT", "fstress 12")
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(None, "d64g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        run(g, g.gui_screen, "[%s] GUI/CD" % vga, "fstress 30")
    finally:
        g.close()

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
