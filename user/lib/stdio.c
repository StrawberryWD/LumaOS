/* user/lib/stdio.c - DAY74: FILE* 流（fopen/fread/fclose/fgetc/fgets/fseek/ftell...）
 *
 * 分层：
 *     fopen / fread / fgets ...      带 512 字节读缓冲的“流”（本文件）
 *         open / read / close / lseek  薄薄的系统调用封装（libluma.c）
 *             int 0x80（SYS_OPEN=7 SYS_READ=8 SYS_CLOSE=9 SYS_LSEEK=10）  -> 内核 fd 表（kernel/syscall/fd.c）
 *
 * 为什么要缓冲：逐字符 fgetc 若每次都 int 0x80（Ring 3 -> Ring 0 -> 再读磁盘缓存），慢两个数量级。
 * 流在第一次需要数据时一次 read 最多 512 字节放进 f->buf，之后的 fgetc/fgets 直接从缓冲里取，
 * 缓冲用完再 read。fread 要的数据 >= 512 字节时绕过缓冲，直接读进调用者的内存（少拷贝一次）。
 *
 * 状态（和 C 标准一致）：
 *     EOF 标志  read 返回 0 时置位；“粘住”，直到 clearerr / fseek / rewind 才清（所以到文件尾后文件再变长也不会
 *               被读到——C 标准就是这样规定的，程序要再试就得 clearerr）。
 *     错误标志  read 失败（-1）时置位，同样粘住。
 *     unget     ungetc 退回的 1 个字符（-1 = 没有）。
 *   “逻辑位置”= 内核里的文件偏移 - 缓冲区里还没被取走的字节数 - (有退回字符 ? 1 : 0)。ftell 返回它；
 *   fseek(SEEK_CUR) 也先把偏移按它换算，然后丢掉缓冲。
 *
 * 一次分配：FILE 结构和 512 字节缓冲在同一块 malloc 里（Day 73）。fopen 失败的所有路径都会把已经开的 fd 和内存退回去。
 */
#include "../include/luma.h"

#define F_EOF    1u
#define F_ERR    2u
#define F_WRITE  4u            /* 只写流（stdout/stderr）；读它 -> EBADF */
#define F_STATIC 8u            /* stdin/stdout/stderr：不可 fclose */
#define BUFSZ    512

struct luma_file {
    int           fd;
    unsigned      flags;
    int           bpos, blen;  /* 缓冲里 [bpos, blen) 是读进来、还没被取走的字节 */
    int           unget;       /* -1 = 无 */
    unsigned char buf[BUFSZ];
};

static struct luma_file f_in  = { 0, F_STATIC,           0, 0, -1, {0} };
static struct luma_file f_out = { 1, F_STATIC | F_WRITE, 0, 0, -1, {0} };
static struct luma_file f_err = { 2, F_STATIC | F_WRITE, 0, 0, -1, {0} };
FILE *stdin = &f_in, *stdout = &f_out, *stderr = &f_err;

extern int write_all(int fd, const char *s, size_t n);          /* libluma.c */

/* ---- 打开 / 关闭 ---- */
FILE *fopen(const char *path, const char *mode)
{
    if (!path || !mode || !mode[0]) { errno = EINVAL; return NULL; }
    /* 只接受 "r" 加可选的 'b'/'t'（二进制/文本在 LumaOS 没有区别）。"w" "a" "+"：写文件还没有 -> ENOSYS。 */
    if (mode[0] != 'r') {
        errno = (mode[0] == 'w' || mode[0] == 'a') ? ENOSYS : EINVAL;
        return NULL;
    }
    for (const char *m = mode + 1; *m; m++) {
        if (*m == '+') { errno = ENOSYS; return NULL; }
        if (*m != 'b' && *m != 't') { errno = EINVAL; return NULL; }
    }
    struct luma_file *f = malloc(sizeof *f);
    if (!f) return NULL;                                          /* malloc 已设 errno = ENOMEM */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { free(f); return NULL; }                         /* errno 来自内核：ENOENT / EISDIR / EMFILE ... */
    f->fd = fd; f->flags = 0; f->bpos = f->blen = 0; f->unget = -1;
    return f;
}

int fclose(FILE *f)
{
    if (!f) { errno = EBADF; return EOF; }
    if (f->flags & F_STATIC) { errno = EBADF; return EOF; }       /* 不让程序关掉标准流 */
    int r = close(f->fd);
    free(f);
    return r < 0 ? EOF : 0;
}

/* ---- 读 ---- */
static int fill(FILE *f)
{
    int n = (int)read(f->fd, f->buf, BUFSZ);
    if (n < 0) { f->flags |= F_ERR; return -1; }
    if (n == 0) { f->flags |= F_EOF; return -1; }
    f->bpos = 0; f->blen = n;
    return 0;
}

