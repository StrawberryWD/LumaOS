"""
tests/day51_test_write.py - Day 51 (sys_write: programs print through `int 0x80`), TEXT build.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day51_test_write.py

Needs: qemu-system-i386, nasm, ld, mtools, python3.

Day 51 acceptance: "用户程序通过 syscall 输出。"
SYS_WRITE(eax=2): ebx = fd (1 stdout, 2 stderr), ecx = buf, edx = len; returns bytes written or -errno
(-9 EBADF, -14 EFAULT). The buffer must lie completely inside the user address area [16 MB, 240 MB) or nothing
is printed at all; at most 4096 bytes per call (short write); NUL bytes are dropped but counted.

Fixtures (nasm, built here, installed in the LMFS root; hello.sw is the real gcc fixture which now prints through
`int 0x80`, see build.sh):
    wr.sw       one write(1, "hi\n", 3); exit code = the value write returned            -> "hi" + code 3
    stderr.sw   write(2, "oops\n", 5)                                                    -> "oops", code 5
    bad.sw      eight bad calls; exit code = (index of the first call whose result was wrong), 0 = all as specified:
                  1 fd 0 -> -9    2 fd 3 -> -9    3 fd -1 -> -9
                  4 NULL buffer -> -14    5 kernel address 0x00010000 -> -14
                  6 buffer starts 1 byte below the user area -> -14
                  7 buffer straddles the top of the user area (HI-1, len 2) -> -14
                  8 wraparound (0xFFFFFFF0, len 0x20) -> -14       (and none of them printed anything)
    zero.sw     write(1, NULL, 0) -> 0 ; write(1, kernel address, 0) -> 0                  -> silent, code 0
    big.sw      write(1, buf, 5000) with a valid 5000-byte buffer of 'x' ending in a marker -> returns 4096 (short write)
    nul.sw      write(1, "a\0b\n", 4) -> prints "ab", returns 4
    nonl.sw     write(1, "no newline", 10), exit 0 -> `run` appends the missing newline (counters see syscall output)
    stack.sw    builds "on stack\n" on the user stack with push, write(1, esp, 9)         -> "on stack"
    mix.sw      api->puts("A"), write "B\n", api->puts("C\n") -> output order A B C kept (one shared output path)
    badexit.sw  write(1, "x", 1) then returns 7                                             -> "x" + newline fix-up + code line

Proves: A boot; B kernel self-test (`sysstat -t`, 15 checks incl. the range function); C hello.sw prints via syscall (all
variants: LMFS, FAT12, with args); D exit codes = return values; E every error case, with nothing printed;
F short write / NUL / no trailing newline / stack buffer / mixing with api->puts; G counters; H shell alive.
"""

import os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
IMG = "/tmp/d51_write.img"
LMFS_LBA = 100000

