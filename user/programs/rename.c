/* user/programs/rename.c - DAY77c（原名 mv.sw，按要求改名为 rename）: `rename.sw 源... 目标`
 * 文件：rename() 系统调用（Day 77b：读 -> 写新 -> 删旧，可跨卷）。目标是已有目录 -> 移进去。已有同名文件被覆盖。
 * 目录：文件系统层没有“目录改名”，所以 复制整棵树 -> 删除原树；中途失败时原树保持完整（只有成功复制后才删）。
 * 不能把目录移进自己里面。 */
#include "../include/luma.h"

static int move_one(const char *src, const char *dst)
{
    char s[160], t[160];
    fs_trim(s, src, sizeof s);
    if (fs_is_dir(dst)) {
        if (fs_join(t, sizeof t, dst, fs_basename(s)) != 0) { dprintf(STDERR_FILENO, "rename: '%s': %s\n", src, strerror(errno)); return 1; }
    } else {
        fs_trim(t, dst, sizeof t);
    }
    stat_t st;
    if (stat(s, &st) != 0) { dprintf(STDERR_FILENO, "rename: cannot stat '%s': %s\n", src, strerror(errno)); return 1; }
    if (strcmp(s, t) == 0) { dprintf(STDERR_FILENO, "rename: '%s' and '%s' are the same file\n", src, dst); return 1; }
    if (st.type == DT_FILE) {
        if (fs_is_dir(t)) { dprintf(STDERR_FILENO, "rename: cannot overwrite directory '%s' with non-directory\n", t); return 1; }
        if (rename(s, t) != 0) { dprintf(STDERR_FILENO, "rename: cannot move '%s' to '%s': %s\n", src, t, strerror(errno)); return 1; }
        return 0;
    }
    if (fs_is_volume_root(s)) { dprintf(STDERR_FILENO, "rename: cannot move '%s': Device or resource busy\n", src); return 1; }
    if (fs_is_dir(t)) { dprintf(STDERR_FILENO, "rename: cannot overwrite directory '%s'\n", t); return 1; }
    if (fs_copy_tree("rename", s, t, 1) != 0) {
        dprintf(STDERR_FILENO, "rename: '%s' not moved (copy failed; partial copy '%s' left behind)\n", src, t);
        return 1;
    }
    return fs_remove("rename", s, 1, 0) != 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { dputs("usage: rename.sw source... dest\n", STDERR_FILENO); return 2; }
    const char *dst = argv[argc - 1];
    if (argc > 3 && !fs_is_dir(dst)) { dprintf(STDERR_FILENO, "rename: target '%s' is not a directory\n", dst); return 1; }
    int rc = 0;
    for (int i = 1; i < argc - 1; i++)
        if (move_one(argv[i], dst) != 0) rc = 1;
    return rc;
}
