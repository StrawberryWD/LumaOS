/* kernel/shell/cmd/type.c
 * LumaOS 5.0
 *
 * BUGFIX (reported: cmd 文件夹里的旧版本号残留 — see cmd_version.c's
 * own header comment for the full "why 5.0" rationale): "LumaOS 4.0
 * Quick Reference" was stale, same class of bug as cmd_version.c's
 * "LumaOS 4.1" had been. */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../shell.h"

static const char *doc[] = {
    "",   /* leading blank line — reviewer feedback: leave breathing
             room between the "IDE0> type" echo line and the doc
             body, rather than the heading landing immediately below
             the prompt with no gap. */
    "LumaOS 5.0 Quick Reference",
    "===========================",
    "",
    "Keyboard:",
    "  UP/DOWN    command history",
    "  LEFT/RIGHT move cursor",
    "  BACKSPACE  delete char",
    "  ENTER      run command",
    "",
    "Commands:",
    "  help       list commands",
    "  clear      clear terminal",
    "  version    show version",
    "  type       this document",
    "  echo [..] print text",
    "  color [n]  set color 0-15",
    "  shutdown   power off",
    "  reboot     restart",
    NULL
};

void cmd_type(terminal_t *t, int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; doc[i]; i++) {
        /* Header lines (title + "====") are indices 1,2 now that
         * index 0 is the new leading blank line above — was `i < 2`
         * before that blank line existed. */
        unsigned char c = (i >= 1 && i <= 2) ? (unsigned char)TERM_COL_INFO
                                              : (unsigned char)TERM_COL_DEFAULT;
        terminal_puts(t, doc[i], c);
        terminal_newline(t);
    }
}