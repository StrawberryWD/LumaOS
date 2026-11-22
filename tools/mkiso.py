#!/usr/bin/env python3
"""tools/mkiso.py - LumaOS 5.0 (Day 58): build a bootable ISO9660 + El Torito image. Pure Python, no xorriso.

    python3 tools/mkiso.py -o lumaos.iso --boot build/cdboot.bin \
            --add BOOT/KERNEL.BIN=build/kernel.bin --add SYSTEM/LMFS.IMG=tests/lmfs-fixture.img

What it writes (every number is a 2048-byte sector, see doc/iso9660-eltorito.md):
    0..15   System Area (zeros)
    16      Primary Volume Descriptor
    17      Boot Record  "EL TORITO SPECIFICATION" -> boot catalog LBA
    18      Volume Descriptor Set Terminator
    19      Boot Catalog: validation entry + default entry (bootable, No-Emulation, load segment 0x07C0)
    20/21   Path tables (little / big endian)
    22..    directories (level 1: 8.3 upper-case names, ";1" version), then file data, 2048-aligned

BOOT INFO TABLE (isolinux's `-boot-info-table` idea, own layout): if the boot image carries the 4-byte
magic b"LCDB" at offset 8, mkiso patches the 36 bytes there after the layout is known:
    +0  "LCDB"          +4  PVD LBA (16)      +8  boot image LBA     +12 boot image length (bytes)
    +16 kernel LBA      +20 kernel bytes      +24 LMFS image LBA     +28 LMFS image bytes
    +32 total ISO sectors
The CD loader (boot/loader.asm built with -DCDBOOT) needs no directory parsing: it reads the kernel straight
from the LBA in this table. Builds are reproducible (fixed timestamp unless SOURCE_DATE_EPOCH is set).
"""
import argparse, os, re, struct, sys, time

SECTOR = 2048
NAME_RE = re.compile(r"^[A-Z0-9_]{1,8}(\.[A-Z0-9_]{0,3})?$")
DIR_RE = re.compile(r"^[A-Z0-9_]{1,8}$")


class MkIsoError(Exception):
    pass


def both32(v): return struct.pack("<I", v) + struct.pack(">I", v)
def both16(v): return struct.pack("<H", v) + struct.pack(">H", v)
def pad(b, n, fill=b" "): return (b + fill * n)[:n]
def sectors(n): return (n + SECTOR - 1) // SECTOR


def dec_datetime(t):
    g = time.gmtime(t)
    return ("%04d%02d%02d%02d%02d%02d00" % (g.tm_year, g.tm_mon, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec)).encode() + b"\x00"


def rec_datetime(t):
    g = time.gmtime(t)
    return bytes([g.tm_year - 1900, g.tm_mon, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, 0])


class Node:
    def __init__(self, name, parent, is_dir, data=b""):
        self.name, self.parent, self.is_dir, self.data = name, parent, is_dir, data
        self.children = {}
        self.lba = 0
        self.size = 0
        self.number = 0           # path-table directory number (dirs only)

    def iso_name(self):
        return self.name if self.is_dir else self.name + ";1"


def build_tree(files):
    root = Node("", None, True)
    for path, data in files:
        parts = path.strip("/").split("/")
        cur = root
        for d in parts[:-1]:
            if not DIR_RE.match(d):
                raise MkIsoError("bad directory name %r (ISO9660 level 1: 1-8 of A-Z 0-9 _)" % d)
            cur = cur.children.setdefault(d, Node(d, cur, True))
            if not cur.is_dir:
                raise MkIsoError("%s is both file and directory" % d)
        fn = parts[-1]
        if not NAME_RE.match(fn):
            raise MkIsoError("bad file name %r (ISO9660 level 1: NAME.EXT, upper case, 8.3)" % fn)
        if fn in cur.children:
            raise MkIsoError("duplicate file %s" % path)
        cur.children[fn] = Node(fn, cur, False, data)
    return root


