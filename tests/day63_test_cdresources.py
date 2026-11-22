"""tests/day63_test_cdresources.py - Day 63: resources (config, font) are read from the ISO 9660 volume.

    python3 tests/day63_test_cdresources.py     (needs ./build.sh and LUMAOS_TEXT=1 ./build.sh)
Default ISO (text build, CD only): CD0 is mounted (ISO 9660 driver), `ls CD0`, case-insensitive paths, subdirectory
`cd`, `read CD0/ETC/LUMAOS.CFG` returns the file, missing files report an error, `font` shows the CD font with the
checksum tools/mkfont.py computes, `config` shows the parsed keys.
Custom ISOs built with tools/mkiso.py: other values (theme/resolution/icons/hostname), an unknown key, a bad value,
a corrupt font file (-> built-in font stays) and a modified font. GUI build: the config is APPLIED (screen size,
theme) and the modified font is really drawn (screenshot differs from the default font's).
"""
import sys, os, re, time, subprocess, hashlib
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
T = "/tmp/d63test"; os.makedirs(T, exist_ok=True)
fails = []
def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n)
    if not c: fails.append(n); print(x)

def mkfont(path, *glyphs):
    args = [sys.executable, "tools/mkfont.py", "-o", path]
    for g in glyphs: args += ["--glyph", g]
    subprocess.check_call(args, stdout=subprocess.DEVNULL)
    return int(subprocess.check_output([sys.executable, "tools/mkfont.py", "--check", path]).strip(), 16)

def mkiso(name, bdir, cfg=None, font=None, bad_font=None):
    cfgf = T + "/" + name + ".cfg"
    open(cfgf, "w").write(cfg if cfg is not None else open("resources/LUMAOS.CFG").read())
    fnt = font or (bad_font and T + "/" + name + ".bad") or bdir + "/sys8x8.fnt"
    if bad_font: open(fnt, "wb").write(bad_font)
    out = T + "/" + name + ".iso"
    subprocess.check_call([sys.executable, "tools/mkiso.py", "-o", out, "--volume", "LUMAOS", "--boot", bdir + "/cdboot.bin",
        "--add", "BOOT/KERNEL.BIN=" + bdir + "/kernel.bin", "--add", "BOOT/LMFS.IMG=" + bdir + "/sys-lmfs.img",
        "--add", "FONTS/SYS8X8.FNT=" + fnt, "--add", "ETC/LUMAOS.CFG=" + cfgf,
        "--kernel-path", "BOOT/KERNEL.BIN", "--lmfs-path", "BOOT/LMFS.IMG"], stdout=subprocess.DEVNULL)
    return out

def text_boot(iso, tag):
    m = Machine(None, tag, gui=False, cdrom=iso, hd=False)
    m.wait_for("Text-mode shell ready", timeout=90)
    return m

def last(m, cmd, wait=1.5):
    """run a command; return the screen text after its echoed prompt line"""
    m.line(cmd, wait)
    s = m.text_screen()
    i = s.rfind("> " + cmd)
    return s[i:] if i >= 0 else s

builtin = mkfont(T + "/builtin.fnt")

# ================= default ISO =================
m = text_boot("lumaos-5.0-text.iso", "d63a")
try:
    s = m.text_screen()
    check("boot: ISO 9660 mounted as CD0", 'mounted as CD0 (read only)' in s and "iso9660" in s, s[-1500:])
    check("boot: font loaded from the CD", "font: loaded 768 bytes from CD0/FONTS/SYS8X8.FNT" in s, s[-1500:])
    check("boot: config parsed (4 keys, 0 errors)", "4 keys from CD0/ETC/LUMAOS.CFG" in s and "0 errors" in s, s[-1500:])
    check("boot: no FAIL anywhere", "FAIL" not in s, s[-1500:])
    check("RAM0 is still the current root (registered last)", "Start prompt on RAM0" in s, s[-300:])
    o = last(m, "mounts")
    check("mounts: CD0 iso9660 + RAM0 lmfs", re.search(r"CD0\s+iso9660", o) and re.search(r"RAM0\s+lmfs\s+\(current root\)", o), o)
    o = last(m, "ls CD0")
    check("ls CD0: BOOT/ ETC/ FONTS/", all(x in o for x in ("BOOT/", "ETC/", "FONTS/")), o)
    o = last(m, "ls CD0/boot")                                   # lower case: ISO lookup is case-insensitive
    check("ls CD0/boot (path inside the ISO is case-insensitive): CDBOOT.BIN KERNEL.BIN LMFS.IMG",
          all(x in o for x in ("CDBOOT.BIN", "KERNEL.BIN", "LMFS.IMG")), o)
    kern = os.path.getsize("build-text/kernel.bin")
    check("ls shows the real kernel size", ("%d bytes" % kern) in o, (o, kern))
    cfg = open("resources/LUMAOS.CFG").read()
    o = last(m, "read CD0/ETC/LUMAOS.CFG", 2.0)
    check("read CD0/ETC/LUMAOS.CFG: whole file, %d bytes" % len(cfg), ("(%d bytes)" % len(cfg)) in o and "theme=light" in o and "hostname=lumaos-live" in o, o)
    o = last(m, "read CD0/ETC/NOPE.CFG")
    check("read of a missing file reports an error", "cannot open" in o and "not found" in o, o)
    o = last(m, "ls CD0/NOPE")
    check("ls of a missing directory reports an error", "no such" in o, o)
    o = last(m, "cd CD0/FONTS")
    check("cd CD0/FONTS works (subdirectory of an ISO volume)", "Changed directory to CD0/FONTS" in o, o)
    o = last(m, "ls")
    check("ls in CD0/FONTS: SYS8X8.FNT 768 bytes", "SYS8X8.FNT" in o and "768 bytes" in o, o)
    o = last(m, "cd /", 1.0); o = last(m, "cd RAM0", 1.0)
    o = last(m, "font")
    check("font: source CD0/FONTS/SYS8X8.FNT, checksum == tools/mkfont.py", "CD0/FONTS/SYS8X8.FNT" in o and ("0x%08x" % builtin) in o.lower(), o)
    o = last(m, "config")
    check("config: source + values", "CD0/ETC/LUMAOS.CFG" in o and "light" in o and "800x450" in o and "lumaos-live" in o, o)
    o = last(m, "run hello.sw", 1.5)
    check("RAM0 programs still run", "Hello from hello.sw!" in o, o)
    o = last(m, "write CD0/ETC/X.TXT", 1.0)
    check("CD0 is read only (no way to create files on it)", "created" not in o, o)
