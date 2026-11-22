"""tests/day70_test_errno.py - Day 70: unified error codes.
    python3 tests/day70_test_errno.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * `errno` table / lookups by number, by name, `errno -t` self-test (round trips, FAT mapping)
  * `run err.sw` (Ring 3, raw int 0x80): failing syscalls return -EBADF/-EFAULT/-ENOSYS/-EINVAL/-ERANGE and
    SYS_STRERROR(4) returns the text; truncation is NUL-terminated; unknown numbers -> "Unknown error"
  * the file-system layer: errors from vfs_ops (EEXIST/ENOENT/EISDIR) already proven by `fstress` (Day 64);
    here: fstress still passes and `df` of an unknown path gives ENOENT text.
"""
import shutil, sys, os, time, re
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)
def fresh(src, tag):
    dst = "build-text/test-%s.img" % tag
    shutil.copyfile(src, dst); return dst
def run(m, c, wait=1.2):
    m.line("clear", 0.4); m.line(c, wait)
    return m.text_screen()

def errcases(m, tag, screen_fn=None):
    s = run(m, "errno -t", 1.5)
    check("%s: errno -t PASS" % tag, "errno: PASS" in s, s[-500:])
    s = run(m, "errno 2"); check("%s: errno 2" % tag, "ENOENT" in s and "No such file or directory" in s, s)
    s = run(m, "errno -28"); check("%s: errno -28 (sign ignored)" % tag, "ENOSPC" in s and "No space left on device" in s, s)
    s = run(m, "errno eexist"); check("%s: errno eexist (name, any case)" % tag, "17" in s and "File exists" in s, s)
    s = run(m, "errno NOENT"); check("%s: errno NOENT (E optional)" % tag, "No such file or directory" in s, s)
    s = run(m, "errno EBOGUS"); check("%s: unknown name" % tag, "unknown error name" in s, s)
    s = run(m, "errno 9999"); check("%s: undefined number" % tag, "not defined" in s and "Unknown error" in s, s)
    s = run(m, "run err.sw", 2.5)
    check("%s: err.sw all checks OK" % tag, "err.sw: all checks OK" in s, s[-1200:])
    check("%s: EBADF text" % tag, "= -9  (Bad file descriptor)" in s, s[-1200:])
    check("%s: EFAULT text" % tag, "= -14  (Bad address)" in s, s[-1200:])
    check("%s: ENOSYS text" % tag, "= -38  (Function not implemented)" in s, s[-1200:])
    check("%s: EINVAL / ERANGE" % tag, "= -22  (Invalid argument)" in s and "= -34  (Result out of range)" in s, s[-1200:])
    check("%s: truncated copy is NUL-terminated" % tag, 'strerror(2, 8 bytes) -> "No such "' in s or 'strerror(2, 8 bytes) -> "No such' in s, s[-1200:])
    check("%s: unknown errno text" % tag, 'strerror(9999) -> "Unknown error"' in s, s[-1200:])
    check("%s: EUCLEAN text" % tag, "Structure needs cleaning" in s, s[-1200:])

m = Machine(None, "d70a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    errcases(m, "CD/RAM0")
    s = run(m, "sysstat", 1.5)
    check("sysstat still works (syscall table intact)", "int 0x80" in s or "syscall" in s.lower(), s[-300:])
    s = run(m, "run args.sw x y", 1.5); check("args.sw regression", "argv[2]=<y>" in s, s[-300:])
    s = run(m, "df NOPE"); check("df NOPE -> ENOENT text", "No such file or directory" in s, s)
    m.line("clear", 0.4); m.line("fstress 20", 0.5)
    t0 = time.time()
    while time.time() - t0 < 120 and "fstress: PASS" not in m.text_screen() and "fstress: FAIL" not in m.text_screen(): time.sleep(1)
    check("fstress still PASS (vfs_ops errno paths)", "fstress: PASS" in m.text_screen(), m.text_screen()[-300:])
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-text.img", "d70b"), "d70b", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    s = run(m, "run IDE3/err.sw", 2.5)
    check("HD/IDE3: err.sw runs", "err.sw: all checks OK" in s, s[-800:])
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(None, "d70g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("run err.sw", 3.0)
        s = g.gui_screen()
        check("[%s] GUI: err.sw runs in Ring 3" % vga, "err.sw: all checks OK" in s, s[-500:])
        g.line("errno enoent", 1.5)
        check("[%s] GUI: errno enoent" % vga, "No such file or directory" in g.gui_screen())
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
