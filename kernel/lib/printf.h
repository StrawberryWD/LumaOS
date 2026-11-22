/* kernel/lib/printf.h
 * LumaOS 5.0 - Printf library interface
 */
#ifndef LUMA_PRINTF_H
#define LUMA_PRINTF_H

#include <stddef.h>
#include <stdarg.h>

int printf(const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t sz, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);

/* kprintf: alias for printf (for 2.2 compatibility) */
#define kprintf printf

#endif /* LUMA_PRINTF_H */
