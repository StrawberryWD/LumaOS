"""
tests/day55_test_ring3_gui.py - Day 55 smoke test of Ring 3 programs on the GUI build.

    ./build.sh && python3 tests/day55_test_ring3_gui.py [outdir]

Programs now run in Ring 3 (iret, cs 0x23) while the GUI keeps drawing (timer/keyboard/mouse IRQs arrive in Ring 3
and are served on the TSS.esp0 kernel stack). Checks: desktop, `run hello.sw`, `sysstat` shows the last syscall came
from cs 0x23 inside the runtime page, `elfrun -v` (via sys_exit, 18 pages), a few other fixtures, terminal alive.
Exhaustive: day55_test_ring3.py (text).
"""
import sys, time, os, shutil, re
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d55_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d55_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "d55gui", gui=True, elf="build/kernel.elf")
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
    m.line("cd IDE3", 1.0)
    m.line("run hello.sw luma", 3.0)
    check("run hello.sw luma works in Ring 3", "Hello, luma!" in tail_of("run hello.sw luma"))
    m.line("sysstat", 2.0)
    t = tail_of("sysstat")
    check("sysstat: last syscall from cs 0x23 (Ring 3, hello.sw int 0x80), not cs 0x08",
          re.search(r"from 0x23:0x0804[0-9a-f]{4}", t) is not None, t)
    m.png(f"{out}/1_sysstat.png")
    m.line("elfrun -v hello.sw", 4.0)
    t = " ".join(tail_of("elfrun -v hello.sw").split())
    check("elfrun -v: exit 0, left via sys_exit, 18 pages", "exited with code 0" in t and "left via sys_exit" in t
          and "address space: 18 pages, 22 frames" in t, t)
    m.png(f"{out}/2_elfrun.png")
    m.line("sysstat -t", 3.0)
    check("kernel-side syscall self-test still OK", "sysstat: OK" in tail_of("sysstat -t"))
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
