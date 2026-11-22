"""
tests/day44_test_elf_gui.py - Day 44 smoke test on the GUI build.

    ./build.sh && python3 tests/day44_test_elf_gui.py [outdir]

Boots the GUI image (default VGA), types `elfload IDE3/hello.sw` and
`elfload IDE0/HELLO.SW` in the GUI terminal and checks g_terminal (the
scrollback the window paints) shows Magic OK / Entry / the LOAD row / the OK
verdict; one bad input shows the FAIL line; a screenshot goes to outdir
(default /tmp/d44_gui) for eyeballing. The same kernel code runs in the text
build (tests/day44_test_elf_load.py has the exhaustive checks).
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
from elf_fixture import elf_facts

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d44_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d44_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "d44gui", gui=True, elf="build/kernel.elf")
fails = []
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(extra)
try:
    scr = m.wait_for("Type 'help'", timeout=150)
    check("GUI booted", "Type 'help'" in scr, scr)
    time.sleep(2)

    m.line("elfload IDE3/hello.sw", 2.5)
    scr = m.gui_screen()
    tail = scr.split("elfload IDE3/hello.sw")[-1]
    _F = elf_facts(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tests/hello.sw"))
    _fs, _ms = _F.load[0]["filesz"], _F.load[0]["memsz"]
    check("LMFS: size line", "IDE3/hello.sw: %d bytes" % _F.size in tail, scr)
    check("LMFS: Magic OK", "Magic:   OK" in tail, scr)
    check("LMFS: Entry == e_entry of the file", "Entry:   0x%08x" % _F.entry in tail, scr)
    check("LMFS: LOAD row", "LOAD       0x00000000 0x08048000 0x%08x 0x%08x R E 0x1000" % (_fs, _ms) in tail, scr)
    check("LMFS: OK verdict", "elfload: OK" in tail, scr)
    m.png(f"{out}/1_lmfs.png")

    m.line("elfload IDE0/HELLO.SW", 2.5)
    scr = m.gui_screen()
    tail = scr.split("elfload IDE0/HELLO.SW")[-1]
    check("FAT: OK verdict", "elfload: OK" in tail and "Entry:   0x%08x" % _F.entry in tail, scr)

    m.line("elfload IDE0/HELLO.TXT", 2.0)
    scr = m.gui_screen()
    tail = scr.split("elfload IDE0/HELLO.TXT")[-1]
    check("non-ELF: FAIL line", "not an ELF file" in tail and "elfload: OK" not in tail, scr)
    m.png(f"{out}/2_fail.png")

    m.line("ls", 1.0)
    scr = m.gui_screen()
    check("terminal still alive (ls works, hello.sw listed)", "hello.sw" in scr.split("$ ls")[-1] or "hello.sw" in scr.split("ls")[-1], scr)
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
