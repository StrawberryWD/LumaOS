"""
tests/day49_test_args.py - Day 49 (argc/argv), TEXT build, FAT12 and LMFS.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day49_test_args.py

Needs: qemu-system-i386, nasm, ld, mtools (mcopy), python3.

Day 49 acceptance: "`run hello.sw luma` 能读取参数。"
`run` cuts the raw command line into words (space/Tab separated, "double quotes" group, "" = empty arg,
unterminated quote = error) and elf_run_args() copies the strings onto the program's user stack; the entry is
`int _start(const luma_api_t *api, int argc, char **argv)`.

Fixtures: hello.sw (prints "Hello, <argv[1]>!" when given an argument), args.sw (prints argc, every argv[i] as <..>,
and whether argv[argc]==NULL) -- both real gcc output --, plus nasm programs built here:
    argc.sw    returns argc                     (-> "exited with code N" line is the observable)
    align.sw   returns 0 if (esp+4)%16==0 at entry, argv%4==0, argv[argc]==NULL, argv[0] > argv, else 1..4
    legacy.sw  Day 47 style: reads ONLY the api pointer ([esp+4]) and prints "legacy ok" (extra args must be harmless)
    sum.sw     returns the sum of strlen(argv[i]) for all i  (checks every string is complete, NUL terminated)

Proves: A boot; B the acceptance line (LMFS/FAT/bare name); C args.sw exact output with quotes / empty / extra
spaces / joined quotes / path argv[0]; D limits (32 words ok, 33 refused, program not run); E unterminated quote;
F alignment + legacy program + string integrity; G elfrun (argc=0) unchanged; H shell alive afterwards.
"""
import os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
HELLO, ARGS = "tests/hello.sw", "tests/args.sw"
IMG = "/tmp/d49_args.img"
FAT12_OFFSET = 5000 * 512
LMFS_LBA = 100000

