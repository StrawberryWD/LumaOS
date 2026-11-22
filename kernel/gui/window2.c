/* kernel/gui/window2.c - LumaOS 5.0 Info + Settings Windows
 *
 * DAY43 PRE-3 changes:
 *   - "Window Opacity Sim" now does something: the Info/Settings content
 *     background is filled through gui_fill_body() (window1.c), which
 *     draws a 50% checkerboard when the switch is ON and the window is
 *     inactive (see gui.h / window1.c for the why).
 *   - "Open Windows" row shows live/capacity ("3/8") from
 *     gui_window_count() — the old loop counted ids 0..MAX_WINDOWS-2 and
 *     assumed exactly one spare slot, wrong for the dynamic table.
 *   - Info text: window capacity line + `term` in the command list.
 *
 * STAGE3 changes:
 *   - Settings window is now a real interactive GUI panel instead of a
 *     scrolling text block:
 *       a. Theme: Light / Dark segmented toggle — calls gui_apply_theme()
 *          immediately and forces a full redraw.
 *       b. Show Desktop Icons: On/Off switch (g_show_desktop_icons).
 *       c. Window Opacity simulation: On/Off switch (g_opacity_sim) —
 *          when on, inactive window bodies are drawn with a stippled
 *          dim pattern to simulate translucency.
 *       d. Read-only info block: resolution + live window count.
 *     Rows are still vertically scrollable if content overflows the
 *     window (gui_window2_settings_scroll / PGUP/PGDN/wheel).
 *   - gui_window2_settings_click(x,y) is the hit-test entry point called
 *     by window1.c's mouse handler; returns 1 if it consumed the click.
 *   - Info window unchanged in spirit (About text), gained wheel scroll
 *     via gui_window2_scroll().
 */

#include "../core/kernel.h"
#include "../core/version.h"
#include "../core/bootinfo.h"
#include "gui.h"
#include "../drivers/keyboard.h"

extern gui_window_t windows[MAX_WINDOWS];
extern terminal_t   g_terminal;

/* ===== About / Info text ===== */
#define ABOUT_LINES 31
/* DAY58: the Info window text was rewritten. It used to describe the Day 1-14
 * system ("8 built-in commands", a fixed BGA address...). Now: what is running
 * (Material GUI, Ring 3 ELF programs, LMFS/FAT/ISO tooling) and, in the lines
 * marked [dyn], values read from the machine at start-up (gui_window2_init). */
static const char *about_text[ABOUT_LINES] = {
    " " LUMAOS_FULLNAME,
    " ===========",
    " Hobby x86 OS, from scratch",
    "",
    " Screen : BGA 800x450 256-color",      /* [4] [dyn] real backend */
    " LFB    : 0xE0000000 (BGA)",           /* [5] [dyn] real address */
    " Boot   : BIOS",                       /* [6] [dyn] boot medium  */
    " Kernel : 0x00010000, BSS 1 MB",
    " Arch   : i386 32-bit Protected Mode",
    " UI     : " LUMAOS_UI,
    " Build  : GCC -m32 -O2 -ffreestanding",
    "",
    " Features:",
    "  * Material GUI: app bar, rail,",
    "    rounded windows, light/dark",
    "  * Z-order window manager",
    "  * Terminal + shell (bash-style)",
    "  * Paging, Ring 3 user programs",
    "  * ELF32 loader, int 0x80 syscalls",
    "  * IDE disk, FAT12/16/32, LMFS",
    "  * ISO9660 + El Torito boot tools",
    "  * PS/2 keyboard, mouse (wheel)",
    "  * ACPI / CF9 power control",
    "",
    " Try:",
    "  help    version  ls",
    "  cd      run      pginfo",
    "  sysstat shutdown reboot",
    "",
    " Docs: doc/iso9660-eltorito.md",
    " (c) " LUMAOS_NAME " " LUMAOS_YEARS,
};

static int about_scroll = 0;

/* ===== Helpers ===== */
static int w2_clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int itoa10(int v, char *out)
{
    char tmp[12];
    int ti = 0;
    if (v == 0) { out[0]='0'; out[1]=0; return 1; }
    int neg = v < 0; if (neg) v = -v;
    while (v > 0 && ti < 11) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; }
    int oi = 0;
    if (neg) out[oi++] = '-';
    while (ti > 0) out[oi++] = tmp[--ti];
    out[oi] = 0;
    return oi;
}

/* ===== Tick ===== */
void gui2_tick(void) { }

