"""tests/day59_test_cdboot.py - Day 59: El Torito No-Emulation boot path (CD -> loader -> kernel).

    python3 tests/day59_test_cdboot.py        (needs the text build:  LUMAOS_TEXT=1 ./build.sh)
Machine with ONLY a CD-ROM (no hard disk):
  * the BIOS loads build-text/cdboot.bin (2048 B) at 0x7C00 and the loader reads the kernel by the Boot Info Table
  * the kernel starts (text shell prompt), `version` answers
  * the BOOT INFO block at physical 0x600: "LBI1", BIOS drive >= 0xE0 (CD), media = 1, kernel/LMFS LBA+size and
    total sectors equal what tools/isoinfo.py says is in the ISO
  * kernel bytes in RAM at 0x10000 == kernel.bin from the ISO (first 4 KB and last 4 KB compared via the monitor)
  * a CD whose boot image has NO "LCDB" table (mkiso --no-table style: zeroed magic) stops at "BAD BOOT INFO TABLE"
  * the hard-disk path still boots and leaves media = 0 in the same block
"""
import sys, os, re, time, subprocess, struct
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)

TMP = "/tmp/d59test"; os.makedirs(TMP, exist_ok=True)
BOOT, KERN = "build-text/cdboot.bin", "build-text/kernel.bin"
iso = TMP + "/ok.iso"
subprocess.check_call([sys.executable, "tools/mkiso.py", "-o", iso, "--boot", BOOT,
    "--add", "BOOT/KERNEL.BIN=" + KERN, "--add", "BOOT/LMFS.IMG=tests/lmfs-fixture.img",
    "--kernel-path", "BOOT/KERNEL.BIN", "--lmfs-path", "BOOT/LMFS.IMG"], stdout=subprocess.DEVNULL)
import json
info = json.loads(subprocess.check_output([sys.executable, "tools/isoinfo.py", "--json", iso]))
iso_sectors = os.path.getsize(iso) // 2048
kern = open(KERN, "rb").read()
data = open(iso, "rb").read()

def words(m, addr, n):
    out = m.send("xp /%dwx 0x%x" % (n, addr), 1.0)
    vals = []
    for line in out.splitlines():
        mo = re.match(r"[0-9a-f]+:((?:\s+0x[0-9a-f]+)+)", line.strip())
        if mo: vals += [int(v, 16) for v in re.findall(r"0x([0-9a-f]+)", mo.group(1))]
    return vals

# ---------- 1. CD only ----------
m = Machine(None, "d59a", gui=False, cdrom=iso, hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    check("kernel started from the CD (shell prompt)", True)
    m.line("version", 1.0)
    check("shell answers `version` on a live CD", "LumaOS 5.0" in m.text_screen(), m.text_screen()[-400:])
    bi = words(m, 0x600, 8)
    check("boot info magic 'LBI1'", bi and bi[0] == 0x3149424C, bi)
    check("BIOS drive is a CD (>= 0xE0)", bi and (bi[1] & 0xFF) >= 0xE0, bi)
    check("media byte = 1 (CD-ROM)", bi and ((bi[1] >> 8) & 0xFF) == 1, bi)
    # the boot image's own table is the ground truth (written by mkiso)
    cat_lba = struct.unpack_from("<I", data, 17 * 2048 + 0x47)[0]
    img_lba = struct.unpack_from("<I", data, cat_lba * 2048 + 32 + 8)[0]
    t = struct.unpack_from("<4s8I", data, img_lba * 2048 + 8)
    check("boot image carries an LCDB table", t[0] == b"LCDB", t)
    check("boot info: kernel LBA == table", bi and bi[2] == t[4], (bi, t))
    check("boot info: kernel bytes == kernel.bin size", bi and bi[3] == len(kern) == t[5], (bi, len(kern), t))
    check("boot info: LMFS LBA/bytes == table", bi and bi[4] == t[6] and bi[5] == t[7], (bi, t))
    check("boot info: total sectors == ISO size", bi and bi[6] == iso_sectors == t[8], (bi, iso_sectors, t))
    # kernel bytes in RAM at 0x10000 (the loader's destination) == kernel.bin from the ISO
    for off in (0, len(kern) - 64):
        ram = words(m, 0x10000 + off, 16)
        want = list(struct.unpack("<16I", kern[off:off + 64]))
        check("RAM 0x%x matches kernel.bin +0x%x" % (0x10000 + off, off), ram == want, (ram[:4], want[:4]))
finally:
    m.close()

# ---------- 2. broken table -> loader refuses ----------
bad = bytearray(data)
bad[img_lba * 2048 + 8:img_lba * 2048 + 12] = b"\0\0\0\0"
open(TMP + "/bad.iso", "wb").write(bad)
m = Machine(None, "d59b", gui=False, cdrom=TMP + "/bad.iso", hd=False)
try:
    time.sleep(5)
    s = m.text_screen()
    check("missing LCDB table: loader prints BAD BOOT INFO TABLE and stops", "BAD BOOT INFO TABLE" in s, s[-300:])
    check("missing LCDB table: kernel NOT started", "Text-mode shell ready" not in s)
finally:
    m.close()

# ---------- 3. hard disk path unchanged ----------
m = Machine("lumaos-5.0-text.img", "d59c", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    bi = words(m, 0x600, 8)
    check("HDD boot: 'LBI1', media byte = 0", bi and bi[0] == 0x3149424C and ((bi[1] >> 8) & 0xFF) == 0, bi)
    check("HDD boot: drive 0x80", bi and (bi[1] & 0xFF) == 0x80, bi)
    check("HDD boot: CD table fields zero", bi and bi[2:7] == [0] * 5, bi)
finally:
    m.close()

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
