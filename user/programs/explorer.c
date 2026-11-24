/* user/programs/explorer.c - DAY75: `explorer.sw [路径]`  —— 目录树浏览器（文本版“资源管理器”）
 *
 * 不带参数：列出所有已挂载的卷，每个卷展开两层。带路径：从那里完整展开（最深 8 层）。
 *     IDE3/
 *     |-- docs/
 *     |   `-- readme.txt (120 B)
 *     |-- hello.sw (644 B)
 *     `-- notes.txt (1.5 KB)
 *     1 directory, 3 files, 2.3 KB
 * 目录在前，按字母（不分大小写）；大小 < 1KB 显示字节，之后显示 KB/MB（一位小数，整数运算——没有浮点）。
 * 每层目录用 malloc 读入、排序，画完这一层（含子树）就 free，所以内存占用 = 深度 x 目录大小，不是整棵树。
 * 路径超过内核的 159 字符上限或层数超过 8 的分支会写 "[...]" 而不是崩。
 * 为什么不是“带键盘操作的交互式”：LumaOS 的 stdin 还不能读（没有键盘输入的系统调用），以后补上再升级。 */
#include "../include/luma.h"

#define MAX_DEPTH 8
static unsigned n_dirs, n_files, total_bytes;

static void put_size(unsigned n)
{
    if (n < 1024) printf("%u B", n);
    else if (n < 1024 * 1024) printf("%u.%u KB", n / 1024, (n % 1024) * 10 / 1024);
    else printf("%u.%u MB", n / (1024 * 1024), (n % (1024 * 1024)) * 10 / (1024 * 1024));
}

static int by_name(const void *pa, const void *pb)
{
    const dirent_t *a = pa, *b = pb;
    if (a->type != b->type) return a->type == DT_DIR ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

/* prefix = 这一层条目前面要画的竖线缀；depth_left = 还允许再下几层 */
static void walk(const char *path, const char *prefix, int depth_left)
{
    size_t cap = 16, n = 0;
    dirent_t *v = malloc(cap * sizeof *v);
    if (!v) { printf("%s[out of memory]\n", prefix); return; }
    int r;
    for (;;) {
        if (n == cap) {
            dirent_t *nv = realloc(v, cap * 2 * sizeof *v);
            if (!nv) break;
            v = nv; cap *= 2;
        }
        r = readdir(path, (unsigned)n, &v[n]);
        if (r != 1) break;
        n++;
    }
    qsort(v, n, sizeof *v, by_name);
    for (size_t i = 0; i < n; i++) {
        int last = (i + 1 == n);
        printf("%s%s%s", prefix, last ? "`-- " : "|-- ", v[i].name);
        if (v[i].type == DT_FILE) {
            printf(" (");
            put_size(v[i].size);
            printf(")\n");
            n_files++; total_bytes += v[i].size;
            continue;
        }
        printf("/\n");
        n_dirs++;
        if (depth_left <= 0) {
            printf("%s%s[...]\n", prefix, last ? "    " : "|   ");
            continue;
        }
        char child[160], sub[160];
        size_t pl = strlen(path);
        int need = (int)pl + 1 + (int)strlen(v[i].name) + 1;
        if (need > (int)sizeof child) { printf("%s%s[path too long]\n", prefix, last ? "    " : "|   "); continue; }
        strcpy(child, path);
        if (pl && child[pl - 1] != '/') strcat(child, "/");
        strcat(child, v[i].name);
        if (strlen(prefix) + 5 > sizeof sub) continue;
        strcpy(sub, prefix); strcat(sub, last ? "    " : "|   ");
        walk(child, sub, depth_left - 1);
    }
    free(v);
}

static void summary(void)
{
    printf("%u %s, %u %s, ", n_dirs, n_dirs == 1 ? "directory" : "directories", n_files, n_files == 1 ? "file" : "files");
    put_size(total_bytes);
    putchar('\n');
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        stat_t st;
        if (stat(argv[1], &st) != 0) {
            dprintf(STDERR_FILENO, "explorer: cannot access '%s': %s\n", argv[1], strerror(errno));
            return 1;
        }
        printf("%s%s\n", argv[1], st.type == DT_DIR ? "/" : "");
        if (st.type == DT_DIR) walk(argv[1], "", MAX_DEPTH);
        else { n_files = 1; total_bytes = st.size; }
        summary();
        return 0;
    }
    dirent_t vol;                                   /* 无参数：所有卷，各展开两层 */
    for (unsigned i = 0; readdir("", i, &vol) == 1; i++) {
        printf("%s/\n", vol.name);
        walk(vol.name, "", 1);
    }
    summary();
    return 0;
}
