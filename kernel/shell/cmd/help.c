/* kernel/shell/cmd/help.c
 * LumaOS 5.0
 *
 * BUGFIX: "LumaOS 5.0 Commands" was stale — see cmd_version.c's
 * header comment.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../shell.h"

void cmd_help(terminal_t *t, int argc, char **argv) {
    (void)argc; (void)argv;
    /* Reviewer feedback: leave a blank line between the "IDE0> help"
     * echo and the heading below it, for readability. */
    terminal_newline(t);
    terminal_puts(t, "LumaOS 5.0 Commands\n", TERM_COL_INFO);
    terminal_puts(t, "-------------------\n", TERM_COL_INFO);
    for (int i = 0; i < cmd_count; i++) {
        terminal_puts(t, "  ", TERM_COL_DEFAULT);
        terminal_puts(t, cmd_table[i].name, TERM_COL_CMD);
        int pad = 10 - (int)strlen(cmd_table[i].name);
        for (int s = 0; s < pad; s++) terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        terminal_puts(t, cmd_table[i].description, TERM_COL_DEFAULT);
        terminal_newline(t);
    }
    terminal_puts(t, "\nUP/DOWN = history  LEFT/RIGHT = cursor\n", TERM_COL_INFO);
}