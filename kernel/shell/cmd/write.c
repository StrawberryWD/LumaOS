/* kernel/shell/cmd/write.c
 * LumaOS 5.0 - Shell command: write
 *
 * DAY43 PRE-4 (GUI): the editor / overwrite dialog is now a real window.
 *   Before, write_gui_draw() painted its own fixed rectangle on top of the
 *   desktop and the loop only watched the keyboard, so the panel could not
 *   be moved. Now gui_modal_open() creates a modal window (window1.c) with
 *   the usual title bar: drag it, maximize/restore it, minimize it to a tab
 *   at the bottom-left of the desktop (click the tab to bring it back) or
 *   close it with [x] (= Esc). write_getkey() keeps calling gui_modal_pump(),
 *   which now also feeds the mouse to the modal, and DROPS typed keys while
 *   the window is minimized so nothing is edited blind. The draw callback
 *   only paints the client area; the frame/background come from the GUI.
 *
 * DAY43 (Week 7, Day 43 - PRE tasks + switching to GUI mode):
 *   PRE① "让 write 命令兼容 LMFS"  PRE② "GUI 模式下 write new.txt 死机"
 *
 *   PRE① - LMFS: through Day 42 this file called fat_open()/fat_create_
 *   file()/fat_write() unconditionally. Those three refuse any volume
 *   whose fs->name is not "fat12"/"fat16"/"fat32", so on the default
 *   LMFS volume (IDE3) `write new.txt` ended in "cannot create ...
 *   (invalid arguments)" - the same trap `create` hit on Day 38. The fix
 *   is the same dispatch create.c/read.c/rm.c already use: split the
 *   resolved path with shell_split_path() (Day 42), look at fs->name,
 *   and call the LMFS trio lmfs_vfs_open_path()/lmfs_vfs_create()/
 *   lmfs_vfs_write() instead. Because the path comes from shell_resolve_
 *   path() it is multi-level ("docs/notes/a.txt" -> "IDE3/docs/notes/
 *   a.txt"); the three wrappers walk the directories themselves.
 *   Three tiny static helpers (write_fs_open/create/save/delete) hide
 *   the FAT-vs-LMFS split so the editor below is untouched by it.
 *
 *   PRE② - GUI freeze. Why it froze: (1) the editor painted through
 *   console_* into VGA TEXT memory (0xB8000). Once vga_init_graphics()
 *   switched the card to the BGA 800x450 framebuffer that memory is no
 *   longer displayed, so nothing appeared; (2) the editor then blocked
 *   inside keyboard_getchar(), and shell_exec() is itself running inside
 *   gui_refresh() -> gui_handle_key() -> terminal_handle_key(); while
 *   we sit in that blocking read nothing repaints the framebuffer, the
 *   mouse cursor stops, and the Y/N prompt (queued in terminal_t, only
 *   drawn after shell_exec returns) is never shown either. To the user
 *   that is a dead machine.
 *   The fix keeps the "editor owns the loop" structure but makes it GUI-
 *   aware: when g_gui_mode is set, rendering goes to a modal overlay
 *   (write_gui_draw(), installed with gui_modal_set_draw()) and key
 *   waiting goes through write_getkey(), which polls keyboard_read()
 *   and calls gui_modal_pump() (one full frame: desktop, windows,
 *   overlay, mouse cursor, vga_flip) every idle iteration. Text mode
 *   (g_gui_mode == 0) takes exactly the old console_* / keyboard_getchar
 *   path, so every Day 25-42 text-mode test still sees the old screen.
 *   Also new: Esc cancels the editor without saving (and removes a file
 *   that this very `write` call had just created).
 *
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "实现 fat_write(file, buf, size)：处理簇链扩展（如果当前簇链不够，
 *    追加新簇）。在 Shell 中输入 write test.txt，写入内容，show 能看到。"
 *
 * This is the Shell-visible exerciser of Day 25's real deliverable,
 * fat_write() (kernel/fs/fat/fat_vfs.c) — the same relationship
 * cmd_create.c (Day 24) has to fat_create_file(). Unlike every other
 * cmd_*.c in this project, `write` cannot just compute its output and
 * hand it to terminal_puts() for shell_exec()'s caller to flush
 * afterwards (text_shell_flush_new_lines(), kernel/core/kmain.c, only
 * runs once shell_exec() has already RETURNED) — an interactive
 * multi-line text editor needs the user to see each keystroke as it
 * happens, so this file talks to the real screen directly through
 * console_getc-style primitives (console_putc_color()/console_set_
 * cursor()/console_get_x()/console_get_y(), kernel/core/console.c) and
 * reads the keyboard itself via keyboard_getchar() (blocking), the
 * exact same pair of primitives kmain.c's own text_shell_run() already
 * uses for its single-line command prompt (see that function's own
 * comment for why this is safe: today's kmain() runs a pure text-mode
 * loop with no GUI event loop competing for the same keyboard queue —
 * see this file's own DAY25 design-decision note below for the fuller
 * version of that argument).
 *
 * WORKFLOW (per this project owner's own worked examples):
 *
 *   write test.txt                      (file doesn't exist yet)
 *     -> creates it (fat_create_file()) exactly like typing `create`
 *        first, then drops straight into the editor.
 *
 *   write test.txt                      (file already exists, non-empty)
 *     ! TEST.TXT ALREADY EXISTS
 *     ! DO YOU WANT TO COVER THE FILE?
 *      - Y/N -                          (Y -> editor; N -> cancelled)
 *
 *   write test.txt                      (file already exists, 0 bytes)
 *     -> DAY27: no prompt — an empty file has nothing to lose, so this
 *        goes straight into the editor exactly like the "doesn't exist
 *        yet" case above (see the `existing.size != 0` guard around the
 *        confirmation block below).
 *
 *   Editor: multi-line, Enter inserts a new numbered line ("1.", "2.",
 *   ...), arrow keys move the cursor (Up/Down between lines, Left/
 *   Right within a line), Backspace deletes a character or, at column
 *   0, merges the current line into the previous one (DAY27: a merge
 *   that wouldn't fully fit no longer drops characters — see the
 *   Backspace handler's own DAY27 comment below). DAY28 ("并提供delete
 *   键支持" — provide Delete-key support): the real Delete key
 *   (keyboard.c decodes it as KEY_DEL) does the mirror-image operation
 *   — deletes the character AT the cursor (not before it) or, at the
 *   END of a line, merges the NEXT line into the current one. That
 *   forward-merge reuses the exact same overflow-safe "pull across
 *   only what fits, keep the remainder rather than dropping it" logic
 *   the Backspace handler's own DAY27 fix established — see the
 *   Delete handler's own comment below for the full mirrored contract.
 *   Ctrl+S ends the
 *   editor and persists the WHOLE buffer via fat_write() — see that
 *   function's own doc comment (fat_vfs.h) for why this is a full
 *   overwrite-from-byte-0, not an append; as of Day 27, fat_write()
 *   also frees the file's OLD cluster chain before writing the new one
 *   (same doc comment's "DAY27 UPDATE" paragraph), so repeated `write`s
 *   to the same file no longer leak clusters the way they did through
 *   Day 26.
 *
 * WHY DIRECT console_* CALLS, NOT terminal_puts() (design decision,
 * expanded): terminal_puts() only appends to a terminal_t's in-memory
 * line buffer (kernel/gui/gui.h's own struct); nothing renders it to
 * the actual screen until the CALLER of shell_exec() flushes it —
 * text_shell_flush_new_lines() in text-mode, or window1.c's redraw
 * path in GUI mode — and both of those only run AFTER shell_exec()
 * returns. An editor that needs to redraw on every keystroke while
 * still INSIDE this one shell_exec() call (it hasn't returned yet —
 * the whole editing session is one call to cmd_write()) therefore
 * cannot go through `t` at all for its live rendering; it must own the
 * real screen directly, the same way kmain.c's text_shell_run() already
 * does for its own single-line input editor. `t` (the terminal_t this
 * function still receives, same signature as every other shell_cmd_fn_t)
 * is used only for the handful of lines printed BEFORE and AFTER the
 * interactive session (the usage error, the overwrite prompt, the
 * final "wrote N bytes" summary) — exactly where the existing
 * terminal_puts()-then-flush-after-return contract still applies
 * normally, same as `create`/`read`.
 *
 * SCOPE LIMITS (deliberate, matching this file's own worked examples
 * and this project's current single-line-per-directory-entry FAT
 * layer): line count capped at WRITE_MAX_LINES (20) and each line
 * capped at WRITE_MAX_LINE_LEN (DAY27: 76, up from 72 — see that
 * constant's own definition below for why) visible characters —
 * generous headroom for every acceptance scenario (a handful of short
 * lines), chosen mainly to guarantee the editor's own on-screen redraw
 * never needs to scroll the real VGA console mid-session (see the "why
 * a fixed budget, not the full 25-row screen" note by WRITE_MAX_LINES'
 * own definition below for the full reasoning) — this project has no
 * text-wrapping/pagination concept anywhere yet, so keeping the
 * editor's own footprint comfortably inside one screen is simpler than
 * teaching it to scroll.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../drivers/keyboard.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../../fs/lmfs/lmfs_vfs.h"   /* DAY43: LMFS open/create/write/delete */
