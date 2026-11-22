"""
tests/day55_test_ring3.py - Day 55 (iret into Ring 3), TEXT build.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day55_test_ring3.py

Day 55 acceptance: "用户程序无法直接访问 Kernel。"
  GDT: user code 0x23 (GDT[4]), user data 0x3B (GDT[7]); TSS.ss0/esp0 for the stack switch on int 0x80/IRQ;
  page tables: user pages carry US, kernel pages do not; elf_enter builds an iret frame (ss esp eflags cs eip).
  Programs reach the kernel only through int 0x80 (DPL 3 gate); the old api->puts/putchar/exit table is now a
  read-only user "runtime page" (0x0A000000) of Ring 3 stubs that use SYS_WRITE/SYS_EXIT.

M1 (normal)  A programs *see* Ring 3: cs=0x23 (CPL 3), ss=ds=es=0x3B, IOPL 0 + IF 1; the api table lives at a user
               address inside the runtime page and api->puts/putchar/exit work from Ring 3; ret / api->exit / sys_exit are
               still told apart; a 300M-iteration busy loop in Ring 3 survives timer interrupts; syscall stats show cs 0x23;
             B hardware stops the program from touching the kernel: every attack runs in its own VM below.
M2..M10      one VM per attack. DAY56: the program is *killed* ("killed: <cause>"), no longer a panic screen:
               kread  mov eax,[0x10000]        -> PAGE FAULT 0x00010000, protection violation (page present, no US), read, user mode
               kwrite mov [0x100000],1         -> PAGE FAULT write, protection violation
               vga    mov [0xB8000],..         -> PAGE FAULT write, protection violation (can't draw on the screen)
               urtw   mov [0x0A000000],0       -> PAGE FAULT write on the read-only runtime page
               hlt / cli / in / out            -> GENERAL PROTECTION FAULT (CPL 3 > IOPL 0)
               lds    mov ds,0x10              -> GENERAL PROTECTION FAULT (cannot load a DPL 0 segment)
               intn   int 0x20 (IRQ0's gate)   -> GENERAL PROTECTION FAULT (gate DPL 0 < CPL 3)
             (Day 55 originally showed panic screens with cs 0x00000023; Day 56 turned them into kills.)
M11 (normal) afterwards the old paths still work: run/args/FAT, the kernel's own int 0x80 self-test, pool intact.
"""
import os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
IMG = "/tmp/d55_ring3.img"
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
    # probe.sw <letter>: the exit code is what the program sees about itself
    "probe": PRE + dispatch([
        ("p", "    mov eax, cs\n    and eax, 3\n    ret\n"),
        ("c", "    mov eax, cs\n    ret\n"),
        ("s", "    mov eax, ss\n    ret\n"),
        ("d", "    mov eax, ds\n    ret\n"),
        ("e", "    mov eax, es\n    ret\n"),
        ("f", "    pushfd\n    pop eax\n    ret\n"),
        # api pointer >> 12 when magic/version are right, else -1
        ("a", "    mov eax, [esp+4]\n    mov ecx, [eax]\n    cmp ecx, 0x4950414C\n    jne .bad\n"
              "    mov ecx, [eax+4]\n    cmp ecx, 2\n    jne .bad\n    shr eax, 12\n    ret\n.bad:\n    mov eax, -1\n    ret\n"),
    ]),
    # atk.sw <letter>: each letter is one forbidden thing
    "atk": PRE + dispatch([
        ("r", "    mov eax, [0x00010000]\n    ret\n"),                    # read kernel memory
        ("w", "    mov dword [0x00100000], 1\n    ret\n"),                 # write kernel memory
        ("v", "    mov dword [0x000B8000], 0x41414141\n    ret\n"),        # scribble on the VGA text screen
        ("u", "    mov dword [0x0A000000], 0\n    ret\n"),                 # write the read-only runtime page
        ("h", "    hlt\n    ret\n"),
        ("c", "    cli\n    ret\n"),
        ("i", "    in al, 0x60\n    ret\n"),
        ("o", "    mov al, 0xFE\n    out 0x64, al\n    ret\n"),
        ("l", "    mov ax, 0x10\n    mov ds, ax\n    ret\n"),
        ("n", "    int 0x20\n    ret\n"),
        ("g", "    lgdt [0x08048000]\n    ret\n"),
        ("3", "    mov eax, cr3\n    ret\n"),
    ]),
    "apiputs": HDR + "    mov eax, [esp+4]\n    push dword msg\n    call [eax+8]\n    add esp, 4\n"
                     "    mov eax, [esp+4]\n    push dword 33\n    call [eax+12]\n    add esp, 4\n"
                     "    mov eax, [esp+4]\n    push dword 10\n    call [eax+12]\n    add esp, 4\n    xor eax, eax\n    ret\n"
                     "section .data\nmsg: db 'puts-from-ring3', 0\n",
    "apiexit": HDR + "    mov eax, [esp+4]\n    push dword 21\n    call [eax+16]\n    mov eax, 99\n    ret\n",
    "busy": HDR + "    mov ecx, 300000000\n.l: dec ecx\n    jnz .l\n    mov eax, 7\n    ret\n",
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


def code_of(scr, name):
    mo = re.search(r"IDE3/%s\.sw exited with code (-?\d+)" % name, flat(scr))
    return int(mo.group(1)) if mo else None


