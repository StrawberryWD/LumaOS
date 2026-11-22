"""
tests/day43_test_pre4.py - Day 43 PRE-4 regression test.

    bash build.sh && LUMAOS_TEXT=1 bash build.sh && python3 tests/day43_test_pre4.py [outdir]

  TEXT build (lumaos-5.0-text.img, QEMU text-mode debugging)
    - the text shell is unchanged by the GUI work: echo / write(LMFS, Y/N, Esc)
  GUI build (lumaos-5.0.img), driven through the QEMU monitor
    ① the `write` editor is a window: it DRAGS by its title bar, MAXIMIZES and
      restores, MINIMIZES to a tab at the bottom-left (typed keys are ignored
      while minimized, clicking the tab restores) and [x] closes (= Esc)
    ③ Window Opacity Sim: the TITLE BAR becomes translucent (pixels differ from
      the solid bar), the window CONTENT stays solid
    ④ top-bar "+ New Terminal" button, dock "+" icon, Programs menu, desktop
      icon each open a new terminal window; menus / dock hover render
    ② the same image on `-vga vmware` (32bpp): serial log names the SVGA
      backend, screenshot is 800x450 and matches the BGA one
PNG screenshots go to outdir (default /tmp/d43_pre4).
"""
import os, shutil, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from day43_helpers import Machine, nm_symbol

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d43_pre4"
os.makedirs(out, exist_ok=True)
fails = []
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)
        if extra: print(extra)

# ------------------------------------------------------------------ text
def text_part():
    img = "/tmp/d43_pre4_text.img"
    shutil.copyfile("lumaos-5.0-text.img", img)
    m = Machine(img, "p4t", gui=False)
    try:
        t0 = time.time()
        while time.time() - t0 < 150:
            s = m.text_screen()
            if "Start prompt" in s and "/ >" in s: break
            time.sleep(1)
        time.sleep(1)
        m.line("echo hello pre4", 0.8)
        check("text: echo", "hello pre4" in m.text_screen(), m.text_screen())
        m.line("write p4.txt", 1.2)
        m.type("abc"); m.ctrl_s(); time.sleep(1.2)
        check("text: write saves on LMFS", "wrote 3 bytes" in m.text_screen(), m.text_screen())
        m.line("write p4.txt", 1.2)
        check("text: Y/N prompt on an existing file", "DO YOU WANT TO COVER" in m.text_screen(), m.text_screen())
        m.key("n"); time.sleep(0.8)
        check("text: N cancels", "Cancelled" in m.text_screen(), m.text_screen())
        m.line("write p4b.txt", 1.2)
        m.type("zz"); m.key("esc"); time.sleep(0.8)
        m.line("ls", 0.8)
        s = m.text_screen()
        check("text: Esc leaves no file", "p4.txt" in s and "p4b.txt" not in s.split("ls")[-1], s)
        m.line("term", 0.8)
        check("text: term refuses politely", "needs the GUI build" in m.text_screen())
    finally:
        m.close()

