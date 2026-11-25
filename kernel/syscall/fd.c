/* kernel/syscall/fd.c - DAY74: 文件描述符表和 open/read/close/lseek 的实现。设计见 fd.h。 */
#define LUMA_KERNEL_SIDE 1
#include "fd.h"
#include "stdin.h"
#include "syscall.h"
#include "../core/errno.h"
#include "../fs/vfs/vfs.h"
#include "../fs/vfs/vfs_ops.h"
#include "../shell/shell.h"
#include "../lib/string.h"

typedef struct {
    int         used;
    vfs_node_t  node;       /* 文件元数据的私有副本：file.node 指向它，不依赖别处的节点缓冲 */
    file_t      file;       /* node 指针 + pos */
} fd_slot_t;

static fd_slot_t g_fd[LUMA_FD_MAX];
static int       g_open;

static fd_slot_t *slot_of(int32_t fd)
{
    if (fd < LUMA_FD_FIRST || fd >= LUMA_FD_FIRST + LUMA_FD_MAX)
        return 0;
    fd_slot_t *s = &g_fd[fd - LUMA_FD_FIRST];
    return s->used ? s : 0;
}

static void slot_close(fd_slot_t *s)
{
    if (s->node.fs && s->node.fs->close)
        s->node.fs->close(&s->file);
    s->used = 0;
    s->file.node = 0;
    g_open--;
}

void fd_reset(void)
{
    for (int i = 0; i < LUMA_FD_MAX; i++)
        if (g_fd[i].used)
            slot_close(&g_fd[i]);
    g_open = 0;
}

/* DAY77: 整张表（含 g_open）的保存/恢复。file.node 指向槽位自己的 node 副本，所以恢复后要把指针重新指好。 */
typedef struct { fd_slot_t slot[LUMA_FD_MAX]; int open; } fd_state_t;
_Static_assert(sizeof(fd_state_t) <= LUMA_FD_STATE_MAX, "LUMA_FD_STATE_MAX too small");
unsigned fd_state_size(void) { return (unsigned)sizeof(fd_state_t); }
void fd_state_save(void *buf)
{
    fd_state_t *st = (fd_state_t *)buf;
    memcpy(st->slot, g_fd, sizeof(g_fd));
    st->open = g_open;
}
void fd_state_load(const void *buf)
{
    const fd_state_t *st = (const fd_state_t *)buf;
    memcpy(g_fd, st->slot, sizeof(g_fd));
    g_open = st->open;
    for (int i = 0; i < LUMA_FD_MAX; i++)
        if (g_fd[i].used)
            g_fd[i].file.node = &g_fd[i].node;
}

int fd_open_count(void) { return g_open; }

/* 从用户内存拷贝 NUL 结尾的路径。每个字节单独检查（页边界会变），最长 max-1 字符。 */
static int copy_path(char *dst, uint32_t usrc, uint32_t max)
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

int32_t fd_open(const char *upath, uint32_t flags)
{
    char raw[LUMA_PATH_MAX], full[LUMA_PATH_MAX];
    if (flags != LUMA_O_RDONLY)
        return -LUMA_ENOSYS;                         /* 写/创建：还没有 */
    int n = copy_path(raw, (uint32_t)(uintptr_t)upath, sizeof(raw));
    if (n < 0)
        return n;
    if (n == 0)
        return -LUMA_ENOENT;                         /* 空路径 */
    if (shell_resolve_path(raw, full, sizeof(full)) != 0)
        return -LUMA_ENOENT;                         /* 没有挂载卷 / 结果放不下 */
    int slot = -1;
    for (int i = 0; i < LUMA_FD_MAX; i++)
        if (!g_fd[i].used) { slot = i; break; }
    if (slot < 0)
        return -LUMA_EMFILE;
    fd_slot_t *s = &g_fd[slot];
    int r = vfs_ops_open_node(full, &s->node);
    if (r < 0)
        return r;
    s->file.node = &s->node;
    s->file.pos = 0;
    if (s->node.fs->open(&s->node, &s->file) != 0)
        return -LUMA_EIO;
    s->file.node = &s->node;                         /* 以防某个后端的 open 改写了它 */
    s->used = 1;
    g_open++;
    return LUMA_FD_FIRST + slot;
}

int32_t fd_read(int32_t fd, uint32_t ubuf, uint32_t len)
{
    if (fd == 0)
        return stdin_read(ubuf, len);                /* DAY77a: 键盘（stdin.c） */
    if (fd > 0 && fd < LUMA_FD_FIRST)
        return -LUMA_EBADF;                          /* 1/2 是只写 */
    fd_slot_t *s = slot_of(fd);
    if (!s)
        return -LUMA_EBADF;
    if (len == 0)
        return 0;
    if (len > LUMA_READ_MAX)
        len = LUMA_READ_MAX;                         /* 短读 */
    if (!luma_user_range_ok(ubuf, len))
        return -LUMA_EFAULT;                         /* 整块检查在先：失败时不读任何东西、pos 不变 */
    int n = s->node.fs->read(&s->file, (uint8_t *)(uintptr_t)ubuf, len);
    if (n < 0)
        return -LUMA_EIO;
    return n;                                        /* 0 = 文件尾 */
}

int32_t fd_close(int32_t fd)
{
    fd_slot_t *s = slot_of(fd);
    if (!s)
        return -LUMA_EBADF;
    slot_close(s);
    return 0;
}

int32_t fd_lseek(int32_t fd, int32_t off, uint32_t whence)
{
    fd_slot_t *s = slot_of(fd);
    if (!s)
        return -LUMA_EBADF;
    int64_t base;
    if      (whence == LUMA_SEEK_SET) base = 0;
    else if (whence == LUMA_SEEK_CUR) base = (int64_t)s->file.pos;
    else if (whence == LUMA_SEEK_END) base = (int64_t)s->node.size;
    else return -LUMA_EINVAL;
    int64_t np = base + (int64_t)off;
    if (np < 0 || np > 0x7FFFFFFFLL)
        return -LUMA_EINVAL;                         /* 负位置不合法；越过文件尾是允许的（读到 0 字节）*/
    s->file.pos = (uint32_t)np;
    return (int32_t)np;
}
