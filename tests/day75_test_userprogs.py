"""tests/day75_test_userprogs.py - Day 75: cat / ls / explorer as user programs (SYS_READDIR/STAT/GETCWD).
    python3 tests/day75_test_userprogs.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * build_user.sh warning-free; syscall numbers 11/12/13 match library and kernel
  * fsops.sw (IDE3 fixture only): all readdir/stat/getcwd checks incl. every errno path, on RAM0, CD0/ETC, IDE3
  * cat / cat -n / missing file exit code 1 / ls.sw -l / ls.sw multiple paths / explorer totals
  * external-command fallback (`cat fixture.txt`), unknown command still says "Unknown:"
  * GUI (std + vmware): `cat` runs in the terminal window
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
def run_prog(m, cmd, wait=8.0):
    m.line("clear", 0.5); m.line(cmd, wait); return m.text_screen()

p = subprocess.run(["tools/build_user.sh", "user-out"], capture_output=True, text=True)
check("build_user.sh succeeds, no warnings", p.returncode == 0 and "warning" not in p.stderr, p.stderr[-800:])
ul = open("user/include/luma.h").read(); ks = open("kernel/syscall/syscall.h").read()
for name, num in (("SYS_READDIR", "11"), ("SYS_STAT", "12"), ("SYS_GETCWD", "13")):
    u = re.search(r"#define\s+%s\s+(\d+)" % name, ul); k = re.search(r"#define\s+%s\s+(\d+)" % name, ks)
    check("%s = %s in library and kernel" % (name, num), u and k and u.group(1) == k.group(1) == num)
syms = subprocess.run("nm user-out/libluma.a", shell=True, capture_output=True, text=True).stdout
check("libluma.a defines readdir stat getcwd qsort", all(re.search(r" T %s$" % f, syms, re.M) for f in "readdir stat getcwd qsort".split()))

# ---- fsops.sw lives only on the HD fixture (the CD's LMFS root is full): boot from the HD alone ----
shutil.copyfile("lumaos-5.0-text.img", "build-text/test-d75h.img")
m = Machine("build-text/test-d75h.img", "d75h", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    for d in ("IDE3", "IDE0"):
        s = run_prog(m, "run IDE3/fsops.sw " + d); q = squash(s)
        check("fsops.sw %s: all checks OK, no FAIL" % d, "fsops.sw:allchecksOK" in q and "FAIL" not in q, s[-900:])
        check("fsops.sw %s: exit code 0" % d, "exitedwithcode" not in q, s[-400:])
finally:
    m.close()

m = Machine(None, "d75a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    # cat
    s = run_prog(m, "cat fixture.txt"); q = squash(s)
    check("external fallback: `cat fixture.txt` prints the file's last line", "120" in q and "Unknown" not in q, s[-500:])
    s = run_prog(m, "cat -n empty.txt fixture.txt nosuch", 10); q = squash(s)
    check("cat -n numbers lines up to 120", "120" in q, s[-500:])
    check("cat: missing file reported, exit code 1", "cat:nosuch:Nosuchfileordirectory" in q and "exitedwithcode1" in q, s[-600:])
    s = run_prog(m, "ls.sw -l"); q = squash(s)
    check("ls.sw -l lists files with sizes", "fixture.txt" in q and "6120" in q, s[-600:])
    s = run_prog(m, "ls.sw CD0/ETC nosuch CD0/BOOT"); q = squash(s)
    check("ls.sw with several paths: headers + ENOENT", "CD0/BOOT:" in q and "CD0/ETC:" in q and "nosuch" in q and "Nosuchfile" in q, s[-700:])
    s = run_prog(m, "explorer", 12); q = squash(s)
    check("explorer prints volume trees with totals", "directories," in q and "files," in q, s[-700:])
    s = run_prog(m, "nosuchcmd", 2)
    check("unknown command still reports Unknown:", "Unknown:" in s, s[-300:])
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(None, "d75g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("cat fixture.txt", 6.0)
        s = g.gui_screen()
        check("[%s] GUI: cat prints the fixture" % vga, "120" in s and "Unknown" not in s, s[-400:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
