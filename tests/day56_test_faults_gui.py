"""
tests/day56_test_faults_gui.py - Day 56 smoke test on the GUI build: killed programs, Ctrl+C, stable repeats.

    ./build.sh && python3 tests/day56_test_faults_gui.py [outdir]

The GUI keeps drawing/animating while a Ring 3 program faults or spins. Checks: desktop, `run atk.sw n` (NULL read ->
"killed: page fault"), divide error, Ctrl+C on an endless loop, a good program afterwards, 10 alternating runs, frame
pool intact, terminal alive. Exhaustive: day56_test_faults.py (text).
"""
import sys, time, os, shutil, subprocess, tempfile, re
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d56_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d56_gui.img"
tmp = tempfile.mkdtemp()
HDR = "BITS 32\nglobal _start\nsection .text\n_start:\n"
PRE = HDR + "    mov ebx, [esp+12]\n    mov esi, [ebx+4]\n    movzx edx, byte [esi]\n"
SRCS = {
    "atk": PRE + """    cmp edx, 110
    je .n
    cmp edx, 100
    je .d
    cmp edx, 108
    je .l
    ret
.n: mov eax, [0]
    ret
.d: xor ecx, ecx
    xor edx, edx
    mov eax, 1
    div ecx
    ret
.l: jmp .l
""",
    "good": HDR + "    mov eax, 2\n    mov ebx, 1\n    mov ecx, msg\n    mov edx, 5\n    int 0x80\n    xor eax, eax\n    ret\nsection .data\nmsg: db 'good', 10\n",
}
files = {}
for n, src in SRCS.items():
    s, o, b = [os.path.join(tmp, n + e) for e in (".asm", ".o", ".sw")]
    open(s, "w").write(src)
    subprocess.check_call(["nasm", "-f", "elf32", s, "-o", o])
    subprocess.check_call(["ld", "-m", "elf_i386", "-static", "-nostdlib", "-e", "_start", "-Ttext-segment=0x08048000",
                           "-z", "noexecstack", "-z", "noseparate-code", "-z", "max-page-size=0x1000",
                           "--build-id=none", "-s", o, "-o", b])
    files[n + ".sw"] = b
shutil.copyfile("lumaos-5.0.img", IMG)
a = [sys.executable, "tools/mkfs.lmfs", "-o", os.path.join(tmp, "lmfs.img"), "--no-verify"]
for n, p in files.items():
    a += ["--add", "%s:%s" % (p, n)]
subprocess.check_call(a, stdout=subprocess.DEVNULL)
subprocess.check_call(["dd", "if=" + os.path.join(tmp, "lmfs.img"), "of=" + IMG, "conv=notrunc", "bs=512", "seek=100000", "status=none"])

m = Machine(IMG, "d56gui", gui=True, elf="build/kernel.elf")
fails = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(extra)


def tail_of(cmd):
    return " ".join(m.gui_screen().split(cmd)[-1].split())


try:
    scr = m.wait_for("Type 'help'", timeout=150)
    check("GUI desktop booted", "Type 'help'" in scr, scr)
    time.sleep(2)
    m.line("cd IDE3", 1.0)
    m.line("run atk.sw n", 3.0)
    t = tail_of("run atk.sw n")
    check("NULL read: killed: page fault: read of 0x00000000 (not mapped)",
          "killed: page fault: read of 0x00000000 (not mapped)" in t, t)
    m.png(f"{out}/1_killed.png")
    m.line("run atk.sw d", 3.0)
    check("divide by zero: killed: divide error (vector 0)", "killed: divide error (vector 0)" in tail_of("run atk.sw d"))
    m.line("run atk.sw l", 1.5)
    time.sleep(1.5)
    m.key("ctrl-c")
    time.sleep(2.5)
    t = tail_of("run atk.sw l")
    check("Ctrl+C ends an endless loop in the GUI: interrupted by Ctrl+C", "interrupted by Ctrl+C" in t, t)
    m.png(f"{out}/2_ctrlc.png")
    ok = True
    for i in range(10):
        m.line("run good.sw", 2.0)
        g = tail_of("run good.sw")
        m.line("run atk.sw %s" % "nd"[i % 2], 2.5)
        k = tail_of("run atk.sw %s" % "nd"[i % 2])
        if "good" not in g or "killed" not in k:
            ok = False; print(g, k); break
    check("10 alternating good/killed runs behave", ok)
    m.line("pginfo", 2.0)
    t = tail_of("pginfo")
    check("frame pool back to 2048 of 2048", "2048 of 2048 frames free" in t, t)
    m.line("ls", 1.5)
    check("terminal alive (ls works)", "good.sw" in tail_of("ls"))
    m.png(f"{out}/3_end.png")
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
