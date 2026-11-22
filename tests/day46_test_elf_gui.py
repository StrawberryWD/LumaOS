"""
tests/day46_test_elf_gui.py - Day 46 smoke test on the GUI build.

    ./build.sh && python3 tests/day46_test_elf_gui.py [outdir]

Boots the GUI image, runs `elfrun IDE3/hello.sw`, `elfrun IDE0/HELLO.SW`, `elfrun IDE3/data.sw`
in the GUI terminal and checks g_terminal shows the program's own output and the exit-code line,
that a second run works, that a rejected file is not executed, and that the terminal is still alive.
(Programs run in Ring 0 inside the kernel's own thread of control, so a GUI that keeps redrawing and
answering `ls` afterwards proves the kernel context - stack, segments, EFLAGS - came back intact.)
Exhaustive checks: day46_test_elf_run.py (text build).
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
from elf_fixture import elf_facts

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d46_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d46_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "d46gui", gui=True, elf="build/kernel.elf")
fails = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)
        print(extra)


try:
    scr = m.wait_for("Type 'help'", timeout=150)
    check("GUI booted", "Type 'help'" in scr, scr)
    time.sleep(2)
    m.line("elfrun IDE3/hello.sw", 3.0)
    scr = m.gui_screen()
    tail = scr.split("elfrun IDE3/hello.sw")[-1]
    check("running line", "elfrun: running IDE3/hello.sw at entry 0x%08x" % elf_facts("tests/hello.sw").entry in tail, scr)
    check("program output", "Hello from hello.sw!" in tail, scr)
    check("exit line", "elfrun: IDE3/hello.sw exited with code 0" in tail, scr)
    m.png(f"{out}/1_elfrun.png")
    m.line("elfrun IDE0/HELLO.SW", 3.5)
    tail = m.gui_screen().split("elfrun IDE0/HELLO.SW")[-1]
    check("FAT: output + exit 0", "Hello from hello.sw!" in tail and "exited with code 0" in tail, tail)
    m.line("elfrun IDE3/data.sw", 3.5)
    tail = m.gui_screen().split("elfrun IDE3/data.sw")[-1]
    check("data.sw (4 segments): .rodata text printed, self-check exit 0",
          "LumaOS Day45 .rodata: segments land at p_vaddr" in tail and "exited with code 0" in tail, tail)
    m.line("elfrun IDE3/hello.sw", 3.0)
    tail = m.gui_screen().split("elfrun IDE3/hello.sw")[-1]
    check("second run of hello.sw works (reloaded)", "Hello from hello.sw!" in tail and "exited with code 0" in tail, tail)
    m.line("elfrun IDE0/HELLO.TXT", 2.0)
    tail = m.gui_screen().split("elfrun IDE0/HELLO.TXT")[-1]
    check("non-ELF is not executed", "cannot load" in tail and "nothing was executed" in tail and "running" not in tail, tail)
    m.line("ls", 1.5)
    check("terminal still alive (ls works, hello.sw listed)", "hello.sw" in m.gui_screen().split("ls")[-1])
    m.png(f"{out}/2_after.png")
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
