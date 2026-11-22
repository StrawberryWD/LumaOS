/* kernel/gui/gui.h
 * LumaOS 5.0 - GUI System Header
 *
 * DAY43 PRE-3 changes:
 *   - MAX_WINDOWS 4 -> 8 and the window table is now DYNAMIC: ids 0..2
 *     stay the three fixed apps (Terminal / Info / Settings, close =
 *     hide), ids GUI_EXTRA_FIRST..MAX_WINDOWS-1 are extra Terminal
 *     windows created with gui_window_new_terminal() (shell command
 *     `term`, Programs -> New Term) and DESTROYED on close. Each extra
 *     terminal owns a private terminal_t (history, scrollback, input).
 *   - TERM_INPUT_MAX 118 -> 238 and TERM_HIST_LEN 120 -> 240 so a long
 *     `echo` line is not cut off by the line editor itself. The
 *     terminal_t field ORDER is unchanged (tests/day43_helpers.py reads
 *     total_lines at lines[TERM_BUF_ROWS]).
 *   - gui_fill_body(): window-body fill that honours the Settings
 *     "Window Opacity Sim" switch (50% stipple on inactive windows).
 *   - gui_terminal_of(), gui_window_count(), gui_focus_topmost().
 *
 * STAGE3 changes:
 *   - Added resize handle constant + WIN_RESIZING flag usage (now wired).
 *   - Added MIN_WIN_W / MIN_WIN_H for resize clamping.
 *   - Added g_theme_light (default 1) + gui_set_palette(int light) form
 *     kept source-compatible via gui_set_palette() wrapper.
 *   - Added mouse wheel plumbing: gui_handle_wheel().
 *   - Added settings toggle state exposed for window2.c.
 *
 * UI layout (unchanged design, scaled to 800x450):
 *   MenuBar  : top,  y=0..19,   dark/light bg depending on theme
 *   Dock     : left, x=0..47,   off-white / light-gray
 *   Desktop  : x=48..799, y=20..449
 *   Windows  : titlebar, border, 3 buttons top-right, resize handle
 */
#ifndef LUMA_GUI_H
#define LUMA_GUI_H

#include <stdint.h>
#include <stddef.h>

/* ===== Screen ===== */
/* DAY58b: the screen size is a run-time value (Settings -> Resolution). */
extern int g_screen_w, g_screen_h;
#define SCREEN_W       g_screen_w
#define SCREEN_H       g_screen_h
int  gui_set_resolution(int w, int h);   /* 0 = ok; re-fits every window */

/* ===== UI Layout ===== */
#define MENUBAR_H      20      /* top menu bar height (px) */
#define DOCK_W         48      /* left dock width (px) */
#define DOCK_ICON_SZ   36      /* dock icon square size */
#define TITLEBAR_H     16      /* window title bar height */
#define WIN_BTN_W      12      /* close/max/min button width */
#define WIN_BTN_H      12      /* button height */
#define RESIZE_HANDLE_SZ 8     /* resize grip size (px, bottom-right) */
#define MIN_WIN_W      120     /* minimum window width  */
#define MIN_WIN_H      80      /* minimum window height */

/* Desktop area */
#define DESK_X         DOCK_W
#define DESK_Y         MENUBAR_H
#define DESK_W         (SCREEN_W - DOCK_W)
#define DESK_H         (SCREEN_H - MENUBAR_H)

/* ===== Colour palette indices ===== */
#define COL_BLACK       0
#define COL_BLUE        1
#define COL_GREEN       2
#define COL_CYAN        3
#define COL_RED         4
#define COL_MAGENTA     5
#define COL_BROWN       6
#define COL_LGRAY       7
#define COL_DGRAY       8
#define COL_LBLUE       9
#define COL_LGREEN     10
#define COL_LCYAN      11
#define COL_LRED       12
#define COL_LMAGENTA   13
#define COL_YELLOW     14
#define COL_WHITE      15

