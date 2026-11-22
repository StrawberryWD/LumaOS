"""
tests/day54_test_aspace_gui.py - Day 54 smoke test of independent address spaces on the GUI build.

    ./build.sh && python3 tests/day54_test_aspace_gui.py [outdir]

While a program runs, CR3 points at the program's page directory. The GUI build keeps drawing through the shared
kernel PDEs (back buffer 0x300000, LFB 0xE0000000+); if sharing were wrong the desktop would fault or go blank.
Checks: desktop, `pginfo -a`, `elfrun -v hello.sw` (address-space lines), `run`, pool fully returned, terminal alive.
Exhaustive: day54_test_aspace.py (text).
"""
import sys, time, os, shutil, re
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d54_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d54_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "d54gui", gui=True, elf="build/kernel.elf")
fails = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(extra)


def tail_of(cmd):
    return m.gui_screen().split(cmd)[-1]


try:
    scr = m.wait_for("Type 'help'", timeout=150)
    check("GUI desktop booted", "Type 'help'" in scr, scr)
    time.sleep(2)
    m.png(f"{out}/0_desktop.png")
    m.line("pginfo -a", 3.5)
    t = tail_of("pginfo -a")
    check("pginfo -a OK (12 [ok])", "pginfo: OK (12/12" in t and "[FAIL]" not in t and t.count("[ok]") == 12, t)
    m.png(f"{out}/1_selftest.png")
    m.line("cd IDE3", 1.0)
    m.line("elfrun -v hello.sw", 4.0)
    t = " ".join(tail_of("elfrun -v hello.sw").split())
    check("elfrun -v: hello printed, exit 0", "Hello from hello.sw!" in t and "exited with code 0" in t, t)
    check("elfrun -v: 18 pages / 22 frames, entry va -> pool pa",
          "address space: 18 pages, 22 frames" in t and re.search(r"entry va 0x08048\w{3} -> pa 0x00[89a-f]\w{5}", t), t)
    m.png(f"{out}/2_elfrun.png")
    m.line("run hello.sw luma", 3.0)
    check("run hello.sw luma works", "Hello, luma!" in tail_of("run hello.sw luma"))
    m.line("pginfo", 2.0)
    t = tail_of("pginfo")
    check("pool back to 2048 of 2048, 0 faults", "2048 of 2048 frames free" in t and "page faults handled: 0" in t, t)
    m.line("ls", 1.5)
    check("terminal alive (ls works)", "hello.sw" in tail_of("ls"))
    m.png(f"{out}/3_end.png")
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
