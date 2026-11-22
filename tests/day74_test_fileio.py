"""tests/day74_test_fileio.py - Day 74: user programs read files (SYS_OPEN/READ/CLOSE/LSEEK + libluma FILE*).
    python3 tests/day74_test_fileio.py   (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
  * tools/build_user.sh builds libluma.a (stdio.c) + fileio.sw warning-free; syscall numbers 7..10 match the kernel
  * `run fileio.sw <path>` (~100 self-checks: fopen failures, fgets/fgetc/fread in many chunk sizes, fseek/ftell/ungetc,
    two streams, fd reuse, bad pointers, EBADF paths, EMFILE at 16, fopen/fclose heap leak) on every backend:
        RAM0 (LMFS from the live CD), CD0 (ISO9660), IDE0 (FAT12), IDE3 (LMFS on the hard disk), GUI std + vmware
  * relative path after `cd`; a program that exits with 3 files still open does not leak fds into the next program
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
def squash(s): return re.sub(r"\s+", "", s)

p = subprocess.run(["tools/build_user.sh", "user-out"], capture_output=True, text=True)
check("tools/build_user.sh succeeds with no compiler warnings", p.returncode == 0 and "warning" not in p.stderr, p.stderr[-800:])
ul = open("user/include/luma.h").read(); ks = open("kernel/syscall/syscall.h").read()
for name, num in (("SYS_OPEN", "7"), ("SYS_READ", "8"), ("SYS_CLOSE", "9"), ("SYS_LSEEK", "10")):
    u = re.search(r"#define\s+%s\s+(\d+)" % name, ul); k = re.search(r"#define\s+%s\s+(\d+)" % name, ks)
    check("%s = %s in the library and the kernel" % (name, num), u and k and u.group(1) == k.group(1) == num)
syms = subprocess.run("nm user-out/libluma.a", shell=True, capture_output=True, text=True).stdout
check("libluma.a defines fopen fclose fread fgets fseek ftell open read close lseek",
      all(re.search(r" T %s$" % f, syms, re.M) for f in "fopen fclose fread fgets fseek ftell open read close lseek".split()), syms[-300:])
check("fixture is 120 lines x 51 bytes", os.path.getsize("doc/fixture.txt") == 6120)

def run_prog(m, cmd, wait=9.0):
    m.line("clear", 0.5); m.line(cmd, wait)
    return m.text_screen()
def verify(tag, s, empty="ok"):
    q = squash(s)
    check("%s: fileio.sw: all checks OK, no FAIL line" % tag, "fileio.sw:allchecksOK" in q and "FAIL" not in q, s[-900:])
    check("%s: 120 lines / 6120 bytes, 16 files before EMFILE" % tag, "lines:120bytes:6120" in q and "openfilesbeforeEMFILE:16" in q, s[-900:])
    check("%s: stdout/stderr streams: fprintf, fputs, fwrite, perror" % tag,
          "stdoutviafprintf:74" in q and "fputstostdout" in q and "fwrite4x3" in q and "perrordemo:Nosuchfileordirectory" in q, s[-900:])
    check("%s: empty file %s" % (tag, empty), ("emptyfile:" + empty) in q.replace("(noneinthisvolume)", "(none)") or ("emptyfile:" + empty.replace(" ", "")) in q, s[-900:])
    check("%s: exit code 0" % tag, "exitedwithcode" not in q, s[-900:])

m = Machine(None, "d74a", gui=False, cdrom="lumaos-5.0-text.iso", hd=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    verify("RAM0 (LMFS, from CD)", run_prog(m, "run fileio.sw RAM0/fixture.txt"))
    verify("CD0 (ISO9660)", run_prog(m, "run fileio.sw CD0/ETC/FIXTURE.TXT"), empty="(none)")
    verify("relative path (bare name)", run_prog(m, "run fileio.sw fixture.txt"))
    s = run_prog(m, "run fileio.sw RAM0/fixture.txt leak"); q = squash(s)
    check("leaking run: 3 files left open on purpose, still all checks OK", "leaking3openfilesonpurpose" in q and "fileio.sw:allchecksOK" in q, s[-600:])
    verify("run after the leaking run (fd table was reset)", run_prog(m, "run fileio.sw RAM0/fixture.txt"))
    s = run_prog(m, "pginfo", 1.5)
    check("no frame pool damage (pginfo prints)", "framepool:" in squash(s))
finally:
    m.close()

m = Machine(fresh("lumaos-5.0-text.img", "d74b"), "d74b", gui=False)
try:
    m.wait_for("Text-mode shell ready", timeout=90)
    verify("IDE3 (LMFS on the hard disk)", run_prog(m, "run fileio.sw IDE3/fixture.txt"))
    verify("IDE0 (FAT12)", run_prog(m, "run fileio.sw IDE0/FIXTURE.TXT"), empty="(none)")
finally:
    m.close()

for vga in ("std", "vmware"):
    g = Machine(None, "d74g" + vga, gui=True, elf="build/kernel.elf", vga=vga, cdrom="lumaos-5.0.iso", hd=False)
    try:
        g.wait_for("Type 'help'", timeout=120); time.sleep(2)
        g.line("run fileio.sw RAM0/fixture.txt", 12.0)
        s = g.gui_screen()
        check("[%s] GUI: fileio.sw all checks OK" % vga, "fileio.sw: all checks OK" in s, s[-600:])
    finally:
        g.close()
print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