#include "../../lib/string.h"
#include "../shell.h"

/* WRITE_MAX_LINES / WRITE_MAX_LINE_LEN — see this file's header
 * comment's "SCOPE LIMITS" paragraph. WRITE_MAX_LINES=20 is chosen so
 * the editor's own UI (a 2-line header showing the filename plus a
 * blank separator) plus 20 numbered content lines plus a short footer
 * hint line comfortably fits inside VGA_ROWS=25 (kernel/core/
 * console.c) without this function ever needing to call
 * console.c's own scroll() — this function redraws by walking
 * `start_y` forward a fixed number of rows every time, and a redraw
 * that overran the 25-row screen would silently scroll the console
 * out from under that fixed math, desyncing every row this function
 * thinks it owns.
 *
 * DAY27 ("添加一行字数如果超出自动换行的逻辑，或者增加一行能显示的最多
 * 字符" — add auto-wrap for an overlong line, OR raise the max
 * characters a line can show): this project picked the second, simpler
 * option — real auto-wrap would mean a typed line's content no longer
 * maps 1:1 to one entry of lines[]/line_lens[] (Enter vs. a wrap point
 * would need to be told apart everywhere: saving, Up/Down navigation,
 * Backspace-at-column-0 merging — see this file's DAY27 backspace fix
 * below for how much care even that ONE case already needs), a much
 * larger redesign this project's own "simpler and provably correct
 * over clever and fragile" preference (see fat_write()'s DAY25 doc
 * comment, fat_vfs.h) argues against for one day's work. Instead,
 * WRITE_MAX_LINE_LEN is raised from 72 to the actual maximum content
 * width VGA_COLS=80 allows: 80 minus the widest possible prefix, a
 * 2-digit line number's "NN. " (4 characters, NN up to 20) — leaving
 * ZERO wasted margin rather than 72's previous 4-column cushion, since
 * write_redraw()'s per-line blanking loop below already clears exactly
 * (prefix + line_lens[i]) through column 79 every redraw regardless of
 * content length, so no stale-character risk comes from using the full
 * width. */