/* ===== Info window init ===== */
/* DAY43 PRE-2: "Screen" and "LFB" are no longer hard-coded BGA text -
 * they show what vga_init_graphics() actually selected (VMware SVGA II
 * 32bpp, or Bochs BGA 8bpp) and the framebuffer address in use. */
static char about_screen_line[40];
static char about_lfb_line[40];
static char about_boot_line[40];

void gui_window2_init(void)
{
    snprintf(about_screen_line, sizeof(about_screen_line),
             " Screen : %s", vga_gfx_backend_name());
    snprintf(about_lfb_line, sizeof(about_lfb_line),
             " LFB    : 0x%08X (%s)", (unsigned)vga_gfx_fb_addr(),
             vga_gfx_is_svga() ? "SVGA" : "BGA");
    snprintf(about_boot_line, sizeof(about_boot_line), " Boot   : %s", boot_media_name());
    about_text[6] = about_boot_line;
    about_text[4] = about_screen_line;
    about_text[5] = about_lfb_line;

    int iw = 336, ih = 250;
    int ix = DOCK_W + (SCREEN_W - DOCK_W - iw) / 2;
    int iy = MENUBAR_H + (SCREEN_H - MENUBAR_H - ih) / 2;
    gui_window_init(WIN_INFO, ix, iy, iw, ih, "Info", 0);
    windows[WIN_INFO].flags &= (unsigned char)~WIN_VISIBLE;
    about_scroll = 0;
}

/* ===== Settings window init ===== */
void gui_window2_settings_init(void)
{
    int sw = 300, sh = 260;
    int sx = DOCK_W + 40;
    int sy = MENUBAR_H + 20;
    gui_window_init(WIN_SETTINGS, sx, sy, sw, sh, "Settings", 0);
    windows[WIN_SETTINGS].flags &= (unsigned char)~WIN_VISIBLE;
}

/* ===== Draw Info window content ===== */
static void draw_info_content(void)
{
    if (!(windows[WIN_INFO].flags & WIN_VISIBLE)) return;
    if (  windows[WIN_INFO].flags & WIN_MINIMIZED) return;

    gui_window_t *w = &windows[WIN_INFO];
    int cx = w->x + 2;
    int cy = w->y + TITLEBAR_H + 1;
    int cw = w->w - 4;
    int ch = w->h - TITLEBAR_H - 3;
    if (cw <= 0 || ch <= 0) return;

    gui_fill_body(WIN_INFO, cx, cy, cw, ch, COL_WIN_BODY);

    int visible = ch / 9;
    int max_sc  = ABOUT_LINES - visible;
    if (max_sc < 0) max_sc = 0;
    about_scroll = w2_clamp(about_scroll, 0, max_sc);

    for (int row = 0; row < visible; row++) {
        int li = about_scroll + row;
        if (li >= ABOUT_LINES) break;
        int max_chars = (cw - 4) / 8;
        unsigned char col = (li < 2) ? (unsigned char)COL_BTN_TOGGLE_ON
                                     : (unsigned char)COL_TEXT_PRIMARY;
        const char *s = about_text[li];
        int sx2 = cx + 2;
        for (int ci = 0; s[ci] && ci < max_chars; ci++)
            vga_draw_char(sx2 + ci*8, cy + row*9, s[ci], col);
    }

    if (ABOUT_LINES > visible) {
        int sbx = w->x + w->w - 5;
        gui_round_rect(sbx, cy, 3, ch, 1, COL_SCROLLBAR);
        int th = (visible * ch) / ABOUT_LINES;
        if (th < 4) th = 4;
        int ty2 = cy + (about_scroll * (ch - th)) / (max_sc > 0 ? max_sc : 1);
        gui_round_rect(sbx, ty2, 3, th, 1, COL_SCROLLTHM);
    }
}

/* =====================================================================
 * Settings panel — interactive rows
 * ===================================================================== */

/* Row layout constants (content-relative y offsets, in px, top to bottom) */
#define ROW_H        26
#define ROW_PAD_X     6

/* Row indices, used both for drawing and hit-testing so they can never
 * drift apart. */
enum {
    ROW_THEME = 0,
    ROW_DESKICONS,
    ROW_OPACITY,
    ROW_INFO_RES,
    ROW_INFO_WINCOUNT,
    ROW_COUNT
};

static int settings_scroll = 0; /* in rows, for overflow scrolling */

