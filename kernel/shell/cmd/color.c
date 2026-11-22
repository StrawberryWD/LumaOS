/* kernel/shell/cmd/color.c */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../shell.h"

static const char *cnames[16] = {
    "black","blue","green","cyan",
    "red","magenta","brown","lgray",
    "dgray","lblue","lgreen","lcyan",
    "lred","lmagenta","yellow","white"
};

void cmd_color(terminal_t *t, int argc, char **argv) {
    if (argc < 2) {
        terminal_puts(t, "Usage: color <0-15>\nColors:\n", TERM_COL_DEFAULT);
        for (int i = 0; i < 16; i++) {
            terminal_putchar(t, '0'+(i/10), TERM_COL_DEFAULT);
            terminal_putchar(t, '0'+(i%10), TERM_COL_DEFAULT);
            terminal_putchar(t, ' ', TERM_COL_DEFAULT);
            terminal_puts(t, cnames[i], (unsigned char)i);
            terminal_newline(t);
        }
        return;
    }
    int n = atoi(argv[1]);
    if (n < 0 || n > 15) {
        terminal_puts(t, "Error: color must be 0-15\n", TERM_COL_ERROR);
        return;
    }
    terminal_puts(t, "Text color -> ", TERM_COL_DEFAULT);
    terminal_puts(t, cnames[n], (unsigned char)n);
    terminal_newline(t);
}