/* Extended GUI palette */
#define COL_DESKTOP    16
#define COL_MENUBAR    17
#define COL_MENUTEXT   18
#define COL_DOCK_BG    19
#define COL_DOCK_ICON0 20
#define COL_DOCK_ICON1 21
#define COL_WIN_TITLE  22
#define COL_WIN_BODY   23
#define COL_WIN_BORDER 24
#define COL_WIN_INACT  25
#define COL_TERM_BG    26
#define COL_TERM_FG    27
#define COL_TERM_CUR   28
#define COL_BTN_CLOSE  29
#define COL_BTN_MIN    30
#define COL_BTN_MAX    31
#define COL_SHADOW     32
#define COL_POPUP_BG   33
#define COL_SCROLLBAR  34
#define COL_SCROLLTHM  35
/* Stage 2+ additions */
#define COL_WIN_TITLE_INACTIVE  36   /* dimmed title bar for inactive windows */
#define COL_RESIZE_HANDLE       37   /* resize corner highlight */
#define COL_DESKTOP_ICON_BG     38   /* desktop icon label background */
#define COL_SETTINGS_BG         39   /* settings window background */
/* Stage 3 additions */
#define COL_TEXT_PRIMARY        40   /* main body text colour (theme dependent) */
#define COL_TEXT_SECONDARY      41   /* dimmer secondary text */
#define COL_BTN_TOGGLE_ON       42   /* settings toggle: ON state */
#define COL_BTN_TOGGLE_OFF      43   /* settings toggle: OFF state */
#define COL_PANEL_BORDER        44   /* settings row separators */
/* PRE-4 additions (all < 64; 64..255 is the blend pool) */
#define COL_ICON_TERM           45   /* app icon tile colours */
#define COL_ICON_INFO           46
#define COL_ICON_SET            47
#define COL_ICON_NEW            48
#define COL_MENU_HILITE         49   /* selected menu row / open menu title */
#define COL_MENU_HILITE_TEXT    50
#define COL_LABEL_TEXT          51   /* desktop icon label */
#define COL_LABEL_SHADOW        52   /* ... and its 1px shadow */
#define COL_ICON_GLYPH          53   /* icon glyph (white-ish) */
#define COL_DOCK_HILITE         54   /* focused app backdrop in the dock */
#define COL_ICON_EDGE           55   /* icon outline */
#define COL_MENUBAR_LINE        56   /* menu bar bottom line */

/* ===== Window flags ===== */
#define WIN_VISIBLE    0x01
#define WIN_ACTIVE     0x02
#define WIN_HAS_TERM   0x04
#define WIN_MINIMIZED  0x08
#define WIN_DRAGGING   0x10
#define WIN_RESIZING   0x20   /* Stage 3: resize in progress */
#define WIN_MAXIMIZED  0x40   /* Stage 3: currently maximized */

/* ===== Window IDs ===== */
#define WIN_TERMINAL   0
#define WIN_INFO       1
#define WIN_SETTINGS   2
#define GUI_EXTRA_FIRST 3      /* PRE-3: first id of the dynamic Terminal slots */
#define MAX_WINDOWS    8       /* PRE-3: was 4 (3 fixed + 1 never-used spare) */

/* ===== Dock app IDs ===== */
#define DOCK_APP_TERM  0
#define DOCK_APP_INFO  1
#define DOCK_APP_SET   2
#define DOCK_APP_NEW   3      /* PRE-4: "+" New Terminal button */
#define DOCK_APPS      4

/* ===== Terminal constants ===== */
#define TERM_BUF_ROWS  400
#define TERM_BUF_COLS  120
#define TERM_HISTORY    64
#define TERM_HIST_LEN  240     /* PRE-3: was 120 (must stay > TERM_INPUT_MAX) */
#define TERM_INPUT_MAX 238     /* PRE-3: was 118 */