/* Returns 1 if row `row` is a togglable control (vs read-only info) */
static int row_is_toggle(int row)
{
    return row == ROW_THEME || row == ROW_DESKICONS || row == ROW_OPACITY || row == ROW_INFO_RES;
}

static int live_window_count(void)
{
    return gui_window_count();      /* PRE-3: visible windows in the dynamic table */
}

/* Draw one row's control (toggle/segmented switch) at absolute (x,y).
 * Returns the pixel width consumed, purely informational. */
static void draw_toggle(int x, int y, int on)
{
    /* DAY57 Material switch: 34x14 pill track, round thumb */
    unsigned char bg = on ? (unsigned char)COL_BTN_TOGGLE_ON
                          : (unsigned char)COL_BTN_TOGGLE_OFF;
    gui_round_rect(x, y, 34, 14, 7, bg);
    int kx = on ? x + 34 - 13 : x + 1;
    gui_fill_disc(kx + 6, y + 7, 6, COL_WHITE);
}

static void draw_segmented(int x, int y, int is_light)
{
    /* Material segmented button: 1px outlined pill, selected half filled */
    int seg_w = 48, seg_h = 16;
    unsigned char on_txt = g_theme_light ? (unsigned char)COL_WHITE : (unsigned char)COL_BLACK;
    gui_round_rect(x, y, seg_w * 2, seg_h, 8, COL_PANEL_BORDER);
    gui_round_rect(x + 1, y + 1, seg_w - 1, seg_h - 2, 7,
                   is_light ? (unsigned char)COL_BTN_TOGGLE_ON : (unsigned char)COL_WIN_BODY);
    gui_round_rect(x + seg_w, y + 1, seg_w - 1, seg_h - 2, 7,
                   !is_light ? (unsigned char)COL_BTN_TOGGLE_ON : (unsigned char)COL_WIN_BODY);
    vga_draw_string(x + 6, y + 4, "Light", is_light ? on_txt : COL_TEXT_SECONDARY);
    vga_draw_string(x + seg_w + 8, y + 4, "Dark", !is_light ? on_txt : COL_TEXT_SECONDARY);
}

void gui_window2_settings_draw(void)
{
    if (!(windows[WIN_SETTINGS].flags & WIN_VISIBLE)) return;
    if (  windows[WIN_SETTINGS].flags & WIN_MINIMIZED) return;

    gui_window_t *w = &windows[WIN_SETTINGS];
    int cx = w->x + 2;
    int cy = w->y + TITLEBAR_H + 1;
    int cw = w->w - 4;
    int ch = w->h - TITLEBAR_H - 3;
    if (cw <= 0 || ch <= 0) return;

    gui_fill_body(WIN_SETTINGS, cx, cy, cw, ch, COL_SETTINGS_BG);

    int visible_rows = ch / ROW_H;
    if (visible_rows < 1) visible_rows = 1;
    int max_sc = ROW_COUNT - visible_rows;
    if (max_sc < 0) max_sc = 0;
    settings_scroll = w2_clamp(settings_scroll, 0, max_sc);

    char numbuf[12];

    for (int slot = 0; slot < visible_rows; slot++) {
        int row = settings_scroll + slot;
        if (row >= ROW_COUNT) break;
        int ry = cy + slot * ROW_H;

        /* Row separator */
        if (slot > 0) vga_draw_hline(cx, ry, cw, COL_PANEL_BORDER);

        int label_y = ry + ROW_H/2 - 4;

        switch (row) {
        case ROW_THEME:
            vga_draw_string(cx + ROW_PAD_X, label_y, "Theme:", COL_TEXT_PRIMARY);
            draw_segmented(cx + cw - 6 - 96, ry + (ROW_H-16)/2, g_theme_light);
            break;

        case ROW_DESKICONS:
            vga_draw_string(cx + ROW_PAD_X, label_y, "Show Desktop Icons:", COL_TEXT_PRIMARY);
            draw_toggle(cx + cw - 6 - 34, ry + (ROW_H-14)/2, g_show_desktop_icons);
            break;

        case ROW_OPACITY:
            vga_draw_string(cx + ROW_PAD_X, label_y, "Window Opacity Sim:", COL_TEXT_PRIMARY);
            draw_toggle(cx + cw - 6 - 34, ry + (ROW_H-14)/2, g_opacity_sim);
            break;

        case ROW_INFO_RES: {
            /* DAY58b: selectable. "<  800x450  >" pill: left half = previous mode, right half = next */
            vga_draw_string(cx + ROW_PAD_X, label_y, "Resolution:", COL_TEXT_PRIMARY);
            int px = cx + cw - 6 - 96, py = ry + (ROW_H-16)/2;
            gui_round_rect(px, py, 96, 16, 8, COL_PANEL_BORDER);
            gui_round_rect(px + 1, py + 1, 94, 14, 7, COL_WIN_BODY);
            vga_draw_string(px + 6, py + 4, "<", COL_BTN_TOGGLE_ON);
            vga_draw_string(px + 96 - 14, py + 4, ">", COL_BTN_TOGGLE_ON);
            int nl = itoa10(SCREEN_W, numbuf);
            numbuf[nl] = 'x';
            itoa10(SCREEN_H, numbuf + nl + 1);
            int tw = (int)strlen(numbuf) * 8;
            vga_draw_string(px + (96 - tw) / 2, py + 4, numbuf, COL_TEXT_PRIMARY);
            break;
        }

        case ROW_INFO_WINCOUNT: {
            vga_draw_string(cx + ROW_PAD_X, label_y, "Open Windows:", COL_TEXT_SECONDARY);
            {
                char cap[12];
                int  nl = itoa10(live_window_count(), numbuf);
                numbuf[nl] = '/';
                itoa10(MAX_WINDOWS, cap);
                int ci = 0; while (cap[ci]) { numbuf[nl + 1 + ci] = cap[ci]; ci++; }
                numbuf[nl + 1 + ci] = 0;
            }
            vga_draw_string(cx + cw - 6 - 32, label_y, numbuf, COL_TEXT_PRIMARY);
            break;
        }
        }
    }

    /* Scrollbar if content overflows */
    if (ROW_COUNT > visible_rows) {
        int sbx = w->x + w->w - 5;
        gui_round_rect(sbx, cy, 3, ch, 1, COL_SCROLLBAR);
        int th = (visible_rows * ch) / ROW_COUNT;
        if (th < 4) th = 4;
        int ty2 = cy + (settings_scroll * (ch - th)) / (max_sc > 0 ? max_sc : 1);
        gui_round_rect(sbx, ty2, 3, th, 1, COL_SCROLLTHM);
    }
}

