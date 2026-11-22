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
 *   printf.c      DAY72：printf 家族（见下面声明）。
 *   Day 73+       malloc/free、fopen/fread/fclose 会继续加在这个库里。
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
typedef unsigned int   uintptr_t;
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, t)   __builtin_va_arg(v, t)
#define NULL ((void *)0)
#define EOF  (-1)

/* ---- 系统调用号（与 kernel/syscall/syscall.h 一致；tests/day71 检查它们没有漂移）---- */
#define SYS_NOP       0
#define SYS_EXIT      1
#define SYS_WRITE     2
#define SYS_GETABI    3
#define SYS_STRERROR  4
#define SYS_BRK       6          /* DAY73 */
#define SYS_OPEN      7          /* DAY74 */
#define SYS_READ      8
#define SYS_CLOSE     9
#define SYS_LSEEK    10

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
/* DAY73: <string.h> 里最基本的几个（malloc/printf 的使用者天天要用；gcc 也会在拷贝结构体时自己生成 memcpy 调用） */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);

/* DAY73: 动态内存。malloc 家族建立在 sbrk（SYS_BRK）之上，见 user/lib/malloc.c 文件头。
 *   malloc(n)       返回 8 字节对齐的块（至少 n 字节，内容未初始化）；失败 NULL + errno=ENOMEM；malloc(0) 返回一个可 free 的小块
 *   free(p)         p 必须是 malloc 家族返回的、还没释放的指针，或 NULL（什么也不做）；非法指针/重复释放 -> 报错并 exit(134)
 *   calloc(a,b)     a*b 字节清零（乘法溢出 -> NULL + ENOMEM）
 *   realloc(p,n)    改变块大小并保留内容；p==NULL 等于 malloc；n==0 等于 free 并返回 NULL；失败返回 NULL 且原块不变
 *   sbrk(incr)      移动 program break，返回旧值；失败 (void *)-1 + errno。不要和 malloc 混用 sbrk 来“偷”堆内存
 *   brk(addr)       把 break 设到 addr；成功 0，失败 -1 + errno
 *   malloc_check()  遍历整个堆检查所有不变量，0 = 完好，其它值 = 第几条被破坏（调试/测试用）
 *   malloc_info()   统计：堆总大小、已分配/空闲字节与块数、顶部空闲块大小 */
void *malloc(size_t n);
void  free(void *p);
void *calloc(size_t a, size_t b);
void *realloc(void *p, size_t n);
void *sbrk(int incr);
int   brk(void *addr);
typedef struct { size_t heap, used, free_bytes, top, nused, nfree; } malloc_info_t;
void  malloc_info(malloc_info_t *mi);
int   malloc_check(void);
const char *strerror(int errnum);         /* 错误号 -> 英文描述；返回的缓冲区属于库（非线程安全，今天只有一个线程） */

typedef struct luma_file FILE;            /* DAY74：见下面“文件与 <stdio.h>” */
ssize_t write(int fd, const void *buf, size_t len);   /* 一次最多写 4096 字节（短写），失败 -1 + errno */
int     putchar(int c);                   /* 写一个字符到 stdout；成功返回该字符（按 unsigned char），失败 EOF + errno */
int     dputc(int c, int fd);             /* 同上，写到任意 fd（DAY74：FILE* 版的 fputc 在下面；这是按 fd 写的底层版本）*/
int     puts(const char *s);              /* 写字符串和一个换行到 stdout；成功返回非负数，失败 EOF */
int     dputs(const char *s, int fd);     /* 写字符串（不加换行）到 fd */
/* DAY72: 格式化输出。支持：%d %i %u %x %X %o %c %s %p %%；标志 - + 空格 # 0；宽度和精度（含 *）；
 * 长度修饰 hh h l ll z。不支持 %f/%e/%g（没有浮点）和 %n；遇到不认识的转换符原样输出（"%q" 输出 "%q"）。
 * 返回值：printf/fprintf 返回写出的字符数（出错 -1 + errno）；snprintf 返回“如果缓冲区足够大本应写出的字符数”
 * （与 C99 一致，所以可以用 snprintf(NULL,0,...) 先量长度）。%s 传 NULL 输出 "(null)"。 */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* POSIX：写到 fd */
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));  /* DAY74：写到 FILE*（stdout/stderr）*/
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* ===================================================================================
 * DAY74: 文件与 <stdio.h>（user/lib/stdio.c）。内核一侧见 kernel/syscall/fd.h。
 * ===================================================================================
 *  底层（POSIX 风格，直接对应系统调用）：
 *    open(path, O_RDONLY)  -> fd（>= 3）或 -1 + errno；今天只能只读
 *    read(fd, buf, n)      -> 读到的字节数，0 = 文件尾，-1 + errno；一次最多 64KB（短读）
 *    close(fd)             -> 0 / -1
 *    lseek(fd, off, whence)-> 新偏移 / -1（SEEK_SET/CUR/END；越过文件尾合法）
 *  C 标准的流 FILE*：
 *    fopen(path, "r"|"rb"|"rt") -> FILE* 或 NULL + errno；其它模式（"w" "a" "+"）-> NULL + ENOSYS（写文件还没有）
 *    fclose(f)             -> 0 / EOF；之后 f 不能再用。程序忘记 fclose，内核在程序结束时替它关 fd，但 FILE 的内存随进程消失
 *    fread(p, size, n, f)  -> 完整读到的“项”数（可能 < n：到文件尾或出错，用 feof/ferror 区分）
 *    fgetc/getc, ungetc(c, f)（只保证回退 1 个字符）, fgets(s, n, f)（读到换行或 n-1 个字符；什么都没读到返回 NULL）
 *    fseek/ftell/rewind    -> 按“逻辑位置”（用户已经读到哪）工作，缓冲区里预读的部分会被正确扣掉
 *    feof/ferror/clearerr/fileno/fflush(只读流上什么也不做，返回 0)
 *    stdin/stdout/stderr   -> fd 0/1/2 的 FILE*；stdin 今天没有数据（读它 -> EOF + ferror）；stdout/stderr 不缓冲
 *    fputc/fputs/fwrite/fprintf/vfprintf 可写到 stdout/stderr；对只读的文件流 -> EOF/0 + EBADF
 *    perror(s)             -> 往 stderr 写 "s: <strerror(errno)>\n"
 *  FILE 带 512 字节的读缓冲（和 malloc 一起一次分配），读一个字符不再是一次系统调用。 */
#define O_RDONLY  0
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
extern FILE *stdin, *stdout, *stderr;
int     open(const char *path, int flags);
ssize_t read(int fd, void *buf, size_t len);
int     close(int fd);
int     lseek(int fd, int off, int whence);
FILE   *fopen(const char *path, const char *mode);
int     fclose(FILE *f);
size_t  fread(void *p, size_t size, size_t n, FILE *f);
size_t  fwrite(const void *p, size_t size, size_t n, FILE *f);
int     fgetc(FILE *f);
int     getc(FILE *f);
int     getchar(void);
int     ungetc(int c, FILE *f);
char   *fgets(char *s, int n, FILE *f);
int     fputc(int c, FILE *f);
int     fputs(const char *s, FILE *f);
int     fseek(FILE *f, long off, int whence);
long    ftell(FILE *f);
void    rewind(FILE *f);
int     feof(FILE *f);
int     ferror(FILE *f);
void    clearerr(FILE *f);
int     fileno(FILE *f);
int     fflush(FILE *f);
void    perror(const char *s);

void    exit(int code) __attribute__((noreturn));      /* SYS_EXIT，永不返回 */

int main(int argc, char **argv);          /* 用户程序提供 */

#endif