def dir_record(lba, size, name_bytes, is_dir, when):
    ln = len(name_bytes)
    n = 33 + ln + (1 if ln % 2 == 0 else 0)
    return (bytes([n, 0]) + both32(lba) + both32(size) + rec_datetime(when) +
            bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([ln]) + name_bytes +
            (b"\x00" if ln % 2 == 0 else b""))


def dir_bytes(node, when):
    parent = node.parent or node
    recs = [dir_record(node.lba, node.size, b"\x00", True, when),
            dir_record(parent.lba, parent.size, b"\x01", True, when)]
    for k in sorted(node.children):
        c = node.children[k]
        recs.append(dir_record(c.lba, c.size, c.iso_name().encode(), c.is_dir, when))
    out = bytearray()
    for r in recs:                                   # a record never straddles a sector
        if (len(out) % SECTOR) + len(r) > SECTOR:
            out += b"\x00" * (SECTOR - len(out) % SECTOR)
        out += r
    return bytes(out)


def path_table(dirs, big, when):
    out = bytearray()
    for d in dirs:
        ident = b"\x00" if d.parent is None else d.name.encode()
        parent = 1 if d.parent is None else d.parent.number
        rec = bytes([len(ident), 0])
        rec += struct.pack(">I" if big else "<I", d.lba)
        rec += struct.pack(">H" if big else "<H", parent)
        rec += ident + (b"\x00" if len(ident) % 2 else b"")
        out += rec
    return bytes(out)


