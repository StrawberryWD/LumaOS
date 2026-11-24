"""
tests/day53_test_paging_gui.py - Day 53 smoke test of paging on the GUI build.

    ./build.sh && python3 tests/day53_test_paging_gui.py [outdir]

The GUI build maps/uses the VMware/Bochs linear framebuffer (0xE0000000..0xFD000000, a 4MB uncached large page under
paging) - if the mapping were wrong the desktop would not draw. Checks: the desktop comes up, `pginfo -t` and `pginfo`
in the GUI terminal, a program run, and that the terminal still answers. Exhaustive: day53_test_paging.py (text).
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d53_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d53_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "d53gui", gui=True, elf="build/kernel.elf")
fails = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(extra)


def tail_of(cmd):
    return m.gui_screen().split(cmd)[-1]


try:
    scr = m.wait_for("Type 'help'", timeout=150)
    check("GUI desktop booted under paging (framebuffer reachable)", "Type 'help'" in scr, scr)
    time.sleep(2)
    m.png(f"{out}/0_desktop.png")
    m.line("pginfo -t", 3.0)
    t = tail_of("pginfo -t")
    check("pginfo -t OK (17 [ok])", "pginfo: OK" in t and "[FAIL]" not in t and t.count("[ok]") == 17, t)
    m.line("pginfo", 2.0)
    t = tail_of("pginfo")
    check("pginfo: ENABLED, 0 faults", "paging: ENABLED" in t and "page faults handled: 0" in t, t)
    m.png(f"{out}/1_pginfo.png")
    m.line("cd IDE3", 1.0)
    m.line("run hello.sw luma", 3.0)
    check("run hello.sw luma works", "Hello, luma!" in tail_of("run hello.sw luma"))
    m.line("ls", 1.5)
    check("terminal alive (ls works)", any(n in tail_of("ls") for n in ("hello.sw", "putc.sw", "printf.sw", "malloc.sw", "fileio.sw", "explorer.sw")))
    m.png(f"{out}/2_end.png")
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
