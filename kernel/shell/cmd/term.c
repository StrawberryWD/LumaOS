/* kernel/shell/cmd/term.c
 * LumaOS 5.0 — DAY43 PRE-3 (② window capacity)
 *
 * `term` opens one more Terminal window (own scrollback / history /
 * input line) in a free dynamic window slot. The window table holds
 * MAX_WINDOWS (8) windows: Terminal, Info, Settings + 5 extra terminals.
 * Closing an extra terminal's [x] destroys it and frees the slot.
 * GUI build only — the text-mode debug build has no windows. */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../shell.h"

void cmd_term(terminal_t *t, int argc, char **argv) {
    (void)argc; (void)argv;
    if (!g_gui_mode) {
        terminal_puts(t, "term: needs the GUI build (no windows in text mode)\n", TERM_COL_ERROR);
        return;
    }
    int id = gui_window_new_terminal();
    if (id < 0) {
        terminal_puts(t, "term: window limit reached (", TERM_COL_ERROR);
        terminal_putchar(t, (char)('0' + MAX_WINDOWS), TERM_COL_ERROR);
        terminal_puts(t, " windows) - close one first\n", TERM_COL_ERROR);
        return;
    }
    terminal_puts(t, "opened window #", TERM_COL_INFO);
    terminal_putchar(t, (char)('0' + id), TERM_COL_INFO);
    terminal_newline(t);
}
