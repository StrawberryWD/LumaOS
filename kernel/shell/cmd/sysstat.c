/* kernel/shell/cmd/sysstat.c
 * LumaOS 5.0 — DAY50 (Week 8, Day 50): "定义 syscall ABI。"  验收："`int 0x80` 能进入 Kernel。"
 *
 * 用法:  sysstat        显示 syscall 通道的状态：ABI 版本、IDT[0x80] 门的内容、已进入内核的次数、
 *                       最近一次调用（调用号/参数/返回值/调用者地址）
 *        sysstat -t     内核自己执行几次 `int 0x80` 做自检：SYS_NOP、SYS_GETABI、一个不存在的号码，
 *                       并核对返回值与计数（程序一侧的检查见 tests/day50_test_syscall.py）
 *        sysstat -r     把计数清零
 *
 * 这是诊断命令（与 elfinfo/elfmap/elfrun 同类）。真正的系统调用由程序执行 `int 0x80` 发起，
 * 原理见 kernel/syscall/syscall.h。每行 < 80 列。
 */
#define LUMA_KERNEL_SIDE 1        /* syscall.h: kernel-only declarations */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../syscall/syscall.h"
#include "../../lib/string.h"
#include "../shell.h"

static void put_hex(terminal_t *t, uint32_t v, int digits)
{
    static const char hexd[] = "0123456789abcdef";
    terminal_puts(t, "0x", TERM_COL_DEFAULT);
    for (int i = digits - 1; i >= 0; i--)
        terminal_putchar(t, hexd[(v >> (i * 4)) & 0xF], TERM_COL_DEFAULT);
}

static void put_sdec(terminal_t *t, int32_t v)
{
    char tmp[12];
    int n = 0;
    uint32_t u = (v < 0) ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) terminal_putchar(t, '-', TERM_COL_DEFAULT);
    while (n) terminal_putchar(t, tmp[--n], TERM_COL_DEFAULT);
}

static void line(terminal_t *t, const char *label, uint32_t v, int is_hex, int digits)
{
    terminal_puts(t, label, TERM_COL_DEFAULT);
    if (is_hex) put_hex(t, v, digits); else put_sdec(t, (int32_t)v);
    terminal_newline(t);
}

static int check(terminal_t *t, const char *what, int ok)
{
    terminal_puts(t, ok ? "  [ok]   " : "  [FAIL] ", ok ? TERM_COL_DEFAULT : TERM_COL_ERROR);
    terminal_puts(t, what, ok ? TERM_COL_DEFAULT : TERM_COL_ERROR);
    terminal_newline(t);
    return ok;
}

