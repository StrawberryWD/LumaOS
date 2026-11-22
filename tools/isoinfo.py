#!/usr/bin/env python3
"""tools/isoinfo.py - LumaOS 5.0 (Day 57): a small pure-Python ISO9660 / El Torito inspector.

    python3 tools/isoinfo.py image.iso          # human readable dump
    python3 tools/isoinfo.py --json image.iso   # machine readable (used by the tests)

It follows exactly the walk the Day 59-63 boot code will have to do:
  1. sectors are 2048 bytes; the volume-descriptor set starts at LBA 16
  2. every descriptor: type(1) + "CD001"(5) + version(1) + 2041 data bytes;
     type 0 = boot record, 1 = primary, 2 = supplementary (Joliet), 255 = terminator
  3. the boot record whose boot-system-id is "EL TORITO SPECIFICATION" holds, at
     offset 0x47, the LBA of the boot catalog
  4. boot catalog: validation entry (header ID 1, key 55 AA, 16-bit word sum == 0),
     then the initial/default entry (0x88 bootable / 0x00 not, media type, load
     segment, system type, sector count in 512-byte units, load RBA in 2048-byte units)
  5. optionally a section header (0x90/0x91) + more section entries.
"""
import struct, sys, json

SECTOR = 2048
MEDIA = {0: "no emulation", 1: "1.2 MB floppy", 2: "1.44 MB floppy", 3: "2.88 MB floppy", 4: "hard disk"}


class IsoError(Exception):
    pass


def _sector(data, lba):
    off = lba * SECTOR
    if off + SECTOR > len(data):
        raise IsoError("LBA %d is beyond the end of the image" % lba)
    return data[off:off + SECTOR]


def _both32(b):            # ISO9660 "both-endian" 32-bit: LE then BE
    le, be = struct.unpack("<I", b[:4])[0], struct.unpack(">I", b[4:8])[0]
    if le != be:
        raise IsoError("both-endian field mismatch (%#x vs %#x)" % (le, be))
    return le


def _str(b):
    return b.decode("latin-1").rstrip(" \x00")


def parse_descriptors(data):
    out = []
    lba = 16
    while True:
        s = _sector(data, lba)
        typ, ident, ver = s[0], s[1:6], s[6]
        if ident != b"CD001":
            raise IsoError("LBA %d: no 'CD001' signature" % lba)
        d = {"lba": lba, "type": typ, "version": ver}
        if typ == 0:
            d["kind"] = "boot record"
            d["boot_system_id"] = _str(s[7:39])
            d["boot_id"] = _str(s[39:71])
            d["boot_catalog_lba"] = struct.unpack("<I", s[0x47:0x4B])[0]
        elif typ in (1, 2):
            d["kind"] = "primary" if typ == 1 else "supplementary"
            d["system_id"] = _str(s[8:40])
            d["volume_id"] = _str(s[40:72])
            d["volume_space_size"] = _both32(s[80:88])
            d["logical_block_size"] = struct.unpack("<H", s[128:130])[0]
            d["path_table_size"] = _both32(s[132:140])
            root = s[156:190]
            d["root_dir_lba"] = _both32(root[2:10])
            d["root_dir_size"] = _both32(root[10:18])
        elif typ == 255:
            d["kind"] = "terminator"
        else:
            d["kind"] = "unknown(%d)" % typ
        out.append(d)
        if typ == 255:
            return out
        lba += 1
        if lba > 16 + 64:
            raise IsoError("no volume descriptor set terminator")


def parse_entry(e):
    return {
        "bootable": e[0] == 0x88,
        "media_type": e[1] & 0x0F,
        "media": MEDIA.get(e[1] & 0x0F, "reserved"),
        "load_segment": struct.unpack("<H", e[2:4])[0] or 0x07C0,
        "system_type": e[4],
        "sector_count_512": struct.unpack("<H", e[6:8])[0],
        "load_rba": struct.unpack("<I", e[8:12])[0],
    }


def parse_catalog(data, lba):
    s = _sector(data, lba)
    v = s[:32]
    if v[0] != 1 or v[30:32] != b"\x55\xaa":
        raise IsoError("boot catalog: bad validation entry header/key")
    words = struct.unpack("<16H", v)
    if sum(words) & 0xFFFF:
        raise IsoError("boot catalog: validation checksum != 0")
    cat = {"platform_id": v[1], "platform": {0: "x86", 1: "PowerPC", 2: "Mac", 0xEF: "EFI"}.get(v[1], "?"),
           "id_string": _str(v[4:28]), "default": parse_entry(s[32:64]), "sections": []}
    pos = 64
    while pos + 32 <= SECTOR and s[pos] in (0x90, 0x91):
        hdr = s[pos]
        n = struct.unpack("<H", s[pos + 2:pos + 4])[0]
        sec = {"platform_id": s[pos + 1], "entries": [], "last": hdr == 0x91}
        pos += 32
        for _ in range(n):
            sec["entries"].append(parse_entry(s[pos:pos + 32]))
            pos += 32
        cat["sections"].append(sec)
        if sec["last"]:
            break
    return cat


def inspect(path):
    data = open(path, "rb").read()
    info = {"file": path, "size": len(data), "sectors": len(data) // SECTOR,
            "descriptors": parse_descriptors(data), "eltorito": None}
    for d in info["descriptors"]:
        if d["type"] == 0 and d["boot_system_id"] == "EL TORITO SPECIFICATION":
            c = parse_catalog(data, d["boot_catalog_lba"])
            c["catalog_lba"] = d["boot_catalog_lba"]
            info["eltorito"] = c
    return info


def main(argv):
    js = "--json" in argv
    args = [a for a in argv if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__); return 2
    try:
        i = inspect(args[0])
    except IsoError as e:
        print("isoinfo: %s" % e, file=sys.stderr); return 1
    if js:
        print(json.dumps(i, indent=1)); return 0
    print("%s: %d bytes = %d sectors of %d" % (i["file"], i["size"], i["sectors"], SECTOR))
    for d in i["descriptors"]:
        extra = ""
        if d["type"] in (1, 2):
            extra = " volume '%s' size=%d blocks root dir @LBA %d" % (d["volume_id"], d["volume_space_size"], d["root_dir_lba"])
        if d["type"] == 0:
            extra = " '%s' -> boot catalog @LBA %d" % (d["boot_system_id"], d["boot_catalog_lba"])
        print("  LBA %-3d type %-3d %s%s" % (d["lba"], d["type"], d["kind"], extra))
    c = i["eltorito"]
    if c:
        e = c["default"]
        print("El Torito catalog @LBA %d: platform %s, id '%s'" % (c["catalog_lba"], c["platform"], c["id_string"]))
        print("  default entry: %s, media=%s, load segment %#06x, %d x 512B sectors, image @LBA %d"
              % ("bootable" if e["bootable"] else "NOT bootable", e["media"], e["load_segment"],
                 e["sector_count_512"], e["load_rba"]))
        for s in c["sections"]:
            for e in s["entries"]:
                print("  extra entry: %s media=%s rba=%d" % ("bootable" if e["bootable"] else "not bootable", e["media"], e["load_rba"]))
    else:
        print("no El Torito boot record")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
