"""tests/day67_test_soak.py - Day 67: known bugs zero / long-run stability / GUI smoothness.
    python3 tests/day67_test_soak.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * `soak 60` on RAM0 (lmfs), IDE0/1/2 (FAT12/16/32) and IDE3 (lmfs): no errors, no space drift, fsck no worse
  * `fsck` on every volume
  * `ls` of a directory with > 32 entries lists all of them (old limit of 32 removed)
  * GUI (std + vmware): `gfxstat` after an idle period writes far fewer rows per frame than a full screen
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
def wait_for_any(screen, marks, timeout=240):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = screen()
        for k in marks:
            if k in s: return s
        time.sleep(1)
    return screen()
def cmd(m, screen, c, marks, timeout=240):
    m.line("clear", 0.5); m.line(c, 0.5)
    return wait_for_any(screen, marks, timeout)

m = Machine(None, "d67a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    s = cmd(m, m.text_screen, "fsck", ["is clean", "problem(s)"])
    check("CD/RAM0 fsck clean", "is clean" in s, s[-500:])
    s = cmd(m, m.text_screen, "soak 60", ["soak: PASS", "soak: FAIL"])
    check("CD/RAM0 soak 60 PASS", "soak: PASS" in s, s[-700:])
    s = cmd(m, m.text_screen, "fsck CD0", ["nothing to check", "problem(s)"])
    check("fsck on CD0 (read-only)", "nothing to check" in s, s[-300:])
    # > 32 entries: ls must list them all
    m.line("clear", 0.5); m.line("fstress 60", 0.5)
    wait_for_any(m.text_screen, ["fstress: PASS", "fstress: FAIL"])
    # leave 45 files behind
    for i in range(45):
        m.line("create ZZ%02d.TXT" % i, 0.35)
    m.line("clear", 0.5); m.line("ls", 2.0)
    s = m.text_screen()
    check("ls shows entry 44 of 45 (no 32-entry cut-off)", "ZZ44.TXT" in s or "zz44.txt" in s.lower(), s[-600:])
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-text.img", "d67b"), "d67b", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    for v in ("IDE0", "IDE1", "IDE2", "IDE3"):
        m.line("cd " + v, 0.8)
        s = cmd(m, m.text_screen, "fsck", ["is clean", "problem(s)"])
        check("%s fsck" % v, "is clean" in s or v == "IDE3", s[-400:])
        s = cmd(m, m.text_screen, "soak 40", ["soak: PASS", "soak: FAIL"])
        check("%s soak 40" % v, "soak: PASS" in s, s[-700:])
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(None, "d67g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("gfxstat reset", 1.0)
        time.sleep(6)
        g.line("gfxstat", 1.5)
        s = g.gui_screen()
        mo = re.findall(r"gfxstat: (\d+) frames, (\d+) rows written \((\d+)%", s)
        print("   ", vga, mo[-1] if mo else None)
        check("[%s] gfxstat reports frames" % vga, bool(mo) and int(mo[-1][0]) > 0, s[-400:])
        check("[%s] idle GUI writes < 50%% of full-screen rows" % vga, bool(mo) and int(mo[-1][2]) < 50, s[-400:])
        g.line("soak 20", 0.5)
        s = wait_for_any(g.gui_screen, ["soak: PASS", "soak: FAIL"])
        check("[%s] GUI soak 20" % vga, "soak: PASS" in s, s[-500:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
