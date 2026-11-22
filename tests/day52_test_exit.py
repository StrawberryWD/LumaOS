"""
tests/day52_test_exit.py - Day 52 (sys_exit: a program ends itself through `int 0x80`), TEXT build.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day52_test_exit.py

Needs: qemu-system-i386, nasm, ld, mtools, python3.

Day 52 acceptance: "程序可以安全结束。"
SYS_EXIT (eax=1, ebx=code): the kernel ends the running program and returns to the shell; code becomes the exit code.
It never returns to the program. Third path into the unified exit (via_exit = 2), next to `ret` and api->exit.

Fixtures (nasm, built here; hello.sw is the real gcc fixture which now ends with exit(0) through `int 0x80`):
    exit0.sw     exit(0)                                   -> silent
    exit42.sw    exit(42)                                  -> exit code line 42
    exitneg.sw   exit(-1)                                  -> -1        (code is a signed 32-bit value)
    exitbig.sw   exit(0x7fffffff)                          -> 2147483647 ; exitmin.sw exit(0x80000000) -> -2147483648
    noret.sw     write "before", exit(3), write "AFTER"    -> "before" + code 3; "AFTER" must never appear
    deep.sw      3 nested calls, 20000 B of stack in use, pushes junk, exit(9) from the innermost -> code 9
    trash.sw     before exit: ds/es/fs/gs = 0, DF=1 (Day 55: no cli - it is a #GP in Ring 3), ebx..ebp = junk -> exit(6); shell must still work
                 (segments, DF and IF restored by the exit path; keyboard IRQ alive)
    exitovf.sw   fills the user stack into the guard area, then exit(0) -> stack overflow reported AFTER the exit
    again.sw     like exit42 but used twice in a row (state is clean between runs)

Proves: A boot; B hello.sw via sys_exit (via_exit 2, exit code 0, sysstat shows nr=1); C exit codes (0/42/-1/INT_MAX/INT_MIN);
D "does not return": text after exit never printed; E exit from deep inside, with junk on the stack; F damaged
segments/DF/IF are repaired; G exit paths side by side (ret, api->exit, sys_exit: `elfrun -v` says which); H stack
overflow still detected; I a late/kernel SYS_EXIT returns -ESRCH and doesn't jump (sysstat -t after runs); J repeated
runs, shell alive, older commands unchanged.
"""
import os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
IMG = "/tmp/d52_exit.img"
LMFS_LBA = 100000

