"""
tests/day47_test_elf_stack.py - Day 47 (user stack + unified exit path), TEXT build, FAT and LMFS.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day47_test_elf_stack.py
    (host unit test of elf_run()'s C logic: tests/day47_elf_host_test.c)

Needs: qemu-system-i386, nasm, ld, readelf/nm (binutils), mtools (mcopy), python3.

Day 47 acceptance: "程序执行后返回 Shell。" - a program now runs on its OWN 64 KB stack
(0x09FF0000..0x0A000000, not the 4 KB kernel stack) and can leave by `ret` or by api->exit();
both go through one exit path (elf_enter.asm: elf_leave) back to the shell.

Programs (nasm, hand-written, loaded from LMFS; FAT copies of two of them):
    esp.sw      returns (esp >> 16) at entry (+100000 if (esp+4) is not 16-byte aligned or api==NULL)
                -> 0x09FF = 2559 proves "esp is inside the user stack, ABI-aligned"
    deep.sw     sub esp,40000 / touch the lowest dword / add esp,40000 / ret
                -> `elfrun -v` must say "stack used 40020" (20 = initial frame), left via ret
    exitdeep.sw 3 nested calls with pushes, then api->exit(9) from the innermost; the code after
                the call (exit 99) must NEVER run -> "exited with code 9", left via api->exit
    ovf.sw      pushes 65360 bytes: runs into the guard area at the bottom of the stack (still mapped) and
                below; returns 0 -> kernel reports "user stack overflow" after the exit line
    esp.sw also lets us look at guest RAM: [TOP-16] must hold the api table address and [TOP-20] the ret stub (both in the Day 55 runtime page).
Synthetic ELFs: one PT_LOAD overlapping the stack (rejected at load time: elfrun/elfload/elfmap),
one ending exactly at the stack base (accepted by elfload).

Proves, through the real kernel in QEMU:
  A boot clean; B esp/ABI; C stack usage + both exit paths; D overflow; E kernel context and shell
  survive (ls, echo, repeated runs, FAT too); F stack/segment clash; G -m 150 (RAM at the program,
  none at the stack): refuses BEFORE running, nothing executed; H earlier commands still work.
"""
import os, re, shutil, struct, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine, nm_symbol

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
HELLO = "tests/hello.sw"
KELF = "build-text/kernel.elf"
IMG = "/tmp/d47_elf.img"
FAT12_OFFSET = 5000 * 512
LMFS_LBA = 100000
TOP, SIZE = 0x0A000000, 0x10000
BASE = TOP - SIZE

