"""tests/day62_test_lmfs_resource.py - Day 62: the LMFS image on the ISO is loaded as a system resource.

    python3 tests/day62_test_lmfs_resource.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
Live CD (QEMU, only the CD, `-boot d`):
  * ATAPI driver finds the drive (IDENTIFY PACKET), the boot CD is recognised by its PVD (CD001 + size)
  * BOOT/LMFS.IMG is copied into RAM at 0x600000, byte-identical to build/sys-lmfs.img (checked in guest RAM)
  * it is mounted as RAM0 (named mount), becomes the current root; `mounts` / `cdinfo` / `ls` / `read` work
  * programs on it run (`run hello.sw`, `run args.sw a b`) and the volume is writable (create, delete)
With a hard disk ALSO attached (boot from the CD): only the CD is taken for a CD (the ATA disk aborts
IDENTIFY PACKET), and the hard-disk image file is byte-for-byte unchanged afterwards.
GUI build on both display backends: same volume visible from the desktop terminal.
"""
import sys, os, re, time, hashlib, struct
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)

sysimg = open("build-text/sys-lmfs.img", "rb").read()
def words(m, addr, n):
    out = m.send("xp /%dwx 0x%x" % (n, addr), 1.0)
    vals = []
    for line in out.splitlines():
        mo = re.match(r"[0-9a-f]+:((?:\s+0x[0-9a-f]+)+)", line.strip())
        if mo: vals += [int(v, 16) for v in re.findall(r"0x([0-9a-f]+)", mo.group(1))]
    return vals

def live_checks(m, tag, screen):
    s = screen()
    check("%s: ATAPI drive found" % tag, "ATAPI drives found: 1" in s, s[-1500:])
    check("%s: boot CD recognised by its PVD" % tag, "Boot CD = cd0 (ISO9660" in s, s[-1500:])
    check("%s: LMFS.IMG copied to RAM 0x00600000" % tag, "copied from CD LBA" in s and "0x00600000" in s, s[-1500:])
    check("%s: mounted as RAM0" % tag, "mounted as RAM0" in s, s[-1500:])
    check("%s: no FAIL on the boot screen" % tag, "FAIL" not in s, s[-1500:])

# ---------------- text build, CD only ----------------
m = Machine(None, "d62a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    live_checks(m, "text/CD", m.text_screen)
    check("shell prompt shows RAM0", "RAM0/ >" in m.text_screen(), m.text_screen()[-200:])
    for off in (0, len(sysimg) - 64):           # guest RAM == build/sys-lmfs.img (before any write)
        ram = words(m, 0x600000 + off, 16)
        want = list(struct.unpack("<16I", sysimg[off:off + 64]))
        check("RAM 0x%x == sys-lmfs.img +0x%x" % (0x600000 + off, off), ram == want, (ram[:4], want[:4]))
    m.line("mounts", 1.0)
    s = m.text_screen()
    check("`mounts` lists RAM0 / lmfs / current root", re.search(r"RAM0\s+lmfs\s+\(current root\)", s) is not None, s[-300:])
    m.line("cdinfo", 1.0)
    s = m.text_screen()
    check("`cdinfo` shows the boot CD + LMFS -> RAM0", "<- boot CD" in s and "-> RAM0" in s and "QEMU DVD-ROM" in s, s[-500:])
    m.line("ls", 1.0)
    s = m.text_screen()
    for f in ("welcome.txt", "hello.sw", "args.sw", "data.sw"):
        check("ls shows %s" % f, f in s, s[-400:])
    m.line("read welcome.txt", 1.5)
    check("`read welcome.txt` returns the file", "Welcome to LumaOS 5.0 (live CD)" in m.text_screen(), m.text_screen()[-400:])
    m.line("run hello.sw", 1.5)
    check("`run hello.sw` runs a program from the CD's LMFS", "Hello from hello.sw!" in m.text_screen(), m.text_screen()[-300:])
    m.line("run args.sw a b", 1.5)
    s = m.text_screen()
    check("`run args.sw a b` gets its arguments", "argv[1]=<a>" in s and "argv[2]=<b>" in s, s[-300:])
    m.line("create t.txt", 1.2)
    m.line("ls", 1.0)
    check("RAM0 is writable (create t.txt)", "t.txt" in m.text_screen(), m.text_screen()[-300:])
    m.line("rm t.txt", 1.2)
    m.line("ls", 1.0)
    s = m.text_screen()
    check("RAM0 is writable (rm t.txt)", s.rstrip().endswith("RAM0/ >") and s[s.rfind("RAM0/ > ls"):].count("t.txt") == 0, s[-300:])
finally:
    m.close()

# ---------------- CD + hard disk attached, boot from CD ----------------
hd = "lumaos-5.0-text.img"
h0 = hashlib.sha256(open(hd, "rb").read()).hexdigest()
m = Machine(hd, "d62b", gui=False, cdrom="lumaos-5.0-text.iso", hd=True)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    live_checks(m, "text/CD+HDD", m.text_screen)
    m.line("run hello.sw", 1.5)
    check("CD+HDD: program runs from RAM0", "Hello from hello.sw!" in m.text_screen())
finally:
    m.close()
check("hard-disk image untouched by a live-CD session (sha256)", hashlib.sha256(open(hd, "rb").read()).hexdigest() == h0)

# ---------------- GUI build ----------------
for vga in ("std", "vmware"):
    g = Machine(None, "d62g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("ls", 1.5)
        s = g.gui_screen()
        check("[%s] GUI/CD: ls shows welcome.txt + hello.sw" % vga, "welcome.txt" in s and "hello.sw" in s, s[-300:])
        g.line("run hello.sw", 2.0)
        check("[%s] GUI/CD: run hello.sw" % vga, "Hello from hello.sw!" in g.gui_screen(), g.gui_screen()[-300:])
        g.line("mounts", 1.5)
        check("[%s] GUI/CD: mounts shows RAM0" % vga, "RAM0" in g.gui_screen())
    finally:
        g.close()

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
