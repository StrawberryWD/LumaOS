/* kernel/core/errno.c - DAY70: 错误号 <-> 文字，以及老式错误码的折算
 *
 * 一张表，三种用途：luma_strerror()（人看的句子）、luma_errname()（符号名，
 * 给日志/`errno` 命令/测试用）、SYS_STRERROR 系统调用（用户程序看的句子，
 * 见 syscall.c）。表按错误号升序排列，所以 luma_errno_nth() 的遍历顺序固定。
 */
#define LUMA_KERNEL_SIDE 1
#include "errno.h"
#include "../fs/fat/fat_vfs.h"      /* FAT_ERR_* for luma_err_from_fat() */
#include "../lib/string.h"

typedef struct {
    int         num;
    const char *name;
    const char *text;
} errinfo_t;

static const errinfo_t g_errs[] = {
    { LUMA_EPERM,        "EPERM",        "Operation not permitted" },
    { LUMA_ENOENT,       "ENOENT",       "No such file or directory" },
    { LUMA_ESRCH,        "ESRCH",        "No such process" },
    { LUMA_EIO,          "EIO",          "Input/output error" },
    { LUMA_E2BIG,        "E2BIG",        "Argument list too long" },
    { LUMA_ENOEXEC,      "ENOEXEC",      "Exec format error (not a valid .sw program)" },
    { LUMA_EBADF,        "EBADF",        "Bad file descriptor" },
    { LUMA_ECHILD,       "ECHILD",       "No child processes" },
    { LUMA_EAGAIN,       "EAGAIN",       "Resource temporarily unavailable" },
    { LUMA_ENOMEM,       "ENOMEM",       "Out of memory" },
    { LUMA_EACCES,       "EACCES",       "Permission denied" },
    { LUMA_EFAULT,       "EFAULT",       "Bad address" },
    { LUMA_EBUSY,        "EBUSY",        "Device or resource busy" },
    { LUMA_EEXIST,       "EEXIST",       "File exists" },
    { LUMA_ENODEV,       "ENODEV",       "No such device or volume" },
    { LUMA_ENOTDIR,      "ENOTDIR",      "Not a directory" },
    { LUMA_EISDIR,       "EISDIR",       "Is a directory" },
    { LUMA_EINVAL,       "EINVAL",       "Invalid argument" },
    { LUMA_EMFILE,       "EMFILE",       "Too many open files" },
    { LUMA_EFBIG,        "EFBIG",        "File too large" },
    { LUMA_ENOSPC,       "ENOSPC",       "No space left on device" },
    { LUMA_EROFS,        "EROFS",        "Read-only file system" },
    { LUMA_ERANGE,       "ERANGE",       "Result out of range" },
    { LUMA_ENAMETOOLONG, "ENAMETOOLONG", "File name too long" },
    { LUMA_ENOSYS,       "ENOSYS",       "Function not implemented" },
    { LUMA_ENOTEMPTY,    "ENOTEMPTY",    "Directory not empty" },
    { LUMA_EUCLEAN,      "EUCLEAN",      "Structure needs cleaning (file system is damaged)" },
};
#define NERRS ((int)(sizeof(g_errs) / sizeof(g_errs[0])))

static const errinfo_t *find(int e)
{
    if (e < 0) e = -e;                      /* 带不带符号都认 */
    for (int i = 0; i < NERRS; i++)
        if (g_errs[i].num == e)
            return &g_errs[i];
    return (const errinfo_t *)0;
}

const char *luma_strerror(int e)
{
    if (e == 0) return "Success";
    const errinfo_t *p = find(e);
    return p ? p->text : "Unknown error";
}

const char *luma_errname(int e)
{
    if (e == 0) return "EOK";
    const errinfo_t *p = find(e);
    return p ? p->name : "E?";
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

int luma_errno_from_name(const char *name)
{
    if (!name || !*name) return -1;
    for (int i = 0; i < NERRS; i++) {
        const char *a = g_errs[i].name;    /* "ENOENT" */
        const char *b = name;
        int ok = 1;
        while (*a || *b) {
            if (up(*a) != up(*b)) { ok = 0; break; }
            if (!*a || !*b) break;
            a++; b++;
        }
        if (ok && !*a && !*b) return g_errs[i].num;
        /* 省略开头的 "E"："NOENT" -> ENOENT */
        a = g_errs[i].name + 1; b = name; ok = 1;
        while (*a || *b) {
            if (up(*a) != up(*b)) { ok = 0; break; }
            if (!*a || !*b) break;
            a++; b++;
        }
        if (ok && !*a && !*b) return g_errs[i].num;
    }
    return -1;
}

int luma_errno_nth(int i)
{
    if (i < 0 || i >= NERRS) return -1;
    return g_errs[i].num;
}

/* FAT 的 fat_error_t 是 Day 21 为“文件不存在/簇链断裂”临时开的小范围错误码。
 * 折算：FAT_ERR_NOT_FOUND 在 fat_open() 里表示“没有这个文件/没有这个卷”；
 * 在 fat_alloc_cluster() 里表示“没有空闲簇”（fat_vfs.h 的说明），含义由调用点决定，
 * 所以这里只做“看到这个码的最常见含义”的折算，需要区分的地方（磁盘满）由 vfs_ops
 * 在调用 FAT 之前先查空闲空间，不依赖这个折算。 */
int luma_err_from_fat(int rc)
{
    if (rc >= 0)                    return 0;
    switch (rc) {
    case FAT_ERR_INVALID_ARGS:      return -LUMA_EINVAL;
    case FAT_ERR_NOT_FOUND:         return -LUMA_ENOENT;
    case FAT_ERR_IO:                return -LUMA_EIO;
    case FAT_ERR_CHAIN_BROKEN:      return -LUMA_EUCLEAN;
    default:                        return -LUMA_EIO;
    }
}
