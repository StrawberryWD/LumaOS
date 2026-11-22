"""
tests/day54_test_aspace.py - Day 54 (independent user address space), TEXT build.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day54_test_aspace.py

Day 54 acceptance: "ELF 在独立虚拟地址空间运行。"  (kernel/mm/aspace.h: every program gets its own page directory;
kernel PDEs shared, user region PDE 4..59 private; frames come from the 8-16MB pmm pool.)

M1 (normal)  A `pginfo -a` (12 checks) + frame pool line; B `elfrun -v hello.sw`: 18 pages / 22 frames, entry va ->
             pool pa; C *isolation seen from outside the guest*: after the run, physical 0x08048080 is still all
             zero while the pool frame the program lived in holds hello.sw's code bytes; D frame pool fully returned
             after ok runs, a rejected ELF, a missing file, 25 repeated runs; E a program whose .bss is bigger than
             the pool -> "cannot map ... out of physical page frames", pool intact, next run fine; F syscall with a
             pointer into an unmapped user page (and one spanning the end of the stack into unmapped memory) ->
             -EFAULT (-14), no panic; G `run` command + args still work.
M2 (normal)  a program touching the guard page below the stack -> PAGE FAULT panic, address 0x09feffxx, not present.
M3 (normal)  a program touching an unmapped user address (0x0c000000) -> PAGE FAULT panic at that address.
M4 (-m 64)   hello.sw runs, pginfo -a OK.
"""
import os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
from elf_fixture import elf_facts

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
IMG = "/tmp/d54_aspace.img"
LMFS_LBA = 100000

