/* user/include/luma.h - DAY71: LumaOS 用户态 C 库（libluma）的公共头文件
 *
 * 这是用户程序（.sw）第一次有“标准库”的样子。以前每个 .sw 自己内嵌 `int $0x80` 汇编（见 hello.sw、
 * err.sw）；从今天起它们 #include 这个头文件，链接 libluma.a，写 `int main(int argc, char **argv)`。
 *
 * 分层（从下到上）：
 *   crt0.c        程序入口 _start：内核按 Day 46~49 的约定调用 _start(api, argc, argv)，crt0 转去调
 *                 main(argc, argv)，main 返回后用 SYS_EXIT 把返回值当退出码交给内核
 *                 （main 不返回就直接 exit 也行）。
 *   luma_syscall  `int 0x80` 的内联封装（本文件里的 luma_syscall0..4）。ABI 见 kernel/syscall/syscall.h。
 *   libluma.c     C 库函数：putchar / fputc / puts / write / exit / strlen / strerror，以及全局 errno。
 *   Day 72+       printf、malloc/free、fopen/fread/fclose 会继续加在这个库里。
 *
 * 错误处理约定（和 C 标准库一致，和内核不同）：
 *   内核的系统调用直接返回 -LUMA_Exxx（见 errno.h）。库函数把它翻译成 C 习惯：失败返回 -1（或 EOF），
 *   并把错误号（正数）放进全局变量 errno。用户程序有自己的数据段，所以能有 errno 变量——内核做不到。
 *
 * 为什么 putchar 今天不做缓冲：
 *   每个 putchar 一次系统调用，最简单、顺序绝对正确（stdout 与 stderr 交错输出不会乱序，程序崩了也不会
 *   丢最后几个字符）。代价是慢（一次 int 0x80 约几百个周期）。Day 72 做 printf 时先把整串格式化到缓冲区，
 *   再一次 write；putchar 的行缓冲留到有 fflush/exit 钩子之后再说。
 *
 * 这个头文件不依赖内核任何头文件，也不依赖主机的 C 库（-ffreestanding -nostdlib）。
 */
#ifndef LUMA_USER_H
#define LUMA_USER_H

/* ---- 基本类型（i386，ILP32）---- */
typedef unsigned int   size_t;
typedef int            ssize_t;
#define NULL ((void *)0)
#define EOF  (-1)

/* ---- 系统调用号（与 kernel/syscall/syscall.h 一致；tests/day71 检查它们没有漂移）---- */
#define SYS_NOP       0
#define SYS_EXIT      1
#define SYS_WRITE     2
#define SYS_GETABI    3
#define SYS_STRERROR  4

/* ---- 标准文件描述符 ---- */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1     /* 程序所在终端，默认色 */
#define STDERR_FILENO  2     /* 同一终端，红色 */

/* ---- 错误号（与 kernel/core/errno.h 一致）---- */
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EIO          5
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EBUSY       16
#define EEXIST      17
#define ENODEV      19
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define EMFILE      24
#define EFBIG       27
#define ENOSPC      28
#define EROFS       30
#define ERANGE      34
#define ENAMETOOLONG 36
#define ENOSYS      38
#define ENOTEMPTY   39
#define EUCLEAN    117

/* ---- 系统调用封装：eax=号，ebx/ecx/edx/esi 参数，返回 eax（负数 = -errno）----
 * 内核保证除 eax 外所有寄存器不变，所以不需要声明 clobber，只要 "memory"（内核可能读/写指针指向的内存）。 */
static inline int luma_syscall0(int nr)
{
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(nr) : "memory");
    return r;
}
static inline int luma_syscall1(int nr, int a)
{
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(nr), "b"(a) : "memory");
    return r;
}
static inline int luma_syscall3(int nr, int a, int b, int c)
{
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

/* ---- C 库 ---- */
extern int errno;                         /* 最近一次失败的库函数设置；成功不清零（与 C 标准一致） */

size_t      strlen(const char *s);
const char *strerror(int errnum);         /* 错误号 -> 英文描述；返回的缓冲区属于库（非线程安全，今天只有一个线程） */

ssize_t write(int fd, const void *buf, size_t len);   /* 一次最多写 4096 字节（短写），失败 -1 + errno */
int     putchar(int c);                   /* 写一个字符到 stdout；成功返回该字符（按 unsigned char），失败 EOF + errno */
int     fputc(int c, int fd);             /* 同上，写到任意 fd（LumaOS 还没有 FILE*，Day 74 才有）*/
int     puts(const char *s);              /* 写字符串和一个换行到 stdout；成功返回非负数，失败 EOF */
int     fputs(const char *s, int fd);     /* 写字符串（不加换行）到 fd */
void    exit(int code) __attribute__((noreturn));      /* SYS_EXIT，永不返回 */

int main(int argc, char **argv);          /* 用户程序提供 */

#endif
