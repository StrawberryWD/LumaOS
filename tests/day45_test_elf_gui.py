"""
tests/day45_test_elf_gui.py - Day 45 smoke test on the GUI build.

    ./build.sh && python3 tests/day45_test_elf_gui.py [outdir]

Boots the GUI image, runs `elfmap IDE3/data.sw` and `elfmap IDE0/HELLO.SW` in the GUI
terminal, checks g_terminal shows the segment lines / [ok] / OK verdict, and that the guest
physical memory (monitor `xp`) holds data.sw's bytes. Exhaustive checks: day45_test_elf_map.py.
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
from elf_fixture import elf_facts

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d45_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d45_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
data = open("tests/data.sw", "rb").read()
F = elf_facts("tests/data.sw")      # every address below comes from the file, not from this script
D = F.data; BSS, BSSLEN = F.bss
m = Machine(IMG, "d45gui", gui=True, elf="build/kernel.elf")
fails = []
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(extra)
try:
    scr = m.wait_for("Type 'help'", timeout=150)
    check("GUI booted", "Type 'help'" in scr, scr)
    time.sleep(2)
    m.line("elfmap IDE3/data.sw", 3.5)
    scr = m.gui_screen()
    tail = scr.split("elfmap IDE3/data.sw")[-1]
    check("size line", "IDE3/data.sw: %d bytes" % len(data) in tail, scr)
    check("4 segments listed, all [ok]", tail.count("[ok]") == 4, scr)
    check(".bss line is zero", "bss %08x: 00 00 00 00 00 00 00 00" % BSS in tail, scr)
    check("OK verdict", "elfmap: OK - image loaded, entry 0x%08x" % F.entry in tail, scr)
    check("guest memory at .data (0x%08x) == data.sw .data bytes" % D["vaddr"], m._read_bytes(D["vaddr"], D["filesz"]) == data[D["offset"]:D["offset"] + D["filesz"]])
    check("guest .bss zero", m._read_bytes(BSS, BSSLEN) == b"\x00" * BSSLEN)
    m.png(f"{out}/1_elfmap.png")
    m.line("elfmap IDE0/HELLO.SW", 3.0)
    tail = m.gui_screen().split("elfmap IDE0/HELLO.SW")[-1]
    check("FAT: OK verdict", "elfmap: OK" in tail, tail)
    m.line("elfmap IDE0/HELLO.TXT", 2.0)
    tail = m.gui_screen().split("elfmap IDE0/HELLO.TXT")[-1]
    check("non-ELF: FAIL line, untouched memory notice", "elfmap: FAIL" in tail and "memory untouched" in tail, tail)
    m.line("ls", 1.0)
    check("terminal still alive", "hello.sw" in m.gui_screen().split("ls")[-1])
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