for need in (SRC, HELLO, KELF, "tools/mkfs.lmfs"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

tmp = tempfile.mkdtemp()

ASM = {
"esp": """
BITS 32
global _start
section .text
_start:
    mov eax, esp
    shr eax, 16                 ; 0x09FF when esp is just under 0x0A000000
    mov ecx, esp
    add ecx, 4
    test ecx, 15                ; (esp+4) must be 16-byte aligned (System V i386 ABI)
    jz .ok
    add eax, 100000
.ok:
    cmp dword [esp + 4], 0      ; api pointer must be the first argument
    jne .ok2
    add eax, 100000
.ok2:
    ret
""",
"deep": """
BITS 32
global _start
section .text
_start:
    sub esp, 40000
    mov dword [esp], 0x12345678 ; lowest touched dword: first byte 0x78 != fill 0xA5
    add esp, 40000
    xor eax, eax
    ret
""",
"exitdeep": """
BITS 32
global _start
section .text
_start:
    mov ebx, [esp + 4]          ; api
    push dword msg
    call [ebx + 8]              ; api->puts(msg)
    add esp, 4
    call f1
    mov eax, 99                 ; must never run
    ret
f1: push ebp
    mov ebp, esp
    call f2
    pop ebp
    ret
f2: push dword 1
    push dword 2
    push dword 9
    call [ebx + 16]             ; api->exit(9): does not return
    mov eax, 98                 ; must never run
    ret
section .data
msg: db "bye", 10, 0
""",
"ovf": """
BITS 32
global _start
section .text
_start:
    mov ecx, 16340
.l: push dword 0xDEAD0001       ; 16340 * 4 = 65360 bytes: into the 256-byte guard at the bottom, but still inside the stack (Day 54: going below it faults)
    loop .l
    add esp, 65360
    xor eax, eax
    ret
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
                           "--build-id=none", "-s", o, "-o", out])
    return out


BYTES = {n: open(build_asm(n, s), "rb").read() for n, s in ASM.items()}

A = 0x08048000
RX = 5


def mkelf(segs, entry, size=400, phoff=52):
    b = bytearray((i * 7 + 1) & 0xFF for i in range(size))
    b[0:16] = b"\x7fELF\x01\x01\x01" + b"\x00" * 9
    struct.pack_into("<HHIIIIIHHHHHH", b, 16, 2, 3, 1, entry, phoff, 0, 0,
                     52, 32, len(segs), 40, 0, 0)
    for i, (t, off, va, fsz, msz, fl, al) in enumerate(segs):
        struct.pack_into("<IIIIIIII", b, phoff + 32 * i, t, off, va, va, fsz, msz, fl, al)
    return bytes(b)


NEG = {   # PT_LOAD [0x09FF0000, +0x1000) lies INSIDE the user stack; the other ends exactly at BASE
    "onstack.sw":  mkelf([(1, 0, BASE, 0x100, 0x1000, RX, 0x1000)], BASE + 0x10),
    "overlap1.sw": mkelf([(1, 0, BASE - 0x1000, 0x100, 0x1001, RX, 0x1000)], BASE - 0x1000 + 0x10),  # 1 byte into the stack
    "belowok.sw":  mkelf([(1, 0, BASE - 0x1000, 0x100, 0x1000, RX, 0x1000)], BASE - 0x1000 + 0x10),  # ends exactly at BASE
}

shutil.copyfile(SRC, IMG)
hello_bytes = open(HELLO, "rb").read()
adds = [("hello.sw", hello_bytes)] + [(n + ".sw", BYTES[n]) for n in BYTES]
adds += list(NEG.items())
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


mcopy("EXITDEEP.SW", BYTES["exitdeep"])
mcopy("DEEP.SW", BYTES["deep"])

# Day 55: the api table and the return stub live in the user runtime page at 0x0A000000 (kernel/exec/urt.asm)
URT_BASE = 0x0A000000
G_API = URT_BASE + nm_symbol(KELF, "urt_api") - nm_symbol(KELF, "urt_start")
EXIT_STUB = URT_BASE + nm_symbol(KELF, "urt_ret_stub") - nm_symbol(KELF, "urt_start")

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
    part = scr.split(cmd, 1)[-1]
    return [l.strip() for l in part.splitlines() if l.strip() and not l.strip().endswith("/ >")]


def stack_line(scr):
    mm = re.search(r"elfrun: stack used (\d+) of (\d+) bytes \(top (0x[0-9a-f]{8})\), left via (ret|api->exit|sys_exit)", scr)
    return (int(mm.group(1)), int(mm.group(2)), mm.group(3), mm.group(4)) if mm else None


def u32(b):
    return int.from_bytes(b, "little")


m = Machine(IMG, "d47", gui=False)
try:
    # ---------------- A: boot ---------------------------------------------------------
    scr, seen = boot(m)
    check("A0 boot to prompt", "/ >" in scr, scr)
    bad = [l for l in seen if "FAIL" in l or "PANIC" in l.upper()]
    check("A1 no FAIL/PANIC line on the boot console (%d lines seen)" % len(seen), not bad, "\n".join(bad))

    # ---------------- B: the program starts on the user stack, ABI-aligned --------------
    scr = run(m, "elfrun -v IDE3/esp.sw")
    check("B1 esp.sw returns 2559 = 0x09FF: esp>>16 is inside the user stack; (esp+4)%16==0; api!=NULL "
          "(a bad value would be +100000 / a kernel-stack address)",
          "elfrun: IDE3/esp.sw exited with code 2559" in scr, scr)
    sl = stack_line(scr)
    check("B2 -v report: stack top 0x0a000000, size 65536, left via ret, used 20 (just the entry frame)",
          sl == (20, 65536, "0x0a000000", "ret"), scr)
    # Day 54: the stack lives in pool frames; elfrun -v prints where ("stack pages pa BASE_PA .. TOP_PA").
    mo = re.search(r"stack pages pa 0x([0-9a-f]{8}) \.\. 0x([0-9a-f]{8})", scr)
    spa = int(mo.group(1), 16) if mo else 0
    check("B2b stack frames are contiguous in the pool (top page = base + 15 pages)",
          mo is not None and int(mo.group(2), 16) - spa == 15 * 4096, scr)
    api_ptr = u32(m._read_bytes(spa + (TOP - 16 - BASE), 4))
    stub = u32(m._read_bytes(spa + (TOP - 20 - BASE), 4))
    check("B3 guest RAM [TOP-16] == the runtime-page api table (0x%08x, Day 55): the api pointer is the 1st cdecl argument" % G_API, api_ptr == G_API, hex(api_ptr))
    check("B4 guest RAM [TOP-20] == the runtime-page ret stub (0x%08x): the 'return address'" % EXIT_STUB, stub == EXIT_STUB, hex(stub))
    # ---------------- C: stack usage and BOTH exit paths --------------------------------
    scr = run(m, "elfrun -v IDE3/deep.sw")
    sl = stack_line(scr)
    mo = re.search(r"stack pages pa 0x([0-9a-f]{8})", scr)
    spa = int(mo.group(1), 16) if mo else 0
    check("C1 deep.sw: 40000 B of locals -> 'stack used 40020' (20 + 40000), left via ret, exit 0",
          sl == (40020, 65536, "0x0a000000", "ret") and "exited with code 0" in scr, scr)
    w = m._read_bytes(spa + (TOP - 40020 - BASE), 4)
    check("C2 guest RAM at TOP-40020 holds the dword the program wrote (0x12345678)", u32(w) == 0x12345678, w.hex())
    scr = run(m, "elfrun -v IDE3/exitdeep.sw")
    got = [l for l in lines_after(scr, "elfrun -v IDE3/exitdeep.sw")
           if not l.startswith(("elfrun: address space", "elfrun: entry va", "elfrun: stack pages"))]   # Day 54 lines
    check("C3 exitdeep.sw: output 'bye' then 'exited with code 9' (the 99/98 after api->exit never ran)",
          got[1:3] == ["bye", "elfrun: IDE3/exitdeep.sw exited with code 9"], scr)
    sl = stack_line(scr)
    check("C4 left via api->exit (3 nested frames + pushes dropped in one step)",
          sl is not None and sl[3] == "api->exit" and sl[0] > 20, scr)
    scr = run(m, "elfrun IDE3/exitdeep.sw")
    check("C5 without -v the output is exactly the Day 46 shape (no stack line)",
          stack_line(scr) is None and "exited with code 9" in scr, scr)
    scr = run(m, "elfrun -v IDE3/hello.sw")
    sl = stack_line(scr)
    check("C6 hello.sw -v: 'Hello from hello.sw!', exit 0, via sys_exit (Day 52: hello.sw exits through int 0x80), small stack use",
          "Hello from hello.sw!" in scr and "exited with code 0" in scr and sl is not None and sl[3] == "sys_exit" and 20 <= sl[0] <= 1024, scr)   # Day 51: hello.sw prints via `int 0x80`, so the interrupt frame + dispatcher run on this stack too (~424 B); still "small" next to 64 KB

    # ---------------- D: overflow is detected AFTER the run -----------------------------
    scr = run(m, "elfrun -v IDE3/ovf.sw", 3.5)
    n = norm(scr)
    check("D1 ovf.sw: still reports 'exited with code 0' (it did return)", "elfrun: IDE3/ovf.sw exited with code 0" in n, scr)
    check("D2 -v report: used 65280..65535 of 65536 (the guard bytes were hit, the stack was not left)",
          stack_line(scr) is not None and 65280 <= stack_line(scr)[0] < 65536, scr)
    check("D3 then 'user stack overflow (guard area overwritten)'", "user stack overflow (guard area overwritten)" in n and "after run" in n, scr)

    # ---------------- E: the kernel and shell are intact --------------------------------
    scr = run(m, "ls", 1.5)
    check("E1 ls works after all those runs", "hello.sw" in scr and "esp.sw" in scr, scr)
    scr = run(m, "echo alive-47", 1.0)
    check("E2 keyboard + echo work", "alive-47" in scr, scr)
    scr = run(m, "elfrun IDE3/hello.sw", 2.5)
    check("E3 hello.sw runs again after an overflow (stack is re-prepared each run)",
          "Hello from hello.sw!" in scr and "exited with code 0" in scr and "overflow" not in scr, scr)
    scr = run(m, "elfrun -v IDE0/EXITDEEP.SW", 3.5)
    check("E4 FAT12: EXITDEEP.SW -> bye, code 9, api->exit",
          "bye" in scr and "exited with code 9" in scr and (stack_line(scr) or (0, 0, 0, ""))[3] == "api->exit", scr)
    scr = run(m, "elfrun -v IDE0/DEEP.SW", 3.5)
    sl = stack_line(scr)
    check("E5 FAT12: DEEP.SW -> stack used 40020, ret", sl is not None and sl[0] == 40020 and sl[3] == "ret", scr)
    scr = run(m, "elfrun")
    check("E6 usage mentions -v", "usage: elfrun [-v] <file>" in scr, scr)
    scr = run(m, "elfrun -v")
    check("E7 '-v' alone is a usage error", "usage: elfrun" in scr and "running" not in scr, scr)

    # ---------------- F: a segment may not overlap the user stack -----------------------
    for name, tag in (("onstack.sw", "inside the stack"), ("overlap1.sw", "1 byte into the stack")):
        scr = run(m, "elfrun IDE3/" + name)
        check("F %s (%s): rejected at load time, nothing executed" % (name, tag),
              "segment overlaps the user stack area" in scr and "nothing was executed" in scr and "running" not in scr, scr)
        scr = run(m, "elfload IDE3/" + name)
        check("F %s: elfload (Day 44) rejects it too" % name, "segment overlaps the user stack area" in scr and "elfload: OK" not in scr, scr)
    scr = run(m, "elfload IDE3/belowok.sw")
    check("F3 belowok.sw (ends exactly at the stack base) is accepted by elfload", "elfload: OK" in scr, scr)

    # ---------------- H: earlier commands still work ------------------------------------
    scr = run(m, "help", 1.5)
    check("H1 help lists elfrun", "elfrun" in scr, scr)
    m.line("cd IDE3", 0.8)
    scr = run(m, "elfmap hello.sw", 3.0)
    check("H2 elfmap (Day 45) still works", "elfmap: OK" in scr, scr)
    scr = run(m, "elfrun hello.sw", 2.5)
    check("H3 bare name after cd", "Hello from hello.sw!" in scr and "exited with code 0" in scr, scr)
finally:
    m.close()

# ---------------- G: RAM at the program, none at the stack (qemu -m 150) ---------------
m150 = Machine(IMG, "d47m150", gui=False, extra=["-m", "150"])
try:
    scr, seen = boot(m150)
    check("G0 boots with 150 MB (program area 0x08048000 is RAM, stack area 0x09FF0000 is not)", "/ >" in scr, scr)
    scr = run(m150, "elfmap IDE3/hello.sw", 3.0)
    check("G1 the program segments load fine at 128 MB (elfmap OK)", "elfmap: OK" in scr, scr)
    scr = run(m150, "elfrun IDE3/hello.sw", 4.0)
    n = norm(scr)
    check("G2 (Day 54) no RAM at 0x09FF0000, but the stack is a pool frame now: elfrun runs, no readback error",
          "Hello from hello.sw!" in n and "exited with code 0" in n and "readback mismatch" not in n, scr)
    check("G3 no 'cannot run' / 'nothing was executed'", "cannot run" not in n and "nothing was executed" not in n, scr)
    scr = run(m150, "ls", 1.5)
    check("G4 shell alive", "hello.sw" in scr, scr)
finally:
    m150.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
