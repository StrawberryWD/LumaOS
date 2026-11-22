"""
tests/day50_test_syscall.py - Day 50 (syscall ABI: `int 0x80` enters the kernel), TEXT build.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day50_test_syscall.py

Needs: qemu-system-i386, nasm, ld, mtools, python3.

Day 50 acceptance: "`int 0x80` 能进入 Kernel。"  ABI (kernel/syscall/syscall.h): eax = number, ebx ecx edx esi edi ebp =
arguments 1..6, result in eax (negative = -errno), every other register preserved.

Fixtures (nasm, built here, installed in the LMFS root):
    getabi.sw   eax=3; int 0x80; ret                -> exit code = ABI version (1)
    nop.sw      eax=0 with six marker arguments     -> exit code 0 (silent)
    enosys.sw   eax=999; int 0x80; ret              -> exit code -38
    reserved.sw eax=5 (unassigned) ; ret            -> exit code -38
    regs.sw     loads ebx ecx edx esi edi ebp with markers, esp/eflags snapshot, int 0x80 (NOP), verifies all
                of them are unchanged afterwards -> exit code 0, else the 1-based index of the first damaged one
    twice.sw    two NOPs then GETABI (3 calls)      -> exit code 1
    deep.sw     calls int 0x80 from 3 nested calls with 20000 bytes of stack in use -> exit code 1

Proves: A boot; B kernel self-test `sysstat -t` (int 0x80 executed by the kernel itself); C the gate in the IDT
(vector 0x80, DPL=3, type 0xEE, selector 0x08); D programs reach the kernel and get return values;
E registers/flags preserved; F counters, last-call record (args, caller address inside the program); G shell
alive afterwards (IF restored, keyboard still works) and Day 43-49 commands unchanged.
"""
import os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
IMG = "/tmp/d50_sys.img"
LMFS_LBA = 100000

