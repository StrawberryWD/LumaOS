"""
tests/day48_test_run_gui.py - Day 48 smoke test of `run` on the GUI build.

    ./build.sh && python3 tests/day48_test_run_gui.py [outdir]

Boots the GUI image and types `run hello.sw` etc. into the GUI terminal: checks g_terminal shows exactly the
program's output (no elfrun-style noise), the non-zero exit line, `.sw`-only refusal, and that the desktop
terminal keeps answering (control returned to the shell). Exhaustive checks: day48_test_run.py (text build).
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d48_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d48_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "d48gui", gui=True, elf="build/kernel.elf")
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
    m.line("run IDE3/hello.sw", 3.0)
    t = tail_of("run IDE3/hello.sw")
    check("run IDE3/hello.sw: program text, no noise", "Hello from hello.sw!" in t and "running" not in t and "exited" not in t, t)
    m.png(f"{out}/1_run.png")
    m.line("cd IDE3", 1.0)
    m.line("run hello.sw", 3.0)
    t = tail_of("run hello.sw")
    check("bare name after cd IDE3 (acceptance line)", "Hello from hello.sw!" in t and "exited" not in t, t)
    m.line("run IDE0/HELLO.SW", 3.5)
    t = tail_of("run IDE0/HELLO.SW")
    check("FAT: run IDE0/HELLO.SW", "Hello from hello.sw!" in t, t)
    m.line("run data.sw", 3.5)
    t = tail_of("run data.sw")
    check("data.sw (4 PT_LOAD, self-check 0): .rodata text, no code line",
          "LumaOS Day45 .rodata: segments land at p_vaddr" in t and "exited with code" not in t, t)
    m.line("run notelf.txt", 2.0)
    t = tail_of("run notelf.txt")
    check("non-.sw refused", "not a .sw program" in t, t)
    m.line("run nope.sw", 2.0)
    t = tail_of("run nope.sw")
    check("missing file", "cannot open file" in t, t)
    m.line("ls", 1.5)
    check("terminal still alive (ls works)", any(n in tail_of("ls") for n in ("hello.sw", "putc.sw", "printf.sw", "malloc.sw", "fileio.sw", "explorer.sw")))
    m.png(f"{out}/2_after.png")
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
