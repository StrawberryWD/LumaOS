/* kernel/shell/cmd/echo.c
 * LumaOS 5.0
 *
 * DAY43 PRE-3 (③ "解决echo多行文本回车后被截断的问题"):
 *
 * WHY IT WAS TRUNCATED (two independent causes, both reproduced in QEMU):
 *   1. The old echo printed argv[1..argc-1]. shell.c's parser only keeps
 *      SHELL_MAX_ARGS (8) tokens — the command word plus 7 words — and
 *      silently drops the rest, so `echo one two ... sixteen` printed
 *      only "one two three four five six seven". It also squeezed every
 *      run of blanks into one space.
 *   2. (GUI) terminal_render() drew at most `visible_cols` (~92) cells of
 *      a history line and cut the remainder; it now wraps (window1.c).
 *
 * NOW: echo does not use argv at all. It prints the RAW rest of the
 * command line (shell_cmdline_args()), so there is no word/length limit
 * beyond the line editor's own TERM_INPUT_MAX, and spacing is preserved.
 *
 * "多行文本": a command line is one line, so line breaks are typed as the
 * escape sequence \n (like `echo -e`). Supported escapes (always on):
 *     \n newline   \t tab   \\ backslash   \" quote
 * A text wrapped in one pair of "double quotes" has them removed.
 * Option -n (first word) suppresses the final newline.
 *     echo "line 1\nline 2\nline 3"
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../shell.h"

void cmd_echo(terminal_t *t, int argc, char **argv) {
    (void)argc; (void)argv;
    const char *p = shell_cmdline_args();
    int newline = 1;

    if (p[0]=='-' && p[1]=='n' && (p[2]==' ' || p[2]=='\t' || p[2]==0)) {
        newline = 0;
        p += 2;
        while (*p==' ' || *p=='\t') p++;
    }

    /* strip ONE surrounding pair of double quotes */
    int n = (int)strlen(p);
    int end = n;
    if (n >= 2 && p[0]=='"' && p[n-1]=='"') { p++; end = n - 2; }

    for (int i = 0; i < end; i++) {
        char c = p[i];
        if (c == '\\' && i + 1 < end) {
            char d = p[i+1];
            if (d == 'n')  { terminal_newline(t);                          i++; continue; }
            if (d == 't')  { terminal_putchar(t, '\t', TERM_COL_DEFAULT);  i++; continue; }
            if (d == '\\') { terminal_putchar(t, '\\', TERM_COL_DEFAULT);  i++; continue; }
            if (d == '"')  { terminal_putchar(t, '"',  TERM_COL_DEFAULT);  i++; continue; }
        }
        terminal_putchar(t, c, TERM_COL_DEFAULT);
    }
    if (newline) terminal_newline(t);
}
