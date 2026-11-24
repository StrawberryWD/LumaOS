"""tests/day76_test_execve.py - Day 76: execve (SYS_EXECVE 14, libluma execv)
    python3 tests/day76_test_execve.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * build_user.sh warning-free; SYS_EXECVE = 14 in library and kernel; libluma defines execv
  * exec.sw (IDE3 fixture): stage0 -> execv -> stage1 (all failure paths keep the old program alive,
    heap/fd table are fresh) -> execv -> stage2 -> execv(args.sw) with exact argv; exit code 0
  * exit code of the LAST program is the run's exit code (execv of err.sw-style: `run exec.sw 9` exits 7 via stage 3)
  * no frame leak: pginfo shows 2048 of 2048 frames free before and after (also after a failed run)
  * GUI (std + vmware): exec.sw works there too
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
def run_prog(m, cmd, wait=9.0):
    m.line("clear", 0.5); m.line(cmd, wait); return m.text_screen()

p = subprocess.run(["tools/build_user.sh", "user-out"], capture_output=True, text=True)
check("build_user.sh succeeds, no warnings", p.returncode == 0 and "warning" not in p.stderr, p.stderr[-800:])
ul = open("user/include/luma.h").read(); ks = open("kernel/syscall/syscall.h").read()
u = re.search(r"#define\s+SYS_EXECVE\s+(\d+)", ul); k = re.search(r"#define\s+SYS_EXECVE\s+(\d+)", ks)
check("SYS_EXECVE = 14 in library and kernel", u and k and u.group(1) == k.group(1) == "14")
syms = subprocess.run("nm user-out/libluma.a", shell=True, capture_output=True, text=True).stdout
check("libluma.a defines execv", re.search(r" T execv$", syms, re.M) is not None)

def verify(tag, s):
    q = squash(s)
    check("%s: stage 0 -> 1 (fresh heap and fd table)" % tag, "stage0:heap=okfile=open" in q and "stage1:argc=4<1><twowords><>" in q, s[-900:])
    check("%s: stage 1 failure paths all OK" % tag, "exec.sw:stage1allchecksOK" in q and "FAIL" not in q, s[-900:])
    check("%s: stage 2 -> args.sw with exact argv" % tag,
          "argc=4" in q and "argv[0]=<args.sw>" in q and "argv[1]=<fromexec>" in q and "argv[2]=<>" in q and "argv[3]=<end>" in q, s[-900:])
    check("%s: exit code 0 (no 'exited with code')" % tag, "exitedwithcode" not in q, s[-400:])

shutil.copyfile("lumaos-5.0-text.img", "build-text/test-d76a.img")
m = Machine("build-text/test-d76a.img", "d76a", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    m.line("cd IDE3", 1.0)
    s = run_prog(m, "pginfo", 1.5)
    check("frame pool full before", "2048of2048framesfree" in squash(s), s[-300:])
    verify("run exec.sw", run_prog(m, "run exec.sw"))
    s = run_prog(m, "pginfo", 1.5)
    check("no frame leak after 3 execv hops", "2048of2048framesfree" in squash(s), s[-300:])
    verify("second run (state was reset)", run_prog(m, "run exec.sw"))
    s = run_prog(m, "run exec.sw 3", 8); q = squash(s)
    check("stage 3 exit code is the run's exit code", "stage3:exiting7" in q and "exitedwithcode7" in q, s[-500:])
    s = run_prog(m, "run exec.sw 4", 8); q = squash(s)
    check("execv from stage 4 to a missing program fails, program continues, exit 5", "stage4:execvfailed:Nosuchfileordirectory" in q and "exitedwithcode5" in q, s[-500:])
    s = run_prog(m, "pginfo", 1.5)
    check("no frame leak after all runs", "2048of2048framesfree" in squash(s), s[-300:])
    s = run_prog(m, "run args.sw a b", 3)
    check("plain run still works", "argv[2]=<b>" in squash(s))
finally:
    m.close()

for vga in ("std", "vmware"):
    shutil.copyfile("lumaos-5.0.img", "build-text/test-d76g%s.img" % vga)
    g = Machine("build-text/test-d76g%s.img" % vga, "d76g" + vga, gui=True, elf="build/kernel.elf", vga=vga)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("cd IDE3", 1.0)
        g.line("run exec.sw", 12.0)
        s = g.gui_screen()
        check("[%s] GUI: exec chain works (stage1 OK + args.sw output)" % vga,
              "exec.sw: stage1 all checks OK" in s and "argv[3]=<end>" in s, s[-600:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
