"""tests/day72_test_printf.py - Day 72: printf family in libluma (printf/fprintf/sprintf/snprintf + v versions).
    python3 tests/day72_test_printf.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * tools/build_user.sh builds libluma.a + printf.sw warning-free
  * printf.sw does ~100 self-checks against the C-standard expected strings (flags, width, precision, '*', length
    modifiers incl. 64-bit without libgcc, %s NULL, %p, invalid conversions, snprintf truncation / measuring)
  * `run printf.sw` on live CD (RAM0), hard disk (IDE3), GUI std + vmware: exact printed lines, output > 128 bytes
    crosses the internal buffer, exit code 0
  * Day 72+ GUI timing: the terminal cursor blinks about every 530 ms (not per redraw), idle redraw rate is low
"""
import shutil, subprocess, sys, os, re, time
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

p = subprocess.run(["tools/build_user.sh", "user-out"], capture_output=True, text=True)
check("tools/build_user.sh succeeds with no compiler warnings", p.returncode == 0 and "warning" not in p.stderr, p.stderr[-800:])
check("printf.sw exists", os.path.exists("user-out/printf.sw"))

EXPECT = ["printf: int=42 neg=-7 hex=0xbeef str=luma chr=Z pct=100%",
          "printf returned 57",
          "[   right][left    ][     007][  +99]",
          "u64: 12345678901234  x64: 123456789abcdef0",
          "long: 0,1111,2222,3333,4444,5555,6666,7777,8888,9999,11110,12221,13332,14443,15554,16665,17776,18887,19998,21109 end",
          "stderr: red text 2", "args(2): <a> <b>", "errno 13 = Permission denied",
          "printf.sw: all checks OK"]
def verify(tag, s):
    s = re.sub(r"\s+", "", s)        # the screen reader folds runs of blanks and wraps long lines: compare without whitespace
    pos = -1
    for line in EXPECT:
        want = re.sub(r"\s+", "", line)
        i = s.find(want, pos + 1)
        check("%s: output contains %r" % (tag, line[:50]), i >= 0, s[-1200:])
        if i >= 0: pos = i
    check("%s: no FAIL line, exit code 0" % tag, "FAIL" not in s and "exited with code" not in s, s[-1200:])

def run_prog(m, cmd, screen):
    m.line("clear", 0.5); m.line(cmd, 4.5)
    return screen()

m = Machine(None, "d72a", gui=False, cdrom="lumaos-5-text.iso" if False else "lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    verify("CD/RAM0", run_prog(m, "run printf.sw a b", m.text_screen))
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-text.img", "d72b"), "d72b", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    verify("HD/IDE3", run_prog(m, "run IDE3/printf.sw a b", m.text_screen))
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(None, "d72g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("run printf.sw a b", 5.0)
        s = g.gui_screen()
        check("[%s] GUI: printf.sw all checks OK" % vga, "printf.sw: all checks OK" in s and "args(2): <a> <b>" in s, s[-600:])
    finally:
        g.close()

# ---- cursor blink / frame pacing (GUI) ----
g = Machine(fresh("lumaos-5.0.img", "d72t"), "d72t", gui=True, elf="build/kernel.elf")
try:
    g.wait_for("Type 'help'", timeout=120); time.sleep(3)
    import struct
    from day43_helpers import nm_symbol
    a = nm_symbol("build/kernel.elf", "gfx_stats")
    f0 = struct.unpack("<I", g._read_bytes(a, 4))[0]; t0 = time.time()
    hs = []
    while time.time() - t0 < 6:
        hs.append((time.time() - t0, g.hash_screen()))
    f1 = struct.unpack("<I", g._read_bytes(a, 4))[0]; dt = time.time() - t0
    ch = [hs[i][0] for i in range(1, len(hs)) if hs[i][1] != hs[i - 1][1]]
    per = (ch[-1] - ch[0]) / (len(ch) - 1) if len(ch) > 2 else 0
    check("cursor toggles every ~530 ms (measured %.3f s)" % per, 0.4 < per < 0.7, ch)
    check("idle redraw rate is low (%.1f frames/s)" % ((f1 - f0) / dt), (f1 - f0) / dt < 30)
finally:
    g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