/* ===== Settings click hit-test =====
 * Called by window1.c's mouse handler with absolute screen coords.
 * Returns 1 if the click was consumed by a control, 0 otherwise (so
 * the caller can still treat it as a plain focus/drag click). */
int gui_window2_settings_click(int x, int y)
{
    if (!(windows[WIN_SETTINGS].flags & WIN_VISIBLE)) return 0;
    if (  windows[WIN_SETTINGS].flags & WIN_MINIMIZED) return 0;

    gui_window_t *w = &windows[WIN_SETTINGS];
    int cx = w->x + 2;
    int cy = w->y + TITLEBAR_H + 1;
    int cw = w->w - 4;
    int ch = w->h - TITLEBAR_H - 3;
    if (cw <= 0 || ch <= 0) return 0;
    if (x < cx || x >= cx+cw || y < cy || y >= cy+ch) return 0;

    int visible_rows = ch / ROW_H;
    if (visible_rows < 1) visible_rows = 1;
    int slot = (y - cy) / ROW_H;
    int row = settings_scroll + slot;
    if (row < 0 || row >= ROW_COUNT) return 0;
    if (!row_is_toggle(row)) return 0; /* read-only rows aren't clickable */

    int ry = cy + slot * ROW_H;

    if (row == ROW_THEME) {
        int seg_x = cx + cw - 6 - 96;
        int seg_y = ry + (ROW_H-16)/2;
        if (x >= seg_x && x < seg_x+48 && y >= seg_y && y < seg_y+16) {
            gui_apply_theme(1); return 1;   /* Light */
        }
        if (x >= seg_x+48 && x < seg_x+96 && y >= seg_y && y < seg_y+16) {
            gui_apply_theme(0); return 1;   /* Dark */
        }
        return 1; /* clicked in row but missed both segments — still consume */
    }

    if (row == ROW_INFO_RES) {
        int px = cx + cw - 6 - 96, py = ry + (ROW_H-16)/2;
        if (x >= px && x < px + 96 && y >= py && y < py + 16) {
            int n = vga_mode_count(), cur = vga_mode_find(SCREEN_W, SCREEN_H), w2, h2;
            if (cur < 0) cur = 0;
            int nxt = (x < px + 48) ? (cur + n - 1) % n : (cur + 1) % n;
            if (vga_mode_get(nxt, &w2, &h2) == 0)
                gui_set_resolution(w2, h2);   /* Settings itself is re-fitted by the call */
        }
        return 1;
    }

    if (row == ROW_DESKICONS) {
        int tx = cx + cw - 6 - 34;
        int ty = ry + (ROW_H-14)/2;
        if (x >= tx && x < tx+34 && y >= ty && y < ty+14) {
            g_show_desktop_icons = !g_show_desktop_icons;
            g_terminal.redraw_needed = 1;
        }
        return 1;
    }

    if (row == ROW_OPACITY) {
        int tx = cx + cw - 6 - 34;
        int ty = ry + (ROW_H-14)/2;
        if (x >= tx && x < tx+34 && y >= ty && y < ty+14) {
            g_opacity_sim = !g_opacity_sim;
            g_terminal.redraw_needed = 1;
        }
        return 1;
    }

    return 0;
}

