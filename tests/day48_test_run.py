"""
tests/day48_test_run.py - Day 48 (Shell integration: the `run` command), TEXT build, FAT and LMFS.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day48_test_run.py

Needs: qemu-system-i386, nasm, ld, mtools (mcopy), python3.

Day 48 acceptance: "`run hello.sw` 成功。"  `run <x.sw>` is the user-facing command built on
elf_exec() (load -> map -> run, kernel/exec/elf_exec.c). Style: silent on success, one red line
if the program exits non-zero, a newline appended when the program's output did not end with one,
only `.sw` files are run (checked BEFORE touching the disk), clear errors otherwise.

Fixtures (LMFS root, rebuilt with tools/mkfs.lmfs --add; FAT12 gets HELLO.SW / EXITDEEP.SW):
    hello.sw    real gcc output, prints "Hello from hello.sw!\\n", returns 0
    UP.SW       copy of hello.sw with an UPPER-CASE extension (extension check is case-insensitive)
    exit.sw     returns -1                       exitdeep.sw  prints "bye", api->exit(9) from 2 calls deep
    nonl.sw     puts("no newline") WITHOUT '\\n', returns 0
    ovf.sw      overflows the user stack, returns 0
    fake.sw     plain text named .sw (-> bad magic)     notelf.txt  plain text    nomagic.sw  1 byte wrong
    onstack.sw  segment overlapping the user stack (rejected at load)

Proves: A boot; B run hello.sw by full path / bare name after cd / '..' path / FAT, output is EXACTLY the
program's text (no 'running', no 'exited'); C exit codes; D newline fix-up; E .sw-only check + errors
(nothing executed); F stack overflow reported after the output; G -m 150 -> runs (Day 54: stack lives in pool frames);
H shell alive, help lists run, elfrun & Days 43-47 commands unchanged.
"""
import os, re, shutil, struct, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
from elf_fixture import elf_facts

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
HELLO = "tests/hello.sw"
IMG = "/tmp/d48_run.img"
FAT12_OFFSET = 5000 * 512
LMFS_LBA = 100000
BASE = 0x09FF0000

