/* kernel/shell/cmd/clear.c */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../shell.h"
void cmd_clear(terminal_t *t, int argc, char **argv) {
    (void)argc; (void)argv;
    terminal_clear(t);
}
