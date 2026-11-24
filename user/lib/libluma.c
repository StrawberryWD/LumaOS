/* user/lib/libluma.c - DAY71: libluma，第一批函数：putchar / fputc / puts / fputs / write / exit / strlen / strerror
 *
 * 全部建立在两个系统调用上：SYS_WRITE(2) 和 SYS_EXIT(1)，strerror 用 SYS_STRERROR(4)（Day 70）。
 * 编译选项见 tools/build_user.sh：-ffreestanding -nostdlib，所以这里不能调用任何主机 libc 函数。
 */
#include "../include/luma.h"

int errno;

/* 把内核的“负数 = -errno”翻译成 C 习惯：成功返回原值，失败返回 -1 并设置 errno */
static int ret_errno(int r)
{
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}


/* DAY74: 文件系统调用的 C 封装（内核一侧见 kernel/syscall/fd.h） */
int open(const char *path, int flags)             { return ret_errno(luma_syscall3(SYS_OPEN, (int)path, flags, 0)); }
ssize_t read(int fd, void *buf, size_t len)       { return ret_errno(luma_syscall3(SYS_READ, fd, (int)buf, (int)len)); }
int close(int fd)                                 { return ret_errno(luma_syscall3(SYS_CLOSE, fd, 0, 0)); }
int lseek(int fd, int off, int whence)            { return ret_errno(luma_syscall3(SYS_LSEEK, fd, off, whence)); }

/* DAY75 */
int readdir(const char *path, unsigned index, dirent_t *out)
{ return ret_errno(luma_syscall3(SYS_READDIR, (int)path, (int)index, (int)out)); }
int stat(const char *path, stat_t *st)            { return ret_errno(luma_syscall3(SYS_STAT, (int)path, (int)st, 0)); }
char *getcwd(char *buf, size_t size)
{
    int r = luma_syscall3(SYS_GETCWD, (int)buf, (int)size, 0);
    if (r < 0) { errno = -r; return NULL; }
    return buf;
}

ssize_t write(int fd, const void *buf, size_t len)
{
    return ret_errno(luma_syscall3(SYS_WRITE, fd, (int)buf, (int)len));
}

/* 内核一次最多写 4096 字节并返回实际字节数（短写）；这里的辅助函数循环到写完或出错 */
int write_all(int fd, const char *s, size_t n)
{
    while (n) {
        int w = (int)write(fd, s, n);
        if (w < 0)
            return -1;
        if (w == 0) {                    /* 防止死循环：内核不应返回 0（n > 0），但别信任它 */
            errno = EIO;
            return -1;
        }
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

int dputc(int c, int fd)
{
    unsigned char ch = (unsigned char)c;        /* C 标准：按 unsigned char 写出，返回值也是它 */
    if (write_all(fd, (const char *)&ch, 1) < 0)
        return EOF;
    return ch;
}

int putchar(int c)
{
    return dputc(c, STDOUT_FILENO);
}

int dputs(const char *s, int fd)
{
    if (write_all(fd, s, strlen(s)) < 0)
        return EOF;
    return 0;
}

int puts(const char *s)
{
    if (dputs(s, STDOUT_FILENO) == EOF)
        return EOF;
    if (putchar('\n') == EOF)
        return EOF;
    return 1;                                   /* 非负即成功（C 标准没有规定具体值） */
}

void exit(int code)
{
    luma_syscall1(SYS_EXIT, code);
    for (;;)                                    /* 内核不会让 SYS_EXIT 返回；万一返回也别跑进随机内存 */
        ;
}

const char *strerror(int errnum)
{
    static char buf[80];
    int n = luma_syscall3(SYS_STRERROR, errnum, (int)buf, (int)sizeof(buf));
    if (n < 0 && n != -ERANGE) {                /* 理论上不会发生；给一个固定的答案 */
        buf[0] = '?'; buf[1] = 0;
    }
    return buf;                                 /* ERANGE 时内核已写入截断后以 NUL 结尾的前 79 字节 */
}
