"""
tests/day44_test_elf_load.py - Day 44 (elf_load(): open + verify an ELF32 via the VFS,
on FAT *and* on LMFS), TEXT build.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day44_test_elf_load.py
    (host unit test of the pure checks:  see tests/day44_elf_host_test.c)

Needs: qemu-system-i386, readelf (binutils), mtools (mcopy), python3.

A scratch copy of lumaos-5.0-text.img is prepared on the HOST:
  * the LMFS volume (IDE3, LBA 100000) is rebuilt with tools/mkfs.lmfs --add so it
    contains hello.sw, a 12000-byte two-segment ELF (needs the indirect block; its
    Program Header table sits at file offset 11000, i.e. the loader must "seek"
    across LMFS blocks), four ELFs with one defect each, and a plain text file;
  * the FAT12 volume (IDE0, LBA 5000) gets the same 12000-byte ELF (multi-cluster)
    and a set of one-defect ELFs injected with mcopy.

What it proves, through the real kernel in QEMU text mode:
  A. boot: every boot-time self-test line seen on the console is free of "FAIL";
  B. `elfload IDE3/hello.sw` (LMFS) prints Magic OK + Entry + segment table, and every
     number equals what the HOST's `readelf` says for tests/hello.sw;
  C. same for the 12000-byte two-segment ELF on LMFS (indirect block, phoff far from 52,
     RW segment with .bss) AND on FAT12 -> identical output on both file systems;
  D. rejected for the RIGHT reason: LMFS (4 cases) and FAT12 (14 cases);
  E. non-ELF / bad paths: text file on LMFS, 0-byte and 20-byte files on FAT, missing
     file, directory, no argument, `..` in the path;
  F. `help` lists elfload, `cd IDE3` + bare file name works, elfinfo still works.
"""
import os, re, shutil, struct, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
HELLO = "tests/hello.sw"
IMG = "/tmp/d44_elf.img"
FAT12_OFFSET = 5000 * 512
LMFS_LBA = 100000

for need in (SRC, HELLO, "tools/mkfs.lmfs"):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

A = 0x08048000
RX, RW = 5, 6          # PF_R|PF_X, PF_R|PF_W
PT_LOAD, PT_NOTE, PT_INTERP, PT_DYNAMIC, GNU_STACK = 1, 4, 3, 2, 0x6474e551


def mkelf(segs, entry, size, phoff=52, machine=3, etype=2):
    """Header + Program Headers at e_phoff, zero/pattern padded to `size` bytes."""
    b = bytearray((i * 7 + 1) & 0xFF for i in range(size))
    b[0:16] = b"\x7fELF\x01\x01\x01" + b"\x00" * 9
    struct.pack_into("<HHIIIIIHHHHHH", b, 16, etype, machine, 1, entry, phoff, 0, 0,
                     52, 32, len(segs), 40, 0, 0)
    for i, (t, off, va, fsz, msz, fl, al) in enumerate(segs):
        struct.pack_into("<IIIIIIII", b, phoff + 32 * i, t, off, va, va, fsz, msz, fl, al)
    return bytes(b)


GOOD1 = (PT_LOAD, 0, A, 0x84, 0x84, RX, 0x1000)
GOOD_RX = (PT_LOAD, 0, A, 0x200, 0x200, RX, 0x1000)
GOOD_RW = (PT_LOAD, 0x1000, A + 0x1000, 0x800, 0x2800, RW, 0x1000)
STACK = (GNU_STACK, 0, 0, 0, 0, RW, 0x10)

BIG = mkelf([GOOD_RX, GOOD_RW, STACK], A + 0x40, 12000, phoff=11000)

def one(seg, entry=A + 0x10, size=400, n_extra=0):
    return mkelf([seg] + [(PT_NOTE, 0, 0, 0, 0, 0, 0)] * n_extra, entry, size)

