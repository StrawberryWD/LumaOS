/* kernel/syscall/fsops.c - DAY75: 目录与状态系统调用。设计见 syscall.h 的 DAY75 一节。
 *
 * 和 fd.c 一样的原则：程序传来的所有指针先验证再用；路径先拷进内核再解析（与 shell 同一套 shell_resolve_path）；
 * 结果写回用户内存前先确认整个输出结构都落在可写的用户页里。 */
#define LUMA_KERNEL_SIDE 1
#include "fsops.h"
#include "fd.h"
#include "syscall.h"
#include "../core/errno.h"
#include "../fs/vfs/vfs.h"
#include "../fs/vfs/vfs_ops.h"
#include "../shell/shell.h"
#include "../lib/string.h"

static int copy_path(char *dst, uint32_t usrc, uint32_t max)       /* 同 fd.c；返回长度或 -errno */
{
    for (uint32_t i = 0; i < max; i++) {
        if (!luma_user_range_ok(usrc + i, 1))
            return -LUMA_EFAULT;
        char c = *(const volatile char *)(uintptr_t)(usrc + i);
        dst[i] = c;
        if (c == 0)
            return (int)i;
    }
    return -LUMA_ENAMETOOLONG;
}

static int resolve(const char *upath, char *full, uint32_t size)
{
    char raw[LUMA_PATH_MAX];
    int n = copy_path(raw, (uint32_t)(uintptr_t)upath, sizeof(raw));
    if (n < 0) return n;
    if (n == 0) return -LUMA_ENOENT;
    if (shell_resolve_path(raw, full, size) != 0) return -LUMA_ENOENT;
    return 0;
}

static void fill_dirent(luma_dirent_t *d, uint32_t size, uint32_t type, const char *name)
{
    memset(d, 0, sizeof(*d));
    d->size = size;
    d->type = type;
    strncpy(d->name, name, sizeof(d->name) - 1);
}

int32_t fsops_readdir(const char *upath, uint32_t index, uint32_t uout)
{
    if (!luma_user_range_ok(uout, sizeof(luma_dirent_t)))
        return -LUMA_EFAULT;
    luma_dirent_t *out = (luma_dirent_t *)(uintptr_t)uout;

    /* path == "" : 列出已挂载的卷 */
    if (luma_user_range_ok((uint32_t)(uintptr_t)upath, 1) && *(const volatile char *)upath == 0) {
        if (index >= vfs_mount_count)
            return 0;
        fill_dirent(out, 0, LUMA_DT_DIR, vfs_mounts[index].name);
        return 1;
    }

    char full[LUMA_PATH_MAX];
    int r = resolve(upath, full, sizeof(full));
    if (r < 0) return r;
    vfs_node_t dir;
    r = vfs_ops_open_dir(full, &dir);
    if (r < 0) return r;

    vfs_node_t child;
    uint32_t raw = 0, shown = 0;
    while (dir.fs->readdir(&dir, raw, &child) == 0) {
        raw++;
        if (strcmp(child.name, ".") == 0 || strcmp(child.name, "..") == 0)
            continue;                                   /* 和 shell 的 ls 一样不显示 */
        if (shown == index) {
            fill_dirent(out, child.type == VFS_TYPE_DIR ? 0 : child.size,
                        child.type == VFS_TYPE_DIR ? LUMA_DT_DIR : LUMA_DT_FILE, child.name);
            return 1;
        }
        shown++;
    }
    return 0;                                           /* 越过最后一项 */
}

int32_t fsops_stat(const char *upath, uint32_t uout)
{
    if (!luma_user_range_ok(uout, sizeof(luma_stat_t)))
        return -LUMA_EFAULT;
    char full[LUMA_PATH_MAX];
    int r = resolve(upath, full, sizeof(full));
    if (r < 0) return r;
    uint32_t size = 0; int is_dir = 0;
    r = vfs_ops_stat(full, &size, &is_dir);
    if (r < 0) return r;
    luma_stat_t *st = (luma_stat_t *)(uintptr_t)uout;
    st->size = is_dir ? 0 : size;
    st->type = is_dir ? LUMA_DT_DIR : LUMA_DT_FILE;
    return 0;
}

int32_t fsops_getcwd(uint32_t ubuf, uint32_t size)
{
    char p[LUMA_PATH_MAX];
    if (shell_resolve_path(".", p, sizeof(p)) != 0)
        return -LUMA_ENOENT;                            /* 没有当前文件系统 */
    uint32_t n = (uint32_t)strlen(p);
    if (n && p[n - 1] != '/') { p[n++] = '/'; p[n] = 0; }
    if (size == 0 || n + 1 > size)
        return -LUMA_ERANGE;
    if (!luma_user_range_ok(ubuf, n + 1))
        return -LUMA_EFAULT;
    memcpy((void *)(uintptr_t)ubuf, p, n + 1);
    return (int32_t)n;
}

/* ---- DAY77b ---- */
int32_t fsops_unlink(const char *upath)
{
    char full[LUMA_PATH_MAX];
    int r = resolve(upath, full, sizeof(full));
    if (r < 0) return r;
    return vfs_ops_delete(full);
}

int32_t fsops_mkdir(const char *upath)
{
    char full[LUMA_PATH_MAX];
    int r = resolve(upath, full, sizeof(full));
    if (r < 0) return r;
    return vfs_ops_mkdir(full);
}

int32_t fsops_rmdir(const char *upath)
{
    char full[LUMA_PATH_MAX];
    int r = resolve(upath, full, sizeof(full));
    if (r < 0) return r;
    return vfs_ops_rmdir(full);
}

#define XFER_MAX (256u * 1024u)
static uint8_t g_xfer[XFER_MAX];                               /* rename 的中转缓冲（.bss） */

int32_t fsops_rename(const char *uold, const char *unew)
{
    char a[LUMA_PATH_MAX], b[LUMA_PATH_MAX];
    int r = resolve(uold, a, sizeof(a));
    if (r < 0) return r;
    r = resolve(unew, b, sizeof(b));
    if (r < 0) return r;
    uint32_t size = 0; int isdir = 0;
    r = vfs_ops_stat(a, &size, &isdir);
    if (r < 0) return r;
    if (isdir) return -LUMA_EISDIR;                            /* 目录改名：文件系统层还没有 */
    if (strcmp(a, b) == 0) return 0;
    r = vfs_ops_writable(a); if (r < 0) return r;
    r = vfs_ops_writable(b); if (r < 0) return r;
    if (size > XFER_MAX) return -LUMA_EFBIG;
    uint32_t dsize = 0; int disdir = 0, existed = 0;
    int sr = vfs_ops_stat(b, &dsize, &disdir);
    if (sr == 0) { if (disdir) return -LUMA_EISDIR; existed = 1; }
    else if (sr != -LUMA_ENOENT) return sr;
    int n = vfs_ops_read(a, g_xfer, XFER_MAX);
    if (n < 0) return n;
    if (!existed) {
        r = vfs_ops_create(b);
        if (r < 0) return r;
    }
    int w = vfs_ops_write(b, g_xfer, (uint32_t)n);
    if (w < 0) {
        if (!existed) vfs_ops_delete(b);                       /* 别留下半成品 */
        return w;
    }
    return vfs_ops_delete(a);
}
