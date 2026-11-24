/* user/lib/string.c - DAY73: <string.h> 的最基本函数（-ffreestanding 下没有主机 libc，必须自己提供）
 * 写法刻意简单：逐字节循环。memcpy/memset 若以后成为热点再改成 rep movsd。 */
#include "../include/luma.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst; const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)      /* 区间可以重叠：dst 在 src 后面时从后往前拷 */
{
    unsigned char *d = dst; const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)      /* C 标准语义：不足 n 个字符时用 NUL 补满 */
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

size_t strlen(const char *s)                              /* DAY71 起就有；DAY73 搬到这里 */
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* ---- DAY75: ls / explorer 要用的几个小函数 ---- */
char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int strcasecmp(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0) return (char *)hay;
    for (; *hay; hay++)
        if (*hay == *needle && strncmp(hay, needle, n) == 0) return (char *)hay;
    return NULL;
}

int atoi(const char *s)
{
    int neg = 0, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* qsort：Shell 排序（Knuth 间隔 1,4,13,40...）。非递归、原地、不需要额外内存，所以不依赖 malloc，也不会爆栈。
 * 不稳定（相等元素的相对顺序可能变）；对几百个目录项绰绰有余。逐字节交换，size 任意。 */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    unsigned char *b = base;
    size_t gap = 1;
    while (gap < n / 3) gap = gap * 3 + 1;
    for (; gap > 0; gap /= 3) {
        for (size_t i = gap; i < n; i++) {
            for (size_t j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size) > 0; j -= gap) {
                unsigned char *x = b + (j - gap) * size, *y = b + j * size;
                for (size_t k = 0; k < size; k++) { unsigned char t = x[k]; x[k] = y[k]; y[k] = t; }
            }
        }
        if (gap == 1) break;
    }
}
