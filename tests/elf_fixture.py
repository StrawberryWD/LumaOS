"""tests/elf_fixture.py - read the facts about a fixture .sw (hello.sw, data.sw, args.sw) FROM THE FILE.

Tests used to hard-code numbers that belong to a fixture (entry 0x08048080, .data at 0x0804b060, ...). Every
time a fixture is rebuilt (Day 46: hello.sw printed text, Day 49: hello.sw takes argv) those numbers changed
and unrelated tests broke. Tests now ask the file instead:

    from elf_fixture import elf_facts
    f = elf_facts("tests/hello.sw")
    f.entry                    # e_entry
    f.load                     # list of PT_LOAD dicts: offset vaddr filesz memsz flags
    f.data                     # the first writable PT_LOAD with file content (.data) or None
    f.bss                      # (start, length) of the zero-filled tail of the first segment with memsz > filesz, or None
    f.total_filesz / f.total_memsz / f.lo / f.hi
"""
import struct


class ElfFacts(object):
    pass


def elf_facts(path):
    b = open(path, "rb").read()
    f = ElfFacts()
    f.size = len(b)
    f.entry = struct.unpack_from("<I", b, 24)[0]
    phoff = struct.unpack_from("<I", b, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", b, 42)
    f.load = []
    for i in range(phnum):
        t, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<IIIIIIII", b, phoff + phentsize * i)
        if t == 1 and msz:
            f.load.append(dict(offset=off, vaddr=va, filesz=fsz, memsz=msz, flags=fl, align=al))
    f.lo = min(s["vaddr"] for s in f.load)
    f.hi = max(s["vaddr"] + s["memsz"] for s in f.load)
    f.total_filesz = sum(s["filesz"] for s in f.load)
    f.total_memsz = sum(s["memsz"] for s in f.load)
    f.data = next((s for s in f.load if (s["flags"] & 2) and s["filesz"]), None)
    bs = next((s for s in f.load if s["memsz"] > s["filesz"]), None)
    f.bss = (bs["vaddr"] + bs["filesz"], bs["memsz"] - bs["filesz"]) if bs else None
    f.bytes = b
    return f
