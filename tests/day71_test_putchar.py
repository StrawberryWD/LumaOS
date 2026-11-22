"""tests/day71_test_putchar.py - Day 71: user-mode output (libluma: crt0 + putchar/puts/fputc/write/strerror).
    python3 tests/day71_test_putchar.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * the library header's SYS_* numbers and errno values equal the kernel's (no drift)
  * tools/build_user.sh builds libluma.a + putc.sw warning-free; putc.sw is a valid ELF32 i386 executable
  * `run putc.sw` (live CD RAM0, hard disk IDE3, GUI std + vmware): exact output lines, exit code 0
  * argv reaches main() through crt0; main's return value becomes the exit code
"""
import shutil, subprocess, sys, os, re, time
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

# ---- static checks ----
def defs(path, pat):
    return dict((m.group(1), int(m.group(2))) for m in re.finditer(pat, open(path).read(), re.M))
ud = defs("user/include/luma.h", r"^#define\s+((?:SYS_|E)[A-Z0-9_]+)\s+(\d+)")
kd = defs("kernel/syscall/syscall.h", r"^#define\s+(SYS_[A-Z0-9_]+)\s+(\d+)")
ke = defs("kernel/core/errno.h", r"^#define\s+LUMA_(E[A-Z0-9]+)\s+(\d+)")
sysnums = {k: v for k, v in ud.items() if k.startswith("SYS_")}
for k, v in sysnums.items():
    if k in kd: check("SYS number %s matches the kernel" % k, kd[k] == v, (k, v, kd[k]))
check("kernel and library agree on all syscall numbers they share", all(kd.get(k, v) == v for k, v in sysnums.items()))
errs = {k: v for k, v in ud.items() if not k.startswith("SYS_")}
miss = [k for k, v in ke.items() if k not in ("EOK", "ERRNO_MAX") and errs.get(k) != v]
check("every kernel errno is in luma.h with the same value", not miss, miss)
p = subprocess.run(["tools/build_user.sh", "user-out"], capture_output=True, text=True)
check("tools/build_user.sh succeeds with no compiler warnings", p.returncode == 0 and "warning" not in p.stderr, p.stderr[-800:])
hdr = open("user-out/putc.sw", "rb").read(20)
check("putc.sw is ELF32 little-endian i386 executable", hdr[:4] == b"\x7fELF" and hdr[4] == 1 and hdr[5] == 1 and hdr[16] == 2 and hdr[18] == 3, hdr)

EXPECT = ["putchar: ABCDEFGHIJKLMNOPQRSTUVWXYZ", "[A]", "puts: line with newline", "Bad file descriptor", "args: <a> <b>", "putc.sw: all checks OK"]
def verify(tag, s):
    pos = -1
    for line in EXPECT:
        i = s.find(line, pos + 1)
        check("%s: output contains %r in order" % (tag, line), i >= 0, s[-900:])
        if i >= 0: pos = i
    check("%s: no FAIL line, exit code 0 (no 'exited with code')" % tag, "FAIL:" not in s and "exited with code" not in s, s[-900:])

def run_prog(m, cmd, screen):
    m.line("clear", 0.5); m.line(cmd, 3.5)
    return screen()

m = Machine(None, "d71a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    verify("CD/RAM0", run_prog(m, "run putc.sw a b", m.text_screen))
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-text.img", "d71b"), "d71b", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    verify("HD/IDE3", run_prog(m, "run IDE3/putc.sw a b", m.text_screen))
    s = run_prog(m, "run IDE3/putc.sw", m.text_screen)
    check("HD: no args -> no 'args:' line", "args:" not in s and "all checks OK" in s, s[-500:])
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(None, "d71g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("run putc.sw a b", 4.0)
        s = g.gui_screen()
        check("[%s] GUI: putc.sw all checks OK" % vga, "putc.sw: all checks OK" in s and "args: <a> <b>" in s, s[-600:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