for need in (SRC, HELLO, "tools/mkfs.lmfs"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

tmp = tempfile.mkdtemp()
HDR = "BITS 32\nglobal _start\nsection .text\n_start:\n"
ASM = {
    "exit": HDR + "    mov eax, -1\n    ret\n",
    "exitdeep": HDR + """    mov ebx, [esp + 4]
    push dword msg
    call [ebx + 8]
    add esp, 4
    call f1
    mov eax, 99
    ret
f1: push dword 9
    call [ebx + 16]
    mov eax, 98
    ret
section .data
msg: db "bye", 10, 0
""",
    "nonl": HDR + """    mov ebx, [esp + 4]
    push dword msg
    call [ebx + 8]
    add esp, 4
    xor eax, eax
    ret
section .data
msg: db "no newline", 0
""",
    "ovf": HDR + "    mov ecx, 16340\n.l: push dword 0xDEAD0001\n    loop .l\n    add esp, 65360\n    xor eax, eax\n    ret\n",
}


def build_asm(name, src):
    s, o, b = [os.path.join(tmp, name + e) for e in (".asm", ".o", ".sw")]
    open(s, "w").write(src)
    subprocess.check_call(["nasm", "-f", "elf32", s, "-o", o])
    subprocess.check_call(["ld", "-m", "elf_i386", "-static", "-nostdlib", "-e", "_start", "-Ttext-segment=0x08048000",
                           "-z", "noexecstack", "-z", "noseparate-code", "-z", "max-page-size=0x1000",
                           "--build-id=none", "-s", o, "-o", b])
    return open(b, "rb").read()


BYTES = {n: build_asm(n, s) for n, s in ASM.items()}
hello_bytes = open(HELLO, "rb").read()
A, RX = 0x08048000, 5


def mkelf(segs, entry, size=400, phoff=52):
    b = bytearray((i * 7 + 1) & 0xFF for i in range(size))
    b[0:16] = b"\x7fELF\x01\x01\x01" + b"\x00" * 9
    struct.pack_into("<HHIIIIIHHHHHH", b, 16, 2, 3, 1, entry, phoff, 0, 0, 52, 32, len(segs), 40, 0, 0)
    for i, (t, off, va, fsz, msz, fl, al) in enumerate(segs):
        struct.pack_into("<IIIIIIII", b, phoff + 32 * i, t, off, va, va, fsz, msz, fl, al)
    return bytes(b)


good = mkelf([(1, 0, A, 0x84, 0x84, RX, 0x1000)], A + 0x10)
files = {
    "hello.sw": hello_bytes, "UP.SW": hello_bytes,
    "exit.sw": BYTES["exit"], "exitdeep.sw": BYTES["exitdeep"], "nonl.sw": BYTES["nonl"], "ovf.sw": BYTES["ovf"],
    "fake.sw": b"this is plain text pretending to be a program, long enough to hold an ELF header....\n",
    "notelf.txt": b"plain text, definitely not an ELF\n",
    "nomagic.sw": b"\x00" + good[1:],
    "onstack.sw": mkelf([(1, 0, BASE, 0x100, 0x1000, RX, 0x1000)], BASE + 0x10),
}
shutil.copyfile(SRC, IMG)
args = [sys.executable, "tools/mkfs.lmfs", "-o", os.path.join(tmp, "lmfs.img"), "--no-verify"]
for n, d in files.items():
    p = os.path.join(tmp, "add_" + n); open(p, "wb").write(d); args += ["--add", "%s:%s" % (p, n)]
subprocess.check_call(args, stdout=subprocess.DEVNULL)
subprocess.check_call(["dd", "if=" + os.path.join(tmp, "lmfs.img"), "of=" + IMG, "conv=notrunc", "bs=512",
                       "seek=%d" % LMFS_LBA, "status=none"])


def mcopy(name, data):
    p = os.path.join(tmp, "fat_" + name); open(p, "wb").write(data)
    subprocess.check_call(["mcopy", "-o", "-i", "%s@@%d" % (IMG, FAT12_OFFSET), p, "::" + name])


mcopy("EXITDEEP.SW", BYTES["exitdeep"])

fails = []


def check(name, cond, scr=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name); print(scr)


def norm(s):
    return re.sub(r"[ \t]+", " ", s)


def boot(m):
    seen, t0, scr = [], time.time(), ""
    while time.time() - t0 < 200:
        scr = m.text_screen()
        for ln in scr.splitlines():
            if ln.strip() and ln not in seen:
                seen.append(ln)
        if "/ >" in scr:
            break
    return scr, seen


def run(m, cmd, settle=2.5):
    m.line("clear", 0.5)
    m.line(cmd, settle)
    return m.text_screen()


def body(scr, cmd):
    """Screen lines after the echoed command, up to (not including) the next prompt line."""
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


m = Machine(IMG, "d48", gui=False)
try:
    # ---------------- A: boot ---------------------------------------------------------
    scr, seen = boot(m)
    check("A0 boot to prompt", "/ >" in scr, scr)
    bad = [l for l in seen if "FAIL" in l or "PANIC" in l.upper()]
    check("A1 no FAIL/PANIC line on the boot console (%d lines seen)" % len(seen), not bad, "\n".join(bad))

    # ---------------- B: run hello.sw ---------------------------------------------------
    H = ["Hello from hello.sw!"]
    expect(m, "B1 LMFS full path", "run IDE3/hello.sw", H)
    expect(m, "B2 FAT12 (case-insensitive FAT name)", "run IDE0/HELLO.SW", H, 3.0)
    expect(m, "B3 '..' in the path", "run kday42/../hello.sw", H, 3.0)
    expect(m, "B4 upper-case extension (.SW) on LMFS", "run IDE3/UP.SW", H)
    m.line("cd IDE3", 0.8)
    expect(m, "B5 bare name after cd IDE3 (the Day 48 acceptance line)", "run hello.sw", H)
    scr = run(m, "run hello.sw", 2.5)
    check("B6 no 'running' / 'exited' / 'stack' noise, prompt returns on its own line",
          not re.search(r"running|exited|stack|entry", scr) and re.search(r"^IDE3/ >", scr, re.M) is not None, scr)

    # ---------------- C: exit codes -------------------------------------------------------
    expect(m, "C1 non-zero exit: one extra line", "run exit.sw", ["run: IDE3/exit.sw exited with code -1"])
    expect(m, "C2 api->exit(9): program text, then the code line",
           "run exitdeep.sw", ["bye", "run: IDE3/exitdeep.sw exited with code 9"], 3.0)
    expect(m, "C3 FAT12 + api->exit", "run IDE0/EXITDEEP.SW", ["bye", "run: IDE0/EXITDEEP.SW exited with code 9"], 3.5)

    # ---------------- D: output without a trailing newline ---------------------------------
    scr = run(m, "run nonl.sw")
    check("D1 'no newline' is printed and exit 0 adds nothing", body(scr, "run nonl.sw") == ["no newline"], scr)
    check("D2 the next prompt is on its own line (not glued to the program's text)",
          not re.search(r"no newline.*>", scr) and re.search(r"^IDE3/ >", scr, re.M) is not None, scr)

    # ---------------- E: only .sw; errors; nothing executed ---------------------------------
    scr = run(m, "run notelf.txt")
    check("E1 notelf.txt -> 'not a .sw program' (extension is checked before any disk access)",
          body(scr, "run notelf.txt") == ["run: notelf.txt: not a .sw program"], scr)
    scr = run(m, "run hello")
    check("E2 `run hello` (no extension) is not guessed", body(scr, "run hello") == ["run: hello: not a .sw program"], scr)
    scr = run(m, "run .sw")
    check("E3 '.sw' alone is not a program name", "not a .sw program" in scr, scr)
    scr = run(m, "run nope.sw")
    check("E4 missing file -> 'cannot open file'", "run: IDE3/nope.sw: cannot open file" in norm(scr), scr)
    scr = run(m, "run fake.sw")
    check("E5 text file named .sw -> bad magic from the ELF validator", "bad magic" in scr and "Hello" not in scr, scr)
    scr = run(m, "run nomagic.sw")
    check("E6 corrupted ELF -> bad magic", "bad magic" in scr, scr)
    scr = run(m, "run onstack.sw")
    check("E7 segment on the user stack -> rejected, not executed", "segment overlaps the user stack area" in scr, scr)
    scr = run(m, "run")
    check("E8 usage", "usage: run <program.sw>" in scr, scr)
    scr = run(m, "run kday42")
    check("E9 directory name without .sw", "not a .sw program" in scr, scr)

    # ---------------- F: stack overflow is reported after the program ran -----------------------
    scr = run(m, "run ovf.sw", 3.5)
    n = norm(scr)
    check("F1 overflow reported: 'user stack overflow (guard area overwritten)'",
          "run: IDE3/ovf.sw:" in n and "user stack overflow (guard area overwritten)" in n or
          "run: IDE3/ovf.sw: user stack overflow" in n, scr)
    expect(m, "F2 hello.sw runs normally afterwards", "run hello.sw", H)

    # ---------------- H: shell and older commands ---------------------------------------------
    scr = run(m, "ls", 1.5)
    check("H1 ls works", "hello.sw" in scr and "UP.SW" in scr, scr)
    scr = run(m, "echo alive-48", 1.0)
    check("H2 echo works", "alive-48" in scr, scr)
    scr = run(m, "help", 1.5)
    check("H3 help lists run and elfrun", re.search(r"^\s*run\b", scr, re.M) is not None and "elfrun" in scr, scr)
    scr = run(m, "elfrun hello.sw", 2.5)
    check("H4 elfrun (Day 46) unchanged", "elfrun: running IDE3/hello.sw at entry 0x%08x" % elf_facts(HELLO).entry in scr and "exited with code 0" in scr, scr)
    scr = run(m, "elfrun -v exit.sw", 2.5)
    check("H5 elfrun -v (Day 47) unchanged", "stack used" in scr and "left via ret" in scr and "code -1" in scr, scr)
    scr = run(m, "elfmap hello.sw", 3.0)
    check("H6 elfmap (Day 45) unchanged", "elfmap: OK" in scr, scr)
finally:
    m.close()

# ---------------- G: no RAM at the stack's physical addresses (-m 150): Day 54 -> still runs ---------------------
m150 = Machine(IMG, "d48m150", gui=False, extra=["-m", "150"])
try:
    scr, seen = boot(m150)
    check("G0 boots with 150 MB", "/ >" in scr, scr)
    scr = run(m150, "run IDE3/hello.sw", 4.0)
    n = norm(scr)
    check("G1 (Day 54) -m 150: the stack is a pool frame, so the program runs",
          "Hello from hello.sw!" in n and "readback mismatch" not in n and "not executed" not in n, scr)
    check("G2 no error text", "cannot" not in n, scr)
    scr = run(m150, "ls", 1.5)
    check("G3 shell alive", "hello.sw" in scr, scr)
finally:
    m150.close()

# ---------------- I: no RAM at the program (-m 64): refused at the map stage ---------------------
m64 = Machine(IMG, "d48m64", gui=False, extra=["-m", "64"])
try:
    scr, seen = boot(m64)
    scr = run(m64, "run IDE3/hello.sw", 4.0)
    n = norm(scr)
    check("I1 -m 64 (Day 54): runs from pool frames, no readback mismatch",
          "Hello" in n and "memory readback mismatch" not in n and "not executed" not in n, scr)
finally:
    m64.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
