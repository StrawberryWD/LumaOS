/* user/lib/fsutil.c - DAY77c: cp / mv / rm 共用的“整棵目录树”操作
 *
 * 为什么放进库：cp -r、mv（目录改名要靠 复制+删除）、rm -r 都要递归遍历目录；写三份容易各错各的。
 * libluma.a 是静态库，没用到的函数不会链进程序，所以 hello.sw 之类不会变大。
 *
 * 约定：prog 是程序名（只用来写错误前缀 "cp: ..."）；返回值 = 出错的条目数（0 = 全部成功），
 * 出错时已经用 strerror 把原因写到 stderr，调用者只需要据此决定退出码。 */
#include "../include/luma.h"

#define PMAX 160
#define MAX_DEPTH 8

const char *fs_basename(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

/* 去掉末尾多余的 '/'（"a/b//" -> "a/b"；单独的 "/" 保持不变） */
void fs_trim(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n); dst[n] = 0;
    while (n > 1 && dst[n - 1] == '/') dst[--n] = 0;
}

int fs_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir), m = strlen(name);
    int slash = (n > 0 && dir[n - 1] != '/');
    if (n + slash + m + 1 > cap) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, dir, n);
    if (slash) out[n++] = '/';
    memcpy(out + n, name, m + 1);
    return 0;
}

int fs_is_dir(const char *path)
{
    stat_t st;
    return stat(path, &st) == 0 && st.type == DT_DIR;
}

/* path 是不是某个卷的根（"IDE3" / "RAM0/"）：这种目录不能删 */
int fs_is_volume_root(const char *path)
{
    char t[PMAX];
    fs_trim(t, path, sizeof t);
    if (strchr(t, '/')) return 0;
    dirent_t d;
    for (unsigned i = 0; readdir("", i, &d) == 1; i++)
        if (strcasecmp(d.name, t) == 0) return 1;
    return 0;
}

/* 读出目录的全部条目（malloc，调用者 free）；失败返回 -1 */
static int load_dir(const char *prog, const char *path, dirent_t **out, size_t *count)
{
    size_t cap = 16, n = 0;
    dirent_t *v = malloc(cap * sizeof *v);
    if (!v) { dprintf(STDERR_FILENO, "%s: %s\n", prog, strerror(errno)); return -1; }
    int r;
    for (;;) {
        if (n == cap) {
            dirent_t *nv = realloc(v, cap * 2 * sizeof *v);
            if (!nv) { dprintf(STDERR_FILENO, "%s: %s\n", prog, strerror(errno)); free(v); return -1; }
            v = nv; cap *= 2;
        }
        r = readdir(path, (unsigned)n, &v[n]);
        if (r != 1) break;
        n++;
    }
    if (r < 0) {
        dprintf(STDERR_FILENO, "%s: cannot read directory '%s': %s\n", prog, path, strerror(errno));
        free(v); return -1;
    }
    *out = v; *count = n;
    return 0;
}

static int copy_file(const char *prog, const char *src, const char *dst)
{
    static char buf[4096];
    FILE *in = fopen(src, "r");
    if (!in) { dprintf(STDERR_FILENO, "%s: cannot open '%s': %s\n", prog, src, strerror(errno)); return 1; }
    FILE *out = fopen(dst, "w");
    if (!out) {
        dprintf(STDERR_FILENO, "%s: cannot create '%s': %s\n", prog, dst, strerror(errno));
        fclose(in); return 1;
    }
    int e = 0; size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { e = errno ? errno : EIO; break; }
    }
    if (!e && ferror(in)) e = errno ? errno : EIO;
    fclose(in);
    if (fclose(out) != 0 && !e) e = errno ? errno : EIO;      /* close 才真正写回，可能 ENOSPC */
    if (e) {
        dprintf(STDERR_FILENO, "%s: error writing '%s': %s\n", prog, dst, strerror(e));
        unlink(dst);                                            /* 不留半成品 */
        return 1;
    }
    return 0;
}