#define WRITE_MAX_LINES     20
#define WRITE_MAX_LINE_LEN  76

/* WRITE_SCREEN_ROWS — DAY26 bugfix support constant. Must match
 * kernel/core/console.c's own (private-to-that-file) VGA_ROWS=25;
 * duplicated here rather than exported from console.c because
 * nothing before today ever needed console.c's row count outside
 * console.c itself — see this file's DAY26 note above where
 * start_y is computed, in cmd_write(), for why this function now
 * needs to know it. */
#define WRITE_SCREEN_ROWS   25

/* ---------------------------------------------------------------------
 * DAY43 PRE①: FAT-vs-LMFS dispatch helpers.
 * Return conventions are normalised to what the editor needs:
 *   write_fs_open()   0 = exists (node filled), 1 = not found, -1 = other error
 *   write_fs_create() 0 = ok
 *   write_fs_save()   bytes written (>=0) or <0 on failure
 *   write_fs_delete() 0 = ok
 * `fs` is the volume of the resolved path (from shell_split_path()).
 * ------------------------------------------------------------------- */
static int write_is_lmfs(filesystem_t *fs)
{
    return fs && fs->name && strcmp(fs->name, "lmfs") == 0;
}

static int write_fs_open(filesystem_t *fs, const char *path, vfs_node_t *node, int *fat_err)
{
    *fat_err = FAT_OK;
    if (write_is_lmfs(fs))
        return lmfs_vfs_open_path(path, node) == 0 ? 0 : 1;
    int e = fat_open(path, node);
    if (e == FAT_OK) return 0;
    *fat_err = e;
    return (e == FAT_ERR_NOT_FOUND) ? 1 : -1;
}

static int write_fs_create(filesystem_t *fs, const char *path, int *fat_err)
{
    *fat_err = FAT_OK;
    if (write_is_lmfs(fs))
        return lmfs_vfs_create(path) == 0 ? 0 : -1;
    int e = fat_create_file(path);
    *fat_err = e;
    return e == FAT_OK ? 0 : -1;
}

static int write_fs_save(filesystem_t *fs, const char *path, const void *buf,
                         uint32_t n, int *fat_err)
{
    *fat_err = FAT_OK;
    if (write_is_lmfs(fs))
        return lmfs_vfs_write(path, buf, n) == 0 ? (int)n : -1;
    int w = fat_write(path, buf, n);
    if (w < 0) *fat_err = w;
    return w;
}

static void write_fs_delete(filesystem_t *fs, const char *path)
{
    if (write_is_lmfs(fs)) (void)lmfs_vfs_delete(path);
    else                   (void)fat_delete_file(path);
}

/* ---------------------------------------------------------------------
 * DAY43 PRE②: GUI-aware keyboard + overlay.
 * ------------------------------------------------------------------- */

/* write_getkey(): one key. Text mode: the old blocking read. GUI mode:
 * poll keyboard_read() and keep the GUI alive (repaint + mouse cursor)
 * while waiting - this is what removes the "frozen machine". */
static int write_getkey(void)
{
    if (!g_gui_mode)
        return (unsigned char)keyboard_getchar();
    for (;;) {
        unsigned char k = keyboard_read();
        if (k) {
            if (gui_modal_minimized()) continue;      /* PRE-4: ignore keys while minimized */
            return k;
        }
        if (gui_modal_take_close()) return KEY_ESC;   /* PRE-4: [x] on the window == Esc */
        gui_modal_pump();
        __asm__ volatile("pause");
    }
}

/* State the overlay draws from (set by write_render()/write_gui_confirm). */
static const char *wg_name;
static char      (*wg_lines)[WRITE_MAX_LINE_LEN + 1];
static const int  *wg_lens;
static int         wg_n, wg_row, wg_col;
static int         wg_confirm;      /* 1 = show the overwrite Y/N dialog */
static int         wg_blink;

/* Window sizes (PRE-4). Client area = window minus title bar and border. */
#define WG_W   680
#define WG_H   (TITLEBAR_H + 4 + WRITE_MAX_LINES * 9 + 6 + 14 + 2)
#define WG_CW  420
#define WG_CH  (TITLEBAR_H + 4 + 76)

/* Client-area painter for the editor and the Y/N dialog. (cx,cy,cw,ch) is
 * the client rectangle in screen coordinates, so it follows the window when
 * it is dragged or maximized. */
static void write_gui_draw(int cx, int cy, int cw, int ch)
{
    (void)cw;
    if (wg_confirm) {
        vga_draw_string(cx + 16, cy + 10, "! FILE ALREADY EXISTS", COL_LRED);
        vga_draw_string(cx + 16, cy + 24, "! DO YOU WANT TO COVER THE FILE?", COL_LRED);
        vga_draw_string(cx + 16, cy + 46, " - Y/N -", COL_TEXT_PRIMARY);
        return;
    }

    for (int i = 0; i < WRITE_MAX_LINES; i++) {
        int y = cy + 3 + i * 9;
        if (i < wg_n) {
            char pre[8];
            sprintf(pre, "%d.", i + 1);
            vga_draw_string(cx + 4, y, pre, COL_TEXT_SECONDARY);
            for (int j = 0; j < wg_lens[i]; j++)
                vga_draw_char(cx + 32 + j * 8, y, wg_lines[i][j], COL_TEXT_PRIMARY);
        }
    }
    wg_blink = (wg_blink + 1) % 30;
    if (wg_blink < 20)
        vga_draw_rect(cx + 32 + wg_col * 8, cy + 3 + wg_row * 9, 2, 8, COL_LRED);

    vga_draw_hline(cx, cy + ch - 14, cw, COL_WIN_BORDER);
    vga_draw_string(cx + 4, cy + ch - 11,
                    "Ctrl+S save   Esc cancel   arrows/Backspace/Del edit",
                    COL_TEXT_SECONDARY);
}

