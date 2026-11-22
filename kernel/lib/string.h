/* kernel/lib/string.h
 * LumaOS 5.0 - String library interface
 */
#ifndef LUMA_STRING_H
#define LUMA_STRING_H

#include <stddef.h>

size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    atoi(const char *s);
long   atol(const char *s);
char  *itoa(int value, char *str, int radix);
void   strrev(char *s);
void   strupper(char *s);
void   strlower(char *s);
int    isdigit(int c);
int    isalpha(int c);
int    isalnum(int c);
int    isspace(int c);

#endif /* LUMA_STRING_H */
