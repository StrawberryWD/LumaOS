"""
tests/day43_test_pre3.py - Day 43 PRE-3 regression test.

    ./build.sh && LUMAOS_TEXT=1 ./build.sh && python3 tests/day43_test_pre3.py [outdir]

  TEXT build (lumaos-5.0-text.img, QEMU text-mode debugging)
    - `echo` with 16 words prints ALL of them (used to stop at 7)
    - long (wrapping) input line + scrolling screen: no stacked garbage
    - \\n / \\t escapes, -n, spacing preserved
  GUI build (lumaos-5.0.img), driven through the QEMU monitor
    - Settings window can be DRAGGED by its titlebar (used to be stuck)
    - Terminal + Info windows can still be dragged
    - Window Opacity Sim switch (PRE-4: now translucent TITLE BARS, content
      stays solid; PRE-3 used a checkerboard body, superseded)
    - `term` opens extra Terminal windows up to MAX_WINDOWS (8) and then
      refuses; [x] on an extra window destroys it and frees the slot
    - GUI echo: the 16-word line is complete in g_terminal AND wrapped
      (not cut) on screen (screenshot saved)
PNG screenshots go to outdir (default /tmp/d43_pre3).
"""
import os, shutil, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine, nm_symbol

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d43_pre3"
os.makedirs(out, exist_ok=True)
fails = []
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)
        if extra: print(extra)

WORDS = "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen"

# ------------------------------------------------------------------ text
def text_part():
    img = "/tmp/d43_pre3_text.img"
    shutil.copyfile("lumaos-5.0-text.img", img)
    m = Machine(img, "p3t", gui=False)
    try:
        t0 = time.time()
        while time.time() - t0 < 150:
            s = m.text_screen()
            if "Start prompt" in s and "/ >" in s: break
            time.sleep(1)
        time.sleep(1)
        for i in range(12):            # push the prompt to the bottom row
            m.line("echo filler%d" % i, 0.4)
        m.line("echo " + WORDS, 1.0)
        s = m.text_screen()
        check("text: echo prints all 16 words", "fifteen" in s and "sixteen" in s, s)
        check("text: no garbage from wrapped input line",
              s.count("echo one two") == 1, s)
        m.line('echo "A1\\nB2\\tC3"', 0.8)
        s = m.text_screen()
        check("text: \\n escape gives separate lines", "\nA1\n" in s and "B2" in s, s)
        m.line("echo   spaced    out", 0.8)
        check("text: spacing preserved", "spaced    out" in m.text_screen())
        m.line("echo -n hi", 0.8)
        check("text: -n works", "\nhi\n" in m.text_screen())
        m.line("term", 0.8)
        check("text: term refuses politely", "needs the GUI build" in m.text_screen())
    finally:
        m.close()

# ------------------------------------------------------------------- gui
WIN_SZ = 76   # sizeof(gui_window_t): 10 ints + flags + title[32], padded
def mem_i32(m, addr):
    b = m._read_bytes(addr, 4)
    return struct.unpack("<i", b)[0] if len(b) == 4 else None

