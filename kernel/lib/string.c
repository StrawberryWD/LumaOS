/* kernel/lib/string.c
 * LumaOS 5.0 - String Library
 * Standard C string functions optimized for kernel
 */

#include "kernel.h"
#include <stdint.h>
#include <stddef.h>

/* Get string length */
size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) {
        len++;
    }
    return len;
}

/* Copy string (null-terminated) */
char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++)) {
        /* Copy each character including null terminator */
    }
    return dest;
}

/* Copy string with length limit */
char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    size_t i = 0;
    while (i < n && *src) {
        *d++ = *src++;
        i++;
    }
    while (i < n) {
        *d++ = 0;
        i++;
    }
    return dest;
}

/* Compare strings */
int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

/* Compare strings with length limit */
int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

/* Find character in string */
char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) {
            return (char *)s;
        }
        s++;
    }
    if (ch == 0) {
        return (char *)s;
    }
    return NULL;
}

/* Find last occurrence of character */
char *strrchr(const char *s, int c) {
    char ch = (char)c;
    char *result = NULL;
    while (*s) {
        if (*s == ch) {
            result = (char *)s;
        }
        s++;
    }
    if (ch == 0) {
        return (char *)s;
    }
    return result;
}

/* Find substring */
char *strstr(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

/* Copy memory area */
void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

/* Set memory area to value */
void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    unsigned char byte = (unsigned char)c;
    while (n--) {
        *p++ = byte;
    }
    return s;
}

/* Compare memory areas */
int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    while (n--) {
        if (*p1 != *p2) {
            return (int)*p1 - (int)*p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

/* Move memory area (handles overlap) */
void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    
    if (d < s) {
        /* Copy forward */
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        /* Copy backward */
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

/* Find character in memory */
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char byte = (unsigned char)c;
    while (n--) {
        if (*p == byte) {
            return (void *)p;
        }
        p++;
    }
    return NULL;
}

/* String to integer */
int atoi(const char *s) {
    int result = 0;
    int sign = 1;
    
    /* Skip whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n') {
        s++;
    }
    
    /* Handle sign */
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    
    /* Convert digits */
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    
    return sign * result;
}

/* String to long integer */
long atol(const char *s) {
    long result = 0;
    int sign = 1;
    
    /* Skip whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n') {
        s++;
    }
    
    /* Handle sign */
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    
    /* Convert digits */
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    
    return sign * result;
}

/* Integer to string */
char *itoa(int value, char *str, int radix) {
    char *ptr = str;
    char *ptr1 = str;
    char tmp_char;
    int tmp_value;
    
    if (radix < 2 || radix > 36) {
        return str;
    }
    
    /* Handle negative for base 10 */
    if (value < 0 && radix == 10) {
        *ptr++ = '-';
        value = -value;
    }
    
    /* Generate digits in reverse */
    do {
        tmp_value = value % radix;
        value /= radix;
        if (tmp_value < 10) {
            *ptr++ = '0' + tmp_value;
        } else {
            *ptr++ = 'a' + tmp_value - 10;
        }
    } while (value);
    
    *ptr-- = 0;
    
    /* Reverse string */
    while (ptr1 < ptr) {
        tmp_char = *ptr1;
        *ptr1 = *ptr;
        *ptr = tmp_char;
        ptr1++;
        ptr--;
    }
    
    return str;
}

/* Reverse string */
void strrev(char *s) {
    int len = strlen(s);
    int i, j;
    char tmp;
    
    for (i = 0, j = len - 1; i < j; i++, j--) {
        tmp = s[i];
        s[i] = s[j];
        s[j] = tmp;
    }
}

/* Uppercase string */
void strupper(char *s) {
    while (*s) {
        if (*s >= 'a' && *s <= 'z') {
            *s = *s - 'a' + 'A';
        }
        s++;
    }
}

/* Lowercase string */
void strlower(char *s) {
    while (*s) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = *s - 'A' + 'a';
        }
        s++;
    }
}

/* Check if character is digit */
int isdigit(int c) {
    return c >= '0' && c <= '9';
}

/* Check if character is alpha */
int isalpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Check if character is alphanumeric */
int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

/* Check if character is whitespace */
int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
