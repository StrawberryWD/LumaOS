"""tests/day73_test_malloc.py - Day 73: user-mode dynamic memory (SYS_BRK + libluma malloc/free/calloc/realloc).
    python3 tests/day73_test_malloc.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * tools/build_user.sh builds libluma.a (string.c, malloc.c) + malloc.sw warning-free; SYS_BRK number matches the kernel
  * `run malloc.sw` on live CD (RAM0), hard disk (IDE3), GUI std + vmware: ~60 self-checks (sbrk/brk, alignment, reuse,
    coalescing, calloc/realloc, 3000-op random stress with pattern checks + heap invariant walks, 2 MB block returned to the
    kernel, exhaustion -> NULL/ENOMEM, recovery)
  * double free -> "free(): invalid pointer", exit code 134;  write past the break -> page fault kills the program
  * the physical frame pool is back to the same size after the heap hog (no frame leaks), and the program can run again
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
def squash(s): return re.sub(r"\s+", "", s)

p = subprocess.run(["tools/build_user.sh", "user-out"], capture_output=True, text=True)
check("tools/build_user.sh succeeds with no compiler warnings", p.returncode == 0 and "warning" not in p.stderr, p.stderr[-800:])
ud = re.search(r"#define\s+SYS_BRK\s+(\d+)", open("user/include/luma.h").read())
kd = re.search(r"#define\s+SYS_BRK\s+(\d+)", open("kernel/syscall/syscall.h").read())
check("SYS_BRK number is the same in the library and the kernel", ud and kd and ud.group(1) == kd.group(1) == "6")
syms = subprocess.run("nm user-out/libluma.a", shell=True, capture_output=True, text=True).stdout
check("libluma.a defines malloc free calloc realloc sbrk brk memcpy memset strcmp",
      all(re.search(r" T %s$" % f, syms, re.M) for f in "malloc free calloc realloc sbrk brk memcpy memset strcmp".split()), syms[-300:])

def run_prog(m, cmd, wait=9.0):
    m.line("clear", 0.5); m.line(cmd, wait)
    return m.text_screen()
def pool(m):
    s = run_prog(m, "pginfo", 1.5)
    r = re.search(r"frame pool:\s*(\d+) of (\d+)", s)
    return int(r.group(1)) if r else None

def verify(tag, s):
    q = squash(s)
    check("%s: malloc.sw: all checks OK, no FAIL line" % tag, "malloc.sw:allchecksOK" in q and "FAIL" not in q, s[-700:])
    check("%s: exhaustion line + heap summary printed" % tag, "exhaustion:got" in q and "heap:32768bytes,used0in0blocks" in q, s[-700:])
    check("%s: exit code 0" % tag, "exitedwithcode" not in q, s[-700:])

m = Machine(None, "d73a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    f0 = pool(m)
    verify("CD/RAM0", run_prog(m, "run malloc.sw"))
    f1 = pool(m)
    check("frame pool unchanged after the heap hog (%s -> %s)" % (f0, f1), f0 is not None and f0 == f1)
    s = run_prog(m, "run malloc.sw crash"); q = squash(s)
    check("double free -> 'free(): invalid pointer' + exit code 134", "free():invalidpointer" in q and "exitedwithcode134" in q, s[-500:])
    s = run_prog(m, "run malloc.sw w"); q = squash(s)
    check("write past the break -> page fault kills the program", "killed" in q and "pagefault" in q and "notmapped" in q, s[-500:])
    f2 = pool(m)
    check("frame pool unchanged after crash and fault (%s -> %s)" % (f0, f2), f0 == f2)
    verify("CD/RAM0 again", run_prog(m, "run malloc.sw"))
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-text.img", "d73b"), "d73b", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    verify("HD/IDE3", run_prog(m, "run IDE3/malloc.sw"))
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(None, "d73g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("run malloc.sw", 12.0)
        s = g.gui_screen()
        check("[%s] GUI: malloc.sw all checks OK" % vga, "malloc.sw: all checks OK" in s, s[-600:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
