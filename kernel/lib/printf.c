/* kernel/lib/printf.c
 * LumaOS 5.0 - Printf Implementation
 * Simplified printf for kernel use (supports %d, %x, %s, %c, %%)
 */

#include "kernel.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* Buffer for printf output */
typedef struct {
    char *buffer;
    int pos;
    int max;
} printf_context_t;

/* Put character to buffer or console */
static void put_char(printf_context_t *ctx, char c) {
    if (ctx->buffer) {
        if (ctx->pos < ctx->max - 1) {
            ctx->buffer[ctx->pos++] = c;
        }
    } else {
        console_putc(c);
    }
}

/* Convert integer to string and output */
static void output_int(printf_context_t *ctx, int value, int radix, int width, int pad) {
    char buffer[32];
    int pos = 0;
    int sign = 0;
    unsigned int val;
    
    if (value < 0 && radix == 10) {
        sign = 1;
        val = -value;
    } else {
        val = (unsigned int)value;
    }
    
    /* Convert to string */
    if (val == 0) {
        buffer[pos++] = '0';
    } else {
        while (val) {
            int digit = val % radix;
            if (digit < 10) {
                buffer[pos++] = '0' + digit;
            } else {
                buffer[pos++] = 'a' + digit - 10;
            }
            val /= radix;
        }
    }
    
    /* Padding */
    int len = pos + (sign ? 1 : 0);
    while (len < width) {
        put_char(ctx, pad);
        len++;
    }
    
    /* Sign */
    if (sign) {
        put_char(ctx, '-');
    }
    
    /* Output in reverse order */
    while (pos > 0) {
        put_char(ctx, buffer[--pos]);
    }
}

/* Core printf implementation */
static int vprintf_internal(printf_context_t *ctx, const char *format, va_list ap) {
    int count = 0;
    
    while (*format) {
        if (*format == '%') {
            format++;
            
            /* Parse flags and width */
            int width = 0;
            char pad = ' ';
            
            if (*format == '0') {
                pad = '0';
                format++;
            }
            
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }
            
            /* Handle conversion specifiers */
            switch (*format) {
                case 'd': {
                    int value = va_arg(ap, int);
                    output_int(ctx, value, 10, width, pad);
                    count++;
                    break;
                }
                
                case 'x':
                case 'X': {
                    int value = va_arg(ap, int);
                    output_int(ctx, value, 16, width, pad);
                    count++;
                    break;
                }
                
                case 'o': {
                    int value = va_arg(ap, int);
                    output_int(ctx, value, 8, width, pad);
                    count++;
                    break;
                }
                
                case 'u': {
                    unsigned int value = va_arg(ap, unsigned int);
                    output_int(ctx, (int)value, 10, width, pad);
                    count++;
                    break;
                }
                
                case 's': {
                    const char *str = va_arg(ap, const char *);
                    if (str) {
                        int len = strlen(str);
                        while (len < width) {
                            put_char(ctx, pad);
                            len++;
                        }
                        while (*str) {
                            put_char(ctx, *str++);
                        }
                    } else {
                        const char *nil = "(null)";
                        while (*nil) {
                            put_char(ctx, *nil++);
                        }
                    }
                    count++;
                    break;
                }
                
                case 'c': {
                    char c = (char)va_arg(ap, int);
                    put_char(ctx, c);
                    count++;
                    break;
                }
                
                case 'p': {
                    int value = va_arg(ap, int);
                    put_char(ctx, '0');
                    put_char(ctx, 'x');
                    output_int(ctx, value, 16, 8, '0');
                    count++;
                    break;
                }
                
                case '%': {
                    put_char(ctx, '%');
                    count++;
                    break;
                }
                
                default:
                    put_char(ctx, '%');
                    put_char(ctx, *format);
                    count += 2;
                    break;
            }
            format++;
        } else if (*format == '\n') {
            put_char(ctx, '\r');
            put_char(ctx, '\n');
            format++;
            count += 2;
        } else {
            put_char(ctx, *format++);
            count++;
        }
    }
    
    return count;
}

/* Printf to console */
int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    
    printf_context_t ctx = {
        .buffer = NULL,
        .pos = 0,
        .max = 0
    };
    
    int result = vprintf_internal(&ctx, format, ap);
    va_end(ap);
    return result;
}

/* Printf to buffer */
int sprintf(char *buffer, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    
    printf_context_t ctx = {
        .buffer = buffer,
        .pos = 0,
        .max = 65536  /* Large enough for most cases */
    };
    
    int result = vprintf_internal(&ctx, format, ap);
    
    if (ctx.pos < ctx.max) {
        ctx.buffer[ctx.pos] = 0;
    } else {
        ctx.buffer[ctx.max - 1] = 0;
    }
    
    va_end(ap);
    return result;
}

/* Printf with size limit */
int snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    
    printf_context_t ctx = {
        .buffer = buffer,
        .pos = 0,
        .max = (int)size
    };
    
    int result = vprintf_internal(&ctx, format, ap);
    
    if (ctx.pos < (int)size) {
        ctx.buffer[ctx.pos] = 0;
    } else if (size > 0) {
        ctx.buffer[size - 1] = 0;
    }
    
    va_end(ap);
    return result;
}

/* Printf with va_list */
int vprintf(const char *format, va_list ap) {
    printf_context_t ctx = {
        .buffer = NULL,
        .pos = 0,
        .max = 0
    };
    
    return vprintf_internal(&ctx, format, ap);
}

/* Printf with va_list to buffer */
int vsprintf(char *buffer, const char *format, va_list ap) {
    printf_context_t ctx = {
        .buffer = buffer,
        .pos = 0,
        .max = 65536
    };
    
    int result = vprintf_internal(&ctx, format, ap);
    
    if (ctx.pos < ctx.max) {
        ctx.buffer[ctx.pos] = 0;
    } else {
        ctx.buffer[ctx.max - 1] = 0;
    }
    
    return result;
}
