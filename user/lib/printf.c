/* user/lib/printf.c - DAY72: printf 家族（printf / fprintf / sprintf / snprintf 和 v 版本）
 *
 * 结构：只有一个格式化引擎 format()，它把每个输出字符交给一个“出口”函数（sink）：
 *     fd 出口   —— 先攒进 128 字节的小缓冲区，满了/结束时用一次 write 系统调用写出
 *                  （比 Day 71 的“一个字符一次 int 0x80”快两个数量级；出错后停止写但继续计数）
 *     串出口    —— 写进调用者的缓冲区，最多 n-1 个字符并补 NUL；超出部分只计数（C99 snprintf 语义）
 *   引擎永远不会越界：长度由出口决定，格式化不需要临时大缓冲区（数字先倒着写进 24 字节的小数组）。
 *
 * 转换规格解析（C 标准 7.21.6.1）：
 *     % [标志 - + 空格 # 0]* [宽度 数字|*] [. 精度 数字|*] [长度 hh|h|l|ll|z] 转换符
 *   处理顺序 = 解析标志 -> 宽度 -> 精度 -> 长度 -> 转换符，然后“左填充 + 符号/前缀 + 精度补零 + 数字 + 右填充”。
 *   规则要点：
 *     - 有精度时忽略 0 标志（整数）；精度 0 且值 0 的整数不输出数字（"%.0d" 配 0 输出空串）。
 *     - '-' 优先于 '0'；'+' 优先于空格。
 *     - '#'：%x/%X 非 0 值加 0x/0X；%o 保证以 0 开头。%p 总是输出 0x + 小写十六进制（NULL 输出 "(nil)"）。
 *     - %c 按 unsigned char；%s 的精度是最多输出的字符数，NULL 输出 "(null)"。
 *   64 位（%lld/%llu/%llx）：i386 没有 libgcc（我们不链接它），所以 64 位除法 __udivdi3 不存在。
 *   做法：用“移位-减法”自己实现除以基数（只用 64 位的加减、移位、比较，gcc 会内联成 add/adc、shld），
 *   慢但正确，而且不引入任何外部符号。32 位值走快路径（普通 / 和 %，i386 的 div 指令）。
 *
 * 不支持：%f %e %g %a（用户态还没有浮点上下文保存）、%n（有安全隐患）、%lc/%ls（宽字符）。
 */
#include "../include/luma.h"

typedef unsigned long long u64;
typedef long long          s64;

typedef struct sink {
    void (*put)(struct sink *, char);
    char  *buf;          /* 串出口：目标；fd 出口：128 字节缓冲区 */
    size_t cap;          /* 串出口：n（含 NUL 位置）；fd 出口：缓冲区大小 */
    size_t len;          /* 串出口：已“应写”总数；fd 出口：缓冲区中已有字节数 */
    int    fd;
    int    err;
    size_t total;        /* 已产生的字符总数（返回值）*/
} sink_t;

/* ---- 出口 ---- */
static void fd_flush(sink_t *s)
{
    if (s->len && !s->err) {
        size_t off = 0;
        while (off < s->len) {
            ssize_t w = write(s->fd, s->buf + off, s->len - off);
            if (w <= 0) { s->err = 1; break; }
            off += (size_t)w;
        }
    }
    s->len = 0;
}
static void put_fd(sink_t *s, char c)
{
    s->buf[s->len++] = c;
    s->total++;
    if (s->len == s->cap)
        fd_flush(s);
}
static void put_str(sink_t *s, char c)
{
    if (s->cap && s->total + 1 < s->cap)        /* 留一个位置给结尾的 NUL */
        s->buf[s->total] = c;
    s->total++;
}

static void pad(sink_t *s, char c, int n)
{
    while (n-- > 0)
        s->put(s, c);
}

/* ---- 数字转字符：倒着写进 tmp（最多 22 位：64 位八进制），返回位数 ---- */
static int utoa_rev(u64 v, unsigned base, int upper, char *tmp)
{
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if ((v >> 32) == 0) {                       /* 快路径：32 位 */
        unsigned x = (unsigned)v;
        do { tmp[n++] = dig[x % base]; x /= base; } while (x);
        return n;
    }
    /* 慢路径：64 位 / base 用移位-减法（见文件头） */
    while (v) {
        u64 q = 0, r = 0;
        for (int i = 63; i >= 0; i--) {
            r = (r << 1) | ((v >> i) & 1);
            if (r >= base) { r -= base; q |= (u64)1 << i; }
        }
        tmp[n++] = dig[(unsigned)r];
        v = q;
    }
    return n ? n : (tmp[0] = '0', 1);
}