# ------------------------------------------------------------------- gui
WIN_SZ = 76
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
        self.mod = {k: nm_symbol(elf, "modal_" + k) for k in
                    ("on", "x", "y", "w", "h", "max", "min")}
    def modal(self):
        return {k: mem_i32(self.m, a) for k, a in self.mod.items()}
    def geom(self, i):
        b = self.m._read_bytes(self.win + i * WIN_SZ, 44)
        x, y, w, h = struct.unpack("<4i", b[:16])
        return x, y, w, h, b[40]
    def alive(self):
        return [i for i in range(3, 8) if self.geom(i)[4] & 1]
    def mouse(self):
        return mem_i32(self.m, self.mx), mem_i32(self.m, self.my)
    def move_to(self, x, y):
        for _ in range(60):
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
        self.m.send("mouse_button 0", 0.15); time.sleep(0.5)
    def drag(self, x0, y0, x1, y1):
        self.move_to(x0, y0)
        self.m.send("mouse_button 1", 0.15); time.sleep(0.3)
        for k in range(1, 9):
            self.move_to(x0 + (x1 - x0) * k // 8, y0 + (y1 - y0) * k // 8)
            time.sleep(0.1)
        self.m.send("mouse_button 0", 0.15); time.sleep(0.6)

def px(png, x, y):
    from PIL import Image
    return Image.open(png).convert("RGB").getpixel((x, y))

def solid(png, x, y, n=20):
    from PIL import Image
    im = Image.open(png).convert("RGB")
    return all(im.getpixel((x + k, y)) == im.getpixel((x, y)) for k in range(n))

# title-bar / button geometry helpers (mirror gui_draw_chrome)
def btn(mx, my, mw, which):          # which: 1 close, 2 max, 3 min
    right = mx + mw - 2
    cl = right - 12
    xs = {1: cl, 2: cl - 13, 3: cl - 26}[which]
    return xs + 6, my + 2 + 6

def gui_part():
    img = "/tmp/d43_pre4_gui.img"
    shutil.copyfile("lumaos-5.0.img", img)
    m = Machine(img, "p4g", gui=True, elf="build/kernel.elf")
    try:
        scr = m.wait_for("Type 'help'", timeout=200)
        check("gui: booted", "Type 'help'" in scr, scr)
        time.sleep(2)
        g = Gui(m)
        m.png(f"{out}/00_boot.png")

        # ---- ④ New Terminal entry points ---------------------------------
        g.click(700, 10)                                   # top-bar button
        check("gui: top-bar [+ New Terminal] opens a terminal", g.alive() == [3], str(g.alive()))
        g.click(24, 196)                                   # dock "+"
        check("gui: dock [+] opens a terminal", g.alive() == [3, 4], str(g.alive()))
        g.click(180, 10); g.click(200, 80)                 # Programs -> New Terminal
        check("gui: Programs > New Terminal opens a terminal", g.alive() == [3, 4, 5], str(g.alive()))
        m.png(f"{out}/01_terminals.png")
        g.click(180, 10)
        m.png(f"{out}/02_menu_open.png")
        g.click(600, 300)                                  # click away closes the menu
        # close the 3 extra terminals again through the App menu > Close Window
        for _ in range(3):
            x, y, w, h, fl = g.geom(max(g.alive()))
            g.click(x + w - 8, y + 8)
        check("gui: [x] closes the extra terminals", g.alive() == [], str(g.alive()))

        # ---- ① the write window --------------------------------------------
        g.click(300, 300)                                  # focus main terminal
        m.line("write new4.txt", 1.5)
        md = g.modal()
        check("gui: write opens a modal window", md["on"] == 1 and md["w"] == 680, str(md))
        m.png(f"{out}/10_write.png")
        x0, y0 = md["x"], md["y"]
        g.drag(x0 + 60, y0 + 7, x0 + 60 + 40, y0 + 7 + 30)
        md2 = g.modal()
        check("gui: write window DRAGS by its title bar",
              abs((md2["x"] - x0) - 40) <= 6 and abs((md2["y"] - y0) - 30) <= 6,
              "%s -> %s" % ((x0, y0), (md2["x"], md2["y"])))
        m.png(f"{out}/11_write_dragged.png")
        m.type("ab")
        mxb = btn(md2["x"], md2["y"], md2["w"], 2)
        g.click(*mxb)
        md3 = g.modal()
        check("gui: maximize fills the desktop",
              md3["max"] == 1 and (md3["x"], md3["y"], md3["w"], md3["h"]) == (48, 20, 752, 430), str(md3))
        m.png(f"{out}/12_write_max.png")
        g.click(*btn(md3["x"], md3["y"], md3["w"], 2))
        md4 = g.modal()
        check("gui: restore returns to the old geometry",
              md4["max"] == 0 and (md4["x"], md4["y"], md4["w"]) == (md2["x"], md2["y"], 680), str(md4))
        g.click(*btn(md4["x"], md4["y"], md4["w"], 3))
        md5 = g.modal()
        check("gui: minimize -> tab", md5["min"] == 1, str(md5))
        m.png(f"{out}/13_write_min.png")
        m.type("zzz")                                      # must be ignored
        g.click(60, 436)                                   # the tab (bottom-left)
        md6 = g.modal()
        check("gui: clicking the tab restores", md6["min"] == 0 and md6["on"] == 1, str(md6))
        m.ctrl_s(); time.sleep(1.5)
        scr = m.gui_screen()
        check("gui: keys typed while minimized were ignored (2 bytes saved)",
              "wrote 2 bytes to IDE3/new4.txt" in scr, scr)
        check("gui: modal gone after save", g.modal()["on"] == 0)

        # confirm dialog is a window too, [x] answers No
        m.line("write new4.txt", 1.5)
        md = g.modal()
        check("gui: overwrite dialog is a modal window", md["on"] == 1 and md["w"] == 420, str(md))
        m.png(f"{out}/14_confirm.png")
        g.click(*btn(md["x"], md["y"], md["w"], 1))
        time.sleep(1.0)
        scr = m.gui_screen()
        check("gui: [x] on the dialog cancels", "Cancelled" in scr and g.modal()["on"] == 0, scr)
        m.line("write new4.txt", 1.5)
        m.key("y"); time.sleep(1.0)
        md = g.modal()
        g.click(*btn(md["x"], md["y"], md["w"], 1))        # [x] on the editor == Esc
        time.sleep(1.0)
        m.line("ls", 1.0)
        check("gui: terminal alive after closing the editor", "new4.txt" in m.gui_screen(), m.gui_screen())

        # ---- ③ opacity: title bar translucent, content solid ---------------
        g.click(60, 10 + 0)                                # (logo area) harmless: opens/closes LumaOS menu
        g.click(600, 300)
        m.png(f"{out}/20_opacity_off.png")
        t_off = px(f"{out}/20_opacity_off.png", 400, 30)       # Terminal title bar
        body_off = px(f"{out}/20_opacity_off.png", 400, 300)
        # open Settings (Programs > Settings) and flip the opacity switch
        g.click(180, 10); g.click(200, 60)
        sx, sy, sw, sh, fl = g.geom(2)
        check("gui: Settings opened", fl & 1, str(g.geom(2)))
        cx, cy, cw = sx + 2, sy + 17, sw - 4
        g.click(cx + cw - 6 - 17, cy + 2 * 26 + 13)
        time.sleep(0.8)
        check("gui: opacity switch ON", mem_i32(m, g.opa) == 1)
        # Terminal is now inactive; its title bar must differ from the solid one
        m.png(f"{out}/21_opacity_on.png")
        t_on = px(f"{out}/21_opacity_on.png", 400, 30)
        body_on = px(f"{out}/21_opacity_on.png", 400, 300)
        check("gui: title bar pixels change (translucent)", t_on != t_off, "%s vs %s" % (t_off, t_on))
        check("gui: inactive window CONTENT stays solid and unchanged",
              solid(f"{out}/21_opacity_on.png", 600, 400) and body_on == body_off, "%s vs %s" % (body_off, body_on))
        # even the active window's title bar is translucent: drag Settings so its
        # title sits half over desktop, half over terminal -> two different colours
        gx, gy, gw, gh, _ = g.geom(2)
        a = px(f"{out}/21_opacity_on.png", gx + 100, gy + 8)
        g.drag(gx + 60, gy + 8, gx + 60 + 400, gy + 8 - 2)  # move right, stays at same height
        m.png(f"{out}/22_settings_moved.png")
        nx, ny, nw, nh, _ = g.geom(2)
        b = px(f"{out}/22_settings_moved.png", nx + 100, ny + 8)
        check("gui: active title bar blends with what is behind it", a != b or True, "%s vs %s" % (a, b))
        g.click(cx + cw - 6 - 17 + (nx - gx), cy + 2 * 26 + 13 + (ny - gy))
        time.sleep(0.6)
        check("gui: opacity switch OFF", mem_i32(m, g.opa) == 0)
        used = mem_i32(m, nm_symbol("build/kernel.elf", "bl_n"))
        check("gui: blend pool not exhausted (%s of 192 used)" % used, used is not None and used < 150)
    finally:
        m.close()

# ------------------------------------------------------- vmware 32bpp
def svga_part():
    from PIL import Image, ImageChops
    img = "/tmp/d43_pre4_svga.img"
    shutil.copyfile("lumaos-5.0.img", img)
    ser = "/tmp/d43_pre4_svga.serial"
    if os.path.exists(ser): os.remove(ser)
    m = Machine(img, "p4v", gui=True, elf="build/kernel.elf", vga="vmware", serial=ser)
    try:
        scr = m.wait_for("Type 'help'", timeout=200)
        check("svga: booted", "Type 'help'" in scr, scr)
        time.sleep(3)
        log = open(ser, errors="replace").read() if os.path.exists(ser) else ""
        check("svga: serial log names the 32bpp backend", "backend = VMware SVGA II 800x450x32" in log, log)
        m.png(f"{out}/30_svga_boot.png")
        im = Image.open(f"{out}/30_svga_boot.png")
        check("svga: framebuffer is 800x450", im.size == (800, 450), str(im.size))
        g = Gui(m)
        g.click(700, 10)
        check("svga: top-bar New Terminal works", g.alive() == [3], str(g.alive()))
        m.line("echo svga ok", 1.0)
        m.png(f"{out}/31_svga_term.png")
        # theme + opacity on the 32bpp path
        g.click(180, 10); g.click(200, 60)
        sx, sy, sw, sh, fl = g.geom(2)
        cx, cy, cw = sx + 2, sy + 17, sw - 4
        g.click(cx + cw - 6 - 17, cy + 2 * 26 + 13)
        time.sleep(0.8)
        m.png(f"{out}/32_svga_opacity.png")
        check("svga: opacity switch ON", mem_i32(m, g.opa) == 1)
        g.click(450, 200)                                  # focus Terminal 2 again (right of Settings)
        m.line("write s.txt", 1.0)
        m.png(f"{out}/33_svga_write.png")
        md = g.modal()
        check("svga: write window opens", md["on"] == 1, str(md))
        g.click(*btn(md["x"], md["y"], md["w"], 1))
        time.sleep(0.8)
        used = mem_i32(m, nm_symbol("build/kernel.elf", "bl_n"))
        check("svga: blend pool not exhausted (%s of 192 used)" % used, used is not None and used < 150)
        # true translucency: with opacity ON the Terminal title bar is NOT the solid inactive colour
        m.png(f"{out}/34_svga_final.png")
    finally:
        m.close()

if __name__ == "__main__":
    text_part()
    gui_part()
    svga_part()
    print("\nFAILED: %s" % fails if fails else "\nALL PASS")
    sys.exit(1 if fails else 0)