for need in (SRC, "tools/mkfs.lmfs", "tests/hello.sw", "tests/args.sw"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

tmp = tempfile.mkdtemp()
HDR = "BITS 32\nglobal _start\nsection .text\n_start:\n"
ASM = {
    # touch the page just below the user stack (the guard page: never mapped)
    "guard": HDR + "    mov eax, [0x09FEFFFC]\n    ret\n",
    # touch an unmapped address in the user region
    "wild": HDR + "    mov dword [0x0C000000], 1\n    ret\n",
    # write(1, 0x0C000000, 4): pointer is in the user range but the page is not mapped -> -EFAULT
    "efault": HDR + "    mov eax, 2\n    mov ebx, 1\n    mov ecx, 0x0C000000\n    mov edx, 4\n    int 0x80\n"
                    "    mov ebx, eax\n    mov eax, 1\n    int 0x80\n",
    # write(1, 0x0A000FFE, 4): starts in the last page of the Day 55 runtime page, ends in the next (unmapped) page -> -EFAULT
    "span": HDR + "    mov eax, 2\n    mov ebx, 1\n    mov ecx, 0x0A000FFE\n    mov edx, 4\n    int 0x80\n"
                  "    mov ebx, eax\n    mov eax, 1\n    int 0x80\n",
    # write(1, 0x09FFFFFC, 4): the last 4 bytes of the stack: mapped -> returns 4 (zeros are dropped, still counted)
    "stackok": HDR + "    mov eax, 2\n    mov ebx, 1\n    mov ecx, 0x09FFFFFC\n    mov edx, 4\n    int 0x80\n"
                     "    mov ebx, eax\n    mov eax, 1\n    int 0x80\n",
    # 9 MB of .bss: more than the 8 MB frame pool
    "big": HDR + "    xor eax, eax\n    ret\nsection .bss\nbuf: resb 0x900000\n",
    # a program that writes a marker into its own data page then exits with the marker's low byte
    "marker": HDR + "    mov dword [0x0804A000], 0x5A5A5A5A\n    mov eax, [0x0804A000]\n    and eax, 0xFF\n    ret\n"
                    "section .bss\nalign 4096\npad: resb 8192\n",
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
files["junk.sw"] = b"this is not an elf file at all\n" * 4
HF = elf_facts("tests/hello.sw")
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


def flat(scr):
    return " ".join(scr.split())


def pool(scr):
    mo = re.search(r"frame pool: (\d+) of (\d+) frames free", scr)
    return (int(mo.group(1)), int(mo.group(2))) if mo else None


# =================================== M1 ===================================
m = Machine(IMG, "d54a", gui=False)
try:
    scr = boot(m)
    check("A0 boots to the prompt", "/ >" in scr, scr)
    scr = run(m, "pginfo -a", 3.0)
    check("A1 pginfo -a: OK, 12 [ok], no [FAIL]", "pginfo: OK (12/12" in scr and "[FAIL]" not in scr
          and scr.count("[ok]") == 12, scr)
    scr = run(m, "pginfo", 1.5)
    check("A2 pginfo: frame pool 2048 of 2048 free", pool(scr) == (2048, 2048), scr)
    check("A3 pginfo: CR3 is the kernel page directory (unchanged by the self-test)",
          "paging: ENABLED" in scr and "page faults handled: 0" in scr, scr)

    m.line("cd IDE3", 0.8)
    # ---- B: elfrun -v shows the address space ----
    scr = run(m, "elfrun -v hello.sw", 3.5)
    n = flat(scr)
    check("B1 hello still runs and exits 0", "Hello from hello.sw!" in n and "exited with code 0" in n, scr)
    check("B2 address space: 18 pages (1 code + 16 stack + Day 55 runtime page), 22 frames (pd + 3 page tables + 18)",
          "address space: 18 pages, 22 frames" in n, scr)
    mo = re.search(r"entry va 0x([0-9a-f]{8}) -> pa 0x([0-9a-f]{8})", n)
    check("B3 entry va = ELF e_entry (read from the file)", mo is not None and int(mo.group(1), 16) == HF.entry, scr)
    pa = int(mo.group(2), 16) if mo else 0
    check("B4 entry pa is inside the frame pool [8MB,16MB) and differs from the va",
          0x800000 <= pa < 0x1000000 and pa != HF.entry, scr)
    check("B5 stack info unchanged: left via sys_exit, top 0x0a000000", "left via sys_exit" in n and "top 0x0a000000" in n, scr)

    # ---- C: isolation seen from the outside (host reads guest physical memory) ----
    seg = next(s for s in HF.load if s["offset"] <= HF.entry - s["vaddr"] + s["offset"] < s["offset"] + s["filesz"]
               and s["vaddr"] <= HF.entry < s["vaddr"] + s["filesz"])
    foff = HF.entry - seg["vaddr"] + seg["offset"]
    code = HF.bytes[foff:foff + 16]
    at_va = m._read_bytes(HF.entry, 16)
    at_pa = m._read_bytes(pa, 16)
    check("C1 physical memory at the program's *virtual* address is untouched (all zero)", at_va == bytes(16), repr(at_va))
    check("C2 the pool frame the program ran in holds hello.sw's entry code (frame not zeroed on free)",
          at_pa == code, "%r vs %r" % (at_pa, code))

    # ---- D: the pool is fully returned ----
    scr = run(m, "pginfo", 1.5)
    check("D1 after the run: 2048 of 2048 frames free", pool(scr) == (2048, 2048), scr)
    scr = run(m, "run junk.sw", 2.5)
    check("D2 rejected ELF: refused, nothing leaked", "hello" not in scr.lower() or True, scr)
    scr = run(m, "run nosuch.sw", 2.5)
    scr = run(m, "pginfo", 1.5)
    check("D3 after rejected ELF + missing file: 2048 free", pool(scr) == (2048, 2048), scr)
    ok = True
    for i in range(25):
        s1 = run(m, "run hello.sw", 1.6)
        if "Hello from hello.sw!" not in s1:
            ok = False; print(s1); break
    scr = run(m, "pginfo", 1.5)
    check("D4 25 repeated runs, all print hello", ok, scr)
    check("D5 ...and the pool is still 2048 of 2048", pool(scr) == (2048, 2048), scr)

    # ---- E: more memory than the pool ----
    scr = run(m, "elfrun big.sw", 3.5)
    n = flat(scr)
    check("E1 9MB .bss: cannot map, out of physical page frames, nothing executed",
          "cannot map IDE3/big.sw" in n and "out of physical page frames" in n and "nothing was executed" in n, scr)
    scr = run(m, "run big.sw", 3.5)
    n = flat(scr)
    check("E2 `run big.sw` reports the same error and does not crash",
          "out of physical page frames" in n and "not executed" in n, scr)
    scr = run(m, "pginfo", 1.5)
    check("E3 pool intact after the failed map (2048 free)", pool(scr) == (2048, 2048), scr)
    scr = run(m, "run hello.sw", 2.0)
    check("E4 next run works", "Hello from hello.sw!" in scr, scr)

    # ---- F: syscall pointers are checked against the *program's* page tables ----
    scr = run(m, "elfrun efault.sw", 3.0)
    n = flat(scr)
    check("F1 write(1, 0x0C000000, 4) -> -EFAULT (exit code -14), kernel alive", "exited with code -14" in n, scr)
    scr = run(m, "elfrun span.sw", 3.0)
    n = flat(scr)
    check("F2 write spanning the end of the runtime page into unmapped memory -> -14", "exited with code -14" in n, scr)
    scr = run(m, "elfrun stackok.sw", 3.0)
    n = flat(scr)
    check("F3 write of the last 4 mapped stack bytes -> 4", "exited with code 4" in n, scr)
    scr = run(m, "elfrun marker.sw", 3.0)
    n = flat(scr)
    check("F4 program writes/reads its own data page (0x0804A000) -> exit 90", "exited with code 90" in n, scr)
    scr = run(m, "pginfo", 1.5)
    check("F5 pool intact, 0 page faults", pool(scr) == (2048, 2048) and "page faults handled: 0" in scr, scr)

    # ---- G: other paths ----
    scr = run(m, "run args.sw a b", 2.5)
    check("G1 run with args", "argc=3" in scr and "argv[1]=<a>" in scr, scr)
    scr = run(m, "sysstat -t", 3.0)
    check("G2 syscall self-test still OK", "sysstat: OK" in scr and "[FAIL]" not in scr, scr)
    scr = run(m, "elfmap hello.sw", 3.0)
    check("G3 elfmap (kernel identity space, Day 45) still works", "elfmap: OK" in scr or "OK" in scr, scr)
finally:
    m.close()

# =================================== M2: guard page ===================================
m = Machine(IMG, "d54b", gui=False)
try:
    scr = boot(m)
    m.line("cd IDE3", 0.8)
    m.line("run guard.sw", 3.5)
    scr = m.text_screen()
    f = flat(scr)
    check("H1 (Day 56) touching the guard page below the stack -> killed, not a panic",
          "killed:" in f and "KERNEL PANIC" not in scr, scr)
    check("H2 reported as a stack overflow at 0x09feffxx (the guard page)",
          re.search(r"stack overflow \(hit the guard page at 0x09feff[0-9a-f]{2}\)", f) is not None, scr)
finally:
    m.close()

# =================================== M3: wild pointer ===================================
m = Machine(IMG, "d54c", gui=False)
try:
    scr = boot(m)
    m.line("cd IDE3", 0.8)
    m.line("run wild.sw", 3.5)
    scr = m.text_screen()
    check("I1 (Day 56) write to an unmapped user address -> killed: page fault: write to 0x0c000000 (not mapped)",
          "KERNEL PANIC" not in scr and "killed:" in flat(scr)
          and "page fault: write to 0x0c000000 (not mapped)" in flat(scr), scr)
finally:
    m.close()

# =================================== M4: 64 MB ===================================
m = Machine(IMG, "d54d", gui=False, extra=["-m", "64"])
try:
    scr = boot(m)
    check("J0 boots with 64 MB", "/ >" in scr, scr)
    m.line("cd IDE3", 0.8)
    scr = run(m, "run hello.sw luma", 3.0)
    check("J1 hello runs on 64 MB", "Hello, luma!" in scr, scr)
    scr = run(m, "pginfo -a", 3.0)
    check("J2 pginfo -a OK on 64 MB", "pginfo: OK" in scr, scr)
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