# name -> (bytes, expected text in "elfload: FAIL - ...")
NEG = {
    "INTERP":   (one((PT_INTERP,) + GOOD1[1:]),                          "dynamic executable"),
    "TOOMANY":  (one(GOOD1, n_extra=8),                                  "too many program headers"),
    "NOLOAD":   (one((PT_NOTE,) + GOOD1[1:]),                            "no loadable segment"),
    "FSZGTMSZ": (one((PT_LOAD, 0, A, 0x100, 0x80, RX, 0x1000)),          "p_filesz > p_memsz"),
    "PASTEOF":  (one((PT_LOAD, 300, A + 300, 200, 200, RX, 0x1000), A + 0x130), "segment data outside the file"),
    "LOWADDR":  (one((PT_LOAD, 0, 0x100000, 0x84, 0x84, RX, 0x1000), 0x100010), "outside the user load area"),
    "WRAPADDR": (one((PT_LOAD, 0, 0xFFFFFFF0, 0x20, 0x20, RX, 0), 0xFFFFFFF0),   "outside the user load area"),
    "BADALIGN": (one((PT_LOAD, 0, A, 0x84, 0x84, RX, 0x1800)),           "alignment invalid"),
    "MISALIGN": (one((PT_LOAD, 0, A + 8, 0x84, 0x84, RX, 0x1000), A + 0x18), "alignment invalid"),
    "OVERLAP":  (mkelf([GOOD1, (PT_LOAD, 0x40, A + 0x40, 0x20, 0x20, RX, 0x1000)], A + 0x10, 400), "overlap"),
    "BSSENTRY": (one(GOOD1, A + 0x84),                                   "not inside an executable segment"),
    "NOXENTRY": (one((PT_LOAD, 0, A, 0x84, 0x84, RW, 0x1000)),           "not inside an executable segment"),
    "ARM":      (mkelf([GOOD1], A + 0x10, 400, machine=40),              "wrong CPU"),
    "DYN":      (mkelf([GOOD1], A + 0x10, 400, etype=3),                 "not ET_EXEC"),
}
LMFS_NEG = {   # 4 one-defect ELFs on LMFS + a text file
    "nomagic.sw":  (b"\x00" + one(GOOD1)[1:],          "not an ELF file"),
    "fsz.sw":      (NEG["FSZGTMSZ"][0],                NEG["FSZGTMSZ"][1]),
    "bssentry.sw": (NEG["BSSENTRY"][0],                NEG["BSSENTRY"][1]),
    "pasteof.sw":  (NEG["PASTEOF"][0],                 NEG["PASTEOF"][1]),
}

tmp = tempfile.mkdtemp()
shutil.copyfile(SRC, IMG)

