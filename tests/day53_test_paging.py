"""
tests/day53_test_paging.py - Day 53 (paging: identity-mapped kernel page table), TEXT build.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day53_test_paging.py

Needs: qemu-system-i386, nasm, ld, mtools, python3.

Day 53 acceptance: "开启分页后系统仍稳定。"  (kernel/mm/paging.h: 0-256MB identity 4KB pages with page 0 unmapped,
256MB-4GB identity 4MB uncached pages, CR4.PSE, CR3, CR0.PG|WP; vector 14 -> panic with CR2 / error code / eip.)

Three virtual machines:
  M1 (normal)  A boot with paging on; B `pginfo -t` (17 checks) and `pginfo` / `pginfo <addr>` translations, error
               handling; C the older features still work under paging: run hello.sw luma (ELF at 0x08048000, user stack,
               int 0x80 write/exit), args.sw, ls, echo, IDE3 file ops, FAT; D repeated runs; E a NULL access from a
               *program* (nulp.sw: mov eax,[0]) -> the page-fault panic screen; the kernel does not limp on.
  M2 (-m 64)   boots and is stable with only 64 MB of RAM (pages above RAM are 'present' but harmless), pginfo -t OK.
  M3 (normal)  `pginfo -f` (kernel dereferences NULL) -> PAGE FAULT panic screen: address 0x00000000, page not present, read,
               kernel mode, eip inside the kernel (0x0001xxxx).
Also proves: a faulting program in M1 reports address 0x00000000 and a program eip (0x0804xxxx).
"""
import os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
IMG = "/tmp/d53_paging.img"
LMFS_LBA = 100000
FAT12_OFFSET = 5000 * 512

