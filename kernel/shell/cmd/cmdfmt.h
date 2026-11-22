/* kernel/shell/cmd/cmdfmt.h - DAY64: 命令实现共用的小工具（printf 风格输出到终端、rdtsc、数字参数）
 *
 * 只有 static inline，哪个 .c 包含就编进哪个，不需要单独的目标文件。
 */
#ifndef LUMA_CMDFMT_H
#define LUMA_CMDFMT_H

#include <stdarg.h>
#include <stdint.h>
#include "../../lib/printf.h"
#include "../../lib/string.h"
#include "../../gui/gui.h"

/* 格式化后输出到终端（一行最多 191 字符，超出被截断） */
static inline void tprintf(terminal_t *t, uint32_t color, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    terminal_puts(t, buf, color);
}

/* CPU 时间戳计数器：没有 PIT 时钟，命令用它粗略计时（单位：CPU 周期，只用来比较快慢） */
static inline uint32_t cmd_rdtsc_lo(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    (void)hi;
    return lo;
}

/* 十进制字符串 -> 无符号数；不是纯数字返回 -1 */
static inline int cmd_parse_uint(const char *s, uint32_t *out)
{
    if (!s || !*s) return -1;
    uint32_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10u + (uint32_t)(*s - '0');
    }
    *out = v;
    return 0;
}

/* 在 dir（shell_resolve_path 的结果，如 "IDE3/docs" 或 "RAM0"）下拼出 "dir/name" */
static inline int cmd_join(char *out, uint32_t cap, const char *dir, const char *name)
{
    uint32_t dl = (uint32_t)strlen(dir), nl = (uint32_t)strlen(name);
    int slash = (dl > 0 && dir[dl - 1] != '/');
    if (dl + (uint32_t)slash + nl + 1 > cap) return -1;
    memcpy(out, dir, dl);
    if (slash) out[dl++] = '/';
    memcpy(out + dl, name, nl + 1);
    return 0;
}

#endif