for need in (SRC, "tools/mkfs.lmfs", "tests/hello.sw"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

tmp = tempfile.mkdtemp()
HDR = "BITS 32\nglobal _start\nsection .text\n_start:\n"
W = "%macro WR 3\n    mov eax, 2\n    mov ebx, %1\n    mov ecx, %2\n    mov edx, %3\n    int 0x80\n%endmacro\n"
ASM = {
    "wr": W + HDR + "    WR 1, msg, 3\n    ret\nsection .data\nmsg: db 'hi', 10\n",
    "stderr": W + HDR + "    WR 2, msg, 5\n    ret\nsection .data\nmsg: db 'oops', 10\n",
    "bad": W + HDR + """    WR 0, msg, 1
    cmp eax, -9
    jne .e1
    WR 3, msg, 1
    cmp eax, -9
    jne .e2
    WR -1, msg, 1
    cmp eax, -9
    jne .e3
    WR 1, 0, 4
    cmp eax, -14
    jne .e4
    WR 1, 0x00010000, 4
    cmp eax, -14
    jne .e5
    WR 1, 0x00FFFFFF, 1
    cmp eax, -14
    jne .e6
    WR 1, 0x0EFFFFFF, 2
    cmp eax, -14
    jne .e7
    WR 1, 0xFFFFFFF0, 0x20
    cmp eax, -14
    jne .e8
    xor eax, eax
    ret
.e1: mov eax, 1
    ret
.e2: mov eax, 2
    ret
.e3: mov eax, 3
    ret
.e4: mov eax, 4
    ret
.e5: mov eax, 5
    ret
.e6: mov eax, 6
    ret
.e7: mov eax, 7
    ret
.e8: mov eax, 8
    ret
section .data
msg: db 'SHOULD NOT APPEAR', 10
""",
    "zero": W + HDR + """    WR 1, 0, 0
    test eax, eax
    jnz .e1
    WR 1, 0x00010000, 0
    test eax, eax
    jnz .e2
    xor eax, eax
    ret
.e1: mov eax, 1
    ret
.e2: mov eax, 2
    ret
""",
    "big": W + HDR + "    WR 1, buf, 5000\n    ret\nsection .data\nbuf: times 4999 db 'x'\n     db 'Z'\n",
    "nul": W + HDR + "    WR 1, msg, 4\n    ret\nsection .data\nmsg: db 'a', 0, 'b', 10\n",
    "nonl": W + HDR + "    WR 1, msg, 10\n    xor eax, eax\n    ret\nsection .data\nmsg: db 'no newline'\n",
    "stack": W + HDR + """    push dword 0x0000000a
    push dword 'tack'
    push dword 'on s'
    WR 1, esp, 9
    add esp, 12
    xor eax, eax
    ret
""",
    "mix": W + HDR + """    mov esi, [esp + 4]          ; api (esi must survive the system call)
    push dword a
    call [esi + 8]
    add esp, 4
    WR 1, b, 2
    push dword c
    call [esi + 8]
    add esp, 4
    xor eax, eax
    ret
section .data
a: db 'A', 0
b: db 'B', 10
c: db 'C', 10, 0
""",
    "badexit": W + HDR + "    WR 1, msg, 1\n    mov eax, 7\n    ret\nsection .data\nmsg: db 'x'\n",
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


m = Machine(IMG, "d51", gui=False)
try:
    scr = boot(m)
    check("A0 boot to prompt", "/ >" in scr, scr)

    # ---------------- B: kernel self-test ---------------------------------------------------------
    scr = run(m, "sysstat -t", 3.0)
    check("B1 sysstat -t: OK verdict, no [FAIL], at least 15 [ok] lines",
          "sysstat: OK" in scr and "[FAIL]" not in scr and scr.count("[ok]") >= 15, scr)

    # ---------------- C: hello.sw prints through the system call ----------------------------------------
    m.line("cd IDE3", 0.8)
    run(m, "sysstat -r", 1.0)
    expect(m, "C1 hello.sw (LMFS)", "run hello.sw", ["Hello from hello.sw!"])
    scr = run(m, "sysstat", 1.5)
    check("C2 that run made 2 system calls (Day 52: write + exit); the write's length argument is 21 (the line), exit is last",
          re.search(r"calls:\s+2\b", scr) and "last: nr=1 ret=0" in scr and "0x00000015" in scr, scr)
    run(m, "sysstat -r", 1.0)
    expect(m, "C3 hello.sw with an argument", "run hello.sw luma", ["Hello from hello.sw!", "Hello, luma!"])
    scr = run(m, "sysstat", 1.5)
    check("C4 4 write calls (greeting, 'Hello, ', argv[1], '!\\n') + the exit (Day 52) = 5 calls", re.search(r"calls:\s+5\b", scr) and "last: nr=1 ret=0" in scr and "0x00000002" in scr, scr)
    expect(m, "C5 FAT12 copy of hello.sw", "run IDE0/HELLO.SW fat", ["Hello from hello.sw!", "Hello, fat!"], 3.0)

    # ---------------- D: return values ------------------------------------------------------------------
    expect(m, "D1 write returns the byte count (3)", "run wr.sw", ["hi", "run: IDE3/wr.sw exited with code 3"])
    expect(m, "D2 fd 2 (stderr) prints on the same terminal", "run stderr.sw", ["oops", "run: IDE3/stderr.sw exited with code 5"])

    # ---------------- E: error cases: right error number, nothing printed -----------------------------------
    scr = run(m, "run bad.sw", 3.0)
    check("E1 bad.sw: all 8 error cases returned the specified errno and printed nothing (exit 0, silent)",
          body(scr, "run bad.sw") == [] and "SHOULD NOT APPEAR" not in scr, scr)
    expect(m, "E2 len 0 returns 0 without looking at the pointer", "run zero.sw", [])

    # ---------------- F: byte semantics --------------------------------------------------------------------------
    scr = run(m, "run big.sw", 4.0)
    check("F1 5000 bytes requested -> short write of 4096 (exit code 4096)", "run: IDE3/big.sw exited with code 4096" in scr, scr)
    check("F2 the 'Z' at byte 5000 was never printed", "Z" not in scr, scr)
    expect(m, "F3 NUL dropped, counted: 'a\\0b\\n' prints ab, returns 4", "run nul.sw", ["ab", "run: IDE3/nul.sw exited with code 4"])
    scr = run(m, "run nonl.sw", 2.5)
    check("F4 syscall output without trailing newline: printed, exit 0 adds nothing", body(scr, "run nonl.sw") == ["no newline"], scr)
    check("F5 next prompt on its own line (run's newline fix-up sees the syscall output)",
          not re.search(r"no newline.*>", scr) and re.search(r"^IDE3/ >", scr, re.M) is not None, scr)
    expect(m, "F6 buffer on the user stack is valid", "run stack.sw", ["on stack"])
    expect(m, "F7 api->puts and write interleave in order; esi survives the call", "run mix.sw", ["AB", "C"])
    expect(m, "F8 write then exit code 7", "run badexit.sw", ["x", "run: IDE3/badexit.sw exited with code 7"])

    # ---------------- G: counters --------------------------------------------------------------------------------
    run(m, "sysstat -r", 1.0)
    run(m, "run wr.sw", 2.5)
    scr = run(m, "sysstat", 1.5)
    check("G1 wr.sw: 2 calls (write + the exit stub's SYS_EXIT, Day 55), last = exit, 0 bad",
          re.search(r"calls:\s+2\b", scr) and "last: nr=1 " in scr and re.search(r"bad calls:\s+0\b", scr), scr)
    run(m, "sysstat -r", 1.0)
    run(m, "run bad.sw", 3.0)
    scr = run(m, "sysstat", 1.5)
    check("G2 bad.sw: 9 calls (8 + the exit stub), none of them -ENOSYS (bad calls counts unknown numbers only)",
          re.search(r"calls:\s+9\b", scr) and re.search(r"bad calls:\s+0\b", scr), scr)

    # ---------------- H: shell alive, older commands -----------------------------------------------------------------
    scr = run(m, "echo alive-51", 1.0)
    check("H1 echo works", "alive-51" in scr, scr)
    scr = run(m, "elfrun hello.sw", 2.5)
    check("H2 elfrun: output through the syscall too, exit 0", "Hello from hello.sw!" in scr and "exited with code 0" in scr, scr)
    scr = run(m, "ls", 1.5)
    check("H3 ls works", "hello.sw" in scr and "wr.sw" in scr, scr)
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
