"""
tests/day46_test_elf_run.py - Day 46 (execute a loaded ELF: `elfrun` / elf_run()),
TEXT build, on FAT *and* LMFS.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day46_test_elf_run.py
    (host unit test of elf_run()'s C logic: tests/day46_elf_host_test.c)

Needs: qemu-system-i386, nasm, ld, readelf (binutils), mtools (mcopy), python3.

A scratch copy of lumaos-5.0-text.img is prepared on the HOST: the LMFS volume (IDE3) is
rebuilt with tools/mkfs.lmfs --add so it holds hello.sw, data.sw (real gcc output, Day 45
fixtures that now RETURN), and five hand-written nasm programs:

    regs.sw     trashes ebx/esi/edi/ebp/ecx/edx, sets DF, clears IF, loads ds/es/fs/gs with the
                NULL selector, then returns 77 with `ret 4` (callee-pops: esp != what it was).
                If elf_enter did not save/restore the kernel context, the very next
                instruction of the kernel would fault or print garbage.
    exit.sw     returns -1                                  (exit code is a signed int)
    ver.sw      prints "OK\\n" through api->putchar, returns api->version + 100  (= 102 since Day 47)
    counter.sw  .data word (5) and .bss word (0) are both incremented; returns data+bss = 7
                (each run must see FRESH .data/.bss: second run also 7, not 8/9)
    ... plus the Day 45 defect files (rejected before anything executes).

The FAT12 volume gets HELLO.SW / COUNTER.SW copies (multi-cluster not needed here).

What it proves, through the real kernel in QEMU text mode:
  A. boot: no FAIL/PANIC line;
  B. `elfrun IDE3/hello.sw` and `IDE0/HELLO.SW`: the program's own output appears between the
     "running ... at entry" and "exited with code 0" lines, identical on both file systems;
  C. data.sw (4 PT_LOAD: .rodata/.data/.bss all used at run time) exits 0 = its self-check;
  D. exit codes: 77, -1, 101; ver.sw's putchar output;
  E. context safety: after regs.sw the shell is intact (ls / help / another run work);
  F. counter.sw twice -> same code 7 both times; guest memory (xp) shows the program's .data
     word stays 0 at the program's virtual address (Day 54: it ran in a private pool frame), also after the 2nd run;
  G. nothing is executed for: rejected ELF, missing file, directory, usage;
  H. `cd IDE3` + bare name; elfinfo/elfload/elfmap (Days 43-45) still work; help lists elfrun;
  I. QEMU -m 64 (no RAM at physical 0x08048000): since Day 54 the program lives in page frames from the 8-16MB pool,
     not at physical 0x08048000, so it runs fine; elfload (validation only) still OK.
"""
import os, re, shutil, struct, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
from elf_fixture import elf_facts

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
HELLO = "tests/hello.sw"
DATA = "tests/data.sw"
IMG = "/tmp/d46_elf.img"
FAT12_OFFSET = 5000 * 512
LMFS_LBA = 100000

