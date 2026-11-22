"""tests/day57_test_isoinfo.py - Day 57: tools/isoinfo.py against ISOs built by pycdlib (an independent
implementation) and against hand-corrupted copies.

    python3 tests/day57_test_isoinfo.py
"""
import sys, os, io, subprocess, tempfile, json, struct
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import isoinfo
try:
    import pycdlib
except ImportError:
    print("SKIP: pycdlib missing (pip install pycdlib)"); sys.exit(0)

fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)

tmp = tempfile.mkdtemp()
boot = bytes((i * 7 + 3) & 255 for i in range(2048 * 2))      # 2 sectors "boot image"

def make(path, emul="noemul", count=4, payload=None):
    payload = payload or boot
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1, vol_ident="LUMAOS")
    iso.add_fp(io.BytesIO(payload), len(payload), "/BOOT.BIN;1")
    iso.add_eltorito("/BOOT.BIN;1", media_name=emul, boot_load_size=count)
    iso.write(path); iso.close()

p = os.path.join(tmp, "a.iso"); make(p)
i = isoinfo.inspect(p)
d = i["descriptors"]
check("descriptors start at LBA 16", d[0]["lba"] == 16)
check("set = primary, boot record, terminator", [x["type"] for x in d] == [1, 0, 255], d)
check("volume id LUMAOS", d[0]["volume_id"] == "LUMAOS")
check("block size 2048", d[0]["logical_block_size"] == 2048)
check("volume space size matches file", d[0]["volume_space_size"] * 2048 == i["size"])
c = i["eltorito"]
check("El Torito boot record found", c is not None)
check("platform x86", c["platform"] == "x86")
e = c["default"]
check("default entry bootable", e["bootable"])
check("no-emulation media (0)", e["media_type"] == 0)
check("sector count = 4", e["sector_count_512"] == 4)
check("load segment defaults to 0x07C0", e["load_segment"] == 0x07C0)
img = open(p, "rb").read()
check("load RBA points at the boot image bytes", img[e["load_rba"] * 2048:e["load_rba"] * 2048 + len(boot)] == boot)
q = os.path.join(tmp, "b.iso")
try:
    make(q, emul="floppy", count=2880, payload=bytes(1474560))
    check("floppy emulation = media 2", isoinfo.inspect(q)["eltorito"]["default"]["media_type"] == 2)
except Exception as ex:
    check("floppy emulation sample built", False, ex)
out = subprocess.run([sys.executable, os.path.join(ROOT, "tools/isoinfo.py"), p], capture_output=True, text=True)
check("CLI exit 0 + text mentions EL TORITO", out.returncode == 0 and "EL TORITO SPECIFICATION" in out.stdout, out.stdout)
j = subprocess.run([sys.executable, os.path.join(ROOT, "tools/isoinfo.py"), "--json", p], capture_output=True, text=True)
check("CLI --json parses", json.loads(j.stdout)["eltorito"]["default"]["bootable"] is True)

# corruption
bad = bytearray(img); bad[16 * 2048 + 1] = ord("X")
open(os.path.join(tmp, "c.iso"), "wb").write(bad)
try: isoinfo.inspect(os.path.join(tmp, "c.iso")); check("bad CD001 rejected", False)
except isoinfo.IsoError: check("bad CD001 rejected", True)
cat = c["catalog_lba"]
bad = bytearray(img); bad[cat * 2048 + 30] = 0
open(os.path.join(tmp, "d.iso"), "wb").write(bad)
try: isoinfo.inspect(os.path.join(tmp, "d.iso")); check("bad validation key rejected", False)
except isoinfo.IsoError: check("bad validation key rejected", True)
bad = bytearray(img); bad[cat * 2048 + 28] ^= 1
open(os.path.join(tmp, "e.iso"), "wb").write(bad)
try: isoinfo.inspect(os.path.join(tmp, "e.iso")); check("bad checksum rejected", False)
except isoinfo.IsoError: check("bad checksum rejected", True)
open(os.path.join(tmp, "f.iso"), "wb").write(img[:20 * 2048])
try: isoinfo.inspect(os.path.join(tmp, "f.iso")); check("truncated image rejected", False)
except isoinfo.IsoError: check("truncated image rejected", True)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