static int copy_tree(const char *prog, const char *src, const char *dst, int recursive, int depth)
{
    stat_t ss;
    if (stat(src, &ss) != 0) {
        dprintf(STDERR_FILENO, "%s: cannot stat '%s': %s\n", prog, src, strerror(errno));
        return 1;
    }
    if (ss.type == DT_FILE) {
        stat_t ds;
        if (stat(dst, &ds) == 0 && ds.type == DT_DIR) {
            dprintf(STDERR_FILENO, "%s: cannot overwrite directory '%s' with non-directory\n", prog, dst);
            return 1;
        }
        return copy_file(prog, src, dst);
    }
    if (!recursive) {
        dprintf(STDERR_FILENO, "%s: -r not specified; omitting directory '%s'\n", prog, src);
        return 1;
    }
    if (depth >= MAX_DEPTH) {
        dprintf(STDERR_FILENO, "%s: '%s': directory nested too deeply\n", prog, src);
        return 1;
    }
    stat_t ds;
    if (stat(dst, &ds) == 0) {
        if (ds.type != DT_DIR) {
            dprintf(STDERR_FILENO, "%s: cannot overwrite non-directory '%s' with directory\n", prog, dst);
            return 1;
        }
    } else if (mkdir(dst) != 0) {
        dprintf(STDERR_FILENO, "%s: cannot create directory '%s': %s\n", prog, dst, strerror(errno));
        return 1;
    }
    dirent_t *v; size_t n;
    if (load_dir(prog, src, &v, &n) != 0) return 1;
    int errs = 0;
    for (size_t i = 0; i < n; i++) {
        char a[PMAX], b[PMAX];
        if (fs_join(a, sizeof a, src, v[i].name) != 0 || fs_join(b, sizeof b, dst, v[i].name) != 0) {
            dprintf(STDERR_FILENO, "%s: '%s': %s\n", prog, v[i].name, strerror(errno));
            errs++; continue;
        }
        errs += copy_tree(prog, a, b, recursive, depth + 1);
    }
    free(v);
    return errs;
}

/* dst 就是目标本身（不做“放进已有目录”的判断）；src 是目录时需要 recursive */
int fs_copy_tree(const char *prog, const char *src, const char *dst, int recursive)
{
    char s[PMAX], d[PMAX];
    fs_trim(s, src, sizeof s); fs_trim(d, dst, sizeof d);
    size_t ls = strlen(s);
    if (strcmp(s, d) == 0) {
        dprintf(STDERR_FILENO, "%s: '%s' and '%s' are the same file\n", prog, src, dst);
        return 1;
    }
    if (fs_is_dir(s) && strncmp(d, s, ls) == 0 && d[ls] == '/') {   /* cp -r a a/b：会无限递归 */
        dprintf(STDERR_FILENO, "%s: cannot copy '%s' into itself, '%s'\n", prog, src, dst);
        return 1;
    }
    return copy_tree(prog, s, d, recursive, 0);
}

/* 常见用法：dst 是已有目录 -> 复制到 dst/basename(src)；否则 dst 就是目标名 */
int fs_copy(const char *prog, const char *src, const char *dst, int recursive)
{
    char s[PMAX], t[PMAX];
    fs_trim(s, src, sizeof s);
    if (fs_is_dir(dst)) {
        if (fs_join(t, sizeof t, dst, fs_basename(s)) != 0) {
            dprintf(STDERR_FILENO, "%s: '%s': %s\n", prog, src, strerror(errno));
            return 1;
        }
        return fs_copy_tree(prog, s, t, recursive);
    }
    return fs_copy_tree(prog, s, dst, recursive);
}

static int remove_tree(const char *prog, const char *path, int recursive, int force, int depth)
{
    stat_t st;
    if (stat(path, &st) != 0) {
        if (force && errno == ENOENT) return 0;
        dprintf(STDERR_FILENO, "%s: cannot remove '%s': %s\n", prog, path, strerror(errno));
        return 1;
    }
    if (st.type == DT_FILE) {
        if (unlink(path) != 0) {
            dprintf(STDERR_FILENO, "%s: cannot remove '%s': %s\n", prog, path, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (!recursive) {
        dprintf(STDERR_FILENO, "%s: cannot remove '%s': Is a directory\n", prog, path);
        return 1;
    }
    if (depth >= MAX_DEPTH) {
        dprintf(STDERR_FILENO, "%s: '%s': directory nested too deeply\n", prog, path);
        return 1;
    }
    dirent_t *v; size_t n;
    if (load_dir(prog, path, &v, &n) != 0) return 1;
    int errs = 0;
    for (size_t i = 0; i < n; i++) {
        char c[PMAX];
        if (fs_join(c, sizeof c, path, v[i].name) != 0) {
            dprintf(STDERR_FILENO, "%s: '%s': %s\n", prog, v[i].name, strerror(errno));
            errs++; continue;
        }
        errs += remove_tree(prog, c, recursive, force, depth + 1);
    }
    free(v);
    if (errs == 0 && rmdir(path) != 0) {                          /* 有子项删失败时目录必然非空，不再重复报错 */
        dprintf(STDERR_FILENO, "%s: cannot remove '%s': %s\n", prog, path, strerror(errno));
        errs++;
    }
    return errs;
}

int fs_remove(const char *prog, const char *path, int recursive, int force)
{
    char p[PMAX];
    fs_trim(p, path, sizeof p);
    const char *b = fs_basename(p);
    if (strcmp(b, ".") == 0 || strcmp(b, "..") == 0 || p[0] == 0 || strcmp(p, "/") == 0 || fs_is_volume_root(p)) {
        dprintf(STDERR_FILENO, "%s: refusing to remove '%s'\n", prog, path);
        return 1;
    }
    return remove_tree(prog, p, recursive, force, 0);
}