finally:
    m.close()

# ================= custom config + corrupt font =================
cfg2 = "# test\ntheme = dark   # inline comment\nresolution=1024x576\n  desktop_icons=0\nhostname=testbox\nbogus=1\nflavour=x\ntheme2\n"
iso = mkiso("c1", "build-text", cfg=cfg2, bad_font=b"LFNT\x01 \x5f\x08" + b"\0" * 10)   # right header, wrong length
m = text_boot(iso, "d63b")
try:
    s = m.text_screen()
    check("corrupt font: boot says FAIL ... built-in font stays", "not a valid LFNT font" in s, s[-1200:])
    o = last(m, "font")
    check("corrupt font: `font` still reports the built-in table", "built-in" in o and ("0x%08x" % builtin) in o.lower(), o)
    o = last(m, "config", 2.0)
    check("config: theme dark / 1024x576 / icons 0 / hostname testbox",
          "dark" in o and "1024x576" in o and re.search(r"desktop_icons : 0", o) and "testbox" in o, o)
    check("config: unknown keys listed (bogus, flavour), 1 error (line without '=')", "unknown key   : bogus" in o and "flavour" in o and "errors 1" in o, o)
finally:
    m.close()
iso = mkiso("c2", "build-text", cfg="theme=purple\nresolution=huge\ndesktop_icons=2\n")
m = text_boot(iso, "d63c")
try:
    o = last(m, "config", 2.0)
    check("bad values are rejected, not guessed: all (unset), 3 errors", o.count("(unset)") >= 3 and "errors 3" in o, o)
finally:
    m.close()

# ================= GUI: the config is applied, the font is drawn =================
fm = mkfont(T + "/M.fnt", "M=FFFFFFFFFFFFFFFF")
check("modified font has another checksum than the built-in", fm != builtin)
cfg_gui = "theme=dark\nresolution=1024x576\ndesktop_icons=0\n"
isoA = mkiso("g1", "build", cfg=cfg_gui)                         # built-in-identical font
isoB = mkiso("g2", "build", cfg=cfg_gui, font=T + "/M.fnt")      # glyph 'M' = solid block
shots = {}
for vga in ("std", "vmware"):
    for tag, iso in (("A", isoA), ("B", isoB)):
        g = Machine(None, "d63g" + vga + tag, gui=True, elf="build/kernel.elf", vga=vga, cdrom=iso, hd=False)
        try:
            g.wait_for("Type 'help'", timeout=120); time.sleep(3)
            g.line("font", 1.5)
            scr = g.gui_screen()
            png = g.png("%s/%s_%s.png" % (T, vga, tag))
            shots[(vga, tag)] = png
            want = fm if tag == "B" else builtin
            check("[%s/%s] GUI `font` reports checksum 0x%08X" % (vga, tag, want), ("0x%08x" % want) in scr.lower(), scr[-300:])
            im = Image.open(png).convert("RGB")
            check("[%s/%s] config applied: screen is 1024x576" % (vga, tag), im.size == (1024, 576), im.size)
            px = im.load()
            lum = sum(sum(px[x, y]) for x in range(4, 40, 4) for y in range(60, 400, 40)) / (3.0 * 9 * 9)
            check("[%s/%s] config applied: dark theme (wallpaper luminance %.0f < 100)" % (vga, tag, lum), lum < 100, lum)
        finally:
            g.close()
    a = Image.open(shots[(vga, "A")]).convert("RGB"); b = Image.open(shots[(vga, "B")]).convert("RGB")
    diff = sum(1 for x in range(0, a.size[0], 1) for y in range(0, a.size[1], 1) if a.getpixel((x, y)) != b.getpixel((x, y)))
    check("[%s] modified font really drawn: screenshots differ (%d px)" % (vga, diff), diff > 30, diff)

print("\nFAILED: %s" % fails if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