void cmd_sysstat(terminal_t *t, int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        syscall_reset_stats();
        terminal_puts(t, "sysstat: counters reset\n", TERM_COL_DEFAULT);
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-t") == 0) {
        int ok = 1;
        syscall_reset_stats();
        terminal_puts(t, "sysstat: executing `int 0x80` from the kernel (Ring 0)\n", TERM_COL_DEFAULT);
        int32_t r0 = luma_syscall0(SYS_NOP);
        ok &= check(t, "SYS_NOP returns 0", r0 == 0);
        int32_t r1 = luma_syscall0(SYS_GETABI);
        ok &= check(t, "SYS_GETABI returns the ABI version", r1 == LUMA_SYSCALL_ABI_VERSION);
        int32_t r2 = luma_syscall3(999, 1, 2, 3);
        ok &= check(t, "unknown number 999 returns -ENOSYS", r2 == -LUMA_ENOSYS);
        int32_t r3 = luma_syscall3(SYS_EXIT, 5, 0, 0);
        ok &= check(t, "SYS_EXIT with no program running -> -ESRCH (no jump)", r3 == -LUMA_ESRCH);
        /* DAY51: sys_write argument checks that don't need a running program */
        static const char kbuf[8] = "kernel";
        int32_t w1 = luma_write(7, kbuf, 5);
        ok &= check(t, "write(fd 7) -> -EBADF", w1 == -LUMA_EBADF);
        int32_t w2 = luma_write(LUMA_STDIN, kbuf, 5);
        ok &= check(t, "write(fd 0) -> -EBADF (stdin is not writable)", w2 == -LUMA_EBADF);
        int32_t w3 = luma_write(LUMA_STDOUT, kbuf, 5);
        ok &= check(t, "write(1, kernel address) -> -EFAULT", w3 == -LUMA_EFAULT);
        int32_t w4 = luma_write(LUMA_STDOUT, (const void *)0, 0);
        ok &= check(t, "write(1, NULL, 0) -> 0 (nothing to check)", w4 == 0);
        terminal_puts(t, "  returned: ", TERM_COL_DEFAULT);
        put_sdec(t, r0); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_sdec(t, r1); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_sdec(t, r2); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_sdec(t, r3); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_sdec(t, w1); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_sdec(t, w2); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_sdec(t, w3); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_sdec(t, w4); terminal_newline(t);
        const syscall_stats_t *s = syscall_get_stats();
        ok &= check(t, "8 calls counted, 1 of them -ENOSYS", s->calls == 8 && s->bad_calls == 1);
        ok &= check(t, "last call recorded (nr 2, ret 0)", s->last_nr == SYS_WRITE && s->last_ret == 0);
        /* the range check itself, as a plain function */
        ok &= check(t, "range: NULL / kernel / below LO refused",
                    !luma_user_range_ok(0, 1) && !luma_user_range_ok(0x10000, 4) && !luma_user_range_ok(LUMA_USER_LO - 1, 1));
        ok &= check(t, "range: LO..HI accepted, last byte ok, one past refused",
                    luma_user_range_ok(LUMA_USER_LO, 1) && luma_user_range_ok(LUMA_USER_HI - 1, 1) &&
                    !luma_user_range_ok(LUMA_USER_HI - 1, 2) && !luma_user_range_ok(LUMA_USER_HI, 1));
        ok &= check(t, "range: 32-bit wraparound refused (0xFFFFFFF0 + 0x20)", !luma_user_range_ok(0xFFFFFFF0u, 0x20));
        ok &= check(t, "range: len 0 always ok", luma_user_range_ok(0, 0));
        ok &= check(t, "caller cs is the kernel code segment 0x08", s->last_cs == 0x08);
        terminal_puts(t, ok ? "sysstat: OK\n" : "sysstat: FAIL\n", ok ? TERM_COL_DEFAULT : TERM_COL_ERROR);
        return;
    }

    const syscall_stats_t *s = syscall_get_stats();
    syscall_gate_t g;
    syscall_read_gate(&g);
    terminal_puts(t, "syscall ABI version ", TERM_COL_DEFAULT);
    put_sdec(t, LUMA_SYSCALL_ABI_VERSION);
    terminal_puts(t, ", vector ", TERM_COL_DEFAULT);
    put_hex(t, LUMA_SYSCALL_VECTOR, 2);
    terminal_newline(t);
    terminal_puts(t, "IDT[0x80]: handler ", TERM_COL_DEFAULT);
    put_hex(t, g.handler, 8);
    terminal_puts(t, "  selector ", TERM_COL_DEFAULT);
    put_hex(t, g.selector, 2);
    terminal_puts(t, "  type ", TERM_COL_DEFAULT);
    put_hex(t, g.type_attr, 2);
    terminal_puts(t, "  DPL=", TERM_COL_DEFAULT);
    put_sdec(t, (g.type_attr >> 5) & 3);
    terminal_newline(t);
    line(t, "calls:     ", s->calls, 0, 0);
    line(t, "bad calls: ", s->bad_calls, 0, 0);
    if (s->calls) {
        terminal_puts(t, "last: nr=", TERM_COL_DEFAULT);
        put_sdec(t, (int32_t)s->last_nr);
        terminal_puts(t, " ret=", TERM_COL_DEFAULT);
        put_sdec(t, s->last_ret);
        terminal_puts(t, " from ", TERM_COL_DEFAULT);
        put_hex(t, s->last_cs, 2);
        terminal_putchar(t, ':', TERM_COL_DEFAULT);
        put_hex(t, s->last_eip, 8);
        terminal_newline(t);
        terminal_puts(t, "last args: ", TERM_COL_DEFAULT);
        for (int i = 0; i < 3; i++) { put_hex(t, s->last_args[i], 8); terminal_putchar(t, ' ', TERM_COL_DEFAULT); }
        terminal_newline(t);
    }
}
