"""
tests/day43_test_write_lmfs.py - Day 43 PRE① (write on LMFS), TEXT build.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day43_test_write_lmfs.py

Checks, on the default LMFS volume (IDE3):
  1. `write new.txt` creates the file and opens the editor (no more
     "invalid arguments"), Ctrl+S saves, `read new.txt` shows the text.
  2. `write new.txt` again -> overwrite prompt; N cancels; Y overwrites.
  3. Esc on a brand-new file leaves nothing behind (not in `ls`).
  4. Multi-level path: `write kday42/deep.txt` works; `write nodir/x.txt` fails cleanly.
  5. `write kday42` (a directory) is refused.
"""
import sys, time, os
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

import shutil
SRC = "lumaos-5.0-text.img"
IMG = "/tmp/d43_wl.img"          # fresh copy: the test writes to disk
shutil.copyfile(SRC, IMG)
m = Machine(IMG, "wl", gui=False)
fails = []
def check(name, cond, scr=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(scr)

try:
    scr = m.wait_for("/ >", timeout=150)
    check("boot to prompt", "/ >" in scr, scr)

    m.line("write new.txt", 1.0)
    scr = m.text_screen()
    check("editor opens on LMFS", "editing" in scr and "invalid arguments" not in scr, scr)
    m.type("hello lmfs"); m.key("ret"); m.type("line two")
    m.ctrl_s(); time.sleep(1.0)
    scr = m.text_screen()
    check("saved message", "wrote 19 bytes" in scr, scr)

    m.line("read new.txt", 1.0)
    scr = m.text_screen()
    check("read shows content", "hello lmfs" in scr and "line two" in scr, scr)

    m.line("write new.txt", 1.0)
    scr = m.text_screen()
    check("overwrite prompt", "ALREADY EXISTS" in scr, scr)
    m.key("n"); time.sleep(0.6)
    scr = m.text_screen()
    check("N cancels", "Cancelled" in scr, scr)

    m.line("write new.txt", 1.0)
    m.key("y"); time.sleep(0.8)
    m.type("second"); m.ctrl_s(); time.sleep(1.0)
    m.line("read new.txt", 1.0)
    scr = m.text_screen()
    check("Y overwrites", "second" in scr and "hello lmfs" not in scr.split("read new.txt")[-1], scr)

    m.line("write gone.txt", 1.0)
    m.type("zzz"); m.key("esc"); time.sleep(0.8)
    m.line("ls", 1.0)
    scr = m.text_screen()
    check("Esc discards new file", "gone.txt" not in scr.split("ls")[-1] and "new.txt" in scr, scr)

    m.line("write kday42/deep.txt", 1.0)
    m.type("deep"); m.ctrl_s(); time.sleep(1.0)
    m.line("read kday42/deep.txt", 1.0)
    scr = m.text_screen()
    check("multi-level path write+read", "deep" in scr.split("read kday42/deep.txt")[-1], scr)

    m.line("write nodir/x.txt", 1.0)
    scr = m.text_screen()
    check("missing parent fails cleanly", "cannot create" in scr, scr)

    m.line("write kday42", 1.0)
    scr = m.text_screen()
    check("directory refused", "is a directory" in scr, scr)
finally:
    m.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