static int readable(FILE *f)
{
    if (f->flags & F_WRITE) { errno = EBADF; f->flags |= F_ERR; return 0; }
    return 1;
}

int fgetc(FILE *f)
{
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (!readable(f)) return EOF;
    if (f->bpos >= f->blen) {
        if (f->flags & F_EOF) return EOF;
        if (fill(f) < 0) return EOF;
    }
    return f->buf[f->bpos++];
}
int getc(FILE *f)  { return fgetc(f); }
int getchar(void)  { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (c == EOF || f->unget >= 0 || (f->flags & F_WRITE)) return EOF;      /* 只保证回退 1 个字符 */
    f->unget = (unsigned char)c;
    f->flags &= ~F_EOF;
    return (unsigned char)c;
}

size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    unsigned long long total = (unsigned long long)size * n;
    if (size == 0 || n == 0) return 0;
    if (total > 0x7FFFFFFFull) { errno = EINVAL; f->flags |= F_ERR; return 0; }
    if (!readable(f)) return 0;
    unsigned char *d = p;
    size_t want = (size_t)total, got = 0;
    if (f->unget >= 0 && want) { d[got++] = (unsigned char)f->unget; f->unget = -1; }
    while (got < want) {
        if (f->bpos < f->blen) {                                  /* 先用缓冲里现成的 */
            size_t k = (size_t)(f->blen - f->bpos);
            if (k > want - got) k = want - got;
            memcpy(d + got, f->buf + f->bpos, k);
            f->bpos += (int)k; got += k;
            continue;
        }
        if (f->flags & F_EOF) break;
        if (want - got >= BUFSZ) {                                /* 要得多：直接读进调用者的内存 */
            int r = (int)read(f->fd, d + got, want - got);
            if (r < 0) { f->flags |= F_ERR; break; }
            if (r == 0) { f->flags |= F_EOF; break; }
            got += (size_t)r;
        } else if (fill(f) < 0) break;
    }
    return got / size;                                            /* 不足一项的零头已经被读走（和 C 标准一致）*/
}

char *fgets(char *s, int n, FILE *f)
{
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;                                      /* 没读到任何字符（EOF 或出错）*/
    if (ferror(f)) return NULL;
    s[i] = 0;
    return s;
}

/* ---- 定位 ---- */
long ftell(FILE *f)
{
    if (f->flags & F_WRITE) { errno = EBADF; return -1; }
    int pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) return -1;
    return (long)pos - (f->blen - f->bpos) - (f->unget >= 0 ? 1 : 0);
}

int fseek(FILE *f, long off, int whence)
{
    if (f->flags & F_WRITE) { errno = EBADF; return -1; }
    if (whence == SEEK_CUR)
        off -= (f->blen - f->bpos) + (f->unget >= 0 ? 1 : 0);     /* 内核偏移比逻辑位置靠前 */
    if (lseek(f->fd, (int)off, whence) < 0) return -1;
    f->bpos = f->blen = 0; f->unget = -1;
    f->flags &= ~F_EOF;
    return 0;
}

void rewind(FILE *f)    { fseek(f, 0, SEEK_SET); f->flags &= ~F_ERR; }
int  feof(FILE *f)      { return (f->flags & F_EOF) != 0; }
int  ferror(FILE *f)    { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f)  { f->flags &= ~(F_EOF | F_ERR); }
int  fileno(FILE *f)    { return f->fd; }
int  fflush(FILE *f)    { (void)f; return 0; }                    /* 没有写缓冲，没有可冲的 */

/* ---- 写（只有 stdout/stderr 可写，不缓冲）---- */
static int writable(FILE *f)
{
    if (!(f->flags & F_WRITE)) { errno = EBADF; f->flags |= F_ERR; return 0; }
    return 1;
}
int fputc(int c, FILE *f)
{
    if (!writable(f)) return EOF;
    return dputc(c, f->fd);
}
int fputs(const char *s, FILE *f)
{
    if (!writable(f)) return EOF;
    return dputs(s, f->fd);
}
size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    unsigned long long total = (unsigned long long)size * n;
    if (size == 0 || n == 0) return 0;
    if (total > 0x7FFFFFFFull) { errno = EINVAL; return 0; }
    if (!writable(f)) return 0;
    if (write_all(f->fd, p, (size_t)total) < 0) { f->flags |= F_ERR; return 0; }
    return n;
}
int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (!writable(f)) return -1;
    return vdprintf(f->fd, fmt, ap);
}
int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

void perror(const char *s)
{
    int e = errno;                                                /* strerror 会走系统调用，先存下来 */
    if (s && *s) { dputs(s, STDERR_FILENO); dputs(": ", STDERR_FILENO); }
    dputs(strerror(e), STDERR_FILENO);
    dputs("\n", STDERR_FILENO);
}
