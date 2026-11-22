"""
tests/day56_test_faults.py - Day 56 (user-mode exceptions + stable interrupt return), TEXT build.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day56_test_faults.py

Day 56 acceptance: "用户程序可以重复稳定运行。"
  A Ring 3 exceptions no longer panic: the kernel kills the offending program (elf_fault.c), `run`/`elfrun` print
    "killed:" + the reason, the shell carries on, page frames are returned:
      NULL read / kernel write / jump into the kernel (#PF), divide by 0 (#DE), ud2 (#UD), hlt/cli/in/out/cr0 read/
      int 0x20/int3 (#GP), runaway recursion (stack overflow = hit the guard page), exit code 128 + vector.
  B Ctrl+C kills a program stuck in an endless loop (`interrupted by Ctrl+C`, exit code 130), the shell works afterwards.
  C interrupt return is exact: a Ring 3 program with DF=1, null es/fs/gs and known values in 5 registers spins through
    thousands of timer IRQs while keys are typed (keyboard IRQs); every register, es and DF are verified each iteration.
  D stability: 60 alternating good/bad runs; the frame pool is back to 2048; kernel faults still panic
    (pginfo's test hook is not needed: a Ring 0 fault is covered by Day 53's tests).
  E regression: Day 55 attacks now killed instead of panic; run/args/FAT/syscall self-test/pginfo -a.
"""
import os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
IMG = "/tmp/d56_faults.img"
LMFS_LBA = 100000