/* Terminal text colour codes */
#define TERM_COL_DEFAULT  COL_TERM_FG
#define TERM_COL_PROMPT   COL_LGREEN
#define TERM_COL_ERROR    COL_LRED
#define TERM_COL_INFO     COL_LCYAN
#define TERM_COL_SUCCESS  COL_LGREEN
#define TERM_COL_CMD      COL_TEXT_PRIMARY

/* ===== Structures ===== */
typedef struct {
    char          text[TERM_BUF_COLS];
    unsigned char color[TERM_BUF_COLS];
    int           len;
} term_line_t;

typedef struct {
    term_line_t lines[TERM_BUF_ROWS];
    int         total_lines;
    int         scroll_offset;
    int         cur_col;
    char        input[TERM_INPUT_MAX + 1];
    int         input_len;
    int         input_cursor;
    char        history[TERM_HISTORY][TERM_HIST_LEN];
    int         history_count;
    int         history_pos;
    int         cursor_blink;
    int         redraw_needed;
} terminal_t;

typedef struct {
    int           x, y, w, h;
    int           drag_ox, drag_oy;
    /* Stage 3: saved geometry for restore-from-maximize */
    int           saved_x, saved_y, saved_w, saved_h;
    unsigned char flags;
    char          title[32];
} gui_window_t;

typedef struct {
    int  app_id;
    int  x, y;
    int  win_id;
    char label[16];
} dock_item_t;

/* ===== Desktop icons (Stage 3) ===== */
/* DAY57 Material shape helpers (window1.c) */
void gui_round_rect(int x, int y, int w, int h, int r, unsigned char c);
void gui_round_rect_ex(int x, int y, int w, int h, int r, unsigned char c, int alpha, int top, int bot);
void gui_fill_disc(int cx, int cy, int r, unsigned char c);

#define DESK_ICON_MAX  8
#define DESK_ACTION_NEWTERM (-2)   /* PRE-4: desk_icon_t.win_id of the New Term icon */
/* === MODIFIED SECTION START ===
 * STAGE4: desktop icons were declared/toggleable in Settings but never
 * actually drawn — DESK_ICON_SZ/DESK_ICON_GAP added so gui_init() can
 * lay out a row of icons and gui_draw_desktop_icon() can render them
 * consistently with the dock icon style. */
#define DESK_ICON_SZ   40      /* desktop icon square size */
#define DESK_ICON_GAP  16      /* vertical gap between stacked icons */
/* === MODIFIED SECTION END === */
typedef struct {
    int  x, y;          /* position on desktop */
    int  win_id;        /* window to open (-1 = none, DESK_ACTION_NEWTERM = new terminal) */
    char label[16];
    unsigned char color; /* PRE-4: icon kind 0..3 (terminal/info/settings/new term) */
    int  visible;
} desk_icon_t;

/* ===== Globals ===== */
extern gui_window_t windows[MAX_WINDOWS];
extern terminal_t   g_terminal;
extern int          active_window;
extern dock_item_t  dock_items[DOCK_APPS];
extern desk_icon_t  desk_icons[DESK_ICON_MAX];
extern int          desk_icon_count;

/* Stage 3: global theme flag. 1 = light (default), 0 = dark. */
extern int g_theme_light;

/* Stage 3: Settings toggle state (owned by window2.c, read by window1.c
 * only indirectly through gui_window2_settings_draw). Exposed here so
 * other modules could query it later if needed. */
extern int g_show_desktop_icons;
extern int g_opacity_sim;

/* ===== GUI Core ===== */
void gui_init(void);
void gui_refresh(void);

/* DAY43: GUI mode flag + modal window API (see window1.c).
 * PRE-4: the modal is a real window (title bar, [_][ ][x], drag, maximize,
 * minimize to a tab at the bottom-left). gui_modal_open() shows it centred;
 * `draw(cx,cy,cw,ch)` paints the CLIENT area (the frame and background are
 * drawn by the GUI). The owner is blocked in its own loop and calls
 * gui_modal_pump() while it waits for a key. */