for need in (SRC, HELLO, DATA, "tools/mkfs.lmfs"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

tmp = tempfile.mkdtemp()

# ---------------------------------------------------------------- nasm fixtures
ASM = {
"regs": """
BITS 32
global _start
section .text
_start:
    mov ebx, 0xDEADBEEF
    mov esi, 0x11111111
    mov edi, 0x22222222
    mov ebp, 0x33333333
    mov ecx, 0x44444444
    mov edx, 0x55555555
    std                     ; DF=1: breaks every rep movs/stos in the kernel if not restored
    ; (Day 55: `cli` would be a #GP in Ring 3; the hardware keeps IF set)
    xor eax, eax
    mov ds, ax              ; NULL selectors: any later memory access through ds/es/fs/gs faults
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov eax, 77
    ret 4                   ; callee pops the api argument: esp differs from a plain `ret`
""",
"exit": """
BITS 32
global _start
section .text
_start:
    mov eax, -1
    ret
""",
"ver": """
BITS 32
global _start
section .text
_start:
    mov ebx, [esp + 4]          ; api
    push dword 'O'
    call [ebx + 12]             ; api->putchar
    add esp, 4
    push dword 'K'
    call [ebx + 12]
    add esp, 4
    push dword 10
    call [ebx + 12]
    add esp, 4
    mov eax, [ebx + 4]          ; api->version
    add eax, 100
    ret
""",
"counter": """
BITS 32
global _start
global cnt
section .text
_start:
    inc dword [cnt]
    inc dword [bcnt]
    mov eax, [cnt]
    add eax, [bcnt]
    ret
section .data
cnt:  dd 5
section .bss
bcnt: resd 1
""",
}


def build_asm(name, src):
    s = os.path.join(tmp, name + ".asm")
    o = os.path.join(tmp, name + ".o")
    out = os.path.join(tmp, name + ".sw")
    open(s, "w").write(src)
    subprocess.check_call(["nasm", "-f", "elf32", s, "-o", o])
    subprocess.check_call(["ld", "-m", "elf_i386", "-static", "-nostdlib", "-e", "_start",
                           "-Ttext-segment=0x08048000", "-z", "noexecstack",
                           "-z", "noseparate-code", "-z", "max-page-size=0x1000",
                           "--build-id=none", o, "-o", out])          # not stripped: we want `cnt`
    return out


FIX = {n: build_asm(n, s) for n, s in ASM.items()}
BYTES = {n: open(p, "rb").read() for n, p in FIX.items()}


def sym(path, name):
    out = subprocess.check_output(["nm", path], text=True)
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 3 and p[2] == name:
            return int(p[0], 16)
    raise KeyError(name)


CNT_ADDR = sym(FIX["counter"], "cnt")

# ---------------------------------------------------------------- defect ELFs (same recipe as Day 45)
A = 0x08048000
RX = 5


def mkelf(segs, entry, size, phoff=52):
    b = bytearray((i * 7 + 1) & 0xFF for i in range(size))
    b[0:16] = b"\x7fELF\x01\x01\x01" + b"\x00" * 9
    struct.pack_into("<HHIIIIIHHHHHH", b, 16, 2, 3, 1, entry, phoff, 0, 0,
                     52, 32, len(segs), 40, 0, 0)
    for i, (t, off, va, fsz, msz, fl, al) in enumerate(segs):
        struct.pack_into("<IIIIIIII", b, phoff + 32 * i, t, off, va, va, fsz, msz, fl, al)
    return bytes(b)


GOOD1 = (1, 0, A, 0x84, 0x84, RX, 0x1000)
NEG = {
    "nomagic.sw": (b"\x00" + mkelf([GOOD1], A + 0x10, 400)[1:], "bad magic"),
    "fsz.sw":     (mkelf([(1, 0, A, 0x100, 0x80, RX, 0x1000)], A + 0x10, 400), "p_filesz > p_memsz"),
    "bssentry.sw": (mkelf([GOOD1], A + 0x84, 400), "not inside an executable segment"),
}

shutil.copyfile(SRC, IMG)
data_bytes = open(DATA, "rb").read()
hello_bytes = open(HELLO, "rb").read()

adds = [("hello.sw", hello_bytes), ("data.sw", data_bytes),
        ("regs.sw", BYTES["regs"]), ("exit.sw", BYTES["exit"]),
        ("ver.sw", BYTES["ver"]), ("counter.sw", BYTES["counter"]),
        ("notelf.txt", b"plain text, definitely not an ELF\n")]
adds += [(k, v[0]) for k, v in NEG.items()]
args = [sys.executable, "tools/mkfs.lmfs", "-o", os.path.join(tmp, "lmfs.img"), "--no-verify"]
for name, data in adds:
    p = os.path.join(tmp, "add_" + name)
    open(p, "wb").write(data)
    args += ["--add", "%s:%s" % (p, name)]
subprocess.check_call(args, stdout=subprocess.DEVNULL)
subprocess.check_call(["dd", "if=" + os.path.join(tmp, "lmfs.img"), "of=" + IMG, "conv=notrunc",
                       "bs=512", "seek=%d" % LMFS_LBA, "status=none"])


def mcopy(name, data):
    p = os.path.join(tmp, "fat_" + name)
    open(p, "wb").write(data)
    subprocess.check_call(["mcopy", "-o", "-i", "%s@@%d" % (IMG, FAT12_OFFSET), p, "::" + name])


mcopy("COUNTER.SW", BYTES["counter"])
mcopy("DATA.SW", data_bytes)
mcopy("FSZ.SW", NEG["fsz.sw"][0])

# ---------------------------------------------------------------- run helpers
fails = []


def check(name, cond, scr=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)
        print(scr)


def norm(scr):
    return re.sub(r"[ \t]+", " ", scr)


def boot(m):
    seen, t0 = [], time.time()
    scr = ""
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


def lines_after(scr, cmd):
    """Non-empty screen lines after the echoed command line, prompt lines dropped."""
    part = scr.split(cmd, 1)[-1]
    return [l.strip() for l in part.splitlines() if l.strip() and not l.strip().endswith("/ >")]


def expect_run(m, tag, path, out_lines, code, settle=2.5, cmd=None):
    """elfrun <path> prints exactly: running line, <program output>, exited line."""
    cmd = cmd or "elfrun " + path
    scr = run(m, cmd, settle)
    got = lines_after(scr, cmd)
    entry = None
    mm = re.search(r"elfrun: running %s at entry (0x[0-9a-f]{8})" % re.escape(path), scr)
    want_tail = "elfrun: %s exited with code %d" % (path, code)
    check("%s: 'running %s at entry 0x........'" % (tag, path), mm is not None, scr)
    check("%s: program output == %r" % (tag, out_lines), got[1:-1] == out_lines, scr)
    check("%s: last line == '%s'" % (tag, want_tail), bool(got) and got[-1] == want_tail, scr)
    return scr


# ================================================================ main run
m = Machine(IMG, "d46", gui=False)
try:
    # ---------------- A: boot -------------------------------------------------------
    scr, seen = boot(m)
    check("A0 boot to prompt", "/ >" in scr, scr)
    bad = [l for l in seen if "FAIL" in l or "PANIC" in l.upper()]
    check("A1 no FAIL/PANIC line on the boot console (%d lines seen)" % len(seen), not bad, "\n".join(bad))

    # ---------------- B: hello.sw on LMFS and FAT ----------------------------------
    s1 = expect_run(m, "B1 hello.sw LMFS", "IDE3/hello.sw", ["Hello from hello.sw!"], 0)
    _entry = elf_facts(HELLO).entry
    check("B1b entry is hello.sw's e_entry 0x%08x (readelf agrees)" % _entry,
          "at entry 0x%08x" % _entry in s1 and
          re.search(r"Entry point address:\s+0x%x" % _entry, subprocess.check_output(["readelf", "-h", HELLO], text=True)) is not None, s1)
    s2 = expect_run(m, "B2 HELLO.SW FAT12", "IDE0/HELLO.SW", ["Hello from hello.sw!"], 0, 3.0)
    strip = lambda s: re.sub(r"IDE[03]/(hello|HELLO)\.(sw|SW)", "X", norm(s))
    check("B3 same output text on LMFS and FAT12", strip(s1) == strip(s2), s1 + "\n----\n" + s2)

    # ---------------- C: data.sw exercises .rodata/.data/.bss at run time -----------
    expect_run(m, "C1 data.sw LMFS (self-check code 0)", "IDE3/data.sw",
               ["LumaOS Day45 .rodata: segments land at p_vaddr"], 0, 3.0)
    expect_run(m, "C2 DATA.SW FAT12", "IDE0/DATA.SW",
               ["LumaOS Day45 .rodata: segments land at p_vaddr"], 0, 4.0)

    # ---------------- D: exit codes + putchar ---------------------------------------
    expect_run(m, "D1 exit.sw returns -1", "IDE3/exit.sw", [], -1)
    expect_run(m, "D2 ver.sw: putchar output + code 102", "IDE3/ver.sw", ["OK"], 102)

    # ---------------- E: kernel context survives a hostile program -------------------
    expect_run(m, "E1 regs.sw returns 77 (regs/DF/IF/segments trashed, ret 4)", "IDE3/regs.sw", [], 77)
    scr = run(m, "ls", 1.5)
    check("E2 shell intact after regs.sw: ls lists files (needs DF=0 and valid ds/es)",
          "hello.sw" in scr and "regs.sw" in scr and "counter.sw" in scr, scr)
    expect_run(m, "E3 hello.sw again after regs.sw", "IDE3/hello.sw", ["Hello from hello.sw!"], 0)
    scr = run(m, "echo still-alive-46", 1.0)
    check("E4 keyboard input still works (IF restored) and echo works", "still-alive-46" in scr, scr)

    # ---------------- F: every run sees fresh .data/.bss; the program ran in its own address space -
    expect_run(m, "F1 counter.sw 1st run: (5+1)+(0+1)=7", "IDE3/counter.sw", [], 7)
    w = int.from_bytes(m._read_bytes(CNT_ADDR, 4), "little")
    check("F2 (Day 54) physical memory at cnt's *virtual* address (0x%08x) is untouched (0): the program ran in its own "
          "address space, its `inc` happened in a pool frame" % CNT_ADDR, w == 0, hex(w))
    expect_run(m, "F3 counter.sw 2nd run: still 7 (loader re-copied .data and re-zeroed .bss)", "IDE3/counter.sw", [], 7)
    w = int.from_bytes(m._read_bytes(CNT_ADDR, 4), "little")
    check("F4 still untouched after the 2nd run", w == 0, hex(w))
    expect_run(m, "F5 COUNTER.SW FAT12", "IDE0/COUNTER.SW", [], 7, 3.0)

    # ---------------- G: nothing executes for bad input -------------------------------
    for name, (_, why) in NEG.items():
        scr = run(m, "elfrun IDE3/" + name)
        check("G LMFS %s -> 'cannot load ... %s', 'nothing was executed'" % (name, why),
              "elfrun: cannot load" in scr and why in scr and "nothing was executed" in scr and
              "running" not in scr and "exited" not in scr, scr)
    scr = run(m, "elfrun IDE0/FSZ.SW")
    check("G FAT FSZ.SW rejected", "cannot load" in scr and "nothing was executed" in scr and "running" not in scr, scr)
    scr = run(m, "elfrun IDE3/notelf.txt")
    check("G1 non-ELF text file", "cannot load" in scr and "nothing was executed" in scr and "running" not in scr, scr)
    scr = run(m, "elfrun IDE3/nope.sw")
    check("G2 missing file", "cannot open" in scr and "running" not in scr, scr)
    scr = run(m, "elfrun kday42")
    check("G3 directory", "directory" in scr and "running" not in scr, scr)
    scr = run(m, "elfrun")
    check("G4 usage", "usage: elfrun" in scr, scr)
    expect_run(m, "G5 '..' in the path", "IDE3/hello.sw", ["Hello from hello.sw!"], 0, 3.0,
               cmd="elfrun kday42/../hello.sw")

    # ---------------- H: help, cd + bare name, Days 43-45 commands unchanged --------------
    scr = run(m, "help", 1.5)
    check("H1 listed by help", "elfrun" in scr, scr)
    m.line("cd IDE3", 0.8)
    expect_run(m, "H2 bare name after cd IDE3", "IDE3/hello.sw", ["Hello from hello.sw!"], 0, 2.5, cmd="elfrun hello.sw")
    scr = run(m, "elfinfo data.sw", 2.0)
    check("H3 elfinfo (Day 43) still works", "check: OK" in scr, scr)
    scr = run(m, "elfload data.sw", 2.0)
    check("H4 elfload (Day 44) still works", "elfload: OK" in scr and "4 PT_LOAD" in scr, scr)
    scr = run(m, "elfmap data.sw", 3.0)
    check("H5 elfmap (Day 45) still works", "elfmap: OK" in scr and "Total: 4 segment(s)" in norm(scr), scr)
finally:
    m.close()

# ---------------- I: no RAM at physical 0x08048000 (qemu -m 64): Day 54 -> runs from pool frames ----------
m64 = Machine(IMG, "d46m64", gui=False, extra=["-m", "64"])
try:
    scr, seen = boot(m64)
    check("I0 boots with only 64 MB of RAM", "/ >" in scr, scr)
    scr = run(m64, "elfrun IDE3/hello.sw", 3.0)
    n = norm(scr)
    check("I1 (Day 54) runs on 64 MB: Hello printed, exited with code 0, no readback error",
          "Hello" in n and "exited with code 0" in n and "readback" not in n, scr)
    check("I2 no 'cannot map', no 'nothing was executed'",
          "cannot map" not in n and "nothing was executed" not in n, scr)
    scr = run(m64, "elfload IDE3/hello.sw", 2.0)
    check("I3 elfload (validation only) still OK on 64 MB", "elfload: OK" in scr, scr)
finally:
    m64.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