def build(files, boot_path, volume="LUMAOS", kernel_path=None, lmfs_path=None, when=None):
    if when is None:
        when = int(os.environ.get("SOURCE_DATE_EPOCH", "1767225600"))     # 2026-01-01 UTC
    fmap = dict(files)
    if boot_path not in fmap:
        raise MkIsoError("boot image %s is not among the files" % boot_path)
    root = build_tree(files)

    # directories in path-table order: breadth first, sorted
    dirs, level = [root], [root]
    while level:
        nxt = []
        for d in level:
            for k in sorted(d.children):
                if d.children[k].is_dir:
                    nxt.append(d.children[k])
        dirs += nxt
        level = nxt
    for i, d in enumerate(dirs, 1):
        d.number = i

    # ---- layout ----
    lba = 22
    # directory sizes need child records only (names), LBAs don't change the length
    for d in dirs:
        d.size = len(dir_bytes(d, when))
    for d in dirs:
        d.lba = lba
        lba += sectors(d.size)

    def walk(n):
        for k in sorted(n.children):
            c = n.children[k]
            if c.is_dir:
                yield from walk(c)
            else:
                yield c
    flist = list(walk(root))
    for f in flist:
        f.size = len(f.data)
        f.lba = lba
        lba += max(1, sectors(f.size))
    total = lba

    def find(path):
        n = root
        for p in path.split("/"):
            n = n.children[p]
        return n
    boot = find(boot_path)

    # ---- boot info table patch (before data is written) ----
    data = bytearray(boot.data)
    if len(data) >= 44 and data[8:12] == b"LCDB":
        k = find(kernel_path) if kernel_path else None
        l = find(lmfs_path) if lmfs_path else None
        tbl = struct.pack("<4s8I", b"LCDB", 16, boot.lba, boot.size,
                          k.lba if k else 0, k.size if k else 0,
                          l.lba if l else 0, l.size if l else 0, total)
        data[8:8 + len(tbl)] = tbl
        boot.data = bytes(data)

    img = bytearray(total * SECTOR)

    def put(lba_, b):
        img[lba_ * SECTOR:lba_ * SECTOR + len(b)] = b

    # PVD
    pvd = bytearray(SECTOR)
    pvd[0] = 1; pvd[1:6] = b"CD001"; pvd[6] = 1
    pvd[8:40] = pad(b"LUMAOS", 32); pvd[40:72] = pad(volume.upper().encode(), 32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1); pvd[124:128] = both16(1); pvd[128:132] = both16(SECTOR)
    ptl = path_table(dirs, False, when)
    ptm = path_table(dirs, True, when)
    pvd[132:140] = both32(len(ptl))
    pvd[140:144] = struct.pack("<I", 20); pvd[148:152] = struct.pack(">I", 21)
    pvd[156:190] = dir_record(root.lba, root.size, b"\x00", True, when)
    pvd[190:318] = pad(volume.upper().encode(), 128)
    pvd[318:446] = pad(b"", 128); pvd[446:574] = pad(b"mkiso.py (LumaOS 5.0)", 128)
    pvd[574:702] = pad(b"LUMAOS 5.0 INSTALL MEDIA", 128)
    pvd[702:813] = b" " * 111
    for off in (813, 830):
        pvd[off:off + 17] = dec_datetime(when)
    pvd[847:864] = b"0" * 16 + b"\x00"; pvd[864:881] = b"0" * 16 + b"\x00"
    pvd[881] = 1
    put(16, pvd)

    # Boot Record
    br = bytearray(SECTOR)
    br[0] = 0; br[1:6] = b"CD001"; br[6] = 1
    br[7:39] = pad(b"EL TORITO SPECIFICATION", 32, b"\x00")
    br[0x47:0x4B] = struct.pack("<I", 19)
    put(17, br)
    # Terminator
    t = bytearray(SECTOR); t[0] = 255; t[1:6] = b"CD001"; t[6] = 1
    put(18, t)

    # Boot Catalog
    cat = bytearray(SECTOR)
    cat[0] = 1; cat[1] = 0
    cat[4:28] = pad(b"LUMAOS", 24, b"\x00")
    cat[30] = 0x55; cat[31] = 0xAA
    s = sum(struct.unpack("<16H", bytes(cat[:32]))) & 0xFFFF
    cat[28:30] = struct.pack("<H", (-s) & 0xFFFF)
    cat[32] = 0x88; cat[33] = 0                      # bootable, No-Emulation
    cat[34:36] = struct.pack("<H", 0)                # load segment 0 -> 0x07C0
    cat[36] = 0
    cat[38:40] = struct.pack("<H", (boot.size + 511) // 512)
    cat[40:44] = struct.pack("<I", boot.lba)
    put(19, cat)

    put(20, ptl); put(21, ptm)
    for d in dirs:
        put(d.lba, dir_bytes(d, when))
    for f in flist:
        put(f.lba, f.data)
    return bytes(img)


def main(argv=None):
    ap = argparse.ArgumentParser(description="LumaOS ISO9660 + El Torito image builder")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--volume", default="LUMAOS")
    ap.add_argument("--boot", required=True, help="No-Emulation boot image (host file)")
    ap.add_argument("--boot-path", default="BOOT/CDBOOT.BIN", help="where the boot image lives inside the ISO")
    ap.add_argument("--add", action="append", default=[], metavar="ISOPATH=FILE")
    ap.add_argument("--kernel-path", default=None, help="ISO path recorded as the kernel in the boot info table")
    ap.add_argument("--lmfs-path", default=None, help="ISO path recorded as the LMFS image in the boot info table")
    a = ap.parse_args(argv)
    files = []
    try:
        files.append((a.boot_path, open(a.boot, "rb").read()))
        for spec in a.add:
            if "=" not in spec:
                raise MkIsoError("--add wants ISOPATH=FILE, got %r" % spec)
            p, f = spec.split("=", 1)
            files.append((p.upper(), open(f, "rb").read()))
        iso = build(files, a.boot_path, a.volume, a.kernel_path, a.lmfs_path)
    except (MkIsoError, OSError) as e:
        print("mkiso: %s" % e, file=sys.stderr)
        return 1
    with open(a.output, "wb") as fh:
        fh.write(iso)
    print("[iso]  %s (%d bytes = %d sectors, %d files)" % (a.output, len(iso), len(iso) // SECTOR, len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
