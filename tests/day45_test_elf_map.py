"""
tests/day45_test_elf_map.py - Day 45 (load PT_LOAD segments into memory: `elfmap`),
TEXT build, on FAT *and* LMFS.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day45_test_elf_map.py
    (host unit test of the loader core: tests/day45_elf_host_test.c)

Needs: qemu-system-i386, readelf (binutils), mtools (mcopy), python3.

A scratch copy of lumaos-5.0-text.img is prepared on the HOST:
  * the LMFS volume (IDE3, LBA 100000) is rebuilt with tools/mkfs.lmfs --add so it holds
    hello.sw, data.sw (real toolchain output: 4 PT_LOADs with .rodata/.data/.bss),
    big.sw (12000-byte synthetic 2-segment ELF with 0x2000 B of .bss, Program Header
    table at file offset 11000), four one-defect ELFs and a plain text file;
  * the FAT12 volume (IDE0, LBA 5000) gets DATA.SW, BIG.SW and a few defects via mcopy
    (multi-cluster files, so segment bytes come from several FAT clusters).

What it proves, through the real kernel in QEMU text mode:
  A. boot: no FAIL/PANIC line on the console;
  B. `elfmap IDE3/data.sw` prints exactly what the HOST computes from the file
     (copy/zero sizes, FNV-1a sums, first bytes of every segment, .bss bytes, entry
     bytes, totals) AND - independent of anything the kernel printed - the guest's
     PHYSICAL MEMORY read through the QEMU monitor (`xp`) equals the file bytes at every
     p_vaddr and is zero over every .bss;
  C. the same for hello.sw, and for FAT12 (IDE0/DATA.SW, IDE0/BIG.SW): identical output
     on both file systems, identical memory;
  D. a rejected file NEVER touches memory (the poison/load only start after validation);
  E. bad paths / non-ELF / usage / '..' / cd + bare name;
  F. help lists elfmap, elfload/elfinfo still work;
  G. QEMU with -m 64 (no RAM at 0x08048000): the load fails with the readback-mismatch
     error and the image is reported NOT loaded.
"""
import os, re, shutil, struct, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
HELLO = "tests/hello.sw"
DATA = "tests/data.sw"
IMG = "/tmp/d45_elf.img"
FAT12_OFFSET = 5000 * 512
LMFS_LBA = 100000

