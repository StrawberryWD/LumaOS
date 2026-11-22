/* user/programs/printf.c - DAY72 自测程序：`run printf.sw`
 *
 * 验收："格式化输出。"  逐条用 snprintf 把结果和标准 C 的期望字符串比较（FAIL 行走 stderr），最后用 printf
 * 本身打印几行给人看。测试脚本认 "printf.sw: all checks OK" 和那几行固定输出。
 */
#include "../include/luma.h"

/* 本程序故意传 NULL 给 %s、写无效转换符 %q、截断……去测库的行为，编译器的格式检查在这里只会添乱 */
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-extra-args"
#pragma GCC diagnostic ignored "-Wformat-zero-length"
#pragma GCC diagnostic ignored "-Wformat-overflow"
#pragma GCC diagnostic ignored "-Wformat-truncation"

static int bad;

#define T(expect, ...) do { char b_[96]; int n_ = snprintf(b_, sizeof b_, __VA_ARGS__); \
    if (strcmp_(b_, expect) || n_ != (int)strlen(expect)) { \
        dprintf(STDERR_FILENO, "FAIL line %d: want [%s] got [%s] (n=%d)\n", __LINE__, expect, b_, n_); bad++; } } while (0)

static int strcmp_(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int main(int argc, char **argv)
{
    /* 整数 */
    T("42", "%d", 42);                T("-42", "%d", -42);          T("0", "%d", 0);
    T("-2147483648", "%d", (int)0x80000000u);
    T("4294967295", "%u", 0xFFFFFFFFu);
    T("ff", "%x", 255);               T("FF", "%X", 255);           T("377", "%o", 255);
    T("0xff", "%#x", 255);            T("0XFF", "%#X", 255);        T("0377", "%#o", 255);
    T("0", "%#x", 0);                 T("0", "%#o", 0);
    /* 宽度、对齐、补零、符号 */
    T("   42", "%5d", 42);            T("42   ", "%-5d", 42);       T("00042", "%05d", 42);
    T("-0042", "%05d", -42);          T("+42", "%+d", 42);          T(" 42", "% d", 42);
    T("+42", "%+ d", 42);             T("42   ", "%-05d", 42);      /* '-' 优先于 '0' */
    T("  0x2a", "%#6x", 42);          T("0x002a", "%#06x", 42);
    /* 精度 */
    T("  042", "%5.3d", 42);          T("", "%.0d", 0);            T("   ", "%3.0d", 0);
    T("00042", "%.5d", 42);           T("  00042", "%7.5d", 42);
    /* * 宽度/精度 */
    T("   42", "%*d", 5, 42);         T("42   ", "%*d", -5, 42);   T("00042", "%.*d", 5, 42);
    /* 长度修饰 */
    T("-1", "%hd", 0xFFFF);           T("65535", "%hu", 0xFFFF);    T("-1", "%hhd", 0xFF);
    T("255", "%hhu", 0x1FF);          T("123456", "%ld", 123456L);  T("7", "%zu", (size_t)7);
    T("-9223372036854775808", "%lld", (long long)0x8000000000000000ull);
    T("9223372036854775807", "%lld", 0x7FFFFFFFFFFFFFFFll);
    T("18446744073709551615", "%llu", 0xFFFFFFFFFFFFFFFFull);
    T("deadbeefcafebabe", "%llx", 0xDEADBEEFCAFEBABEull);
    T("1000000000000", "%lld", 1000000000000ll);
    T("1777777777777777777777", "%llo", 0xFFFFFFFFFFFFFFFFull);
    /* 字符与字符串 */
    T("A", "%c", 'A');                T("  A", "%3c", 'A');         T("A  ", "%-3c", 'A');
    T("hello", "%s", "hello");        T("  hi", "%4s", "hi");       T("hi  ", "%-4s", "hi");
    T("hel", "%.3s", "hello");        T("  hel", "%5.3s", "hello"); T("", "%.0s", "abc");
    T("(null)", "%s", (char *)0);     T("he", "%.*s", 2, "hello");
    /* 指针 */
    T("0x1234abcd", "%p", (void *)0x1234abcd);                      T("(nil)", "%p", (void *)0);
    /* 百分号与无效转换符 */
    T("100%", "100%%");               T("%q", "%q");                T("a%", "a%");
    T("%5", "%5");
    /* snprintf 截断语义：返回“应写”长度，缓冲区总以 NUL 结尾 */
    {
        char b[8];
        int n = snprintf(b, sizeof b, "%s", "0123456789");
        if (n != 10 || strcmp_(b, "0123456")) { dprintf(STDERR_FILENO, "FAIL trunc [%s] n=%d\n", b, n); bad++; }
        n = snprintf(b, 1, "%d", 12345);
        if (n != 5 || b[0] != 0) { dprintf(STDERR_FILENO, "FAIL n=1\n"); bad++; }
        n = snprintf(NULL, 0, "%d-%s", 12345, "ab");
        if (n != 8) { dprintf(STDERR_FILENO, "FAIL measure n=%d\n", n); bad++; }
        char big[300]; char *w = big; int len = 0;
        for (int i = 0; i < 25; i++) len += sprintf(w + len, "%03d,", i);
        if (len != 100 || strcmp_(big + 96, "024,")) { dprintf(STDERR_FILENO, "FAIL sprintf chain len=%d\n", len); bad++; }
    }

    /* 人看的输出（测试脚本按行匹配）。printf 一次最多没有长度限制：下面这行 > 128 字节，要跨缓冲区 */
    int n = printf("printf: int=%d neg=%d hex=%#x str=%s chr=%c pct=100%%\n", 42, -7, 0xBEEF, "luma", 'Z');
    printf("printf returned %d\n", n);
    printf("[%8s][%-8s][%08.3d][%+5d]\n", "right", "left", 7, 99);
    printf("u64: %llu  x64: %llx\n", 12345678901234ull, 0x123456789ABCDEF0ull);
    printf("long: ");
    for (int i = 0; i < 20; i++) printf("%s%d", i ? "," : "", i * 1111);
    printf(" end\n");
    dprintf(STDERR_FILENO, "stderr: %s %d\n", "red text", 2);
    if (argc > 1) {
        printf("args(%d):", argc - 1);
        for (int i = 1; i < argc; i++) printf(" <%s>", argv[i]);
        printf("\n");
    }
    errno = EACCES;
    printf("errno %d = %s\n", errno, strerror(errno));
    printf(bad ? "printf.sw: FAILED (%d)\n" : "printf.sw: all checks OK\n", bad);
    return bad;
}
