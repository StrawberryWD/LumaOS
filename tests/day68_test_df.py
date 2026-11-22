"""tests/day68_test_df.py - Day 68: `df`.
    python3 tests/day68_test_df.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
Live CD: df lists RAM0 (lmfs) + CD0 (iso9660, ro); Size = Used + Free on every row; creating a file lowers Free;
`df .` / `df CD0` select a volume. HD boot: IDE0/IDE1/IDE2 FAT + IDE3 lmfs rows. GUI std + vmware.
"""
import shutil, sys, os, time, re
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)
def fresh(src, tag):
    dst = "build-text/test-%s.img" % tag
    shutil.copyfile(src, dst); return dst

ROW = re.compile(r"^(\S+)\s+(lmfs|fat12|fat16|fat32|iso9660)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)%", re.M)
def rows(s):
    return {r[0]: (r[1], int(r[2]), int(r[3]), int(r[4]), int(r[5])) for r in ROW.findall(s)}

def df(m, screen, arg=""):
    m.line("clear", 0.5); m.line(("df " + arg).strip(), 2.5)
    return screen()

def consistent(tag, d):
    for k, (ty, tot, used, free, pct) in d.items():
        check("%s %s: size == used + free" % (tag, k), tot == used + free, (k, d[k]))
        check("%s %s: percent in range" % (tag, k), 0 <= pct <= 100, d[k])

m = Machine(None, "d68a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    s = df(m, m.text_screen)
    check("header shown", "Volume" in s and "Free(KB)" in s, s[-500:])
    d = rows(s); print("   ", d)
    check("RAM0 row (lmfs)", "RAM0" in d and d["RAM0"][0] == "lmfs", s[-500:])
    check("CD0 row is read-only", re.search(r"CD0\s+iso9660.*ro", s) is not None, s[-500:])
    consistent("CD", d)
    free0 = d["RAM0"][3]
    m.line("create big.txt", 1.0)
    m.line("create filler.txt", 1.0)
    d2 = rows(df(m, m.text_screen, "RAM0"))
    check("`df RAM0` shows only that volume", list(d2) == ["RAM0"], d2)
    check("free space did not grow after creating files", d2["RAM0"][3] <= free0, (free0, d2))
    d3 = rows(df(m, m.text_screen, "."))
    check("`df .` = current volume", list(d3) == ["RAM0"], d3)
    s = df(m, m.text_screen, "NOPE")
    check("df of unknown volume gives an error text", "No such" in s or "no such" in s.lower() or "Invalid" in s or "bad" in s, s[-300:])
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-text.img", "d68b"), "d68b", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    s = df(m, m.text_screen)
    d = rows(s); print("   ", d)
    for v, ty in (("IDE0", "fat"), ("IDE1", "fat"), ("IDE2", "fat"), ("IDE3", "lmfs")):
        check("%s row present (%s)" % (v, ty), v in d and d[v][0].startswith(ty), s[-800:])
    consistent("HD", d)
    f0 = d["IDE2"][3]
    m.line("cd IDE2", 0.8); m.line("fstress 8", 0.5)
    t0 = time.time()
    while time.time() - t0 < 120 and "fstress: PASS" not in m.text_screen(): time.sleep(1)
    d2 = rows(df(m, m.text_screen, "IDE2"))
    check("after fstress cleanup FAT32 free space equals the start", d2["IDE2"][3] == f0, (f0, d2))
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(None, "d68g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("df", 2.0)
        s = g.gui_screen()
        check("[%s] GUI df shows RAM0 + CD0" % vga, "RAM0" in s and "CD0" in s and "Free" in s, s[-400:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
