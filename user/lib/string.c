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
