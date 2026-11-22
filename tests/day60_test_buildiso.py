"""tests/day60_test_buildiso.py - Day 60: build.sh produces a correct, reproducible bootable .iso.
    python3 tests/day60_test_buildiso.py       (after ./build.sh and LUMAOS_TEXT=1 ./build.sh)
"""
import sys, os, json, hashlib, subprocess, struct
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)
def sh(*a): return subprocess.check_output(list(a))

for iso, bdir, img in (("lumaos-5.0.iso", "build", "lumaos-5.0.img"), ("lumaos-5.0-text.iso", "build-text", "lumaos-5.0-text.img")):
    check("%s exists (build.sh made it next to %s)" % (iso, img), os.path.exists(iso))
    info = json.loads(sh(sys.executable, "tools/isoinfo.py", "--json", iso))
    d = open(iso, "rb").read()
    check("%s: size is a multiple of 2048" % iso, len(d) % 2048 == 0, len(d))
    cat = struct.unpack_from("<I", d, 17 * 2048 + 0x47)[0]
    e = 2048 and struct.unpack_from("<BBHBBHI", d, cat * 2048 + 32)
    check("%s: default entry bootable / No-Emulation / 4 sectors" % iso, e[0] == 0x88 and e[1] == 0 and e[5] == 4, e)
    boot_lba = e[6]
    boot = d[boot_lba * 2048: boot_lba * 2048 + 2048]
    src = open(bdir + "/cdboot.bin", "rb").read()
    # identical to build/cdboot.bin except the 36-byte Boot Info Table at offset 8
    same = boot[:8] == src[:8] and boot[44:] == src[44:]
    check("%s: boot image == cdboot.bin (only the table at +8..+43 patched)" % iso, same)
    t = struct.unpack_from("<4s8I", boot, 8)
    check("%s: table magic LCDB, PVD LBA 16, own LBA/len right" % iso, t[0] == b"LCDB" and t[1] == 16 and t[2] == boot_lba and t[3] == 2048, t)
    k = open(bdir + "/kernel.bin", "rb").read()
    check("%s: kernel at table LBA is byte-identical to kernel.bin" % iso, d[t[4] * 2048: t[4] * 2048 + t[5]] == k and t[5] == len(k))
    check("%s: kernel fits the loader limit (96 x 2048)" % iso, len(k) <= 96 * 2048, len(k))
    l = open(bdir + "/sys-lmfs.img", "rb").read()
    check("%s: LMFS.IMG byte-identical" % iso, d[t[6] * 2048: t[6] * 2048 + t[7]] == l)
    check("%s: total sectors in table == file size" % iso, t[8] == len(d) // 2048, t)
    # reproducible: build the same ISO again with the CLI
    again = "/tmp/d60_again.iso"
    sh(sys.executable, "tools/mkiso.py", "-o", again, "--boot", bdir + "/cdboot.bin", "--add", "BOOT/KERNEL.BIN=" + bdir + "/kernel.bin",
       "--add", "BOOT/LMFS.IMG=" + bdir + "/sys-lmfs.img",
       "--add", "FONTS/SYS8X8.FNT=" + bdir + "/sys8x8.fnt", "--add", "ETC/LUMAOS.CFG=resources/LUMAOS.CFG", "--volume", "LUMAOS",
       "--kernel-path", "BOOT/KERNEL.BIN", "--lmfs-path", "BOOT/LMFS.IMG")
    check("%s: reproducible (second build is byte-identical)" % iso, hashlib.sha256(open(again, "rb").read()).digest() == hashlib.sha256(d).digest())
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