extern int g_gui_mode;                       /* 1 once gui_init() has run */
void gui_modal_open(const char *title, int w, int h,
                    void (*draw)(int cx, int cy, int cw, int ch));
void gui_modal_close(void);
int  gui_modal_active(void);
int  gui_modal_minimized(void);              /* 1 while shrunk to its tab */
int  gui_modal_take_close(void);             /* 1 once if the user pressed [x] */
void gui_modal_pump(void);                   /* repaint one frame + mouse, no key dispatch */
void gui_set_palette(void);          /* applies palette for current g_theme_light */
void gui_apply_theme(int light);     /* sets g_theme_light and re-applies palette  */
void gui_draw_desktop(void);
/* === MODIFIED SECTION START ===
 * STAGE4: renders one desktop icon (background square, border, label).
 * Called from gui_draw_desktop() for each entry in desk_icons[]. */
void gui_draw_desktop_icon(const desk_icon_t *icon);
/* === MODIFIED SECTION END === */
void gui_draw_menubar(void);
void gui_draw_dock(void);
void gui_draw_cursor(int x, int y);

/* ===== Window ===== */
void gui_window_init(int id, int x, int y, int w, int h,
                     const char *title, unsigned char flags);
void gui_window_draw(int id);
void gui_window_focus(int id);

/* DAY43 PRE-3 ---------------------------------------------------------
 * gui_window_new_terminal(): open one more Terminal window in a free
 *   dynamic slot. Returns its window id, or -1 when all MAX_WINDOWS
 *   slots are in use (or the GUI is not running).
 * gui_terminal_of(id): the terminal_t shown by window `id` (g_terminal
 *   for WIN_TERMINAL, a private one for an extra terminal), NULL for
 *   Info/Settings/unused slots.
 * gui_window_count(): number of live windows that are VISIBLE.
 * gui_focus_topmost(): give focus to the front-most visible,
 *   non-minimized window (used after a close/minimize).
 * gui_fill_body(id,...): vga_draw_rect() for a window's background; when
 *   "Window Opacity Sim" is ON and window `id` is not the active one,
 *   only every other pixel is written (checkerboard) so whatever is
 *   behind the window shows through — "screen-door" transparency, the
 *   only kind an 8bpp indexed framebuffer can do without a blend table. */
int         gui_window_new_terminal(void);
terminal_t *gui_terminal_of(int id);
int         gui_window_count(void);
void        gui_focus_topmost(void);
void        gui_fill_body(int id, int x, int y, int w, int h, unsigned char color);

/* ===== Terminal ===== */
void terminal_init(terminal_t *t);
void terminal_putchar(terminal_t *t, char c, unsigned char color);
void terminal_puts(terminal_t *t, const char *s, unsigned char color);
void terminal_newline(terminal_t *t);
void terminal_clear(terminal_t *t);
void terminal_render(terminal_t *t, int wx, int wy, int ww, int wh);
void terminal_handle_key(terminal_t *t, unsigned char key);
void terminal_run_command(terminal_t *t, const char *cmd);
void terminal_scroll(terminal_t *t, int delta_lines);   /* Stage 3: wheel scroll */

/* ===== Events ===== */
void gui_handle_key(unsigned char key);
void gui_handle_mouse(int x, int y, int left, int right);
void gui_handle_wheel(int x, int y, int wheel_delta);   /* Stage 3: scroll wheel */
void gui_update_mouse(int mx, int my);

/* ===== Clipping helper (Stage 3) ===== */
/* Returns 1 if (x,y,w,h) after clipping to (cx,cy,cw,ch) is non-empty */
int gui_clip(int *x, int *y, int *w, int *h,
             int cx, int cy, int cw, int ch);

#endif /* LUMA_GUI_H */