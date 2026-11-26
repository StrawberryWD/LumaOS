"""tests/day77b_test_write.py - Day 77b: writing files (open O_WRONLY/O_CREAT/..., write(fd), unlink, mkdir, rename, fsync, fopen w/a/+)
    python3 tests/day77b_test_write.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * build_user.sh warning-free; SYS_UNLINK/MKDIR/RENAME/FSYNC = 20..23 in library and kernel; libluma defines them
  * wfile.sw on IDE3 (HD boot, LMFS): 14 scenarios (create/append/trunc, lseek + zero fill, error paths, fsync, 3-write-fd limit,
    stdio modes, 200000-byte file and EFBIG at 256KB, unlink, mkdir, rename, fork does not share write fds)
  * a program that exits without close still gets its data written (leak/check), and data survives a reboot (persist.txt)
  * RAM0 (CD boot, in-memory LMFS): the same scenarios; writing on the CD (CD0) fails with EROFS
  * GUI (std + vmware): wfile.sw all checks OK
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
def squash(s): return re.sub(r"\s+", "", s)
def run_prog(m, cmd, wait=12.0):
    m.line("clear", 0.5); m.line(cmd, wait); return m.text_screen()

p = subprocess.run(["tools/build_user.sh", "user-out"], capture_output=True, text=True)
check("build_user.sh succeeds, no warnings", p.returncode == 0 and "warning" not in p.stderr, p.stderr[-800:])
ul = open("user/include/luma.h").read(); ks = open("kernel/syscall/syscall.h").read()
for name, num in (("SYS_UNLINK", "20"), ("SYS_MKDIR", "21"), ("SYS_RENAME", "22"), ("SYS_FSYNC", "23")):
    u = re.search(r"#define\s+%s\s+(\d+)" % name, ul); k = re.search(r"#define\s+%s\s+(\d+)" % name, ks)
    check("%s = %s in library and kernel" % (name, num), u and k and u.group(1) == k.group(1) == num)
syms = subprocess.run("nm user-out/libluma.a", shell=True, capture_output=True, text=True).stdout
check("libluma.a defines unlink mkdir rename fsync", all(re.search(r" T %s$" % f, syms, re.M) for f in "unlink mkdir rename fsync".split()))

def verify(tag, s):
    q = squash(s)
    check("%s: wfile.sw all checks OK, no FAIL" % tag, "wfile.sw:allchecksOK" in q and "FAIL" not in s, s[-1500:])
    for n in range(1, 14):
        check("%s: scenario %d reached" % (tag, n), re.search(r"(^|\s)%d:" % n, s) is not None or ("%d:" % n) in s, s[-300:])

IMG = "build-text/test-d77w.img"
shutil.copyfile("lumaos-5.0-text.img", IMG)
m = Machine(IMG, "d77w", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    m.line("cd IDE3", 1.0)
    s = run_prog(m, "pginfo", 1.5)
    check("frame pool full before", "2048of2048framesfree" in squash(s), s[-300:])
    verify("IDE3", run_prog(m, "run wfile.sw IDE3"))
    s = run_prog(m, "pginfo", 1.5)
    check("no frame leak after wfile.sw", "2048of2048framesfree" in squash(s), s[-300:])
    verify("IDE3 second run (files from the first run exist)", run_prog(m, "run wfile.sw IDE3"))
    s = run_prog(m, "run wfile.sw IDE3 leak", 5)
    check("leak: program exits without close", "leak:exitingwithoutclose" in squash(s), s[-300:])
    s = run_prog(m, "run wfile.sw IDE3 check", 5)
    check("leak: the kernel wrote the unclosed file back at exit", "wfile.swcheck:allchecksOK" in squash(s), s[-500:])
    s = run_prog(m, "run cat.sw IDE3/persist.txt", 4)
    check("shell sees the file written by a program", "survivesreboot" in squash(s), s[-300:])
    s = run_prog(m, "run args.sw a b", 3)
    check("plain run still works", "argv[2]=<b>" in squash(s))
finally:
    m.close()

# reboot: same image, new QEMU
m = Machine(IMG, "d77w2", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    m.line("cd IDE3", 1.0)
    s = run_prog(m, "run wfile.sw IDE3 check", 6)
    check("data survives a reboot (persist.txt, leak.txt read back from the disk image)", "wfile.swcheck:allchecksOK" in squash(s), s[-500:])
finally:
    m.close()

m = Machine(None, "d77r", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    verify("RAM0", run_prog(m, "run wfile.sw RAM0"))
    s = run_prog(m, "run wfile.sw RAM0 rofs CD0/ETC/FIXTURE.TXT", 6)
    check("writing on the CD fails with EROFS (open, create, unlink, mkdir, fopen)", "wfile.swrofs:allchecksOK" in squash(s), s[-500:])
finally:
    m.close()

for vga in ("std", "vmware"):
    shutil.copyfile("lumaos-5.0.img", "build-text/test-d77wg%s.img" % vga)
    g = Machine("build-text/test-d77wg%s.img" % vga, "d77wg" + vga, gui=True, elf="build/kernel.elf", vga=vga)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("cd IDE3", 1.0)
        g.line("run wfile.sw IDE3", 20.0)
        s = g.gui_screen()
        check("[%s] GUI: wfile.sw all checks OK" % vga, "wfile.sw: all checks OK" in s, s[-600:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
