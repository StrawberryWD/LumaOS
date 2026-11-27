/* user/programs/echo.c - DAY77c: `echo.sw [-n] [-e] 文字... [> 文件 | >> 文件]`
 * -n 不加结尾换行；-e 解释 \n \t \\。LumaOS 的 shell 还没有重定向，所以 echo.sw 自己认 ">"（覆盖）/ ">>"（追加）：
 * 参数里出现单独的 ">" 或 ">>"，它后面那个词就是文件名，前面的文字写进文件而不是屏幕。 */
#include "../include/luma.h"

int main(int argc, char **argv)
{
    int nl = 1, esc = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i] + 1; int ok = 1;
        for (; *o; o++) if (*o != 'n' && *o != 'e') ok = 0;
        if (!ok) break;
        for (o = argv[i] + 1; *o; o++) { if (*o == 'n') nl = 0; else esc = 1; }
    }
    int end = argc; const char *file = 0; int append = 0;
    for (int k = i; k < argc; k++) {
        if (strcmp(argv[k], ">") == 0 || strcmp(argv[k], ">>") == 0) {
            append = argv[k][1] == '>';
            if (k + 1 >= argc || k + 2 < argc) { dputs("echo: redirection needs exactly one file name\n", STDERR_FILENO); return 2; }
            file = argv[k + 1]; end = k; break;
        }
    }
    FILE *out = stdout;
    if (file) {
        out = fopen(file, append ? "a" : "w");
        if (!out) { dprintf(STDERR_FILENO, "echo: %s: %s\n", file, strerror(errno)); return 1; }
    }
    for (int k = i; k < end; k++) {
        if (k > i) fputc(' ', out);
        for (const char *p = argv[k]; *p; p++) {
            if (esc && *p == '\\' && p[1]) {
                p++;
                fputc(*p == 'n' ? '\n' : *p == 't' ? '\t' : *p, out);
            } else fputc(*p, out);
        }
    }
    if (nl) fputc('\n', out);
    if (file && fclose(out) != 0) { dprintf(STDERR_FILENO, "echo: %s: %s\n", file, strerror(errno)); return 1; }
    return 0;
}
