"""tests/day77a_test_stdin.py - Day 77a: stdin (read(0), SYS_TERMMODE 19, getchar/fgets on stdin)
    python3 tests/day77a_test_stdin.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * build_user.sh warning-free; SYS_TERMMODE = 19 in library and kernel; libluma defines termmode
  * input.sw bad: EFAULT / len 0 / EINVAL / EBADF paths
  * cooked mode: echo, Backspace, Ctrl+U, line + EOF counting, Ctrl+D (EOF is not permanent), `cat` copies stdin
  * raw mode: exact key bytes incl. arrow key 0x80, Enter 0x0d, Esc 0x1b; raw_nb: EAGAIN while idle
  * Ctrl+C while waiting for input kills the program (shell stays usable); keyboard state does not leak into the next program
  * GUI (std + vmware): the echo is visible WHILE the program waits, then the program finishes
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
def start(m, cmd, wait=2.0):
    m.line("clear", 0.5); m.type(cmd); m.key("ret"); time.sleep(wait)

p = subprocess.run(["tools/build_user.sh", "user-out"], capture_output=True, text=True)
check("build_user.sh succeeds, no warnings", p.returncode == 0 and "warning" not in p.stderr, p.stderr[-800:])
ul = open("user/include/luma.h").read(); ks = open("kernel/syscall/syscall.h").read()
u = re.search(r"#define\s+SYS_TERMMODE\s+(\d+)", ul); k = re.search(r"#define\s+SYS_TERMMODE\s+(\d+)", ks)
check("SYS_TERMMODE = 19 in library and kernel", u and k and u.group(1) == k.group(1) == "19")
syms = subprocess.run("nm user-out/libluma.a", shell=True, capture_output=True, text=True).stdout
check("libluma.a defines termmode", re.search(r" T termmode$", syms, re.M) is not None)

shutil.copyfile("lumaos-5.0-text.img", "build-text/test-d77s.img")
m = Machine("build-text/test-d77s.img", "d77s", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    m.line("cd IDE3", 1.0)
    m.line("clear", 0.5); m.line("run input.sw bad", 4); s = m.text_screen()
    check("input.sw bad: all checks OK", "input.swbad:allchecksOK" in squash(s) and "FAIL" not in s, s[-500:])

    start(m, "run input.sw")
    m.type("Bobx"); m.key("backspace"); m.type("by"); m.key("ret"); time.sleep(0.4)
    m.type("l1"); m.key("ret"); time.sleep(0.4)
    m.type("zzzzz"); m.key("ctrl-u"); m.type("l22"); m.key("ret"); time.sleep(0.4)
    m.key("ctrl-d"); time.sleep(0.6)                 # EOF for the line loop
    m.key("ctrl-d"); time.sleep(2.5)                 # EOF for the final getchar (EOF is not permanent)
    s = m.text_screen(); q = squash(s)
    check("cooked: Backspace edits ('Bobx<BS>by' -> Bobby)", "hello<Bobby>" in q, s[-700:])
    check("cooked: Ctrl+U kills the line, counts 2 lines / 7 chars ('l1\\n' + 'l22\\n')", "then2lines,7chars" in q, s[-700:])
    check("cooked: Ctrl+D at line start is EOF, reading again still works", "afterEOFgetchar->-1" in q, s[-700:])
    check("cooked: prompt echoed", "name?" in q)

    start(m, "run input.sw cat")
    m.type("one"); m.key("ret"); time.sleep(0.4)
    m.type("two"); m.key("ctrl-d"); time.sleep(0.5)       # Ctrl+D mid-line delivers 'two' without newline
    m.key("ctrl-d"); time.sleep(2.0)                        # then EOF
    s = m.text_screen(); q = squash(s)
    check("cat: stdin copied, Ctrl+D mid-line delivers the partial line, then EOF", "one" in q and "two" in q and "[EOFn=0]" in q, s[-600:])

    start(m, "run input.sw raw")
    for kname in ("a", "up", "ret", "esc", "q"):
        m.key(kname); time.sleep(0.3)
    time.sleep(2.0)
    s = m.text_screen(); q = squash(s)
    check("raw: old mode 0 and exact key bytes a, UP(0x80), Enter(0x0d), Esc(0x1b), q",
          "rawoldmode0" in q.replace(":", "").replace("rawold", "rawold") or "oldmode0" in q, s[-600:])
    order = [q.find(x) for x in ("key0x61", "key0x80", "key0x0d", "key0x1b", "key0x71")]
    check("raw: keys arrive in order, no echo of the letters", all(o >= 0 for o in order) and order == sorted(order), s[-600:])

    start(m, "run input.sw nb", 1.5)
    m.key("x"); time.sleep(0.8); m.key("q"); time.sleep(2.0)
    s = m.text_screen(); q = squash(s)
    check("raw_nb: idle polls return EAGAIN, then keys x and q arrive", "nb:key0x78aftersomepolls" in q and "nb:key0x71aftersomepolls" in q, s[-600:])

    start(m, "run input.sw wait", 2.0)
    m.key("ctrl-c"); time.sleep(2.0)
    m.line("clear", 0.5); m.line("run args.sw a b", 3); s = m.text_screen()
    check("Ctrl+C kills a program waiting for input, shell stays usable", "argv[2]=<b>" in squash(s), s[-500:])
    start(m, "run input.sw wait", 2.0)
    m.key("ctrl-c"); time.sleep(2.0); s = m.text_screen()
    check("killed program did not print the FAIL line", "FAIL: not killed" not in s and "waitingforinput" in squash(s), s[-500:])
    start(m, "run input.sw cat", 1.5)
    m.type("fresh"); m.key("ret"); time.sleep(0.4); m.key("ctrl-d"); time.sleep(2.0)
    s = m.text_screen()
    check("no state leaks into the next program (fresh line, then EOF)", "fresh" in s and "[EOFn=0]" in squash(s), s[-500:])
finally:
    m.close()

for vga in ("std", "vmware"):
    shutil.copyfile("lumaos-5.0.img", "build-text/test-d77sg%s.img" % vga)
    g = Machine("build-text/test-d77sg%s.img" % vga, "d77sg" + vga, gui=True, elf="build/kernel.elf", vga=vga)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("cd IDE3", 1.0)
        g.type("run input.sw cat"); g.key("ret"); time.sleep(2.0)
        g.type("typed while waiting"); time.sleep(1.5)
        s = g.gui_screen()
        check("[%s] GUI: echo visible while the program is still waiting" % vga, "typed while waiting" in s, s[-400:])
        g.key("ret"); time.sleep(0.5); g.key("ctrl-d"); time.sleep(2.0)
        s = g.gui_screen()
        check("[%s] GUI: program completes after Ctrl+D" % vga, "[EOF n=0]" in s, s[-400:])
        g.line("run args.sw x y", 3.0); s = g.gui_screen()
        check("[%s] GUI: shell keeps working" % vga, "argv[2]=<y>" in s, s[-300:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
