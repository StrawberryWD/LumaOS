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

/* DAY77b 设计：文件系统层（vfs_ops_write）只会“整个文件覆盖写”。所以写打开的文件在内核里有一块写缓冲
 * g_wbuf[i]（256 KB，.bss，不占内核镜像），open 时把现有内容（除非 O_TRUNC）读进去，write(fd) 只改缓冲里的字节，
 * close / fsync / 程序退出（fd_reset）时才把整个缓冲一次写回。好处：任意位置写、lseek、O_APPEND 都很自然，
 * 写回是一次原子的“覆盖写”；代价：同一个文件的别的 open 在 close 之前看不到新内容、文件最大 256 KB、
 * 最多同时 3 个写打开。fork 的子进程不继承写打开的文件（缓冲是全局的，只属于打开它的进程）。 */
static uint8_t g_wbuf[LUMA_WFD_MAX][LUMA_WBUF_MAX];
static uint8_t g_wbuf_used[LUMA_WFD_MAX];

typedef struct {
    int         used;
    vfs_node_t  node;       /* 文件元数据的私有副本：file.node 指向它，不依赖别处的节点缓冲 */
    file_t      file;       /* node 指针 + pos */
    int         wbuf;       /* DAY77b：-1 = 只读 fd；否则 g_wbuf 的下标 */
    int         acc;        /* 写 fd 的访问方式：LUMA_O_WRONLY / LUMA_O_RDWR */
    int         append;
    int         dirty;
    uint32_t    wlen;       /* 写缓冲里文件的当前长度 */
    char        path[LUMA_PATH_MAX];   /* 写回时用的完整路径 */
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

static int flush_slot(fd_slot_t *s)
{
    if (s->wbuf < 0 || !s->dirty)
        return 0;
    int r = vfs_ops_write(s->path, g_wbuf[s->wbuf], s->wlen);
    if (r < 0)
        return r;                                    /* -ENOSPC / -EIO ...：保持 dirty，fsync 可以重试 */
    s->dirty = 0;
    return 0;
}

static int slot_close(fd_slot_t *s)
{
    int r = flush_slot(s);
    if (s->wbuf >= 0) {
        g_wbuf_used[s->wbuf] = 0;
        s->wbuf = -1;
    } else if (s->node.fs && s->node.fs->close) {
        s->node.fs->close(&s->file);
    }
    s->used = 0;
    s->file.node = 0;
    g_open--;
    return r;
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

/* DAY77b：写方式打开（O_WRONLY / O_RDWR）。full 是已经解析好的完整路径。 */
static int32_t open_write(const char *full, uint32_t flags)
{
    int acc = (int)(flags & 3u);
    uint32_t size = 0; int isdir = 0;
    int r = vfs_ops_writable(full);
    if (r < 0)
        return r;
    int sr = vfs_ops_stat(full, &size, &isdir);
    if (sr == 0) {
        if (isdir) return -LUMA_EISDIR;
        if ((flags & LUMA_O_CREAT) && (flags & LUMA_O_EXCL)) return -LUMA_EEXIST;
    } else if (sr == -LUMA_ENOENT) {
        if (!(flags & LUMA_O_CREAT)) return -LUMA_ENOENT;
        size = 0;
    } else {
        return sr;
    }
    int slot = -1, wb = -1;
    for (int i = 0; i < LUMA_FD_MAX; i++)
        if (!g_fd[i].used) { slot = i; break; }
    if (slot < 0)
        return -LUMA_EMFILE;
    for (int i = 0; i < LUMA_WFD_MAX; i++)
        if (!g_wbuf_used[i]) { wb = i; break; }
    if (wb < 0)
        return -LUMA_EMFILE;                         /* 写打开的文件太多（最多 LUMA_WFD_MAX 个）*/
    if (sr == 0 && !(flags & LUMA_O_TRUNC) && size > LUMA_WBUF_MAX)
        return -LUMA_EFBIG;
    if (strlen(full) >= LUMA_PATH_MAX)
        return -LUMA_ENAMETOOLONG;
    if (sr == -LUMA_ENOENT) {
        r = vfs_ops_create(full);                    /* 父目录不存在 -ENOENT、磁盘满 -ENOSPC ... */
        if (r < 0)
            return r;
    }
    fd_slot_t *s = &g_fd[slot];
    s->wlen = 0; s->dirty = 0;
    if (sr == 0 && size > 0) {
        if (flags & LUMA_O_TRUNC) {
            s->dirty = 1;                            /* 清空是一次要写回的修改 */
        } else {
            int n = vfs_ops_read(full, g_wbuf[wb], LUMA_WBUF_MAX);
            if (n < 0)
                return n;
            s->wlen = (uint32_t)n;
        }
    }
    g_wbuf_used[wb] = 1;
    s->wbuf = wb; s->acc = acc; s->append = (flags & LUMA_O_APPEND) ? 1 : 0;
    strcpy(s->path, full);
    memset(&s->node, 0, sizeof(s->node));
    s->file.node = &s->node;
    s->file.pos = s->append ? s->wlen : 0;
    s->used = 1;
    g_open++;
    return LUMA_FD_FIRST + slot;
}

int32_t fd_open(const char *upath, uint32_t flags)
{
    char raw[LUMA_PATH_MAX], full[LUMA_PATH_MAX];
    uint32_t known = 3u | LUMA_O_CREAT | LUMA_O_EXCL | LUMA_O_TRUNC | LUMA_O_APPEND;
    if ((flags & ~known) != 0 || (flags & 3u) == 3u)
        return -LUMA_EINVAL;
    if ((flags & 3u) == LUMA_O_RDONLY && flags != LUMA_O_RDONLY)
        return -LUMA_EINVAL;                         /* CREAT/TRUNC/APPEND 只对写打开有意义 */
    int n = copy_path(raw, (uint32_t)(uintptr_t)upath, sizeof(raw));
    if (n < 0)
        return n;
    if (n == 0)
        return -LUMA_ENOENT;                         /* 空路径 */
    if (shell_resolve_path(raw, full, sizeof(full)) != 0)
        return -LUMA_ENOENT;                         /* 没有挂载卷 / 结果放不下 */
    if ((flags & 3u) != LUMA_O_RDONLY)
        return open_write(full, flags);
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
    s->wbuf = -1;
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
    if (s->wbuf >= 0) {                              /* DAY77b：读写打开的文件从写缓冲里读 */
        if (s->acc == LUMA_O_WRONLY)
            return -LUMA_EBADF;
        if (s->file.pos >= s->wlen)
            return 0;
        uint32_t n = s->wlen - s->file.pos;
        if (n > len) n = len;
        memcpy((void *)(uintptr_t)ubuf, g_wbuf[s->wbuf] + s->file.pos, n);
        s->file.pos += n;
        return (int32_t)n;
    }
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
    return slot_close(s);                            /* DAY77b：写打开的文件在这里写回，失败返回 -errno（fd 仍然被关掉） */
}

int32_t fd_fsync(int32_t fd)
{
    fd_slot_t *s = slot_of(fd);
    if (!s)
        return -LUMA_EBADF;
    return flush_slot(s);                            /* 只读 fd：什么都不用做，返回 0 */
}

int32_t fd_write(int32_t fd, uint32_t ubuf, uint32_t len)
{
    fd_slot_t *s = slot_of(fd);
    if (!s || s->wbuf < 0)
        return -LUMA_EBADF;                          /* 没打开 / 只读打开 */
    if (len == 0)
        return 0;
    if (len > LUMA_WRITE_FD_MAX)
        len = LUMA_WRITE_FD_MAX;                     /* 短写 */
    if (!luma_user_range_ok(ubuf, len))
        return -LUMA_EFAULT;
    uint32_t pos = s->append ? s->wlen : s->file.pos;
    if (pos >= LUMA_WBUF_MAX)
        return -LUMA_EFBIG;
    if (len > LUMA_WBUF_MAX - pos)
        len = LUMA_WBUF_MAX - pos;                   /* 写到上限为止（短写），下一次才报 EFBIG */
    uint8_t *b = g_wbuf[s->wbuf];
    if (pos > s->wlen)
        memset(b + s->wlen, 0, pos - s->wlen);       /* lseek 越过文件尾再写：中间补零 */
    memcpy(b + pos, (const void *)(uintptr_t)ubuf, len);
    pos += len;
    if (pos > s->wlen)
        s->wlen = pos;
    s->file.pos = pos;
    s->dirty = 1;
    return (int32_t)len;
}

/* fork：子进程拿到的 fd 表副本里去掉写打开的文件（写缓冲是全局的，只属于打开它的那个进程）。 */
void fd_state_strip_writers(void *buf)
{
    typedef struct { fd_slot_t slot[LUMA_FD_MAX]; int open; } st_t;
    st_t *st = (st_t *)buf;
    for (int i = 0; i < LUMA_FD_MAX; i++)
        if (st->slot[i].used && st->slot[i].wbuf >= 0) {
            st->slot[i].used = 0;
            st->slot[i].wbuf = -1;
            st->open--;
        }
}

int32_t fd_lseek(int32_t fd, int32_t off, uint32_t whence)
{
    fd_slot_t *s = slot_of(fd);
    if (!s)
        return -LUMA_EBADF;
    int64_t base;
    uint32_t endpos = s->wbuf >= 0 ? s->wlen : s->node.size;
    if      (whence == LUMA_SEEK_SET) base = 0;
    else if (whence == LUMA_SEEK_CUR) base = (int64_t)s->file.pos;
    else if (whence == LUMA_SEEK_END) base = (int64_t)endpos;
    else return -LUMA_EINVAL;
    int64_t np = base + (int64_t)off;
    if (np < 0 || np > 0x7FFFFFFFLL)
        return -LUMA_EINVAL;                         /* 负位置不合法；越过文件尾是允许的（读到 0 字节）*/
    s->file.pos = (uint32_t)np;
    return (int32_t)np;
}
