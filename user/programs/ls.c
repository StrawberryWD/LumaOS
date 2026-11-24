/* user/programs/ls.c - DAY75: `ls.sw [-l] [路径...]`  —— shell 内置 ls 的用户态版本
 *
 * 和内置 ls 的区别：排序（目录在前，其余按字母，不分大小写）；-l 多一列类型；多个路径时每个先打印 "路径:"；
 * 出错用 strerror 给出准确原因并以退出码 1 结束（其余路径仍然处理）。没有路径参数时列当前目录。
 * 用到的库：readdir/stat（Day 75 系统调用）、malloc/realloc（Day 73）、qsort、printf（Day 72）。 */
#include "../include/luma.h"

static int by_name(const void *pa, const void *pb)
{
    const dirent_t *a = pa, *b = pb;
    if (a->type != b->type) return a->type == DT_DIR ? -1 : 1;           /* 目录在前 */
    return strcasecmp(a->name, b->name);
}

static void print_entry(const dirent_t *d, int longfmt)
{
    char shown[40];
    snprintf(shown, sizeof shown, "%s%s", d->name, d->type == DT_DIR ? "/" : "");
    if (longfmt) {
        printf("%c %8u  %s\n", d->type == DT_DIR ? 'd' : '-', d->size, shown);
        return;
    }
    printf("%s", shown);
    for (int s = (int)strlen(shown); s < 16; s++) putchar(' ');          /* 对齐到 16 列，和内置 ls 一样 */
    if (d->type == DT_FILE) printf("%u bytes", d->size);
    putchar('\n');
}

static int list_one(const char *path, int longfmt, int header)
{
    stat_t st;
    if (stat(path, &st) != 0) {
        dprintf(STDERR_FILENO, "ls: cannot access '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (header) printf("%s:\n", path);
    if (st.type == DT_FILE) {                                            /* ls 文件：只列这一个 */
        dirent_t d;
        const char *base = strrchr(path, '/');
        memset(&d, 0, sizeof d);
        strncpy(d.name, base ? base + 1 : path, sizeof d.name - 1);
        d.type = DT_FILE; d.size = st.size;
        print_entry(&d, longfmt);
        return 0;
    }
    size_t cap = 16, n = 0;
    dirent_t *v = malloc(cap * sizeof *v);
    if (!v) { dprintf(STDERR_FILENO, "ls: %s\n", strerror(errno)); return 1; }
    int r;
    for (;;) {
        if (n == cap) {
            dirent_t *nv = realloc(v, cap * 2 * sizeof *v);
            if (!nv) { dprintf(STDERR_FILENO, "ls: %s\n", strerror(errno)); free(v); return 1; }
            v = nv; cap *= 2;
        }
        r = readdir(path, (unsigned)n, &v[n]);
        if (r != 1) break;
        n++;
    }
    if (r < 0) {
        dprintf(STDERR_FILENO, "ls: cannot open '%s': %s\n", path, strerror(errno));
        free(v);
        return 1;
    }
    if (n == 0) puts("(empty)");
    qsort(v, n, sizeof *v, by_name);
    for (size_t i = 0; i < n; i++) print_entry(&v[i], longfmt);
    free(v);
    return 0;
}

int main(int argc, char **argv)
{
    int longfmt = 0, rc = 0, npaths = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) longfmt = 1;
        else if (argv[i][0] == '-' && argv[i][1]) {
            dprintf(STDERR_FILENO, "ls: unknown option '%s' (usage: ls.sw [-l] [path...])\n", argv[i]);
            return 2;
        } else npaths++;
    }
    if (npaths == 0) return list_one(".", longfmt, 0);
    int shown = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;
        if (shown++) putchar('\n');
        rc |= list_one(argv[i], longfmt, npaths > 1);
    }
    return rc;
}