/* ===== Public Info-window draw entry point =====
 * Called from gui_window_draw() (window1.c) inside the z-order-
 * respecting per-window draw pass, exactly like
 * gui_window2_settings_draw() already is for Settings. Previously the
 * Info content was painted by gui2_draw_overlay() as a *global* pass
 * that ran after every window had already been drawn in z-order, so it
 * always ended up on top of everything else on screen -- even windows
 * stacked above the (possibly not even focused) Info window. Routing
 * it through the same per-window path as Settings/Terminal fixes that:
 * it now only ever draws where Info's own z-order slot puts it. */
void gui_window2_info_draw(void)
{
    /* DAY58b: the screen size can change at run time - keep the line current */
    snprintf(about_screen_line, sizeof(about_screen_line),
             " Screen : %s %dx%d", vga_gfx_is_svga() ? "SVGA32" : "BGA 8bpp", SCREEN_W, SCREEN_H);
    draw_info_content();
}

/* ===== Public draw overlay =====
 * No longer draws Info content (see gui_window2_info_draw() above) --
 * kept as a hook for any future overlay that must genuinely sit above
 * every window (e.g. a global toast/notification), which the Info
 * window's z-order-scoped content must not.  Currently a no-op. */
void gui2_draw_overlay(void)
{
    /* Settings content is drawn by gui_window_draw() via callback */
}

/* ===== Key routing — Info window ===== */
void gui_window2_key(unsigned char key)
{
    if (key == KEY_UP || key == KEY_PGUP) {
        if (about_scroll > 0) { about_scroll--; g_terminal.redraw_needed=1; }
    } else if (key == KEY_DOWN || key == KEY_PGDN) {
        int visible = (windows[WIN_INFO].h - TITLEBAR_H - 3) / 9;
        int max_sc  = ABOUT_LINES - visible;
        if (max_sc < 0) max_sc = 0;
        if (about_scroll < max_sc) { about_scroll++; g_terminal.redraw_needed=1; }
    }
}

/* ===== Wheel scroll — Info window ===== */
void gui_window2_scroll(int delta)
{
    int visible = (windows[WIN_INFO].h - TITLEBAR_H - 3) / 9;
    int max_sc  = ABOUT_LINES - visible;
    if (max_sc < 0) max_sc = 0;
    about_scroll = w2_clamp(about_scroll + delta, 0, max_sc);
    g_terminal.redraw_needed = 1;
}

/* ===== Key routing — Settings window ===== */
void gui_window2_settings_key(unsigned char key)
{
    int visible_rows = (windows[WIN_SETTINGS].h - TITLEBAR_H - 3) / ROW_H;
    if (visible_rows < 1) visible_rows = 1;
    int max_sc = ROW_COUNT - visible_rows;
    if (max_sc < 0) max_sc = 0;

    if (key == KEY_UP || key == KEY_PGUP) {
        if (settings_scroll > 0) { settings_scroll--; g_terminal.redraw_needed=1; }
    } else if (key == KEY_DOWN || key == KEY_PGDN) {
        if (settings_scroll < max_sc) { settings_scroll++; g_terminal.redraw_needed=1; }
    }
}

/* ===== Wheel scroll — Settings window ===== */
void gui_window2_settings_scroll(int delta)
{
    int visible_rows = (windows[WIN_SETTINGS].h - TITLEBAR_H - 3) / ROW_H;
    if (visible_rows < 1) visible_rows = 1;
    int max_sc = ROW_COUNT - visible_rows;
    if (max_sc < 0) max_sc = 0;
    settings_scroll = w2_clamp(settings_scroll + delta, 0, max_sc);
    g_terminal.redraw_needed = 1;
}