# ---- host side 1: rebuild the LMFS volume with files preinstalled --------------
adds = [("hello.sw", open(HELLO, "rb").read()), ("big.sw", BIG),
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

mcopy("BIG.SW", BIG)
for k, (data, _) in NEG.items():
    mcopy(k + ".SW", data)
mcopy("SHORT.SW", mkelf([GOOD1], A + 0x10, 400)[:20])
mcopy("EMPTY.SW", b"")
bigf = os.path.join(tmp, "big.sw"); open(bigf, "wb").write(BIG)

# ---- host truth from readelf ----------------------------------------------------
def readelf(path):
    h = subprocess.check_output(["readelf", "-hW", path], text=True)
    l = subprocess.check_output(["readelf", "-lW", path], text=True)
    entry = int(re.search(r"Entry point address:\s+(0x[0-9a-f]+)", h).group(1), 16)
    segs = []
    for m in re.finditer(r"^\s+(\w+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+([RWE ]{1,3}?)\s+(0x[0-9a-f]+)\s*$", l, re.M):
        segs.append(m.groups())
    return entry, segs

def expected_rows(path):
    """readelf -l rows rendered in elfload's column format."""
    entry, segs = readelf(path)
    rows = []
    for (t, off, va, pa, fsz, msz, fl, al) in segs:
        flg = ("R" if "R" in fl else " ") + ("W" if "W" in fl else " ") + ("E" if "E" in fl else " ")
        rows.append("%-10s %s %s %s %s %s %s" % (t, "0x%08x" % int(off, 16), "0x%08x" % int(va, 16),
                    "0x%08x" % int(fsz, 16), "0x%08x" % int(msz, 16), flg, al))
    return entry, rows

# ---- run ------------------------------------------------------------------------
m = Machine(IMG, "d44", gui=False)
fails = []
def check(name, cond, scr=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)
        print(scr)

def run(cmd, settle=1.5):
    m.line("clear", 0.5)
    m.line(cmd, settle)
    return m.text_screen()

def norm(scr):
    return re.sub(r"[ \t]+", " ", scr)

try:
    # ---------------- A: boot self-tests --------------------------------------
    seen, t0 = [], time.time()
    while time.time() - t0 < 200:
        scr = m.text_screen()
        for ln in scr.splitlines():
            if ln.strip() and ln not in seen:
                seen.append(ln)
        if "/ >" in scr:
            break
        time.sleep(0.35)
    allboot = "\n".join(seen)
    check("A0 boot to prompt", "/ >" in scr, scr)
    bad = [l for l in seen if "FAIL" in l or "PANIC" in l.upper()]
    check("A1 no FAIL/PANIC line on the boot console (%d lines seen)" % len(seen), not bad, "\n".join(bad))

    # ---------------- B: hello.sw on LMFS vs readelf ---------------------------
    entry, rows = expected_rows(HELLO)
    scr = run("elfload IDE3/hello.sw")
    n = norm(scr)
    size = os.path.getsize(HELLO)
    check("B1 size line", "IDE3/hello.sw: %d bytes" % size in scr, scr)
    check("B2 Magic OK", "Magic: OK" in n, scr)
    check("B3 Entry == readelf (0x%08x)" % entry, "Entry: 0x%08x" % entry in n, scr)
    for r in rows:
        check("B4 phdr row == readelf: " + r, norm(r) in n, scr)
    # derived from the file's first Program Header (not hard-coded: hello.sw changes with the fixture)
    _h = open(HELLO, "rb").read()
    _po = struct.unpack_from("<I", _h, 28)[0]
    _va, _fs, _ms = struct.unpack_from("<III", _h, _po + 8)[0], struct.unpack_from("<I", _h, _po + 16)[0], struct.unpack_from("<I", _h, _po + 20)[0]
    check("B5 range/totals", "1 PT_LOAD, 0x%08x..0x%08x, from file %d B, in memory %d B" % (_va, _va + _ms, _fs, _ms) in n, scr)
    check("B6 OK verdict", "elfload: OK" in scr, scr)

    # ---------------- C: big two-segment ELF, LMFS == FAT12 ---------------------
    entry, rows = expected_rows(bigf)
    outs = {}
    for label, path in (("LMFS", "IDE3/big.sw"), ("FAT12", "IDE0/BIG.SW")):
        scr = run("elfload " + path, 2.0)
        n = outs[label] = norm(scr)
        check("C %s size line" % label, "%s: 12000 bytes" % path in scr, scr)
        check("C %s Entry == readelf" % label, "Entry: 0x%08x (in segment #0, +0x40)" % entry in n, scr)
        check("C %s Program Headers (3)" % label, "Program Headers (3):" in n, scr)
        for r in rows:
            check("C %s row == readelf: %s" % (label, r), norm(r) in n, scr)
        check("C %s load summary" % label,
              "2 PT_LOAD, 0x08048000..0x0804b800, from file 2560 B, in memory 10752 B" in n, scr)
        check("C %s OK verdict" % label, "elfload: OK" in scr, scr)
    strip = lambda s: re.sub(r"^.*?bytes\n", "", s, flags=re.S)
    check("C same table on both file systems", strip(outs["LMFS"]) == strip(outs["FAT12"]), outs["LMFS"] + "\n----\n" + outs["FAT12"])

    # ---------------- D: rejected for the right reason -------------------------
    for fname, (_, why) in LMFS_NEG.items():
        scr = run("elfload IDE3/" + fname)
        check("D LMFS %s -> '%s'" % (fname, why), why in scr and "elfload: OK" not in scr, scr)
    for k, (_, why) in NEG.items():
        scr = run("elfload IDE0/%s.SW" % k)
        check("D FAT  %s.SW -> '%s'" % (k, why), "elfload: FAIL" in scr and why in scr and "elfload: OK" not in scr, scr)
    # header-level failures stop BEFORE the segment table is printed
    scr = run("elfload IDE0/ARM.SW")
    check("D header failure prints no table", "Program Headers" not in scr and "Magic:   OK" in scr, scr)

    # ---------------- E: non-ELF and bad paths ---------------------------------
    scr = run("elfload IDE3/notelf.txt")
    check("E1 text file on LMFS", "Magic:   BAD" in scr and "not an ELF file" in scr, scr)
    scr = run("elfload IDE0/SHORT.SW")
    check("E2 20-byte file", "Magic:   BAD" in scr and "shorter than" in scr, scr)
    scr = run("elfload IDE0/EMPTY.SW")
    check("E3 0-byte file", "0 bytes" in scr and "shorter than" in scr, scr)
    scr = run("elfload IDE0/HELLO.TXT")
    check("E4 text file on FAT", "not an ELF file" in scr, scr)
    scr = run("elfload IDE3/nope.sw")
    check("E5 missing file", "cannot open" in scr, scr)
    scr = run("elfload kday42")
    check("E6 directory", "directory" in scr, scr)
    scr = run("elfload")
    check("E7 usage", "usage: elfload" in scr, scr)
    scr = run("elfload kday42/../hello.sw")
    check("E8 '..' in the path resolves", "elfload: OK" in scr and "IDE3/hello.sw" in scr, scr)

    # ---------------- F: help, cd + bare name, elfinfo unchanged -----------------
    scr = run("help", 1.5)
    check("F1 listed by help", "elfload" in scr, scr)
    m.line("cd IDE3", 0.8)
    scr = run("elfload hello.sw", 2.0)
    check("F2 bare name after cd IDE3 (LMFS)", "elfload: OK" in scr, scr)
    scr = run("elfinfo hello.sw", 2.0)
    check("F3 elfinfo on LMFS still works", "check: OK" in scr, scr)
    scr = run("elfinfo big.sw", 2.0)
    check("F4 elfinfo on the multi-block LMFS file", "check: OK" in scr and "PH off:" in scr and "11000" in scr, scr)
    scr = run("ls", 1.5)
    check("F5 ls still lists the preinstalled files", "hello.sw" in scr and "big.sw" in scr and "12000 bytes" in scr, scr)
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
