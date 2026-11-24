"""
tests/day49_test_args_gui.py - Day 49 smoke test of argc/argv on the GUI build.

    ./build.sh && python3 tests/day49_test_args_gui.py [outdir]

Boots the GUI image, types `run hello.sw luma` (the Day 49 acceptance line) and `run args.sw ...` into the GUI
terminal. Exhaustive checks: day49_test_args.py (text build) and day47_elf_host_test.c.
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d49_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d49_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "d49gui", gui=True, elf="build/kernel.elf")
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
    m.line("run hello.sw luma", 3.0)
    t = tail_of("run hello.sw luma")
    check("run hello.sw luma -> 'Hello, luma!' (acceptance line)", "Hello from hello.sw!" in t and "Hello, luma!" in t, t)
    m.png(f"{out}/1_luma.png")
    m.line('run args.sw one "two words" ""', 3.0)
    t = tail_of('run args.sw one')
    check("args.sw: argc=4 and quoted/empty args",
          "argc=4" in t and "argv[2]=<two words>" in t and "argv[3]=<>" in t and "argv[argc]=NULL" in t, t)
    m.png(f"{out}/2_args.png")
    m.line("run hello.sw", 3.0)
    t = tail_of("run hello.sw")
    check("no argument: no greeting line", "Hello from hello.sw!" in t and "Hello," not in t, t)
    m.line('run args.sw "abc', 2.0)
    check("unterminated quote reported", "unterminated quote" in tail_of('run args.sw "abc'))
    m.line("ls", 1.5)
    check("terminal still alive (ls works)", any(n in tail_of("ls") for n in ("hello.sw", "putc.sw", "printf.sw", "malloc.sw", "fileio.sw", "explorer.sw")))
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
