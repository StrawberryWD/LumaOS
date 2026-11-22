"""
tests/day43_test_elf.py - Day 43 (ELF32 structures + ELF Header parsing), TEXT build.

    LUMAOS_TEXT=1 ./build.sh && python3 tests/day43_test_elf.py

Needs: qemu-system-i386, readelf (binutils), mtools (mcopy).

What it proves (all through the real kernel, in QEMU text mode):
  A. `elfinfo IDE0/HELLO.SW` prints a header whose every number matches what the
     HOST's `readelf -h tests/hello.sw` says (cross-validation by an independent
     implementation) and ends with "check: OK".
  B. Bad inputs are rejected for the RIGHT reason (each is a one-field mutation
     of HELLO.SW injected into the FAT12 volume of a scratch image copy):
       bad magic, ELF64, big-endian, wrong CPU (ARM), ET_DYN (PIE), no program
       headers, program-header table outside the file, entry == 0,
       truncated file (< 52 bytes), empty file.
  C. Non-ELF files / paths: text file on FAT (IDE0/HELLO.TXT), a text file on
     LMFS (IDE3 — exercises the LMFS open/read route), missing file,
     directory, no argument.
  D. `elfinfo` is listed by `help`; `cd IDE0` + bare name works.
"""
import os, re, shutil, struct, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
SRC = "lumaos-5.0-text.img"
HELLO = "tests/hello.sw"
IMG = "/tmp/d43_elf.img"
FAT12_OFFSET = 5000 * 512      # build.sh embeds the FAT12 volume at LBA 5000

for need in (SRC, HELLO):
    if not os.path.exists(need):
        sys.exit("missing %s - run LUMAOS_TEXT=1 ./build.sh first" % need)

shutil.copyfile(SRC, IMG)
good = open(HELLO, "rb").read()

def mutate(off, fmt, val):
    b = bytearray(good)
    struct.pack_into(fmt, b, off, val)
    return bytes(b)

NEG = {                                   # NAME.SW -> (bytes, expected text)
    "BADMAGIC.SW": (mutate(0, "<B", 0x00),    "bad magic"),
    "ELF64.SW":    (mutate(4, "<B", 2),       "not ELF32"),
    "BIGEND.SW":   (mutate(5, "<B", 2),       "not little-endian"),
    "ARM.SW":      (mutate(18, "<H", 40),     "wrong CPU"),
    "DYN.SW":      (mutate(16, "<H", 3),      "not ET_EXEC"),
    "NOPH.SW":     (mutate(44, "<H", 0),      "no program headers"),
    "BIGPH.SW":    (mutate(28, "<I", 0x1000), "outside the file"),
    "NOENTRY.SW":  (mutate(24, "<I", 0),      "entry point is 0"),
    "SHORT.SW":    (good[:30],                "shorter than"),
    "EMPTY.SW":    (b"",                      "shorter than"),
}
tmp = tempfile.mkdtemp()
for name, (data, _) in NEG.items():
    p = os.path.join(tmp, name)
    open(p, "wb").write(data)
    subprocess.check_call(["mcopy", "-o", "-i", "%s@@%d" % (IMG, FAT12_OFFSET), p, "::" + name])

# ---- host-side truth from readelf ------------------------------------------
rd = subprocess.check_output(["readelf", "-h", HELLO], text=True)
def rd_val(label):
    m = re.search(re.escape(label) + r":\s+(\S+)", rd)
    return m.group(1)
truth = {
    "entry": int(rd_val("Entry point address"), 16),
    "phoff": int(rd_val("Start of program headers")),
    "shoff": int(rd_val("Start of section headers")),
    "phsz":  int(rd_val("Size of program headers")),
    "phnum": int(rd_val("Number of program headers")),
    "shsz":  int(rd_val("Size of section headers")),
    "shnum": int(rd_val("Number of section headers")),
    "shstr": int(rd_val("Section header string table index")),
    "ehsz":  int(rd_val("Size of this header")),
}

m = Machine(IMG, "elf", gui=False)
fails = []
def check(name, cond, scr=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)
        print(scr)

def run(cmd, settle=1.2):
    m.line("clear", 0.5)
    m.line(cmd, settle)
    return m.text_screen()

def field(scr, label):
    mm = re.search(r"^\s*" + re.escape(label) + r"\s+(\S+)", scr, re.M)
    return mm.group(1) if mm else None

try:
    scr = m.wait_for("/ >", timeout=150)
    check("boot to prompt", "/ >" in scr, scr)

    # ---------------- A: the good file ------------------------------------
    scr = run("elfinfo IDE0/HELLO.SW", 2.0)
    check("A1 size line", "IDE0/HELLO.SW: %d bytes" % len(good) in scr, scr)
    check("A2 magic bytes", "7f 45 4c 46 01 01 01 00" in scr, scr)
    check("A3 class/data", "ELF32" in scr and "little endian" in scr, scr)
    check("A4 type EXEC / machine i386", "EXEC" in scr and "Intel 80386" in scr, scr)
    check("A5 entry == readelf", field(scr, "Entry:") == "0x%08x" % truth["entry"], scr)
    for key, lab in (("phoff", "PH off:"), ("phsz", "PH size:"), ("phnum", "PH num:"),
                     ("shoff", "SH off:"), ("shsz", "SH size:"), ("shnum", "SH num:"),
                     ("shstr", "SH strndx:"), ("ehsz", "EH size:")):
        check("A6 %s == readelf (%d)" % (lab, truth[key]),
              field(scr, lab) == str(truth[key]), scr)
    check("A7 check OK", "check: OK" in scr, scr)

    # ---------------- B: rejected for the right reason --------------------
    for name, (_, why) in NEG.items():
        scr = run("elfinfo IDE0/" + name)
        check("B %s -> '%s'" % (name, why), why in scr and "check: OK" not in scr, scr)

    # ---------------- C: non-ELF / bad paths ------------------------------
    scr = run("elfinfo IDE0/HELLO.TXT")
    check("C1 text file on FAT: not an ELF file", "not an ELF file" in scr and "4c 75 6d 61" in scr, scr)

    m.line("clear", 0.5)
    m.line("write note.txt", 1.0)
    m.type("plain text"); m.ctrl_s(); time.sleep(1.0)
    scr = run("elfinfo note.txt")
    check("C2 text file on LMFS (IDE3): not an ELF file", "not an ELF file" in scr and "70 6c 61 69" in scr, scr)

    scr = run("elfinfo IDE0/NOPE.SW")
    check("C3 missing file", "cannot open" in scr, scr)
    scr = run("elfinfo kday42")
    check("C4 directory", "is a directory" in scr, scr)
    scr = run("elfinfo")
    check("C5 usage", "usage: elfinfo" in scr, scr)

    # ---------------- D: help + relative name -----------------------------
    scr = run("help", 1.5)
    check("D1 listed by help", "elfinfo" in scr, scr)
    m.line("cd IDE0", 0.8)
    scr = run("elfinfo hello.sw", 2.0)
    check("D2 bare name after cd IDE0", "check: OK" in scr, scr)
finally:
    m.close()
    shutil.rmtree(tmp, ignore_errors=True)
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)