# =================================== M1 ===================================
m = Machine(IMG, "d55a", gui=False)
try:
    scr = boot(m)
    check("A0 boots", "/ >" in scr, scr)
    m.line("cd IDE3", 0.8)
    for arg, want, what in (("p", 3, "CPL (cs & 3) == 3: the program runs in Ring 3"),
                            ("c", 0x23, "cs == 0x23 (user code segment, RPL 3)"),
                            ("s", 0x3B, "ss == 0x3B (user data segment)"),
                            ("d", 0x3B, "ds == 0x3B"), ("e", 0x3B, "es == 0x3B")):
        scr = run(m, "elfrun probe.sw %s" % arg, 3.0)
        got = code_of(scr, "probe")
        check("A1 probe %s: %s" % (arg, what), got == want, scr)
    scr = run(m, "elfrun probe.sw f", 3.0)
    ef = code_of(scr, "probe")
    check("A2 eflags seen by the program: IF=1, IOPL=0, DF=0 (0x202 plus whatever status flags)",
          ef is not None and (ef >> 9) & 1 == 1 and (ef >> 12) & 3 == 0 and (ef >> 10) & 1 == 0, scr)
    scr = run(m, "elfrun probe.sw a", 3.0)
    pg = code_of(scr, "probe")
    check("A3 api pointer is a user address in the runtime page 0x0A000xxx, magic 'LAPI', version 2",
          pg == 0x0A000, scr + " got %r" % pg)
    scr = run(m, "elfrun -v apiputs.sw", 3.0)
    n = flat(scr)
    check("A4 api->puts + api->putchar work from Ring 3 (via SYS_WRITE stubs)",
          "puts-from-ring3!" in n and "exited with code 0" in n, scr)
    scr = run(m, "elfrun -v apiexit.sw", 3.0)
    n = flat(scr)
    check("A5 api->exit(21): code 21, 'left via api->exit', the code after the call never ran",
          "exited with code 21" in n and "left via api->exit" in n, scr)
    scr = run(m, "elfrun -v probe.sw p", 3.0)
    check("A6 entry-function ret is told apart: 'left via ret'", "left via ret" in flat(scr), scr)
    scr = run(m, "elfrun -v hello.sw", 3.0)
    check("A7 hello.sw (int 0x80 write + exit): 'left via sys_exit'", "left via sys_exit" in flat(scr)
          and "Hello from hello.sw!" in scr, scr)
    scr = run(m, "sysstat", 2.0)
    check("A8 sysstat: the last syscall came from 0x23:0x0804xxxx (Ring 3), not cs 0x08",
          re.search(r"from 0x23:0x0804[0-9a-f]{4}", scr) is not None, scr)
    t0 = time.time()
    scr = run(m, "elfrun busy.sw", 12.0)
    check("A9 300M-iteration loop in Ring 3 (timer IRQs arrive while in Ring 3) returns 7", code_of(scr, "busy") == 7, scr)
    scr = run(m, "echo alive-55", 1.2)
    check("A10 keyboard + shell still work after IRQs from Ring 3", "alive-55" in scr, scr)
    scr = run(m, "pginfo", 1.5)
    check("A11 frame pool intact (2048 of 2048), 0 page faults", "2048 of 2048" in scr and "page faults handled: 0" in scr, scr)
finally:
    m.close()


def attack(tag, prog, want_all, desc):
    prog, arg = prog if isinstance(prog, tuple) else (prog, "")
    m = Machine(IMG, "d55_" + tag, gui=False)
    try:
        boot(m)
        m.line("cd IDE3", 0.8)
        m.line("clear", 0.5)
        m.line("run %s.sw %s" % (prog, arg), 3.5)
        scr = m.text_screen()
        f = flat(scr)
        ok = "KERNEL PANIC" not in scr and "killed:" in f and all(w in f for w in want_all)
        check("%s %s -> %s" % (tag, prog, desc), ok, scr)
        return scr
    finally:
        m.close()


attack("B1", ("atk", "r"), ["page fault: read of 0x00010000 (protection violation) at eip 0x0804"],
       "page present but supervisor-only: protection violation, read, user mode, cs 0x23")
attack("B2", ("atk", "w"), ["page fault: write to 0x00100000 (protection violation)"], "write to kernel memory refused")
attack("B3", ("atk", "v"), ["page fault: write to 0x000b8000 (protection violation)"], "cannot draw on the VGA text screen")
attack("B4", ("atk", "u"), ["page fault: write to 0x0a000000 (protection violation)"],
       "the runtime page is read-only")
for tag, arg, desc in (("C1", "h", "hlt"), ("C2", "c", "cli"), ("C3", "i", "in al,0x60"),
                       ("C4", "o", "out 0x64,al (would reset the PC)"), ("C5", "l", "mov ds,0x10 (kernel selector)"),
                       ("C6", "n", "int 0x20 (gate DPL 0)"), ("C7", "g", "lgdt (privileged)"),
                       ("C8", "3", "mov eax,cr3 (privileged)")):
    attack(tag, ("atk", arg), ["general protection fault (vector 13) at eip 0x0804"],
           "#GP: " + desc)

# =================================== M11 ===================================
m = Machine(IMG, "d55z", gui=False)
try:
    boot(m)
    m.line("cd IDE3", 0.8)
    scr = run(m, "run hello.sw luma", 3.0)
    check("Z1 run hello.sw luma", "Hello, luma!" in scr, scr)
    scr = run(m, "run args.sw a b", 3.0)
    check("Z2 run args.sw a b", "argc=3" in scr and "argv[2]=<b>" in scr, scr)
    scr = run(m, "run IDE0/HELLO.SW", 3.0)
    check("Z3 FAT12 program", "Hello from hello.sw!" in scr, scr)
    scr = run(m, "sysstat -t", 3.0)
    check("Z4 kernel-side int 0x80 self-test still OK (cs 0x08)", "sysstat: OK" in scr and "[FAIL]" not in scr, scr)
    scr = run(m, "pginfo -a", 3.0)
    check("Z5 pginfo -a OK", "pginfo: OK" in scr, scr)
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
