/* user/programs/head.c - DAY77c: `head.sw [-n N] [文件...]` —— 每个文件的前 N 行（默认 10）
 * 多个文件时每个前面加 "==> 名字 <==" 标题（文件之间空一行）。没有文件参数：读标准输入（Day 77a），Ctrl+D 结束。
 * 也接受 -N 的简写。 */
#include "../include/luma.h"

static int head_stream(FILE *f, int lines)
{
    int c, seen = 0;
    if (lines <= 0) return 0;
    while ((c = fgetc(f)) != EOF) {
        putchar(c);
        if (c == '\n' && ++seen >= lines) break;
    }
    return ferror(f) ? -1 : 0;
}

int main(int argc, char **argv)
{
    int lines = 10, i = 1, rc = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) { dputs("head: option requires an argument -- 'n'\n", STDERR_FILENO); return 2; }
            lines = atoi(argv[i + 1]); i += 2;
        } else if (argv[i][1] >= '0' && argv[i][1] <= '9') { lines = atoi(argv[i] + 1); i++; }
        else { dprintf(STDERR_FILENO, "head: invalid option '%s'\n", argv[i]); return 2; }
    }
    if (lines < 0) { dputs("head: invalid number of lines\n", STDERR_FILENO); return 2; }
    int nfiles = argc - i;
    if (nfiles == 0) return head_stream(stdin, lines) < 0;
    for (int k = i; k < argc; k++) {
        FILE *f = fopen(argv[k], "r");
        if (!f) { dprintf(STDERR_FILENO, "head: cannot open '%s': %s\n", argv[k], strerror(errno)); rc = 1; continue; }
        if (nfiles > 1) printf("%s==> %s <==\n", k > i ? "\n" : "", argv[k]);
        if (head_stream(f, lines) < 0) { dprintf(STDERR_FILENO, "head: %s: %s\n", argv[k], strerror(errno)); rc = 1; }
        fclose(f);
    }
    return rc;
}