for need in (SRC, HELLO, DATA, "tools/mkfs.lmfs"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

A = 0x08048000
RX, RW = 5, 6
PT_LOAD, PT_NOTE, GNU_STACK = 1, 4, 0x6474e551


def mkelf(segs, entry, size, phoff=52):
    b = bytearray((i * 7 + 1) & 0xFF for i in range(size))
    b[0:16] = b"\x7fELF\x01\x01\x01" + b"\x00" * 9
    struct.pack_into("<HHIIIIIHHHHHH", b, 16, 2, 3, 1, entry, phoff, 0, 0,
                     52, 32, len(segs), 40, 0, 0)
    for i, (t, off, va, fsz, msz, fl, al) in enumerate(segs):
        struct.pack_into("<IIIIIIII", b, phoff + 32 * i, t, off, va, va, fsz, msz, fl, al)
    return bytes(b)


GOOD_RX = (PT_LOAD, 0, A, 0x200, 0x200, RX, 0x1000)
GOOD_RW = (PT_LOAD, 0x1000, A + 0x1000, 0x800, 0x2800, RW, 0x1000)
STACK = (GNU_STACK, 0, 0, 0, 0, RW, 0x10)
GOOD1 = (PT_LOAD, 0, A, 0x84, 0x84, RX, 0x1000)
BIG = mkelf([GOOD_RX, GOOD_RW, STACK], A + 0x40, 12000, phoff=11000)


def one(seg, entry=A + 0x10, size=400):
    return mkelf([seg], entry, size)


LMFS_NEG = {   # one defect each (all rejected by elf_load() BEFORE any memory is touched)
    "nomagic.sw":  (b"\x00" + one(GOOD1)[1:],                                  "bad magic"),
    "fsz.sw":      (one((PT_LOAD, 0, A, 0x100, 0x80, RX, 0x1000)),             "p_filesz > p_memsz"),
    "bssentry.sw": (one(GOOD1, A + 0x84),                                      "not inside an executable segment"),
    "pasteof.sw":  (one((PT_LOAD, 300, A + 300, 200, 200, RX, 0x1000), A + 0x130), "segment data outside the file"),
}

tmp = tempfile.mkdtemp()
shutil.copyfile(SRC, IMG)
data_bytes = open(DATA, "rb").read()
hello_bytes = open(HELLO, "rb").read()
bigf = os.path.join(tmp, "big.sw"); open(bigf, "wb").write(BIG)

# ---- host side 1: rebuild the LMFS volume with files preinstalled --------------
adds = [("hello.sw", hello_bytes), ("data.sw", data_bytes), ("big.sw", BIG),
        ("notelf.txt", b"plain text, definitely not an ELF\n")]
adds += [(k, v[0]) for k, v in LMFS_NEG.items()]
args = [sys.executable, "tools/mkfs.lmfs", "-o", os.path.join(tmp, "lmfs.img"), "--no-verify"]
for name, data in adds:
    p = os.path.join(tmp, "add_" + name)
    open(p, "wb").write(data)
    args += ["--add", "%s:%s" % (p, name)]
subprocess.check_call(args, stdout=subprocess.DEVNULL)
subprocess.check_call(["dd", "if=" + os.path.join(tmp, "lmfs.img"), "of=" + IMG, "conv=notrunc",
                       "bs=512", "seek=%d" % LMFS_LBA, "status=none"])


# ---- host side 2: FAT12 volume --------------------------------------------------
def mcopy(name, data):
    p = os.path.join(tmp, "fat_" + name)
    open(p, "wb").write(data)
    subprocess.check_call(["mcopy", "-o", "-i", "%s@@%d" % (IMG, FAT12_OFFSET), p, "::" + name])


mcopy("DATA.SW", data_bytes)
mcopy("BIG.SW", BIG)
mcopy("FSZ.SW", LMFS_NEG["fsz.sw"][0])


# ---- host truth ------------------------------------------------------------------
def fnv(b):
    h = 0x811C9DC5
    for x in b:
        h ^= x
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def segments(path):
    """[(index, off, va, filesz, memsz, flags_str)] of every PT_LOAD with memsz > 0, from readelf."""
    l = subprocess.check_output(["readelf", "-lW", path], text=True)
    rows = []
    for m_ in re.finditer(r"^\s+(\w+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+([RWE ]{1,3}?)\s+(0x[0-9a-f]+)\s*$", l, re.M):
        rows.append(m_.groups())
    out = []
    for i, (t, off, va, pa, fsz, msz, fl, al) in enumerate(rows):
        if t == "LOAD" and int(msz, 16):
            out.append((i, int(off, 16), int(va, 16), int(fsz, 16), int(msz, 16), fl.strip()))
    entry = int(re.search(r"Entry point address:\s+(0x[0-9a-f]+)", subprocess.check_output(["readelf", "-hW", path], text=True)).group(1), 16)
    return out, entry


def flg3(fl):
    return ("R" if "R" in fl else " ") + ("W" if "W" in fl else " ") + ("E" if "E" in fl else " ")


def hexs(b):
    return " ".join("%02x" % x for x in b)


def expected_lines(path, data):
    """Every line elfmap prints after the 'Pre-filled' line, as the HOST computes it."""
    segs, entry = segments(path)
    lines = []
    for (i, off, va, fsz, msz, fl) in segs:
        lines.append("#%d 0x%08x %s copy 0x%08x zero 0x%08x sum 0x%08x [ok]" %
                     (i, va, flg3(fl), fsz, msz - fsz, fnv(data[off:off + fsz])))
        if fsz:
            lines.append("%08x: %s" % (va, hexs(data[off:off + min(16, fsz)])))
        if msz > fsz:
            lines.append("bss %08x: %s" % (va + fsz, hexs(b"\x00" * min(8, msz - fsz))))
    es = next(s for s in segs if s[2] <= entry and entry - s[2] < s[3])
    room = es[3] - (entry - es[2])
    eoff = es[1] + (entry - es[2])
    lines.append("entry %08x: %s" % (entry, hexs(data[eoff:eoff + min(16, room)])))
    lines.append("Total: %d segment(s), %d B copied, %d B zeroed" %
                 (len(segs), sum(s[3] for s in segs), sum(s[4] - s[3] for s in segs)))
    lines.append("elfmap: OK - image loaded, entry 0x%08x" % entry)
    return lines, segs, entry


# ---- run -------------------------------------------------------------------------
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
            break          # no sleep: poll as fast as the monitor answers, so short-lived boot lines are not missed
    return scr, seen


def run(m, cmd, settle=2.5):
    m.line("clear", 0.5)
    m.line(cmd, settle)
    return m.text_screen()


def check_memory(m, tag, segs, data):
    """The guest's PHYSICAL memory (monitor `xp`) == file bytes at p_vaddr, zeros over .bss."""
    for (i, off, va, fsz, msz, fl) in segs:
        if fsz:
            got = m._read_bytes(va, fsz)
            check("%s memory seg#%d [0x%08x,+0x%x) == file bytes" % (tag, i, va, fsz),
                  got == data[off:off + fsz], "first diff at %s" % next(
                      (k for k in range(min(len(got), fsz)) if got[k] != data[off + k]), "len %d/%d" % (len(got), fsz)))
        if msz > fsz:
            got = m._read_bytes(va + fsz, msz - fsz)
            check("%s memory seg#%d .bss [0x%08x,+0x%x) all zero (RAM was pre-filled 0xA5)" % (tag, i, va + fsz, msz - fsz),
                  got == b"\x00" * (msz - fsz), "non-zero bytes: %d" % sum(1 for x in got if x))


m = Machine(IMG, "d45", gui=False)
try:
    # ---------------- A: boot -------------------------------------------------------
    scr, seen = boot(m)
    check("A0 boot to prompt", "/ >" in scr, scr)
    bad = [l for l in seen if "FAIL" in l or "PANIC" in l.upper()]
    check("A1 no FAIL/PANIC line on the boot console (%d lines seen)" % len(seen), not bad, "\n".join(bad))

    # ---------------- B/C: real ELFs and synthetic ELF on LMFS and FAT12 -----------------
    cases = [
        ("B data.sw  LMFS", "IDE3/data.sw", DATA, data_bytes, 3.0),
        ("B hello.sw LMFS", "IDE3/hello.sw", HELLO, hello_bytes, 2.5),
        ("C big.sw   LMFS", "IDE3/big.sw", bigf, BIG, 3.0),
        ("C DATA.SW  FAT12", "IDE0/DATA.SW", DATA, data_bytes, 4.0),
        ("C BIG.SW   FAT12", "IDE0/BIG.SW", bigf, BIG, 4.0),
        ("C HELLO.SW FAT12", "IDE0/HELLO.SW", HELLO, hello_bytes, 3.0),
    ]
    outputs = {}
    for tag, path, hostpath, data, settle in cases:
        want, segs, entry = expected_lines(hostpath, data)
        scr = run(m, "elfmap " + path, settle)
        n = norm(scr)
        outputs[tag] = n
        check("%s: size line" % tag, "%s: %d bytes" % (path, len(data)) in scr, scr)
        check("%s: 'Pre-filled with 0xA5, loading %d PT_LOAD segment(s)'" % (tag, len(segs)),
              "Pre-filled with 0xA5, loading %d PT_LOAD segment(s):" % len(segs) in n, scr)
        for ln in want:
            check("%s: line == host: %s" % (tag, ln), norm(ln) in n, scr)
        check("%s: no [BSS NOT ZERO] / [SUM MISMATCH] / FAIL" % tag,
              "BSS NOT ZERO" not in n and "SUM MISMATCH" not in n and "elfmap: FAIL" not in n, scr)
        check_memory(m, tag, segs, data)

    # same output text on both file systems (everything after the first line)
    strip = lambda s: re.sub(r"^.*?bytes\n", "", s, flags=re.S)
    for a, b in (("B data.sw  LMFS", "C DATA.SW  FAT12"), ("C big.sw   LMFS", "C BIG.SW   FAT12"),
                 ("B hello.sw LMFS", "C HELLO.SW FAT12")):
        check("C %s == %s (same output on LMFS and FAT12)" % (a.split()[1], b.split()[1]),
              strip(outputs[a]) == strip(outputs[b]), outputs[a] + "\n----\n" + outputs[b])

    # ---------------- D: a rejected file never touches memory ------------------------------
    run(m, "elfmap IDE3/data.sw", 3.0)
    before = m._read_bytes(A, 0x100)
    seg0 = segments(DATA)[0][0][3]           # filesz of PT_LOAD #0 (headers only: 0xd4)
    check("D0 data.sw's first segment (%d B) is in memory at 0x08048000" % seg0,
          before[:seg0] == data_bytes[:seg0])
    for name, (_, why) in LMFS_NEG.items():
        scr = run(m, "elfmap IDE3/" + name)
        check("D LMFS %s -> 'elfmap: FAIL - ... %s' + 'image NOT loaded (memory untouched)'" % (name, why),
              "elfmap: FAIL" in scr and why in scr and "elfmap: OK" not in scr and
              "image NOT loaded (memory untouched)" in scr and "Pre-filled" not in scr, scr)
    scr = run(m, "elfmap IDE0/FSZ.SW")
    check("D FAT FSZ.SW rejected, nothing pre-filled", "elfmap: FAIL" in scr and "p_filesz > p_memsz" in scr and "Pre-filled" not in scr, scr)
    after = m._read_bytes(A, 0x100)
    check("D1 memory at 0x08048000 untouched by every rejected file (no poison, no load)", after == before)

    # ---------------- E: non-ELF and bad paths ------------------------------------------
    scr = run(m, "elfmap IDE3/notelf.txt")
    check("E1 34-byte text file on LMFS", "shorter than the 52-byte ELF header" in scr and "elfmap: FAIL" in scr and "NOT loaded" in scr, scr)
    scr = run(m, "elfmap IDE0/HELLO.TXT")
    check("E2 text file on FAT (>52 bytes)", "bad magic" in scr and "elfmap: FAIL" in scr and "NOT loaded" in scr, scr)
    scr = run(m, "elfmap IDE3/nope.sw")
    check("E3 missing file", "cannot open" in scr, scr)
    scr = run(m, "elfmap kday42")
    check("E4 directory", "directory" in scr, scr)
    scr = run(m, "elfmap")
    check("E5 usage", "usage: elfmap" in scr, scr)
    scr = run(m, "elfmap kday42/../hello.sw", 3.0)
    check("E6 '..' in the path resolves", "elfmap: OK" in scr and "IDE3/hello.sw" in scr, scr)

    # ---------------- F: help, cd + bare name, other commands unchanged ------------------
    scr = run(m, "help", 1.5)
    check("F1 listed by help", "elfmap" in scr, scr)
    m.line("cd IDE3", 0.8)
    scr = run(m, "elfmap data.sw", 3.0)
    check("F2 bare name after cd IDE3", "elfmap: OK" in scr and "Total: 4 segment(s)" in norm(scr), scr)
    scr = run(m, "elfload data.sw", 2.0)
    check("F3 elfload (Day 44) still works", "elfload: OK" in scr and "4 PT_LOAD" in scr, scr)
    scr = run(m, "elfinfo data.sw", 2.0)
    check("F4 elfinfo (Day 43) still works", "check: OK" in scr, scr)
    scr = run(m, "ls", 1.5)
    check("F5 ls lists data.sw and hello.sw", "data.sw" in scr and "hello.sw" in scr, scr)
finally:
    m.close()

# ---------------- G: no RAM at the load address (qemu -m 64) -----------------------------
m64 = Machine(IMG, "d45m64", gui=False, extra=["-m", "64"])
try:
    scr, seen = boot(m64)
    check("G0 boots with only 64 MB of RAM", "/ >" in scr, scr)
    scr = run(m64, "elfmap IDE3/hello.sw", 3.0)
    n = norm(scr)
    check("G1 load fails with the readback-mismatch error",
          "elfmap: FAIL - memory readback mismatch" in n, scr)
    check("G2 failing address is the first byte of the segment (0x08048000): wrote 0x7f, read back 0x00 (or 0xff)",
          re.search(r"at 0x08048000: wrote 0x7f, read back 0x(00|ff)", n) is not None, scr)
    check("G3 image reported NOT loaded, no OK line",
          "image NOT loaded" in n and "elfmap: OK" not in n, scr)
    scr = run(m64, "elfload IDE3/hello.sw", 2.0)
    check("G4 elfload (validation only, no memory access) still OK on 64 MB", "elfload: OK" in scr, scr)
finally:
    m64.close()
    shutil.rmtree(tmp, ignore_errors=True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
