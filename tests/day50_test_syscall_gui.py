"""
tests/day50_test_syscall_gui.py - Day 50 smoke test of the `int 0x80` channel on the GUI build.

    ./build.sh && python3 tests/day50_test_syscall_gui.py [outdir]

Boots the GUI image, types `sysstat -t` (the kernel executes `int 0x80` itself) and `sysstat` into the GUI
terminal, then checks the desktop still answers. Exhaustive checks: day50_test_syscall.py (text build).
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d50_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d50_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "d50gui", gui=True, elf="build/kernel.elf")
fails = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(extra)


def tail_of(cmd):
    return m.gui_screen().split(cmd)[-1]


try:
    scr = m.wait_for("Type 'help'", timeout=150)
    check("GUI booted", "Type 'help'" in scr, scr)
    time.sleep(2)
    m.line("sysstat -t", 3.0)
    t = tail_of("sysstat -t")
    check("sysstat -t: all [ok], verdict OK", t.count("[ok]") >= 15 and "sysstat: OK" in t and "[FAIL]" not in t, t)
    m.png(f"{out}/1_selftest.png")
    m.line("sysstat", 2.0)
    t = tail_of("sysstat")
    check("sysstat: gate type 0xee DPL=3, 8 calls / 1 bad",
          "selector 0x08" in t and "type 0xee" in t and "DPL=3" in t and "calls:     8" in t and "bad calls: 1" in t, t)
    m.png(f"{out}/2_status.png")
    m.line("cd IDE3", 1.0)
    m.line("run hello.sw luma", 3.0)
    t = tail_of("run hello.sw luma")
    check("run hello.sw luma still works", "Hello, luma!" in t, t)
    m.line("ls", 1.5)
    check("terminal still alive (ls works)", any(n in tail_of("ls") for n in ("hello.sw", "putc.sw", "printf.sw", "malloc.sw", "fileio.sw", "explorer.sw")))
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
