"""
tests/day47_test_elf_gui.py - Day 47 smoke test on the GUI build.

    ./build.sh && python3 tests/day47_test_elf_gui.py [outdir]

Boots the GUI image and, in the GUI terminal, runs hello.sw (ret path), a program that leaves
through api->exit() from nested calls, and the overflow case; checks the terminal shows the
exit lines / `-v` stack report, and that the desktop terminal keeps answering afterwards
(= control returned to the shell). Exhaustive checks: day47_test_elf_stack.py (text build).
"""
import sys, time, os, shutil, subprocess, tempfile, re
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d47_gui"
os.makedirs(out, exist_ok=True)
IMG = "/tmp/d47_gui.img"
tmp = tempfile.mkdtemp()
shutil.copyfile("lumaos-5.0.img", IMG)

EXITDEEP = """
BITS 32
global _start
section .text
_start:
    mov ebx, [esp + 4]
    push dword msg
    call [ebx + 8]
    add esp, 4
    call f1
    mov eax, 99
    ret
f1: push ebp
    call f2
    pop ebp
    ret
f2: push dword 9
    call [ebx + 16]
    ret
section .data
msg: db "bye from nested call", 10, 0
"""
OVF = """
BITS 32
global _start
section .text
_start:
    mov ecx, 16340
.l: push dword 0xDEAD0001
    loop .l
    add esp, 65360
    xor eax, eax
    ret
"""
files = {}
for n, src in (("exitdeep", EXITDEEP), ("ovf", OVF)):
    s, o, b = [os.path.join(tmp, n + e) for e in (".asm", ".o", ".sw")]
    open(s, "w").write(src)
    subprocess.check_call(["nasm", "-f", "elf32", s, "-o", o])
    subprocess.check_call(["ld", "-m", "elf_i386", "-static", "-nostdlib", "-e", "_start", "-Ttext-segment=0x08048000",
                           "-z", "noexecstack", "-z", "noseparate-code", "-z", "max-page-size=0x1000", "--build-id=none", "-s", o, "-o", b])
    files[n] = open(b, "rb").read()
args = [sys.executable, "tools/mkfs.lmfs", "-o", os.path.join(tmp, "lmfs.img"), "--no-verify",
        "--add", "tests/hello.sw:hello.sw"]
for n, d in files.items():
    p = os.path.join(tmp, "add_" + n); open(p, "wb").write(d); args += ["--add", "%s:%s.sw" % (p, n)]
subprocess.check_call(args, stdout=subprocess.DEVNULL)
subprocess.check_call(["dd", "if=" + os.path.join(tmp, "lmfs.img"), "of=" + IMG, "conv=notrunc", "bs=512", "seek=100000", "status=none"])

m = Machine(IMG, "d47gui", gui=True, elf="build/kernel.elf")
fails = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(extra)


try:
    scr = m.wait_for("Type 'help'", timeout=150)
    check("GUI booted", "Type 'help'" in scr, scr)
    time.sleep(2)
    m.line("elfrun -v IDE3/hello.sw", 3.5)
    tail = m.gui_screen().split("elfrun -v IDE3/hello.sw")[-1]
    check("hello.sw: output, exit 0, stack report (Day 52: leaves via sys_exit)",
          "Hello from hello.sw!" in tail and "exited with code 0" in tail and
          re.search(r"stack used \d+ of 65536 bytes \(top 0x0a000000\), left via (ret|sys_exit)", tail) is not None, tail)
    m.png(f"{out}/1_hello.png")
    m.line("elfrun -v IDE3/exitdeep.sw", 3.5)
    tail = m.gui_screen().split("elfrun -v IDE3/exitdeep.sw")[-1]
    check("exitdeep.sw: 'bye', exit code 9, left via api->exit (code after the call never ran)",
          "bye from nested call" in tail and "exited with code 9" in tail and "left via api->exit" in tail and "code 99" not in tail, tail)
    m.line("elfrun -v IDE3/ovf.sw", 4.5)
    tail = m.gui_screen().split("elfrun -v IDE3/ovf.sw")[-1]
    check("ovf.sw: exit 0 line, then the stack-overflow error", "exited with code 0" in tail and "user stack overflow" in tail, tail)
    m.line("elfrun IDE3/hello.sw", 3.0)
    tail = m.gui_screen().split("elfrun IDE3/hello.sw")[-1]
    check("hello.sw again after the overflow", "Hello from hello.sw!" in tail and "overflow" not in tail, tail)
    m.line("ls", 1.5)
    check("terminal still alive: back at the shell, ls works", "hello.sw" in m.gui_screen().split("ls")[-1])
    m.png(f"{out}/2_after.png")
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
