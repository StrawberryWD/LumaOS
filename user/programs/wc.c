/* user/programs/wc.c - DAY77c: `wc.sw [-l] [-w] [-c] [文件...]` —— 行数、单词数、字节数
 * 不给选项 = 三项都显示（行 单词 字节，各占 7 列，再加文件名）。行数 = 换行符个数；单词 = 以空白分隔的连续非空白串。
 * 多个文件最后加一行 "total"。没有文件参数：读标准输入。 */
#include "../include/luma.h"

typedef struct { unsigned long l, w, c; } counts_t;

static int count_stream(FILE *f, counts_t *t)
{
    int c, in_word = 0;
    t->l = t->w = t->c = 0;
    while ((c = fgetc(f)) != EOF) {
        t->c++;
        if (c == '\n') t->l++;
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\v' || c == '\f') in_word = 0;
        else if (!in_word) { in_word = 1; t->w++; }
    }
    return ferror(f) ? -1 : 0;
}

static void show(const counts_t *t, int sl, int sw, int sc, const char *name)
{
    if (sl) printf("%7lu", t->l);
    if (sw) printf("%7lu", t->w);
    if (sc) printf("%7lu", t->c);
    if (name) printf(" %s", name);
    putchar('\n');
}

int main(int argc, char **argv)
{
    int sl = 0, sw = 0, sc = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'l') sl = 1; else if (*o == 'w') sw = 1; else if (*o == 'c') sc = 1;
            else { dprintf(STDERR_FILENO, "wc: invalid option -- '%c'\n", *o); return 2; }
        }
    }
    if (!sl && !sw && !sc) sl = sw = sc = 1;
    counts_t total = {0, 0, 0}, t;
    int nfiles = argc - i, ok = 0;
    if (nfiles == 0) {
        if (count_stream(stdin, &t) < 0) { dprintf(STDERR_FILENO, "wc: stdin: %s\n", strerror(errno)); return 1; }
        show(&t, sl, sw, sc, 0);
        return 0;
    }
    for (int k = i; k < argc; k++) {
        FILE *f = fopen(argv[k], "r");
        if (!f) { dprintf(STDERR_FILENO, "wc: %s: %s\n", argv[k], strerror(errno)); rc = 1; continue; }
        int r = count_stream(f, &t);
        fclose(f);
        if (r < 0) { dprintf(STDERR_FILENO, "wc: %s: %s\n", argv[k], strerror(errno)); rc = 1; continue; }
        show(&t, sl, sw, sc, argv[k]);
        total.l += t.l; total.w += t.w; total.c += t.c; ok++;
    }
    if (nfiles > 1) show(&total, sl, sw, sc, "total");
    (void)ok;
    return rc;
}