class Gui:
    def __init__(self, m, elf="build/kernel.elf"):
        self.m = m
        self.win = nm_symbol(elf, "windows")
        self.mx = nm_symbol(elf, "mouse_x")
        self.my = nm_symbol(elf, "mouse_y")
        self.opa = nm_symbol(elf, "g_opacity_sim")
        self.extra = 0x500000
    def geom(self, i):
        b = self.m._read_bytes(self.win + i * WIN_SZ, 44)
        x, y, w, h = struct.unpack("<4i", b[:16])
        return x, y, w, h, b[40]
    def mouse(self):
        return mem_i32(self.m, self.mx), mem_i32(self.m, self.my)
    def move_to(self, x, y):
        for _ in range(40):
            cx, cy = self.mouse()
            dx, dy = x - cx, y - cy
            if abs(dx) <= 1 and abs(dy) <= 1: return
            sx = max(-3, min(3, dx)) if abs(dx) < 12 else max(-60, min(60, dx))
            sy = max(-3, min(3, dy)) if abs(dy) < 12 else max(-60, min(60, dy))
            self.m.send("mouse_move %d %d" % (sx, sy), 0.15)
            time.sleep(0.12)
    def click(self, x, y):
        self.move_to(x, y)
        self.m.send("mouse_button 1", 0.15); time.sleep(0.25)
        self.m.send("mouse_button 0", 0.15); time.sleep(0.4)
    def drag(self, x0, y0, x1, y1):
        self.move_to(x0, y0)
        self.m.send("mouse_button 1", 0.15); time.sleep(0.3)
        # move in steps while the button is held
        steps = 8
        for k in range(1, steps + 1):
            self.move_to(x0 + (x1 - x0) * k // steps, y0 + (y1 - y0) * k // steps)
            time.sleep(0.1)
        self.m.send("mouse_button 0", 0.15); time.sleep(0.5)

def checker(png, x, y, n=20):
    """fraction of horizontally adjacent pixel pairs that differ"""
    from PIL import Image
    im = Image.open(png).convert("RGB")
    d = 0
    for k in range(n):
        if im.getpixel((x + k, y)) != im.getpixel((x + k + 1, y)): d += 1
    return d / n

def gui_part():
    img = "/tmp/d43_pre3_gui.img"
    shutil.copyfile("lumaos-5.0.img", img)
    m = Machine(img, "p3g", gui=True, elf="build/kernel.elf")
    try:
        scr = m.wait_for("Type 'help'", timeout=200)
        check("gui: booted", "Type 'help'" in scr, scr)
        time.sleep(2)
        g = Gui(m)

        # ---- ③ echo in the GUI -------------------------------------
        m.line("echo " + WORDS + " " + WORDS, 1.2)      # ~190 chars -> wraps on screen
        scr = m.gui_screen()
        check("gui: long echo complete in buffer",
              "sixteen one two" in scr.replace("\n", " ") and scr.replace("\n", "").count("sixteen") >= 2, scr)
        m.png(f"{out}/1_echo.png")

        # ---- ① drag every window ------------------------------------
        # open Settings via Settings menu -> Settings, Info via Programs -> Info
        g.click(180, 10); g.click(200, 60)           # PRE-4 menu: Programs > Settings
        x, y, w, h, fl = g.geom(2)
        check("gui: Settings opened", fl & 1, str(g.geom(2)))
        g.drag(x + 60, y + 8, x + 60 + 150, y + 8 + 90)
        x2, y2, *_ = g.geom(2)
        check("gui: Settings window DRAGS (was stuck)", abs((x2 - x) - 150) <= 6 and abs((y2 - y) - 90) <= 6,
              "from (%d,%d) to (%d,%d)" % (x, y, x2, y2))
        g.click(180, 10); g.click(200, 44)               # PRE-4 menu: Programs > Info
        x, y, w, h, fl = g.geom(1)
        g.drag(x + 60, y + 8, x + 60 - 70, y + 8 + 40)
        x2, y2, *_ = g.geom(1)
        check("gui: Info window drags", abs((x2 - x) + 70) <= 6 and abs((y2 - y) - 40) <= 6,
              "from (%d,%d) to (%d,%d)" % (x, y, x2, y2))
        m.png(f"{out}/2_dragged.png")

        # ---- ② opacity ------------------------------------------------
        # Terminal (id 0) is now inactive (Info active). Probe an empty
        # terminal-body spot not covered by Settings/Info.
        px, py = 700, 400
        m.png(f"{out}/3_opacity_off.png")
        off = checker(f"{out}/3_opacity_off.png", px, py)
        from PIL import Image
        title_off = Image.open(f"{out}/3_opacity_off.png").convert("RGB").getpixel((400, 30))
        sx, sy, sw, sh, _ = g.geom(2)
        # focus Settings by clicking its body, then flip the Opacity toggle (row 2)
        cx, cy, cw = sx + 2, sy + 17, sw - 4
        g.click(sx + 30, sy + 8)                        # focus: Settings TITLE BAR (the bigger Info window since Day 58 may cover its body)
        g.click(cx + cw - 6 - 17, cy + 2 * 26 + 13)     # toggle at row 2
        time.sleep(0.8)
        check("gui: opacity switch flipped", mem_i32(m, g.opa) == 1, "g_opacity_sim=%s" % mem_i32(m, g.opa))
        m.png(f"{out}/4_opacity_on.png")
        on = checker(f"{out}/4_opacity_on.png", px, py)
        title_on = Image.open(f"{out}/4_opacity_on.png").convert("RGB").getpixel((400, 30))
        check("gui: inactive Terminal BODY stays solid when opacity ON",
              off < 0.1 and on < 0.1, "off=%.2f on=%.2f" % (off, on))
        check("gui: Terminal TITLE BAR becomes translucent when opacity ON",
              title_on != title_off, "%s vs %s" % (title_off, title_on))
        g.click(cx + cw - 6 - 17, cy + 2 * 26 + 13)     # back OFF
        time.sleep(0.8)
        m.png(f"{out}/5_opacity_off_again.png")
        again = checker(f"{out}/5_opacity_off_again.png", px, py)
        check("gui: opacity OFF keeps the body solid", again < 0.1, "again=%.2f" % again)

        # ---- ② capacity ------------------------------------------------
        # click Terminal body to focus it, then open terminals with `term`
        g.click(200, 400)
        for i in range(6):
            m.line("term", 1.0)
        alive = [i for i in range(3, 8) if g.geom(i)[4] & 1]
        check("gui: 5 extra terminals open, 6th refused", alive == [3, 4, 5, 6, 7], str(alive))
        m.png(f"{out}/6_eight_windows.png")
        # last extra terminal's buffer should contain the refusal text
        base = g.extra + (7 - 3) * 0  # not needed; verify through screenshot + flags only
        # close window 5 via its [x]
        x, y, w, h, fl = g.geom(5)
        g.click(x + w - 2 - 12 + 6, y + 2 + 6)
        alive = [i for i in range(3, 8) if g.geom(i)[4] & 1]
        check("gui: [x] destroys an extra window and frees the slot", 5 not in alive and len(alive) == 4, str(alive))
        # new term must reuse a free slot
        g.click(*(lambda gg: (gg[0] + 40, gg[1] + 60))(g.geom(7)))   # focus window 7
        m.line("term", 1.0)
        alive = [i for i in range(3, 8) if g.geom(i)[4] & 1]
        check("gui: freed slot is reused", alive == [3, 4, 5, 6, 7], str(alive))
        m.png(f"{out}/7_reused.png")
    finally:
        m.close()

if __name__ == "__main__":
    text_part()
    gui_part()
    print("\nFAILED: %s" % fails if fails else "\nALL PASS")
    sys.exit(1 if fails else 0)