for need in (SRC, HELLO, ARGS, "tools/mkfs.lmfs"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

tmp = tempfile.mkdtemp()
HDR = "BITS 32\nglobal _start\nsection .text\n_start:\n"
ASM = {
    "argc": HDR + "    mov eax, [esp + 8]\n    ret\n",
    "align": HDR + """    lea eax, [esp + 4]          ; the value (esp+4) that the entry sees: 16-aligned per the i386 ABI
    test eax, 15
    jnz .b1
    mov ecx, [esp + 8]          ; argc
    mov edx, [esp + 12]         ; argv
    test edx, 3
    jnz .b2
    cmp dword [edx + ecx*4], 0  ; argv[argc] == NULL
    jnz .b3
    mov eax, [edx]              ; argv[0]
    cmp eax, edx                ; strings live above the pointer array
    jbe .b4
    xor eax, eax
    ret
.b1: mov eax, 1
    ret
.b2: mov eax, 2
    ret
.b3: mov eax, 3
    ret
.b4: mov eax, 4
    ret
""",
    "legacy": HDR + """    mov ebx, [esp + 4]
    push dword msg
    call [ebx + 8]
    add esp, 4
    xor eax, eax
    ret
section .data
msg: db "legacy ok", 10, 0
""",
    "sum": HDR + """    mov ecx, [esp + 8]
    mov edx, [esp + 12]
    xor eax, eax
    xor ebx, ebx
.next:
    cmp ebx, ecx
    jge .done
    mov esi, [edx + ebx*4]
.len:
    cmp byte [esi], 0
    je .eos
    inc eax
    inc esi
    jmp .len
.eos:
    inc ebx
    jmp .next
.done:
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


files = {"hello.sw": open(HELLO, "rb").read(), "args.sw": open(ARGS, "rb").read()}
for n, s in ASM.items():
    files[n + ".sw"] = build_asm(n, s)

shutil.copyfile(SRC, IMG)
a = [sys.executable, "tools/mkfs.lmfs", "-o", os.path.join(tmp, "lmfs.img"), "--no-verify"]
for n, d in files.items():
    p = os.path.join(tmp, "add_" + n); open(p, "wb").write(d); a += ["--add", "%s:%s" % (p, n)]
subprocess.check_call(a, stdout=subprocess.DEVNULL)
subprocess.check_call(["dd", "if=" + os.path.join(tmp, "lmfs.img"), "of=" + IMG, "conv=notrunc", "bs=512",
                       "seek=%d" % LMFS_LBA, "status=none"])
for n in ("hello.sw", "args.sw"):
    p = os.path.join(tmp, "fat_" + n); open(p, "wb").write(files[n])
    subprocess.check_call(["mcopy", "-o", "-i", "%s@@%d" % (IMG, FAT12_OFFSET), p, "::" + n.upper()])

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


m = Machine(IMG, "d49", gui=False)
try:
    scr = boot(m)
    check("A0 boot to prompt", "/ >" in scr, scr)

    # ---------------- B: the acceptance line ---------------------------------------------------
    m.line("cd IDE3", 0.8)
    expect(m, "B1 acceptance (bare name after cd)", "run hello.sw luma", ["Hello from hello.sw!", "Hello, luma!"])
    expect(m, "B2 no argument -> no greeting line", "run hello.sw", ["Hello from hello.sw!"])
    expect(m, "B3 extra args are ignored by hello.sw", "run hello.sw a b c", ["Hello from hello.sw!", "Hello, a!"])
    expect(m, "B4 full LMFS path", "run IDE3/hello.sw Claude", ["Hello from hello.sw!", "Hello, Claude!"])
    expect(m, "B5 FAT12", "run IDE0/HELLO.SW fat", ["Hello from hello.sw!", "Hello, fat!"], 3.0)

    # ---------------- C: args.sw exact output ---------------------------------------------------
    expect(m, "C1 argv[0] only", "run args.sw", ["argc=1", "argv[0]=<args.sw>", "argv[argc]=NULL"])
    expect(m, "C2 path is argv[0] verbatim", "run IDE3/args.sw x",
           ["argc=2", "argv[0]=<IDE3/args.sw>", "argv[1]=<x>", "argv[argc]=NULL"])
    expect(m, "C3 quotes, spaces, empty arg", 'run args.sw one "two words" ""',
           ["argc=4", "argv[0]=<args.sw>", "argv[1]=<one>", "argv[2]=<two words>", "argv[3]=<>", "argv[argc]=NULL"], 3.0)
    expect(m, "C4 repeated spaces collapse", "run args.sw   a     b",
           ["argc=3", "argv[0]=<args.sw>", "argv[1]=<a>", "argv[2]=<b>", "argv[argc]=NULL"])
    expect(m, "C5 quotes inside a word join it", 'run args.sw a"b c"d',
           ["argc=2", "argv[0]=<args.sw>", "argv[1]=<ab cd>", "argv[argc]=NULL"])
    expect(m, "C6 FAT12 args", "run IDE0/ARGS.SW q", ["argc=2", "argv[0]=<IDE0/ARGS.SW>", "argv[1]=<q>", "argv[argc]=NULL"], 3.0)
    expect(m, "C7 a long argument (60 chars) is not cut", "run args.sw " + "z" * 60,
           ["argc=2", "argv[0]=<args.sw>", "argv[1]=<%s>" % ("z" * 60), "argv[argc]=NULL"])

    # ---------------- D: limits -------------------------------------------------------------------
    scr = run(m, "run argc.sw " + " ".join(["a"] * 31))      # the echoed command wraps at 80 columns: check the result line
    check("D1 32 words (argv[0] + 31) are accepted: exit code 32 = argc", "run: IDE3/argc.sw exited with code 32" in scr
          and "too many" not in scr, scr)
    scr = run(m, "run argc.sw " + " ".join(["a"] * 32))
    check("D2 33 words refused, program not run", "run: too many or too long arguments" in scr and "exited with code" not in scr, scr)
    expect(m, "D3 argc.sw with no args returns 1", "run argc.sw", ["run: IDE3/argc.sw exited with code 1"])

    # ---------------- E: quote errors ---------------------------------------------------------------
    scr = run(m, 'run args.sw "abc')
    check("E1 unterminated quote", "run: unterminated quote" in scr and "argc=" not in scr, scr)
    scr = run(m, 'run ""')
    check("E2 empty program name -> usage", "usage: run <program.sw>" in scr, scr)
    scr = run(m, 'run "args.sw" q')
    check("E3 quoted program name works", "argv[1]=<q>" in scr, scr)

    # ---------------- F: ABI properties ----------------------------------------------------------------
    expect(m, "F1 alignment/NULL/ordering at entry (0 = ok), no args", "run align.sw", [])
    expect(m, "F2 same with 5 args of odd lengths", "run align.sw a bb ccc dddd eeeee", [])
    expect(m, "F3 legacy 1-argument program still works with extra args", "run legacy.sw x y", ["legacy ok"])
    # strings complete: lengths of 'sum.sw'(6) + 'hello'(5) + ''(0) + 'a b'(3) = 14
    expect(m, "F4 all strings are complete and NUL terminated (total length 14)", 'run sum.sw hello "" "a b"',
           ["run: IDE3/sum.sw exited with code 14"])
    expect(m, "F5 independent runs: args of one run do not leak into the next", "run args.sw",
           ["argc=1", "argv[0]=<args.sw>", "argv[argc]=NULL"])

    # ---------------- G/H: older commands and shell alive ----------------------------------------------
    scr = run(m, "elfrun hello.sw", 2.5)
    check("G1 elfrun (argc=0) unchanged: prints only the greeting line, exit 0",
          "Hello from hello.sw!" in scr and "Hello," not in scr and "exited with code 0" in scr, scr)
    scr = run(m, "elfrun -v args.sw", 2.5)
    check("G2 elfrun -v with argc=0: argc=0 and argv[argc]=NULL", "argc=0" in scr and "argv[argc]=NULL" in scr and "stack used" in scr, scr)
    scr = run(m, "echo alive-49", 1.0)
    check("H1 echo works", "alive-49" in scr, scr)
    scr = run(m, "ls", 1.5)
    check("H2 ls works", "hello.sw" in scr and "args.sw" in scr, scr)
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