for need in (SRC, "tools/mkfs.lmfs", "tests/hello.sw", "tests/args.sw"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

tmp = tempfile.mkdtemp()
HDR = "BITS 32\nglobal _start\nsection .text\n_start:\n"
PRE = HDR + "    mov ebx, [esp+12]\n    mov esi, [ebx+4]\n    movzx edx, byte [esi]\n"


def dispatch(table):
    out = ""
    for ch, _ in table:
        out += "    cmp edx, %d\n    je .%s\n" % (ord(ch), ch)
    out += "    mov eax, -77\n    ret\n"
    for ch, body in table:
        out += ".%s:\n%s" % (ch, body)
    return out


ASM = {
    # atk.sw <letter>
    "atk": PRE + dispatch([
        ("n", "    mov eax, [0]\n    ret\n"),                                  # NULL read
        ("w", "    mov dword [0x00100000], 1\n    ret\n"),                      # write kernel memory
        ("j", "    jmp 0x00010000\n"),                                           # jump into the kernel
        ("d", "    xor ecx, ecx\n    mov eax, 5\n    xor edx, edx\n    div ecx\n    ret\n"),   # #DE
        ("u", "    ud2\n"),                                                      # #UD
        ("h", "    hlt\n"),                                                      # #GP
        ("c", "    cli\n    ret\n"),                                             # #GP
        ("i", "    int 0x20\n    ret\n"),                                         # #GP (gate DPL 0)
        ("3", "    int3\n    ret\n"),                                            # #GP (breakpoint gate DPL 0)
        ("r", "    mov eax, cr0\n    ret\n"),                                    # #GP
        ("s", "    call .rec\n.rec:\n    push dword 0\n    jmp .rec2\n.rec2:\n    call .rec\n    ret\n"),  # runaway recursion
        ("l", ".spin:\n    jmp .spin\n"),                                        # endless loop (Ctrl+C test)
        ("b", "    bound eax, [0]\n    ret\n"),                                  # #PF first (address 0), not #BR
    ]),
    # regs.sw: Day 56 interrupt-return torture. Exit 0 = everything survived.
    "regs": HDR + """    xor eax, eax
    mov es, ax
    mov fs, ax
    mov gs, ax
    std
    mov esi, 0x11111111
    mov edi, 0x22222222
    mov ebp, 0x33333333
    mov ebx, 0x44444444
    mov edx, 0x55555555
    push dword 30000000
.loop:
    cmp esi, 0x11111111
    jne .e1
    cmp edi, 0x22222222
    jne .e2
    cmp ebp, 0x33333333
    jne .e3
    cmp ebx, 0x44444444
    jne .e4
    cmp edx, 0x55555555
    jne .e5
    mov ax, es
    test ax, ax
    jnz .e6
    mov ax, fs
    test ax, ax
    jnz .e6
    mov ax, gs
    test ax, ax
    jnz .e6
    pushfd
    pop eax
    test eax, 0x400
    jz .e7
    dec dword [esp]
    jnz .loop
    add esp, 4
    xor eax, eax
    ret
.e1: add esp, 4
    mov eax, 1
    ret
.e2: add esp, 4
    mov eax, 2
    ret
.e3: add esp, 4
    mov eax, 3
    ret
.e4: add esp, 4
    mov eax, 4
    ret
.e5: add esp, 4
    mov eax, 5
    ret
.e6: add esp, 4
    mov eax, 6
    ret
.e7: add esp, 4
    mov eax, 7
    ret
""",
    # good.sw: does a syscall write and returns 3
    "good": HDR + "    mov eax, 2\n    mov ebx, 1\n    mov ecx, msg\n    mov edx, 5\n    int 0x80\n    mov eax, 3\n    ret\nsection .data\nmsg: db 'good', 10\n",
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
files["args.sw"] = open("tests/args.sw", "rb").read()
shutil.copyfile(SRC, IMG)
a = [sys.executable, "tools/mkfs.lmfs", "-o", os.path.join(tmp, "lmfs.img"), "--no-verify"]
for n, d in files.items():
    p = os.path.join(tmp, "add_" + n); open(p, "wb").write(d); a += ["--add", "%s:%s" % (p, n)]
subprocess.check_call(a, stdout=subprocess.DEVNULL)
subprocess.check_call(["dd", "if=" + os.path.join(tmp, "lmfs.img"), "of=" + IMG, "conv=notrunc", "bs=512",
                       "seek=%d" % LMFS_LBA, "status=none"])
p = os.path.join(tmp, "fat_hello"); open(p, "wb").write(files["hello.sw"])
subprocess.check_call(["mcopy", "-o", "-i", "%s@@%d" % (IMG, 5000 * 512), p, "::HELLO.SW"])

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


def flat(scr):
    return " ".join(scr.split())


def pool(scr):
    mo = re.search(r"frame pool: (\d+) of (\d+) frames free", scr)
    return (int(mo.group(1)), int(mo.group(2))) if mo else None


CASES = [
    ("n", ["killed: page fault: read of 0x00000000 (not mapped) at eip 0x0804"], "NULL read -> #PF (not mapped)"),
    ("w", ["killed: page fault: write to 0x00100000 (protection violation)"], "write kernel memory -> #PF (protection)"),
    ("j", ["killed: page fault: read of 0x00010000 (protection violation) at eip 0x00010000"], "jump into the kernel -> #PF at eip 0x10000"),
    ("d", ["killed: divide error (vector 0) at eip 0x0804"], "divide by zero -> #DE"),
    ("u", ["killed: invalid opcode (vector 6) at eip 0x0804"], "ud2 -> #UD"),
    ("h", ["killed: general protection fault (vector 13) at eip 0x0804"], "hlt -> #GP"),
    ("c", ["killed: general protection fault (vector 13)"], "cli -> #GP"),
    ("i", ["killed: general protection fault (vector 13)"], "int 0x20 (gate DPL 0) -> #GP"),
    ("3", ["killed: general protection fault (vector 13)"], "int3 (gate DPL 0) -> #GP"),
    ("r", ["killed: general protection fault (vector 13)"], "mov eax,cr0 -> #GP"),
    ("s", ["killed: stack overflow (hit the guard page at 0x09feff"], "runaway recursion -> stack overflow (guard page)"),
]

# =================================== M1 ===================================
m = Machine(IMG, "d56a", gui=False)
try:
    scr = boot(m)
    check("A0 boots", "/ >" in scr, scr)
    m.line("cd IDE3", 0.8)
    for arg, want, desc in CASES:
        scr = run(m, "run atk.sw " + arg, 3.5 if arg != "s" else 6.0)
        f = flat(scr)
        ok = all(w in f for w in want) and "KERNEL PANIC" not in scr and "run: IDE3/atk.sw killed:" in f
        check("A1 %s: %s" % (arg, desc), ok, scr)
        s2 = run(m, "echo alive-%s" % arg, 1.0)
        check("A2 shell alive after %s" % arg, "alive-%s" % arg in s2, s2)
    scr = run(m, "elfrun -v atk.sw d", 3.0)
    f = flat(scr)
    check("A3 elfrun -v: killed, 'exit code 128' (128 + vector 0), 'left via killed'",
          "elfrun: IDE3/atk.sw killed:" in f and "exit code 128, left via killed" in f, scr)
    scr = run(m, "elfrun -v atk.sw n", 3.0)
    check("A4 NULL read: exit code 142 (128 + 14)", "exit code 142" in flat(scr), scr)
    scr = run(m, "pginfo", 1.5)
    check("A5 after 11 killed programs: frame pool 2048 of 2048, 0 'handled' kernel faults",
          pool(scr) == (2048, 2048) and "page faults handled: 0" in scr, scr)
    scr = run(m, "run good.sw", 2.5)
    check("A6 a normal program still runs (prints 'good', exit code 3)", "good" in scr and "exited with code 3" in scr, scr)

    # ---- B: Ctrl+C on an endless loop ----
    m.line("clear", 0.5)
    m.line("run atk.sw l", 1.5)
    time.sleep(1.0)
    m.key("ctrl-c")
    time.sleep(2.0)
    scr = m.text_screen()
    f = flat(scr)
    check("B1 Ctrl+C ends the endless loop: killed, 'interrupted by Ctrl+C'",
          "run: IDE3/atk.sw killed:" in f and "interrupted by Ctrl+C at eip 0x0804" in f, scr)
    scr = run(m, "echo alive-b", 1.0)
    check("B2 shell alive after Ctrl+C", "alive-b" in scr, scr)
    m.line("clear", 0.5)
    m.line("elfrun -v atk.sw l", 1.5)
    time.sleep(1.0)
    m.key("ctrl-c")
    time.sleep(2.0)
    check("B3 elfrun -v: exit code 130", "exit code 130, left via killed" in flat(m.text_screen()), m.text_screen())
    scr = run(m, "run good.sw", 2.5)
    check("B4 and again: a program runs normally; Ctrl+C when idle is just the shell's", "good" in scr, scr)

    # ---- C: interrupt return torture ----
    m.line("clear", 0.5)
    m.line("run regs.sw", 0.5)
    for k in "hello":
        time.sleep(0.5)
        m.key(k)                                   # keyboard IRQs while the program is in Ring 3
    t0 = time.time(); scr = ""
    while time.time() - t0 < 120:
        scr = m.text_screen()
        if "/ >" in scr.split("run regs.sw")[-1]:
            break
        time.sleep(1.0)
    after = scr.split("run regs.sw")[-1]
    check("C1 regs.sw (5 regs + es/fs/gs=0 + DF=1 verified every iteration, 30M iterations, timer + keyboard IRQs): "
          "no 'exited with code' line = exit 0", "exited with code" not in after and "killed" not in after, scr)
    scr = run(m, "echo alive-c", 1.0)
    check("C2 shell alive", "alive-c" in scr, scr)

    # ---- D: stability ----
    ok = True
    for i in range(30):
        s1 = run(m, "run good.sw", 1.6)
        s2 = run(m, "run atk.sw %s" % "ndu"[i % 3], 1.8)
        if "good" not in s1 or "killed" not in s2:
            ok = False; print(s1); print(s2); break
    check("D1 30 rounds of (good, killed) programs all behave", ok)
    scr = run(m, "pginfo", 1.5)
    check("D2 frame pool 2048 of 2048 after 60 runs", pool(scr) == (2048, 2048), scr)
    scr = run(m, "pginfo -a", 3.0)
    check("D3 pginfo -a OK", "pginfo: OK" in scr, scr)

    # ---- E: regression ----
    scr = run(m, "run hello.sw luma", 3.0)
    check("E1 hello with args", "Hello, luma!" in scr, scr)
    scr = run(m, "run args.sw a b", 3.0)
    check("E2 args.sw", "argc=3" in scr, scr)
    scr = run(m, "run IDE0/HELLO.SW", 3.0)
    check("E3 FAT12", "Hello from hello.sw!" in scr, scr)
    scr = run(m, "sysstat -t", 3.0)
    check("E4 syscall self-test", "sysstat: OK" in scr and "[FAIL]" not in scr, scr)
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