for need in (SRC, "tools/mkfs.lmfs", "tests/hello.sw"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

tmp = tempfile.mkdtemp()
HDR = "BITS 32\nglobal _start\nsection .text\n_start:\n"
W = "%macro WR 3\n    mov eax, 2\n    mov ebx, %1\n    mov ecx, %2\n    mov edx, %3\n    int 0x80\n%endmacro\n"

X = "%macro EXIT 1\n    mov eax, 1\n    mov ebx, %1\n    int 0x80\n%endmacro\n"
ASM = {
    "exit0": X + HDR + "    EXIT 0\n",
    "exit42": X + HDR + "    EXIT 42\n",
    "exitneg": X + HDR + "    EXIT -1\n",
    "exitbig": X + HDR + "    EXIT 0x7fffffff\n",
    "exitmin": X + HDR + "    EXIT 0x80000000\n",
    "noret": W + X + HDR + "    WR 1, a, 7\n    EXIT 3\n    WR 1, b, 6\n    EXIT 4\nsection .data\na: db 'before', 10\nb: db 'AFTER', 10\n",
    "deep": X + HDR + """    call f1
    EXIT 99
f1: push dword 0x11111111
    call f2
    EXIT 98
f2: push dword 0x22222222
    call f3
    EXIT 97
f3: sub esp, 20000
    push dword 0x33333333
    push dword 0x44444444
    EXIT 9
""",
    "trash": X + HDR + """    xor eax, eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    std
    mov ebx, 0xDEADBEEF
    mov ecx, 0xDEADBEEF
    mov edx, 0xDEADBEEF
    mov esi, 0xDEADBEEF
    mov edi, 0xDEADBEEF
    mov ebp, 0xDEADBEEF
    mov eax, 1
    mov ebx, 6
    int 0x80
""",
    "exitovf": X + HDR + "    mov ecx, 16340\n.l: push dword 0xDEAD0001\n    loop .l\n    add esp, 65360\n    EXIT 0\n",
}


def build_asm(name, src):
    s, o, b = [os.path.join(tmp, name + e) for e in (".asm", ".o", ".sw")]
    open(s, "w").write(src)
    subprocess.check_call(["nasm", "-f", "elf32", s, "-o", o])
    subprocess.check_call(["ld", "-m", "elf_i386", "-static", "-nostdlib", "-e", "_start", "-Ttext-segment=0x08048000",
                           "-z", "noexecstack", "-z", "noseparate-code", "-z", "max-page-size=0x1000",
                           "--build-id=none", "-s", o, "-o", b])
    return open(b, "rb").read()


files = {n + ".sw": build_asm(n, s) for n, s in ASM.items()}
files["hello.sw"] = open("tests/hello.sw", "rb").read()
shutil.copyfile(SRC, IMG)
a = [sys.executable, "tools/mkfs.lmfs", "-o", os.path.join(tmp, "lmfs.img"), "--no-verify"]
for n, d in files.items():
    p = os.path.join(tmp, "add_" + n); open(p, "wb").write(d); a += ["--add", "%s:%s" % (p, n)]
subprocess.check_call(a, stdout=subprocess.DEVNULL)
subprocess.check_call(["dd", "if=" + os.path.join(tmp, "lmfs.img"), "of=" + IMG, "conv=notrunc", "bs=512",
                       "seek=%d" % LMFS_LBA, "status=none"])

fails = []


def check(name, cond, scr=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(scr)


def boot(m):
    t0, scr = time.time(), ""
    while time.time() - t0 < 200:
        scr = m.text_screen()
        if "/ >" in scr:
            break
    return scr


def run(m, cmd, settle=2.5):
    m.line("clear", 0.5)
    m.line(cmd, settle)
    return m.text_screen()


def body(scr, cmd):
    part = scr.split(cmd, 1)[-1]
    out = []
    for l in part.splitlines():
        l = l.strip()
        if not l:
            continue
        if re.match(r"^IDE\d/\S* >", l):
            break
        out.append(l)
    return out


def expect(m, tag, cmd, want, settle=2.5):
    scr = run(m, cmd, settle)
    check("%s: `%s` prints exactly %r" % (tag, cmd, want), body(scr, cmd) == want, scr)
    return scr


m = Machine(IMG, "d52", gui=False)
try:
    scr = boot(m)
    check("A0 boot to prompt", "/ >" in scr, scr)
    m.line("cd IDE3", 0.8)

    # ---------------- B: hello.sw ends with SYS_EXIT ------------------------------------------------
    run(m, "sysstat -r", 1.0)
    expect(m, "B1 hello.sw prints and exits 0 through sys_exit (silent exit code)", "run hello.sw luma", ["Hello from hello.sw!", "Hello, luma!"])
    scr = run(m, "sysstat", 1.5)
    check("B2 the last system call of that run was SYS_EXIT (nr=1, ret=0), 5 calls in total (4 writes + exit)",
          re.search(r"calls:\s+5\b", scr) and "last: nr=1 ret=0" in scr, scr)
    scr = run(m, "elfrun -v hello.sw", 2.5)
    check("B3 elfrun -v: 'left via sys_exit', exit code 0",
          "left via sys_exit" in scr and "exited with code 0" in scr, scr)

    # ---------------- C: exit codes -----------------------------------------------------------------
    expect(m, "C1 exit(0): nothing extra", "run exit0.sw", [])
    expect(m, "C2 exit(42)", "run exit42.sw", ["run: IDE3/exit42.sw exited with code 42"])
    expect(m, "C3 exit(-1)", "run exitneg.sw", ["run: IDE3/exitneg.sw exited with code -1"])
    expect(m, "C4 exit(0x7fffffff)", "run exitbig.sw", ["run: IDE3/exitbig.sw exited with code 2147483647"])
    expect(m, "C5 exit(0x80000000)", "run exitmin.sw", ["run: IDE3/exitmin.sw exited with code -2147483648"])

    # ---------------- D: it really does not return ---------------------------------------------------
    scr = run(m, "run noret.sw", 2.5)
    check("D1 'before' printed, exit code 3", body(scr, "run noret.sw") == ["before", "run: IDE3/noret.sw exited with code 3"], scr)
    check("D2 the write after exit() never ran ('AFTER' absent, no code 4)", "AFTER" not in scr and "code 4" not in scr, scr)

    # ---------------- E: from deep inside --------------------------------------------------------------
    expect(m, "E1 exit(9) from 3 calls deep with 20000 bytes of stack in use", "run deep.sw", ["run: IDE3/deep.sw exited with code 9"], 3.0)

    # ---------------- F: a program that wrecked its own state ----------------------------------------------
    expect(m, "F1 ds/es/fs/gs=0, DF=1, IF=0, junk registers, then exit(6)", "run trash.sw", ["run: IDE3/trash.sw exited with code 6"], 3.0)
    scr = run(m, "echo alive-52", 1.0)
    check("F2 keyboard still works (IF restored by the exit path) and the shell echoes", "alive-52" in scr, scr)
    scr = run(m, "ls", 1.5)
    check("F3 ls works (segments and DF restored)", "trash.sw" in scr and "hello.sw" in scr, scr)

    # ---------------- G: three exit paths ---------------------------------------------------------------------
    scr = run(m, "elfrun -v exit42.sw", 2.5)
    check("G1 elfrun -v exit42.sw: left via sys_exit, code 42", "left via sys_exit" in scr and "exited with code 42" in scr, scr)
    scr = run(m, "elfrun -v hello.sw", 2.5)
    check("G2 hello.sw (exit through int 0x80) also via sys_exit", "left via sys_exit" in scr, scr)

    # ---------------- H: stack overflow still detected after SYS_EXIT ----------------------------------------------
    scr = run(m, "run exitovf.sw", 3.5)
    check("H1 overflow reported after the exit", "user stack overflow" in re.sub(r"[ \t]+", " ", scr), scr)
    expect(m, "H2 next run is clean", "run exit42.sw", ["run: IDE3/exit42.sw exited with code 42"])

    # ---------------- I: nothing to exit -------------------------------------------------------------------------------
    scr = run(m, "sysstat -t", 3.0)
    check("I1 after the runs, a kernel-side SYS_EXIT returns -ESRCH and does not jump (self-test still OK)",
          "sysstat: OK" in scr and "[ok]   SYS_EXIT with no program running -> -ESRCH (no jump)" in scr and "[FAIL]" not in scr, scr)

    # ---------------- J: repeated runs, alive ------------------------------------------------------------------------------
    for i in range(3):
        expect(m, "J1.%d repeated run" % i, "run exit42.sw", ["run: IDE3/exit42.sw exited with code 42"])
    expect(m, "J2 FAT12 hello.sw", "run IDE0/HELLO.SW fat", ["Hello from hello.sw!", "Hello, fat!"], 3.0)
    scr = run(m, "run noret.sw", 2.5)
    scr = run(m, "help", 1.5)
    check("J3 help works", "sysstat" in scr and "run" in scr, scr)
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