for need in (SRC, "tools/mkfs.lmfs", "tests/hello.sw", "tests/args.sw"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

tmp = tempfile.mkdtemp()
HDR = "BITS 32\nglobal _start\nsection .text\n_start:\n"
ASM = {
    "nulp": HDR + "    mov eax, [0]\n    ret\n",
    "nulw": HDR + "    mov dword [4], 1\n    ret\n",
    "ok": HDR + "    mov eax, [0x08048000]\n    xor eax, eax\n    ret\n",
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
subprocess.check_call(["mcopy", "-o", "-i", "%s@@%d" % (IMG, FAT12_OFFSET), p, "::HELLO.SW"])

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


def selftest(m, tag):
    scr = run(m, "pginfo -t", 3.0)
    check("%s: pginfo -t verdict OK, 17 [ok] lines, no [FAIL]" % tag,
          "pginfo: OK" in scr and "[FAIL]" not in scr and scr.count("[ok]") == 17, scr)
    return scr


def panic_screen(scr):
    return "KERNEL PANIC" in scr and "PAGE FAULT" in scr


# =================================== M1 ===================================
m = Machine(IMG, "d53a", gui=False)
try:
    scr = boot(m)
    check("A0 boots to the prompt with paging on", "/ >" in scr, scr)
    selftest(m, "B1")

    scr = run(m, "pginfo", 1.5)
    check("B2 pginfo: ENABLED, CR0 0x8001001x (PG|WP|PE), CR4 0x10 (PSE)",
          "paging: ENABLED" in scr and re.search(r"CR0 0x8001001[0-9a-f]", scr) and "CR4 0x00000010" in scr, scr)
    check("B3 pginfo: 64 page tables, 960 large pages, 65535 pages (255 MB), 0 faults",
          "64 page tables" in scr and "960 large pages" in scr and "4KB pages mapped: 65535 (255 MB)" in scr
          and "page faults handled: 0" in scr, scr)
    for addr, want in (("0", "not mapped"), ("fff", "not mapped"), ("0x1000", "0x00001000  RW supervisor 4KB-page"),
                       ("08048123", "0x08048123  RW supervisor 4KB-page"),
                       ("fd123456", "0xfd123456  RW supervisor 4MB-page uncached"),
                       ("0x10000000", "0x10000000  RW supervisor 4MB-page uncached"),
                       ("0xFFFFF000", "0xfffff000  RW supervisor 4MB-page uncached")):
        scr = run(m, "pginfo " + addr, 1.2)
        check("B4 pginfo %s -> %s" % (addr, want), want in scr, scr)
    scr = run(m, "pginfo zzz", 1.2)
    check("B5 bad address -> usage line", "usage: pginfo" in scr, scr)

    # ---- C: the older features under paging ----
    m.line("cd IDE3", 0.8)
    scr = run(m, "run hello.sw luma", 3.0)
    check("C1 run hello.sw luma works under paging (ELF load, user stack, int 0x80 write/exit)",
          body(scr, "run hello.sw luma") == ["Hello from hello.sw!", "Hello, luma!"], scr)
    scr = run(m, 'run args.sw "a b" c', 3.0)
    check("C2 args.sw: argc/argv", "argc=3" in scr and "argv[1]=<a b>" in scr and "argv[2]=<c>" in scr, scr)
    scr = run(m, "run IDE0/HELLO.SW fat", 3.0)
    check("C3 FAT12 program", "Hello, fat!" in scr, scr)
    scr = run(m, "ls", 1.5)
    check("C4 ls", "hello.sw" in scr and "nulp.sw" in scr, scr)
    scr = run(m, "echo alive-53", 1.0)
    check("C5 echo (keyboard IRQs under paging)", "alive-53" in scr, scr)
    scr = run(m, "run ok.sw", 2.5)
    check("C6 (Day 55) a program reading its own code page (0x08048000) is fine and exits 0 silently", body(scr, "run ok.sw") == [], scr)
    scr = run(m, "sysstat -t", 3.0)
    check("C7 syscall self-test still OK", "sysstat: OK" in scr and "[FAIL]" not in scr, scr)

    # ---- D: repeated runs ----
    ok = True
    for i in range(6):
        scr = run(m, "run hello.sw", 2.5)
        ok &= body(scr, "run hello.sw") == ["Hello from hello.sw!"]
    check("D1 6 repeated runs stay stable", ok, scr)
    selftest(m, "D2 (after the runs)")
    scr = run(m, "pginfo", 1.5)
    check("D3 still 0 page faults handled", "page faults handled: 0" in scr, scr)

    # ---- E: DAY56: a *program* dereferencing NULL is killed (shell lives on); a *kernel* NULL read still panics ----
    m.line("clear", 0.5)
    m.line("run nulp.sw", 3.0)
    scr = m.text_screen()
    check("E1 (Day 56) NULL read in a program -> killed, not a panic",
          "killed:" in scr and "page fault: read of 0x00000000 (not mapped)" in scr and not panic_screen(scr), scr)
    m.line("clear", 0.5)
    m.line("pginfo -f", 3.0)
    scr = m.text_screen()
    check("E2 kernel-mode NULL read (pginfo -f) -> PAGE FAULT panic screen", panic_screen(scr), scr)
    check("E3 address 0x00000000, page not present, read, kernel mode, cs 0x08",
          "at address 0x00000000" in scr and "page not present, read, kernel mode" in scr
          and re.search(r"faulting instruction at 0x00000008:0x000[0-9a-f]{5}", scr) is not None, scr)
    time.sleep(2)
    scr2 = m.text_screen()
    check("E4 the system stays halted (screen unchanged after 2 s)", scr2 == scr, scr2)
finally:
    m.close()

# =================================== M2: only 64 MB ===================================
shutil.copyfile(SRC, IMG)
subprocess.check_call(["dd", "if=" + os.path.join(tmp, "lmfs.img"), "of=" + IMG, "conv=notrunc", "bs=512",
                       "seek=%d" % LMFS_LBA, "status=none"])
m = Machine(IMG, "d53b", gui=False, extra=["-m", "64"])
try:
    scr = boot(m)
    check("F0 boots with 64 MB RAM under paging", "/ >" in scr, scr)
    selftest(m, "F1 (-m 64)")
    m.line("cd IDE3", 0.8)
    scr = run(m, "run hello.sw luma", 3.0)
    check("F2 with 64 MB there is no RAM at 0x08048000 (128 MB); since Day 54 the program runs from pool frames anyway",
          "Hello, luma!" in scr and "memory readback mismatch" not in scr, scr)
    scr = run(m, "echo still-alive", 1.0)
    check("F3 shell alive afterwards", "still-alive" in scr, scr)
finally:
    m.close()

# =================================== M3: the kernel dereferences NULL ===================================
m = Machine(IMG, "d53c", gui=False)
try:
    scr = boot(m)
    check("G0 boot", "/ >" in scr, scr)
    m.line("pginfo -f", 3.0)
    scr = m.text_screen()
    check("G1 `pginfo -f` -> KERNEL PANIC: PAGE FAULT", panic_screen(scr), scr)
    check("G2 address 0x00000000, error code 0x00000000 (page not present, read, kernel mode)",
          "at address 0x00000000" in scr and "error code 0x00000000" in scr and "page not present, read, kernel mode" in scr, scr)
    check("G3 faulting instruction is in the kernel (0x00000008:0x000xxxxx, below 1 MB)", re.search(r"faulting instruction at 0x00000008:0x000[0-9a-f]{5}", scr) is not None, scr)
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
