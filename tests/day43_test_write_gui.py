"""
tests/day43_test_write_gui.py - Day 43 PRE② (GUI `write` must not freeze).

    ./build.sh && python3 tests/day43_test_write_gui.py [outdir]

Boots the GUI image, types `write new.txt` in the GUI terminal and checks:
  - the framebuffer CHANGES while the editor is open and keys are typed
    (screendump hash) -> the GUI loop is alive, not frozen;
  - Ctrl+S returns to the terminal and g_terminal shows "wrote N bytes";
  - a second `write new.txt` shows the Y/N dialog (screenshot), N cancels,
    and the terminal still accepts commands afterwards (`ls`);
  - Esc cancels a fresh file.
PNG screenshots go to outdir (default /tmp/d43_gui).
"""
import sys, time, os, shutil
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d43_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d43_gui.img"
shutil.copyfile("lumaos-5.0.img", IMG)
m = Machine(IMG, "wg", gui=True, elf="build/kernel.elf")
fails = []
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(extra)
try:
    scr = m.wait_for("Type 'help'", timeout=150)
    check("GUI booted", "Type 'help'" in scr, scr)
    time.sleep(2)
    m.png(f"{out}/0_boot.png")

    m.line("write new.txt", 1.5)
    h1 = m.hash_screen()
    m.png(f"{out}/1_editor.png")
    m.type("hello gui"); m.key("ret"); m.type("two")
    time.sleep(0.5)
    h2 = m.hash_screen()
    check("framebuffer changes while editing (not frozen)", h1 != h2)
    m.png(f"{out}/2_typed.png")
    m.ctrl_s(); time.sleep(1.5)
    scr = m.gui_screen()
    check("saved, back at terminal", "wrote 13 bytes to IDE3/new.txt" in scr, scr)
    m.png(f"{out}/3_saved.png")

    m.line("write new.txt", 1.5)
    m.png(f"{out}/4_confirm.png")
    m.key("n"); time.sleep(1.0)
    scr = m.gui_screen()
    check("N cancels", "Cancelled" in scr, scr)

    m.line("write gone.txt", 1.5)
    m.type("xx"); m.key("esc"); time.sleep(1.0)
    m.line("ls", 1.0)
    scr = m.gui_screen()
    check("terminal alive after Esc; gone.txt not created",
          "new.txt" in scr and "gone.txt  " not in scr.split("$ ls")[-1] and "gone.txt" not in scr.split("ls")[-1], scr)
    m.png(f"{out}/5_after.png")
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
