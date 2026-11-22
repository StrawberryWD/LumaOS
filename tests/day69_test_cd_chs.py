"""tests/day69_test_cd_chs.py - Day 69: cd relative paths + boot on BIOSes without INT 13h extensions.
    python3 tests/day69_test_cd_chs.py   (needs ./build.sh, LUMAOS_TEXT=1 ./build.sh and
                                          LUMAOS_FORCE_CHS=1 LUMAOS_TEXT=1 BUILD=build-chs IMG=lumaos-5.0-chs.img ./build.sh)
Part 1 (live CD): cd with relative paths, ./ and ../, trailing slash, `cd` alone, `cd -`, case-insensitive volume names,
        failed cd leaves the directory unchanged, file/dir mixups, `pwd`, relative create/read/rm from sub-directories.
Part 2: the hard disk booted by the normal loader (LBA/INT13 extensions) reports "Hard disk (BIOS)";
        the FORCE_CHS loader (never probes extensions, reads sector by sector via CHS) boots the whole 250 KB kernel,
        reports "CHS fallback", and the file systems work (fsck, fstress).
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
def wait_for_any(screen, marks, timeout=240):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = screen()
        for k in marks:
            if k in s: return s
        time.sleep(1)
    return screen()
def run(m, c, wait=1.0):
    m.line("clear", 0.4); m.line(c, wait)
    return m.text_screen()
def last_prompt(m):
    lines = [l for l in m.text_screen().splitlines() if l.strip()]
    return lines[-1].strip() if lines else ""

m = Machine(None, "d69a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    run(m, "mkdir a"); run(m, "mkdir a/b"); run(m, "create a/b/f.txt")
    s = run(m, "pwd"); check("pwd at the volume root", re.search(r"^RAM0/\s*$", s, re.M) is not None, s)
    s = run(m, "cd a/b"); check("cd a/b (two levels)", "Changed directory to RAM0/a/b/" in s, s)
    s = run(m, "pwd"); check("pwd in a/b", re.search(r"^RAM0/a/b/\s*$", s, re.M) is not None, s)
    s = run(m, "cd ../.."); check("cd ../..", "Changed directory to RAM0/" in s and last_prompt(m) == "RAM0/ >", s)
    s = run(m, "cd ./a/./b"); check("cd ./a/./b", last_prompt(m) == "RAM0/a/b/ >", s)
    s = run(m, "cd .."); check("cd ..", last_prompt(m) == "RAM0/a/ >", s)
    s = run(m, "cd b/../b/"); check("cd b/../b/ (trailing slash)", last_prompt(m) == "RAM0/a/b/ >", s)
    s = run(m, "cd"); check("`cd` alone -> volume root", last_prompt(m) == "RAM0/ >", s)
    s = run(m, "cd -"); check("`cd -` -> previous directory", last_prompt(m) == "RAM0/a/b/ >" and "RAM0/a/b" in s, s)
    s = run(m, "cd -"); check("`cd -` again toggles back", last_prompt(m) == "RAM0/ >", s)
    s = run(m, "cd nosuch"); check("failed cd: no such directory", "no such directory" in s and last_prompt(m) == "RAM0/ >", s)
    s = run(m, "cd welcome.txt"); check("cd into a file refused", "not a directory" in s and last_prompt(m) == "RAM0/ >", s)
    run(m, "cd a")
    s = run(m, "ls b"); check("ls b (relative) from a/", "f.txt" in s, s)
    s = run(m, "read b/f.txt"); check("read b/f.txt (relative file)", "f.txt" in s and "0 bytes" in s, s)
    run(m, "create b/g.txt"); s = run(m, "ls b")
    check("create b/g.txt relative", "g.txt" in s, s)
    run(m, "rm b/g.txt"); s = run(m, "ls b")
    check("rm b/g.txt relative", "g.txt" not in s and "f.txt" in s, s)
    s = run(m, "cd ../a/b/../.."); check("cd ../a/b/../.. from a/", last_prompt(m) == "RAM0/ >", s)
    s = run(m, "cd ram0/a"); check("volume names are case-insensitive (ram0/a)", last_prompt(m) == "RAM0/a/ >", s)
    s = run(m, "cd cd0/boot"); check("cd cd0/boot (iso9660 sub-directory, lower case)", last_prompt(m).upper().startswith("CD0/BOOT"), s)
    s = run(m, "cd -"); check("`cd -` across volumes", last_prompt(m) == "RAM0/a/ >", s)
    s = run(m, "cd /"); check("cd / = current volume root", last_prompt(m) == "RAM0/ >", s)
    s = run(m, "cd /a/b"); check("cd /a/b (absolute inside volume)", last_prompt(m) == "RAM0/a/b/ >", s)
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-text.img", "d69b"), "d69b", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    s = run(m, "cdinfo", 1.5)
    check("normal HD boot: Hard disk (BIOS), no CHS", "Hard disk (BIOS)" in s and "CHS" not in s, s[-600:])
    s = run(m, "cd ide0"); check("HD: cd ide0 (lower case)", last_prompt(m) == "IDE0/ >", s)
    s = run(m, "cd IDE0/x"); check("FAT volume has no sub-directories", "no such directory" in s, s)
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-chs.img", "d69c"), "d69c", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=240)
    s = run(m, "cdinfo", 1.5)
    check("FORCE_CHS boot: reports CHS fallback", "CHS fallback" in s, s[-600:])
    s = run(m, "fsck", 3.0)
    wait_for_any(m.text_screen, ["is clean", "problem(s)"], 60)
    s = m.text_screen()
    check("CHS boot: fsck works", "is clean" in s or "problem(s)" in s, s[-400:])
    run(m, "cd IDE0")
    m.line("clear", 0.4); m.line("fstress 8", 0.5)
    s = wait_for_any(m.text_screen, ["fstress: PASS", "fstress: FAIL"])
    check("CHS boot: fstress on FAT", "fstress: PASS" in s, s[-500:])
finally:
    m.close()
for vga in ("std", "vmware"):
    g = Machine(None, "d69g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("mkdir q", 1.0); g.line("cd q", 1.0); g.line("pwd", 1.5)
        s = g.gui_screen()
        check("[%s] GUI cd q + pwd" % vga, "RAM0/q/" in s, s[-300:])
        g.line("cd ..", 1.0); g.line("cd -", 1.0); g.line("pwd", 1.5)
        check("[%s] GUI cd -" % vga, s.count("RAM0/q/") >= 1 and g.gui_screen().count("RAM0/q/") >= 2, g.gui_screen()[-300:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
