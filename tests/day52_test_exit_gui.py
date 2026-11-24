"""
tests/day52_test_exit_gui.py - Day 52 smoke test of sys_exit on the GUI build.

    ./build.sh && python3 tests/day52_test_exit_gui.py [outdir]

hello.sw ends with exit(0) through `int 0x80`; checks via `sysstat` that SYS_EXIT was the last call, `elfrun -v` says
"left via sys_exit", and the GUI terminal (keyboard IRQ) still answers afterwards. Exhaustive: day52_test_exit.py (text).
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d52_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d52_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "d52gui", gui=True, elf="build/kernel.elf")
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
    m.line("cd IDE3", 1.0)
    m.line("sysstat -r", 1.0)
    m.line("run hello.sw luma", 3.0)
    t = tail_of("run hello.sw luma")
    check("run hello.sw luma prints both lines, back at the prompt", "Hello, luma!" in t and "exited" not in t, t)
    m.png(f"{out}/1_hello.png")
    m.line("sysstat", 2.0)
    t = tail_of("sysstat")
    check("last call = SYS_EXIT (nr=1 ret=0), 5 calls", "calls:     5" in t and "last: nr=1 ret=0" in t, t)
    m.line("elfrun -v hello.sw", 3.0)
    check("elfrun -v: left via sys_exit", "left via sys_exit" in tail_of("elfrun -v hello.sw"))
    m.line("sysstat -t", 3.0)
    check("kernel SYS_EXIT -> -ESRCH, self-test OK", "sysstat: OK" in tail_of("sysstat -t"))
    m.png(f"{out}/2_after.png")
    m.line("ls", 1.5)
    check("keyboard + terminal alive after the exits (ls works)", any(n in tail_of("ls") for n in ("hello.sw", "putc.sw", "printf.sw", "malloc.sw", "fileio.sw", "explorer.sw")))
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
