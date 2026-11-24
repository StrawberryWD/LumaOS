"""
tests/day51_test_write_gui.py - Day 51 smoke test of sys_write on the GUI build.

    ./build.sh && python3 tests/day51_test_write_gui.py [outdir]

Boots the GUI image, runs `run hello.sw luma` (hello.sw prints through `int 0x80`), then checks via `sysstat` that the
output really went through SYS_WRITE (4 calls), and that the desktop terminal is still alive.
Exhaustive checks: day51_test_write.py (text build).
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d51_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d51_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "d51gui", gui=True, elf="build/kernel.elf")
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
    check("run hello.sw luma -> both lines, no noise", "Hello from hello.sw!" in t and "Hello, luma!" in t and "exited" not in t, t)
    m.png(f"{out}/1_hello.png")
    m.line("sysstat", 2.0)
    t = tail_of("sysstat")
    check("the output went through SYS_WRITE: 4 writes + the exit (Day 52) = 5 calls, last arg len 2", "calls:     5" in t and "0x00000002" in t, t)
    m.png(f"{out}/2_stat.png")
    m.line("sysstat -t", 3.0)
    check("sysstat -t still OK", "sysstat: OK" in tail_of("sysstat -t"))
    m.line("run IDE0/HELLO.SW", 3.0)
    check("FAT copy prints too", "Hello from hello.sw!" in tail_of("run IDE0/HELLO.SW"))
    m.line("ls", 1.5)
    check("terminal still alive (ls works)", any(n in tail_of("ls") for n in ("hello.sw", "putc.sw", "printf.sw", "malloc.sw", "fileio.sw", "explorer.sw")))
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