/* ---- 引擎 ---- */
static void format(sink_t *s, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') { s->put(s, *f); continue; }
        const char *spec = f;                   /* 记住 '%' 的位置：不认识的转换符要原样输出 */
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        for (;;) {
            char c = *++f;
            if      (c == '-') left = 1;
            else if (c == '+') plus = 1;
            else if (c == ' ') space = 1;
            else if (c == '#') alt = 1;
            else if (c == '0') zero = 1;
            else break;
        }
        int width = 0;
        if (*f == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }     /* 负宽度 = '-' 标志 + 正宽度 */
            f++;
        } else
            while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int prec = -1;
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }   /* 负精度 = 没有精度 */
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        int lng = 0;                            /* 0=int, 1=long, 2=long long, -1=short, -2=char */
        if      (f[0] == 'h' && f[1] == 'h') { lng = -2; f += 2; }
        else if (f[0] == 'h')                { lng = -1; f++; }
        else if (f[0] == 'l' && f[1] == 'l') { lng = 2;  f += 2; }
        else if (f[0] == 'l')                { lng = 1;  f++; }
        else if (f[0] == 'z')                { lng = 1;  f++; }     /* size_t 在 ILP32 就是 unsigned long */
        char conv = *f;
        if (!conv) { for (; *spec; spec++) s->put(s, *spec); break; }   /* 末尾孤立的 % */

        char tmp[24];
        const char *body = tmp;                 /* 要输出的主体（不含符号/前缀/精度补零）*/
        int blen = 0, zeros = 0;
        char prefix[3]; int plen = 0;

        switch (conv) {
        case '%': s->put(s, '%'); continue;
        case 'c': tmp[0] = (char)va_arg(ap, int); blen = 1; zero = 0; break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int n = 0;
            while (str[n] && (prec < 0 || n < prec)) n++;
            body = str; blen = n; zero = 0;
            break;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            int is_signed = (conv == 'd' || conv == 'i');
            unsigned base = (conv == 'o') ? 8 : (conv == 'u' || is_signed) ? 10 : 16;
            u64 v; int neg = 0;
            if (conv == 'p') {
                v = (unsigned)(uintptr_t)va_arg(ap, void *);
                if (v == 0) { body = "(nil)"; blen = 5; zero = 0; break; }
                alt = 1; lng = 1;
            } else if (is_signed) {
                s64 sv = lng == 2 ? va_arg(ap, long long) : (s64)va_arg(ap, int);   /* long = int（ILP32）*/
                if (lng == -1) sv = (short)sv; else if (lng == -2) sv = (signed char)sv;
                neg = sv < 0;
                v = neg ? (u64)0 - (u64)sv : (u64)sv;
            } else {
                v = lng == 2 ? va_arg(ap, unsigned long long) : (u64)va_arg(ap, unsigned);
                if (lng == -1) v = (unsigned short)v; else if (lng == -2) v = (unsigned char)v;
            }
            if (prec >= 0) zero = 0;            /* 有精度时 0 标志无效 */
            blen = (v == 0 && prec == 0) ? 0 : utoa_rev(v, base, conv == 'X', tmp);
            /* utoa_rev 是倒序：翻转成正序 */
            for (int a = 0, b = blen - 1; a < b; a++, b--) { char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t; }
            if (is_signed) {
                if (neg) prefix[plen++] = '-';
                else if (plus) prefix[plen++] = '+';
                else if (space) prefix[plen++] = ' ';
            } else if (alt && base == 16 && (v != 0 || conv == 'p')) {
                prefix[plen++] = '0'; prefix[plen++] = (conv == 'X') ? 'X' : 'x';
            } else if (alt && base == 8 && (blen == 0 || tmp[0] != '0')) {
                zeros = 1;                      /* %#o：保证首位是 0 */
            }
            if (prec > blen + zeros) zeros = prec - blen;
            break;
        }
        default:                                /* 不认识：从 '%' 起原样输出 */
            for (; spec <= f; spec++) s->put(s, *spec);
            continue;
        }

        int total = plen + zeros + blen;
        int fill = width > total ? width - total : 0;
        if (!left && !zero) pad(s, ' ', fill);
        for (int i = 0; i < plen; i++) s->put(s, prefix[i]);
        if (!left && zero) pad(s, '0', fill);
        pad(s, '0', zeros);
        for (int i = 0; i < blen; i++) s->put(s, body[i]);
        if (left) pad(s, ' ', fill);
    }
}

/* ---- 公共接口 ---- */
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    sink_t s = { put_str, buf, n, 0, -1, 0, 0 };
    format(&s, fmt, ap);
    if (n)
        buf[s.total < n ? s.total : n - 1] = 0;
    return (int)s.total;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    char buf[128];
    sink_t s = { put_fd, buf, sizeof(buf), 0, fd, 0, 0 };
    format(&s, fmt, ap);
    fd_flush(&s);
    return s.err ? -1 : (int)s.total;           /* write 失败时 errno 已由 write() 设置 */
}

int vprintf(const char *fmt, va_list ap) { return vdprintf(STDOUT_FILENO, fmt, ap); }
int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, (size_t)-1 >> 1, fmt, ap); }

int printf(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vdprintf(STDOUT_FILENO, fmt, ap); va_end(ap); return r; }
int dprintf(int fd, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vdprintf(fd, fmt, ap); va_end(ap); return r; }
int sprintf(char *buf, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vsprintf(buf, fmt, ap); va_end(ap); return r; }
int snprintf(char *buf, size_t n, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
