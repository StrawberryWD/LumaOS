"""tests/day77_test_fork.py - Day 77: fork prototype (SYS_FORK 15, SYS_WAIT 16, SYS_GETPID 17, SYS_YIELD 18)
    python3 tests/day77_test_fork.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * build_user.sh warning-free; syscall numbers match; libluma defines fork wait getpid yield
  * fork.sw (IDE3 fixture): independent globals/heap, 3 children + ECHILD, independent fd offsets, yield ping-pong
    (pcpcpc), execv in a child, crashing child (killed, parent survives), grandchild, process table full (EAGAIN
    after 7 children), child-grown heap
  * `run fork.sw orphan`: the parent exits first, the child still runs to completion before the prompt returns
  * no frame leak (pginfo 2048 of 2048 before/after, also after the orphan run); programs without fork unaffected
  * GUI (std + vmware)
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
def run_prog(m, cmd, wait=10.0):
    m.line("clear", 0.5); m.line(cmd, wait); return m.text_screen()

p = subprocess.run(["tools/build_user.sh", "user-out"], capture_output=True, text=True)
check("build_user.sh succeeds, no warnings", p.returncode == 0 and "warning" not in p.stderr, p.stderr[-800:])
ul = open("user/include/luma.h").read(); ks = open("kernel/syscall/syscall.h").read()
for name, num in (("SYS_FORK", "15"), ("SYS_WAIT", "16"), ("SYS_GETPID", "17"), ("SYS_YIELD", "18")):
    u = re.search(r"#define\s+%s\s+(\d+)" % name, ul); k = re.search(r"#define\s+%s\s+(\d+)" % name, ks)
    check("%s = %s in library and kernel" % (name, num), u and k and u.group(1) == k.group(1) == num)
syms = subprocess.run("nm user-out/libluma.a", shell=True, capture_output=True, text=True).stdout
check("libluma.a defines fork wait getpid yield", all(re.search(r" T %s$" % f, syms, re.M) for f in "fork wait getpid yield".split()))

def verify(tag, s):
    q = squash(s)
    check("%s: fork.sw all checks OK, no FAIL" % tag, "fork.sw:allchecksOK" in q and "FAIL" not in q, s[-1200:])
    check("%s: child runs only after the parent waits (output order)" % tag,
          q.find("1:parentforked,childhasnotrunyet") < q.find("1:childruns,exits42") < q.find("1:wait->pid2status42"), s[-1200:])
    check("%s: yield ping-pong is exactly pcpcpc" % tag, "pcpcpc" in q, s[-1200:])
    check("%s: process table full after 7 children" % tag, "8:processtablefullafter7children,usableagain" in q, s[-1200:])
    check("%s: exit code 0" % tag, "exitedwithcode" not in q, s[-300:])

shutil.copyfile("lumaos-5.0-text.img", "build-text/test-d77a.img")
m = Machine("build-text/test-d77a.img", "d77a", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    m.line("cd IDE3", 1.0)
    verify("run fork.sw", run_prog(m, "run fork.sw", 12))
    s = run_prog(m, "pginfo", 1.5)
    check("no frame leak after fork.sw", "2048of2048framesfree" in squash(s), s[-300:])
    s = run_prog(m, "run fork.sw orphan", 8); q = squash(s)
    check("orphan: parent exits first, child still completes before the prompt",
          "orphan:parentexitsfirst" in q and "orphan:childstarts" in q and "orphan:childstillrunsaftertheparentexited" in q, s[-600:])
    check("orphan: order parent, child start, child end",
          q.find("parentexitsfirst") < q.find("childstarts") < q.find("childstillruns"), s[-600:])
    s = run_prog(m, "pginfo", 1.5)
    check("no frame leak after the orphan run", "2048of2048framesfree" in squash(s), s[-300:])
    verify("second run (state reset)", run_prog(m, "run fork.sw", 12))
    s = run_prog(m, "run exec.sw", 10)
    check("programs without fork unaffected (exec.sw chain)", "exec.sw:stage1allchecksOK" in squash(s), s[-500:])
    s = run_prog(m, "run args.sw a b", 3)
    check("plain run still works", "argv[2]=<b>" in squash(s))
finally:
    m.close()

for vga in ("std", "vmware"):
    shutil.copyfile("lumaos-5.0.img", "build-text/test-d77g%s.img" % vga)
    g = Machine("build-text/test-d77g%s.img" % vga, "d77g" + vga, gui=True, elf="build/kernel.elf", vga=vga)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("cd IDE3", 1.0)
        g.line("run fork.sw", 15.0)
        s = g.gui_screen()
        check("[%s] GUI: fork.sw all checks OK" % vga, "fork.sw: all checks OK" in s, s[-600:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