/* write_read_y_n(t)
 *
 * Blocking single-keypress Y/N prompt for the "file already exists"
 * confirmation. Deliberately NOT part of the line-editor's own
 * keystroke loop below (it happens BEFORE the editor starts, and only
 * needs one keypress, not a redrawing multi-line UI) — kept as its own
 * tiny helper so cmd_write() itself reads as "ask, maybe bail, then
 * edit" rather than interleaving this one-shot decision into the
 * editor's own state machine. Returns 1 for Y/y, 0 for N/n/anything
 * else (matches this project's own worked example, which only shows
 * Y and N as the two documented answers — an unrecognized keypress is
 * treated as the safer of the two, "don't overwrite", rather than
 * silently proceeding to destroy the file's old content). */
static int write_read_y_n(void)
{
    for (;;) {
        char c = (char)write_getkey();   /* DAY43: GUI-aware */
        if (c == 'Y' || c == 'y')
            return 1;
        if (c == 'N' || c == 'n' || c == (char)KEY_ESC)   /* PRE-4: Esc / [x] = No */
            return 0;
        /* Anything else (including Enter) — keep waiting rather than
         * silently defaulting, so a stray keypress can't accidentally
         * pick either answer for the user. */
    }
}

/* write_prefix_len(line_index)
 *
 * DAY27 CURSOR BUGFIX ("光标绘制在将输入文字的正下方，而不是跳一个" —
 * draw the cursor directly at the input text's own position, not one
 * column further along): every column computation below through Day 26
 * hardcoded the "NN. " line-number prefix as exactly 4 characters wide
 * — true for two-digit line numbers (10.-20.) but WRONG for the single
 * digit line numbers 1.-9. sprintf(prefix, "%d. ", i+1) actually prints
 * just 3 characters for those ("1. ", "2. ", ... "9. "), one column
 * NARROWER than the "+4"/"4 +" math elsewhere in this file assumed.
 * Confirmed live: after opening the editor and typing "AB" on line 1
 * (screen shows "1. AB", so the next character belongs at column 5),
 * the hardware VGA cursor (read back via the CRTC cursor-location
 * registers, 0x3D4 index 0x0E/0x0F) was actually sitting at column 6 —
 * one column PAST the real insertion point, for every line 1 through 9
 * (the overwhelming majority of any real editing session, since
 * WRITE_MAX_LINES caps out at 20). The visible characters on screen
 * were always drawn correctly (console_write_color(prefix, ...) prints
 * the real, variable-length string) — only the blinking cursor's own
 * position, computed separately from a hardcoded constant instead of
 * the prefix's actual length, was wrong.
 *
 * Returns the on-screen width of the "N. "/"NN. " prefix write_redraw()
 * prints before line `line_index`'s own content: 3 for line numbers
 * 1-9 (line_index 0-8), 4 for 10-20 (line_index 9-19) — exactly
 * strlen() of whatever sprintf(prefix, "%d. ", line_index + 1) below
 * actually produces, computed without needing a second sprintf() call
 * at every use site. */
static int write_prefix_len(int line_index)
{
    return (line_index + 1 >= 10) ? 4 : 3;
}

/* write_redraw(filename, lines, line_lens, num_lines, cur_row, cur_col,
 *               start_y)
 *
 * Redraws the WHOLE editor from `start_y` down, then repositions the
 * hardware cursor to (cur_col + 4, start_y + 2 + cur_row) — the "+4"
 * accounts for the "NN. " line-number prefix (see WRITE_MAX_LINE_LEN's
 * own comment above), the "+2" for this function's own 2-line header
 * (filename + a blank separator line) before line "1." begins.
 *
 * A full redraw on every keystroke (rather than an incremental patch
 * of just the changed cell) is the same "simpler and provably correct
 * over clever and fragile" trade-off this project has favored
 * elsewhere (e.g. fat_write()'s own "always allocate a fresh cluster
 * rather than reuse an old chain's tail" simplification, fat_vfs.h) —
 * WRITE_MAX_LINES/WRITE_MAX_LINE_LEN's own small fixed bound keeps a
 * full redraw cheap enough (at most 20 lines * 72 columns) that the
 * flicker/cost of redrawing unchanged lines is not a real concern at
 * this project's current scale. */
static void write_redraw(const char *filename,
                          char lines[][WRITE_MAX_LINE_LEN + 1],
                          const int *line_lens, int num_lines,
                          int cur_row, int cur_col, int start_y)
{
    int y = start_y;

    console_set_cursor(0, y);
    console_write_color("-- editing ", TERM_COL_INFO);
    console_write_color(filename, TERM_COL_INFO);
    console_write_color(" (Ctrl+S to save) --                                  ",
                         TERM_COL_INFO);
    y++;
    console_set_cursor(0, y);
    console_write_color("                                                                                ",
                         TERM_COL_DEFAULT);
    y++;

    for (int i = 0; i < WRITE_MAX_LINES; i++) {
        console_set_cursor(0, y + i);
        if (i < num_lines) {
            char prefix[8];
            sprintf(prefix, "%d. ", i + 1);
            console_write_color(prefix, TERM_COL_PROMPT);
            for (int j = 0; j < line_lens[i]; j++)
                console_putc_color(lines[i][j], COLOR_DEFAULT);
        }
        /* Blank out the rest of this row so a shortened line (e.g.
         * after Backspace deleted trailing characters, or a merged-
         * away line further down shifting everything up) doesn't leave
         * stale characters from the previous redraw sitting past the
         * new, shorter content.
         *
         * DAY27: use write_prefix_len(i), not a hardcoded 4 — see that
         * helper's own header comment for why 4 was wrong for the
         * 9-out-of-20 lines whose number is a single digit. */
        int printed = (i < num_lines) ? (write_prefix_len(i) + line_lens[i]) : 0;
        for (int col = printed; col < 80; col++)
            console_putc_color(' ', COLOR_DEFAULT);
    }

    /* DAY27: write_prefix_len(cur_row), not a hardcoded 4 — this is the
     * exact fix for the "cursor lands one column past the real
     * insertion point" bug this helper's own header comment documents
     * reproducing live under QEMU (hardware cursor read back via the
     * VGA CRTC registers). */
    console_set_cursor(write_prefix_len(cur_row) + cur_col, start_y + 2 + cur_row);
}

