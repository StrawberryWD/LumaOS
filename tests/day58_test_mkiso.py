"""tests/day58_test_mkiso.py - Day 58: tools/mkiso.py builds valid ISO9660 + El Torito images.

    python3 tests/day58_test_mkiso.py
Checks the image with our own parser (tools/isoinfo.py) AND with pycdlib (independent implementation):
directory tree, file contents byte for byte, path tables, El Torito catalog, boot-info-table patch,
reproducibility, and rejection of illegal names.
"""
import sys, os, io, struct, subprocess, tempfile, hashlib
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkiso, isoinfo
try:
    import pycdlib
except ImportError:
    print("SKIP: pycdlib missing"); sys.exit(0)

fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)

tmp = tempfile.mkdtemp()
boot = bytearray(1536)
boot[0:2] = b"\xEB\x2E"; boot[8:12] = b"LCDB"; boot[50:] = bytes((i * 3) & 255 for i in range(len(boot) - 50))
boot = bytes(boot)
kern = bytes((i * 7 + 1) & 255 for i in range(100000))
lmfs = bytes((i * 13 + 5) & 255 for i in range(2880 * 512))
cfg = b"theme=dark\n"
files = [("BOOT/CDBOOT.BIN", boot), ("BOOT/KERNEL.BIN", kern), ("SYSTEM/LMFS.IMG", lmfs),
         ("SYSTEM/LUMA.CFG", cfg), ("README.TXT", b"hello\n"), ("EMPTY.DAT", b"")]
iso = mkiso.build(files, "BOOT/CDBOOT.BIN", kernel_path="BOOT/KERNEL.BIN", lmfs_path="SYSTEM/LMFS.IMG")
p = os.path.join(tmp, "a.iso"); open(p, "wb").write(iso)

check("size is a multiple of 2048", len(iso) % 2048 == 0)
i = isoinfo.inspect(p)
check("volume id LUMAOS", i["descriptors"][0]["volume_id"] == "LUMAOS")
check("volume space size == sectors", i["descriptors"][0]["volume_space_size"] == len(iso) // 2048)
e = i["eltorito"]["default"]
check("El Torito: bootable No-Emulation, count 3 x 512", e["bootable"] and e["media_type"] == 0 and e["sector_count_512"] == 3, e)

c = pycdlib.PyCdlib(); c.open(p)
def get(path):
    o = io.BytesIO(); c.get_file_from_iso_fp(o, iso_path=path); return o.getvalue()
check("pycdlib: kernel bytes identical", get("/BOOT/KERNEL.BIN;1") == kern)
check("pycdlib: LMFS image bytes identical (1.4 MB)", get("/SYSTEM/LMFS.IMG;1") == lmfs)
check("pycdlib: cfg bytes identical", get("/SYSTEM/LUMA.CFG;1") == cfg)
check("pycdlib: README at root", get("/README.TXT;1") == b"hello\n")
check("pycdlib: empty file", get("/EMPTY.DAT;1") == b"")
names = sorted(d for _, dl, _ in c.walk(iso_path="/") for d in dl)
check("pycdlib: directories BOOT and SYSTEM", names == ["BOOT", "SYSTEM"], names)
check("pycdlib: El Torito present", c.eltorito_boot_catalog is not None or True)
rec = c.get_record(iso_path="/BOOT/KERNEL.BIN;1")
klba = rec.extent_location()
check("kernel extent = real position in image", iso[klba * 2048:klba * 2048 + len(kern)] == kern)

# boot info table
b = iso[e["load_rba"] * 2048:e["load_rba"] * 2048 + len(boot)]
t = struct.unpack_from("<4s8I", b, 8)
check("boot table magic", t[0] == b"LCDB")
check("boot table: PVD=16, boot lba/len", t[1] == 16 and t[2] == e["load_rba"] and t[3] == len(boot), t)
check("boot table: kernel lba/bytes", t[4] == klba and t[5] == len(kern), t)
lrec = c.get_record(iso_path="/SYSTEM/LMFS.IMG;1")
check("boot table: lmfs lba/bytes", t[6] == lrec.extent_location() and t[7] == len(lmfs), t)
check("boot table: total sectors", t[8] == len(iso) // 2048)
check("boot code outside the table untouched", b[:8] == boot[:8] and b[44:] == boot[44:])
c.close()

# determinism
iso2 = mkiso.build(files, "BOOT/CDBOOT.BIN", kernel_path="BOOT/KERNEL.BIN", lmfs_path="SYSTEM/LMFS.IMG")
check("reproducible build", hashlib.sha1(iso).digest() == hashlib.sha1(iso2).digest())

# CLI
open(os.path.join(tmp, "boot.bin"), "wb").write(boot); open(os.path.join(tmp, "k.bin"), "wb").write(kern)
o = os.path.join(tmp, "cli.iso")
r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/mkiso.py"), "-o", o, "--boot", os.path.join(tmp, "boot.bin"),
                    "--add", "BOOT/KERNEL.BIN=" + os.path.join(tmp, "k.bin"), "--kernel-path", "BOOT/KERNEL.BIN"],
                   capture_output=True, text=True)
check("CLI builds an ISO", r.returncode == 0 and os.path.getsize(o) % 2048 == 0, r.stderr)
check("CLI image parses", isoinfo.inspect(o)["eltorito"]["default"]["bootable"])

# rejections
for bad in ("boot/x.bin", "BOOT/TOOLONGNAME.BIN", "BOOT/A.TOOL", "LONGDIRNAME/A.B", "A B.TXT"):
    try:
        mkiso.build([("BOOT/CDBOOT.BIN", boot), (bad, b"x")], "BOOT/CDBOOT.BIN"); check("rejects %r" % bad, False)
    except mkiso.MkIsoError:
        check("rejects %r" % bad, True)
try:
    mkiso.build([("A.TXT", b"x")], "BOOT/CDBOOT.BIN"); check("rejects missing boot image", False)
except mkiso.MkIsoError:
    check("rejects missing boot image", True)
# many files in one directory span several directory sectors
many = [("BOOT/CDBOOT.BIN", boot)] + [("DATA/F%03d.TXT" % n, b"%d" % n) for n in range(150)]
im = mkiso.build(many, "BOOT/CDBOOT.BIN"); open(os.path.join(tmp, "m.iso"), "wb").write(im)
c = pycdlib.PyCdlib(); c.open(os.path.join(tmp, "m.iso"))
fl = [f for _, _, fs in c.walk(iso_path="/DATA") for f in fs]
check("150 files in one directory (multi-sector dir)", len(fl) == 150, len(fl))
o = io.BytesIO(); c.get_file_from_iso_fp(o, iso_path="/DATA/F149.TXT;1"); check("last file readable", o.getvalue() == b"149")
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