for need in (SRC, "tools/mkfs.lmfs", "tests/hello.sw"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

tmp = tempfile.mkdtemp()
HDR = "BITS 32\nglobal _start\nsection .text\n_start:\n"
ASM = {
    "getabi": HDR + "    mov eax, 3\n    int 0x80\n    ret\n",
    "nop": HDR + """    mov eax, 0
    mov ebx, 0x11111111
    mov ecx, 0x22222222
    mov edx, 0x33333333
    mov esi, 0x44444444
    mov edi, 0x55555555
    mov ebp, 0x66666666
    int 0x80
    xor eax, eax
    ret
""",
    "enosys": HDR + "    mov eax, 999\n    int 0x80\n    ret\n",
    "reserved": HDR + "    mov eax, 5\n    int 0x80\n    ret\n",   # 5 = first unassigned number (Day 52: SYS_EXIT; Day 70: 4 became SYS_STRERROR)
    "regs": HDR + """    push ebp
    mov [saved_esp], esp
    pushfd
    pop eax
    mov [saved_fl], eax
    mov ebx, 0xB0B0B0B1
    mov ecx, 0xC0C0C0C2
    mov edx, 0xD0D0D0D3
    mov esi, 0x5151515E
    mov edi, 0xD1D1D1D5
    mov ebp, 0xBBBBBBB6
    mov eax, 0
    int 0x80
    pushfd                      ; flags right after the call (cmp below would change them)
    pop eax
    mov [after_fl], eax
    cmp ebx, 0xB0B0B0B1
    jne .e1
    cmp ecx, 0xC0C0C0C2
    jne .e2
    cmp edx, 0xD0D0D0D3
    jne .e3
    cmp esi, 0x5151515E
    jne .e4
    cmp edi, 0xD1D1D1D5
    jne .e5
    cmp ebp, 0xBBBBBBB6
    jne .e6
    mov ebp, esp
    cmp ebp, [saved_esp]
    jne .e7
    mov eax, [after_fl]
    cmp eax, [saved_fl]         ; eflags (IF, DF, arithmetic flags...) restored by iret
    jne .e8
    pop ebp
    xor eax, eax
    ret
.e1: mov eax, 1
    jmp .out
.e2: mov eax, 2
    jmp .out
.e3: mov eax, 3
    jmp .out
.e4: mov eax, 4
    jmp .out
.e5: mov eax, 5
    jmp .out
.e6: mov eax, 6
    jmp .out
.e7: mov eax, 7
    jmp .out
.e8: mov eax, 8
.out:
    mov esp, [saved_esp]
    pop ebp
    ret
section .data
saved_esp: dd 0
saved_fl:  dd 0
after_fl:  dd 0
""",
    "twice": HDR + "    xor eax, eax\n    int 0x80\n    xor eax, eax\n    int 0x80\n    mov eax, 3\n    int 0x80\n    ret\n",
    "deep": HDR + """    call f1
    ret
f1: call f2
    ret
f2: call f3
    ret
f3: sub esp, 20000
    mov eax, 3
    int 0x80
    add esp, 20000
    ret
""",
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


m = Machine(IMG, "d50", gui=False)
try:
    scr = boot(m)
    check("A0 boot to prompt", "/ >" in scr, scr)

    # ---------------- B: the kernel executes int 0x80 itself -------------------------------------
    scr = run(m, "sysstat -t", 2.5)
    # (Day 51 grew the self-test: SYS_WRITE is implemented now, SYS_EXIT is the reserved one; 8 calls)
    for what in ("SYS_NOP returns 0", "SYS_GETABI returns the ABI version", "unknown number 999 returns -ENOSYS",
                 "SYS_EXIT with no program running -> -ESRCH (no jump)", "8 calls counted, 1 of them -ENOSYS",
                 "last call recorded (nr 2, ret 0)", "caller cs is the kernel code segment 0x08"):
        check("B `sysstat -t`: " + what, "[ok]   " + what in scr, scr)
    check("B8 sysstat -t verdict OK", "sysstat: OK" in scr and "FAIL" not in scr, scr)

    # ---------------- C: the gate ---------------------------------------------------------------------
    scr = run(m, "sysstat -r", 1.0)
    scr = run(m, "sysstat", 1.5)
    check("C1 ABI version 1, vector 0x80", "syscall ABI version 1, vector 0x80" in scr, scr)
    mt = re.search(r"IDT\[0x80\]: handler 0x([0-9a-f]{8})\s+selector 0x08\s+type 0xee\s+DPL=3", scr)
    check("C2 IDT[0x80]: selector 0x08, type 0xee (present, 32-bit interrupt gate), DPL=3", mt is not None, scr)
    check("C3 counters were reset: calls: 0", re.search(r"calls:\s+0\b", scr) is not None and "last:" not in scr, scr)

    # ---------------- D: programs enter the kernel and get values back -------------------------------
    m.line("cd IDE3", 0.8)
    expect(m, "D1 SYS_GETABI returns the ABI version as exit code", "run getabi.sw", ["run: IDE3/getabi.sw exited with code 1"])
    expect(m, "D2 unknown call number -> -ENOSYS (-38)", "run enosys.sw", ["run: IDE3/enosys.sw exited with code -38"])
    expect(m, "D3 unassigned number 5 -> -ENOSYS", "run reserved.sw",
           ["run: IDE3/reserved.sw exited with code -38"])
    expect(m, "D4 SYS_NOP with six arguments returns 0 (silent run)", "run nop.sw", [])

    # ---------------- E: preserved registers ---------------------------------------------------------------
    expect(m, "E1 ebx ecx edx esi edi ebp esp eflags all unchanged by int 0x80", "run regs.sw", [])
    expect(m, "E2 syscall from 3 nested calls with 20000 B of stack in use", "run deep.sw", ["run: IDE3/deep.sw exited with code 1"], 3.0)

    # ---------------- F: counters and last-call record ---------------------------------------------------
    run(m, "sysstat -r", 1.0)
    run(m, "run nop.sw", 2.5)
    scr = run(m, "sysstat", 1.5)
    check("F1 one run of nop.sw = 2 calls (nop + the exit stub's SYS_EXIT, Day 55), 0 bad", re.search(r"calls:\s+2\b", scr) and re.search(r"bad calls:\s+0\b", scr), scr)
    check("F2 last: nr=1 (the SYS_EXIT of the ret stub)", "last: nr=1 " in scr, scr)
    check("F3 caller is Ring 3 code in the runtime page (cs 0x23, eip 0x0A000xxx)", re.search(r"from 0x23:0x0a000[0-9a-f]{3}\b", scr) is not None, scr)
    check("F4 arguments recorded (exit code 0 in ebx)", "last args: 0x00000000" in scr, scr)
    run(m, "sysstat -r", 1.0)
    run(m, "run twice.sw", 2.5)
    scr = run(m, "sysstat", 1.5)
    check("F5 twice.sw = 4 calls (3 + exit stub), last is SYS_EXIT", re.search(r"calls:\s+4\b", scr) and "last: nr=1 " in scr, scr)
    run(m, "sysstat -r", 1.0)
    run(m, "run enosys.sw", 2.5)
    scr = run(m, "sysstat", 1.5)
    check("F6 enosys.sw counted as exactly one bad call (2 calls: 999 + exit)", re.search(r"calls:\s+2\b", scr) and re.search(r"bad calls:\s+1\b", scr), scr)

    # ---------------- G: shell alive; older commands unchanged ---------------------------------------------
    expect(m, "G1 hello.sw still runs (Day 48/49)", "run hello.sw luma", ["Hello from hello.sw!", "Hello, luma!"])
    scr = run(m, "echo alive-50", 1.0)
    check("G2 echo works after syscalls (keyboard IRQ, IF restored)", "alive-50" in scr, scr)
    scr = run(m, "help", 1.5)
    check("G3 help lists sysstat", re.search(r"^\s*sysstat\b", scr, re.M) is not None, scr)
    scr = run(m, "elfrun hello.sw", 2.5)
    check("G4 elfrun unchanged", "exited with code 0" in scr and "Hello from hello.sw!" in scr, scr)
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