/* write_render(): one redraw of the editor, text or GUI (DAY43). */
static void write_render(const char *filename,
                         char lines[][WRITE_MAX_LINE_LEN + 1],
                         const int *line_lens, int num_lines,
                         int cur_row, int cur_col, int start_y)
{
    if (g_gui_mode) {
        wg_name = filename; wg_lines = lines; wg_lens = line_lens;
        wg_n = num_lines; wg_row = cur_row; wg_col = cur_col;
        wg_confirm = 0;
        gui_modal_pump();
    } else {
        write_redraw(filename, lines, line_lens, num_lines, cur_row, cur_col, start_y);
    }
}

void cmd_write(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: write <filename>  (e.g. write test.txt, "
                          "or write IDE0/TEST.TXT)\n", TERM_COL_ERROR);
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "write: no mount-slot given and no "
                          "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }
    const char *path = path_buf;

    /* DAY43 PRE①: which filesystem owns this path? (see header comment) */
    filesystem_t *fs = (filesystem_t *)0;
    const char   *sub = "";
    if (shell_split_path(path, &fs, &sub) != 0 || !fs) {
        terminal_puts(t, "write: no such mounted volume in '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "'\n", TERM_COL_ERROR);
        return;
    }
    if (sub[0] == '\0') {
        terminal_puts(t, "write: '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' is a directory\n", TERM_COL_ERROR);
        return;
    }

    int created_here = 0;   /* DAY43: did THIS call create the file? (Esc undo) */
    int ferr = FAT_OK;
    vfs_node_t existing;
    int open_rc = write_fs_open(fs, path, &existing, &ferr);

    if (open_rc == 0) {
        if (existing.type != VFS_TYPE_FILE) {
            terminal_puts(t, "write: '", TERM_COL_ERROR);
            terminal_puts(t, path, TERM_COL_ERROR);
            terminal_puts(t, "' is a directory\n", TERM_COL_ERROR);
            return;
        }

        /* DAY27: an empty file has nothing to lose - no Y/N prompt. */
        if (existing.size != 0) {
            /* Uppercased bare filename for the prompt (DAY25 worked example). */
            const char *slash = strrchr(path, '/');
            const char *fname = slash ? slash + 1 : path;
            char upname[SHELL_ARG_LEN];
            uint32_t k = 0;
            while (fname[k] != '\0' && k < sizeof(upname) - 1) {
                char c = fname[k];
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                upname[k] = c;
                k++;
            }
            upname[k] = '\0';

            terminal_puts(t, "! ", TERM_COL_ERROR);
            terminal_puts(t, upname, TERM_COL_ERROR);
            terminal_puts(t, " ALREADY EXISTS\n", TERM_COL_ERROR);
            terminal_puts(t, "! DO YOU WANT TO COVER THE FILE?\n", TERM_COL_ERROR);
            terminal_puts(t, " - Y/N - ", TERM_COL_PROMPT);

            int yes;
            if (g_gui_mode) {
                /* DAY43 PRE②: the terminal_t above is not drawn until we
                 * return, so ask in a modal dialog the GUI loop paints. */
                wg_name = path; wg_confirm = 1;
                char ctitle[80] = "write: ";
                { int n = 7; const char *f = path; while (*f && n < 70) ctitle[n++] = *f++; ctitle[n] = 0; }
                gui_modal_open(ctitle, WG_CW, WG_CH, write_gui_draw);
                yes = write_read_y_n();
                gui_modal_close();
                terminal_putchar(t, yes ? 'Y' : 'N', TERM_COL_PROMPT);
                terminal_newline(t);
            } else {
                /* Text mode: also print through console_* so the prompt is
                 * visible while we block (DAY25 note; re-printed harmlessly
                 * by the flush after shell_exec returns). */
                console_write_color("! ", COLOR_ERROR);
                console_write_color(upname, COLOR_ERROR);
                console_write_color(" ALREADY EXISTS\n", COLOR_ERROR);
                console_write_color("! DO YOU WANT TO COVER THE FILE?\n", COLOR_ERROR);
                console_write_color(" - Y/N - ", COLOR_PROMPT);
                yes = write_read_y_n();
                console_putc('\n');
            }

            if (!yes) {
                terminal_puts(t, "Cancelled.\n", TERM_COL_INFO);
                if (!g_gui_mode) console_write_color("Cancelled.\n", COLOR_INFO);
                return;
            }
        }
    } else if (open_rc == 1) {
        /* Doesn't exist yet - create it first (DAY25), FAT or LMFS. */
        if (write_fs_create(fs, path, &ferr) != 0) {
            terminal_puts(t, "write: cannot create '", TERM_COL_ERROR);
            terminal_puts(t, path, TERM_COL_ERROR);
            if (write_is_lmfs(fs))
                terminal_puts(t, "' (parent directory missing, bad/duplicate name, "
                                  "or directory full)\n", TERM_COL_ERROR);
            else {
                terminal_puts(t, "' (", TERM_COL_ERROR);
                terminal_puts(t, fat_strerror(ferr), TERM_COL_ERROR);
                terminal_puts(t, ")\n", TERM_COL_ERROR);
            }
            return;
        }
        created_here = 1;
    } else {
        terminal_puts(t, "write: cannot open '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' (", TERM_COL_ERROR);
        terminal_puts(t, fat_strerror(ferr), TERM_COL_ERROR);
        terminal_puts(t, ")\n", TERM_COL_ERROR);
        return;
    }

    /* ---- Interactive multi-line editor ---- */
    static char lines[WRITE_MAX_LINES][WRITE_MAX_LINE_LEN + 1];
    static int  line_lens[WRITE_MAX_LINES];
    int num_lines = 1;
    int cur_row = 0, cur_col = 0;

    for (int i = 0; i < WRITE_MAX_LINES; i++)
        line_lens[i] = 0;

    /* Record where the editor begins on the real screen — every
     * redraw is relative to this fixed row, per write_redraw()'s own
     * comment on why this function never lets the console scroll out
     * from under that assumption.
     *
     * DAY26 BUGFIX ("字符输入出现错误，只输入不显示，或显示多个"):
     * the original code below just took whatever row console_get_y()
     * happened to report at this point and trusted write_redraw()'s
     * "comfortably fits inside VGA_ROWS=25" assumption (this file's
     * own WRITE_SCREEN_ROWS comment near the top) without ever
     * actually CHECKING it. write_redraw() always draws 2 header rows
     * + WRITE_MAX_LINES (20) content rows = 22 rows starting at
     * start_y — the moment start_y exceeds 3 (almost guaranteed: a
     * shell prompt, and for the "overwrite?" path also the Y/N
     * question printed just above, already sit several rows down the
     * screen before `write`'s editor even starts), some of those 22
     * rows fall past WRITE_SCREEN_ROWS-1. console_set_cursor()
     * (kernel/core/console.c) silently IGNORES an out-of-range row
     * instead of clamping/scrolling — `if (y >= 0 && y < VGA_ROWS)
     * cursor_y = y;`, else cursor_y is left wherever it last validly
     * was — so every console_putc_color() call meant for an off-
     * screen row instead lands on top of whatever row the cursor was
     * last validly moved to, blending several lines' worth of
     * characters together. The keystrokes themselves are still
     * recorded correctly in lines[]/line_lens[] (fat_write() below
     * saves the real, uncorrupted buffer) — only the ON-SCREEN redraw
     * silently misplaces itself, exactly matching this day's roadmap
     * description. The "覆盖" (overwrite-confirmed) path hits this
     * harder than a fresh create, since write_read_y_n()'s own
     * multi-line prompt pushes the cursor down further first.
     *
     * Fix: actually enforce the assumption instead of just documenting
     * it — clear the screen and start at row 0 whenever the current
     * row wouldn't leave room for the full 22-row editor, rather than
     * trusting whatever row prior shell/prompt output happened to
     * leave the cursor on. */
    int start_y = 0;
    if (g_gui_mode) {
        char etitle[80] = "write - ";
        { int n = 8; const char *f = path; while (*f && n < 70) etitle[n++] = *f++; etitle[n] = 0; }
        wg_confirm = 0;
        gui_modal_open(etitle, WG_W, WG_H, write_gui_draw);   /* PRE-4: draggable window */
    } else {
    start_y = console_get_y();
    if (start_y + 2 + WRITE_MAX_LINES >= WRITE_SCREEN_ROWS) {
        console_clear();
        start_y = 0;
    } else if (start_y != 0) {
        console_putc('\n');   /* start the editor on its own fresh line */
        start_y = console_get_y();
    }
    }

    write_render(path, lines, line_lens, num_lines, cur_row, cur_col, start_y);

    int cancelled = 0;
    for (;;) {
        char c = (char)write_getkey();   /* DAY43: GUI-aware */

        if (c == (char)KEY_CTRL_S) {
            break;   /* save & exit */
        } else if (c == (char)KEY_ESC) {
            cancelled = 1;   /* DAY43: Esc = leave without saving */
            break;
        } else if (c == (char)KEY_UP) {
            if (cur_row > 0) {
                cur_row--;
                if (cur_col > line_lens[cur_row])
                    cur_col = line_lens[cur_row];
            }
        } else if (c == (char)KEY_DOWN) {
            if (cur_row < num_lines - 1) {
                cur_row++;
                if (cur_col > line_lens[cur_row])
                    cur_col = line_lens[cur_row];
            }
        } else if (c == (char)KEY_LEFT) {
            if (cur_col > 0)
                cur_col--;
        } else if (c == (char)KEY_RIGHT) {
            if (cur_col < line_lens[cur_row])
                cur_col++;
        } else if (c == '\r' || c == '\n' || c == (char)KEY_ENTER) {
            /* Insert a new line after cur_row, splitting the current
             * line's content at cur_col — same "everything after the
             * cursor moves to the new line" behavior any text editor's
             * Enter key has. Only if there's still room under
             * WRITE_MAX_LINES; silently ignored otherwise (this
             * project's own SCOPE LIMITS, see this file's header
             * comment — no error message for hitting the cap, matching
             * how terminal_t's own TERM_INPUT_MAX simply stops
             * accepting more characters rather than erroring, kernel/
             * gui/window1.c's terminal_handle_key()). */
            if (num_lines < WRITE_MAX_LINES) {
                for (int i = num_lines; i > cur_row + 1; i--) {
                    line_lens[i] = line_lens[i - 1];
                    memcpy(lines[i], lines[i - 1], (uint32_t)line_lens[i]);
                }
                int tail_len = line_lens[cur_row] - cur_col;
                if (tail_len < 0) tail_len = 0;
                memcpy(lines[cur_row + 1], &lines[cur_row][cur_col], (uint32_t)tail_len);
                line_lens[cur_row + 1] = tail_len;
                line_lens[cur_row] = cur_col;
                num_lines++;
                cur_row++;
                cur_col = 0;
            }
        } else if (c == '\b' || c == (char)KEY_BS) {
            if (cur_col > 0) {
                memmove(&lines[cur_row][cur_col - 1], &lines[cur_row][cur_col],
                         (uint32_t)(line_lens[cur_row] - cur_col));
                line_lens[cur_row]--;
                cur_col--;
            } else if (cur_row > 0) {
                /* Merge current line into the end of the previous one,
                 * then remove this line and shift every later line up
                 * — same "backspace at column 0 joins lines" behavior
                 * this file's own header comment documents.
                 *
                 * DAY27 BUGFIX ("解决文本编辑器跨行删除时的问题" —
                 * fix the text editor's cross-line-delete issue):
                 * through Day 26, `take` capped how much of the
                 * current line could fit into the previous line's
                 * remaining room, but the characters beyond `take`
                 * were simply never copied anywhere before this line
                 * got deleted and everything below shifted up over it
                 * — silent, permanent DATA LOSS whenever two lines'
                 * combined length exceeded WRITE_MAX_LINE_LEN (easy to
                 * hit: two full lines can total up to 2x
                 * WRITE_MAX_LINE_LEN). Confirmed live: a 70-char line 1
                 * followed by a 60-char line 2, fully backspaced
                 * together, saved as only 71 bytes — the last 59
                 * characters of line 2 vanished with no error or
                 * warning.
                 *
                 * FIX: only delete the current line (the full-merge
                 * path above) when its ENTIRE content fits in the
                 * previous line's remaining room. When it doesn't fit,
                 * pull across only the `room` characters that fit —
                 * filling the previous line to exactly
                 * WRITE_MAX_LINE_LEN — and shift the REMAINDER to the
                 * front of the current line instead of discarding it;
                 * the current line shrinks but stays right where it
                 * was (nothing below it moves), and the cursor lands
                 * at the join point (end of the now-full previous
                 * line). No character the user typed is ever dropped. */
                int prev_len = line_lens[cur_row - 1];
                int this_len = line_lens[cur_row];
                int room = WRITE_MAX_LINE_LEN - prev_len;

                if (this_len <= room) {
                    /* Fits entirely — full merge, same as before. */
                    memcpy(&lines[cur_row - 1][prev_len], lines[cur_row],
                           (uint32_t)this_len);
                    line_lens[cur_row - 1] = prev_len + this_len;

                    for (int i = cur_row; i < num_lines - 1; i++) {
                        line_lens[i] = line_lens[i + 1];
                        memcpy(lines[i], lines[i + 1], (uint32_t)line_lens[i]);
                    }
                    num_lines--;
                    cur_row--;
                    cur_col = prev_len;
                } else if (room > 0) {
                    /* Doesn't fit — partial merge, no data lost: take
                     * only what fits, keep the rest on this same line. */
                    memcpy(&lines[cur_row - 1][prev_len], lines[cur_row],
                           (uint32_t)room);
                    line_lens[cur_row - 1] = WRITE_MAX_LINE_LEN;

                    int remaining = this_len - room;
                    memmove(lines[cur_row], &lines[cur_row][room],
                            (uint32_t)remaining);
                    line_lens[cur_row] = remaining;

                    cur_row--;
                    cur_col = prev_len;
                }
                /* room == 0: previous line is already completely full —
                 * nothing can be pulled across yet, so this Backspace
                 * is a no-op (same "can't do it yet" shape as the
                 * WRITE_MAX_LINE_LEN check on ordinary typing below);
                 * the user can Backspace from inside the current line
                 * first to make room. */
            }
        } else if (c == (char)KEY_DEL) {
            /* DAY28 ("并提供delete键支持"): forward-delete — the mirror
             * image of the Backspace handler just above. Two cases:
             *
             *   cur_col < line_lens[cur_row]: an ordinary character
             *   sits under/after the cursor on THIS line — shift
             *   everything from cur_col+1 onward left by one. cur_col
             *   itself does NOT move (Delete removes the character
             *   the cursor is sitting in front of; unlike Backspace,
             *   it has no reason to step the cursor back).
             *
             *   cur_col == line_lens[cur_row] (cursor at the END of
             *   this line) and cur_row < num_lines - 1: nothing left
             *   on this line to delete, so — same as Backspace merging
             *   the CURRENT line into the PREVIOUS one at column 0 —
             *   this merges the NEXT line into the CURRENT one. Reuses
             *   the identical DAY27 overflow-safe recipe (see the
             *   Backspace handler's own DAY27 comment above for the
             *   full data-loss story this guards against): if the
             *   next line's entire content fits in this line's
             *   remaining room, pull it ALL across and delete the now-
             *   empty next line (shifting every later line up); if it
             *   doesn't fit, pull across only the `room` characters
             *   that DO fit — filling this line to exactly
             *   WRITE_MAX_LINE_LEN — and shift the remainder to the
             *   FRONT of the next line instead of discarding it. Either
             *   way cur_row/cur_col are left exactly as they were
             *   (the join point never moves for a forward-delete,
             *   whichever branch fires) — no character is ever lost,
             *   and this line's own trailing whitespace-blanking in
             *   write_redraw() picks up the shorter/longer result on
             *   the very next redraw below regardless of which case
             *   ran.
             *
             *   cur_col == line_lens[cur_row] and cur_row == num_lines
             *   - 1 (end of the LAST line): nothing after the cursor
             *   anywhere in the buffer — no-op, same "can't do it yet"
             *   shape the Backspace handler's own room==0 case uses. */
            if (cur_col < line_lens[cur_row]) {
                memmove(&lines[cur_row][cur_col], &lines[cur_row][cur_col + 1],
                         (uint32_t)(line_lens[cur_row] - cur_col - 1));
                line_lens[cur_row]--;
            } else if (cur_row < num_lines - 1) {
                int this_len = line_lens[cur_row];
                int next_len = line_lens[cur_row + 1];
                int room = WRITE_MAX_LINE_LEN - this_len;

                if (next_len <= room) {
                    /* Fits entirely — full merge. */
                    memcpy(&lines[cur_row][this_len], lines[cur_row + 1],
                           (uint32_t)next_len);
                    line_lens[cur_row] = this_len + next_len;

                    for (int i = cur_row + 1; i < num_lines - 1; i++) {
                        line_lens[i] = line_lens[i + 1];
                        memcpy(lines[i], lines[i + 1], (uint32_t)line_lens[i]);
                    }
                    num_lines--;
                } else if (room > 0) {
                    /* Doesn't fit — partial merge, no data lost: take
                     * only what fits, keep the rest on the next line. */
                    memcpy(&lines[cur_row][this_len], lines[cur_row + 1],
                           (uint32_t)room);
                    line_lens[cur_row] = WRITE_MAX_LINE_LEN;

                    int remaining = next_len - room;
                    memmove(lines[cur_row + 1], &lines[cur_row + 1][room],
                            (uint32_t)remaining);
                    line_lens[cur_row + 1] = remaining;
                }
                /* room == 0: this line is already completely full —
                 * nothing can be pulled across yet; no-op, same shape
                 * as the Backspace handler's own room==0 case. */
            }
            /* else: end of the last line — nothing after the cursor
             * anywhere in the buffer; no-op. */
        } else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
            if (line_lens[cur_row] < WRITE_MAX_LINE_LEN) {
                memmove(&lines[cur_row][cur_col + 1], &lines[cur_row][cur_col],
                         (uint32_t)(line_lens[cur_row] - cur_col));
                lines[cur_row][cur_col] = c;
                line_lens[cur_row]++;
                cur_col++;
            }
        }
        /* Any other key (function keys, Home/End/PgUp/PgDn, unhandled
         * Ctrl combos, ...) is silently ignored — same "not every key
         * does something" tolerance terminal_handle_key() itself
         * already has for keys it doesn't recognize. */

        write_render(path, lines, line_lens, num_lines, cur_row, cur_col, start_y);
    }

    if (g_gui_mode) gui_modal_close();   /* window off */

    if (cancelled) {
        if (created_here) write_fs_delete(fs, path);
        if (!g_gui_mode) console_set_cursor(0, start_y + 2 + WRITE_MAX_LINES);
        terminal_puts(t, "Cancelled (nothing saved).\n", TERM_COL_INFO);
        if (!g_gui_mode) console_write_color("Cancelled (nothing saved).\n", COLOR_INFO);
        return;
    }

    /* ---- Flatten the multi-line buffer into one '\n'-joined byte
     * string and persist it via fat_write() — see that function's own
     * doc comment (fat_vfs.h) for the full overwrite-from-0 contract
     * this relies on. ---- */
    static uint8_t content[WRITE_MAX_LINES * (WRITE_MAX_LINE_LEN + 1)];
    uint32_t total = 0;
    for (int i = 0; i < num_lines; i++) {
        memcpy(&content[total], lines[i], (uint32_t)line_lens[i]);
        total += (uint32_t)line_lens[i];
        if (i < num_lines - 1)
            content[total++] = '\n';
    }

    /* Move the real cursor past the editor's own on-screen area before
     * printing the final result, so this doesn't overwrite the last
     * content row. WRITE_MAX_LINES rows of content + 2 header rows. */
    if (!g_gui_mode) console_set_cursor(0, start_y + 2 + WRITE_MAX_LINES);

    int written = write_fs_save(fs, path, content, total, &ferr);
    if (written < 0) {
        terminal_puts(t, "write: cannot write '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' (", TERM_COL_ERROR);
        terminal_puts(t, write_is_lmfs(fs) ? "lmfs write failed" : fat_strerror(ferr), TERM_COL_ERROR);
        terminal_puts(t, ")\n", TERM_COL_ERROR);
        if (!g_gui_mode) console_write_color("write: failed to save\n", COLOR_ERROR);
        return;
    }

    terminal_puts(t, "wrote ", TERM_COL_SUCCESS);
    {
        char numbuf[16];
        sprintf(numbuf, "%d", written);
        terminal_puts(t, numbuf, TERM_COL_SUCCESS);
    }
    terminal_puts(t, " bytes to ", TERM_COL_SUCCESS);
    terminal_puts(t, path, TERM_COL_SUCCESS);
    terminal_newline(t);

    if (!g_gui_mode) {
    console_write_color("wrote ", COLOR_SUCCESS);
    {
        char numbuf[16];
        sprintf(numbuf, "%d", written);
        console_write_color(numbuf, COLOR_SUCCESS);
    }
    console_write_color(" bytes to ", COLOR_SUCCESS);
    console_write_color(path, COLOR_SUCCESS);
    console_putc('\n');
    }
}
