/* kernel/exec/elf_fault.c - LumaOS 5.0 - DAY56 用户态异常：杀掉出错的程序，而不是让内核 panic
 *
 * Day 55 之后程序在 Ring 3 运行，它犯的错（访问不存在/不属于它的页、除以 0、执行非法指令、执行特权指令……）
 * 由 CPU 以异常的形式报告。以前所有异常一律 panic，整台机器停住。现在按“谁出的错”区分：
 *   - 出错时 CPL == 3（栈上 cs 的低 2 位是 3）：错在用户程序，不在内核。内核把原因记下来，
 *     然后走和 SYS_EXIT 同一条出口（elf_sys_exit，via = 3 = “被杀死”）——一步把 esp 换回 elf_enter
 *     保存的位置，丢掉异常帧，回到 elf_run 的调用者；shell 报告一行“killed by ...”，继续工作。
 *   - 出错时 CPL == 0：内核自己的 bug，没法安全继续，仍然 panic（Day 53 的行为）。
 * 异常的入口帧在 TSS.esp0 的内核栈上，elf_sys_exit 把它整块丢掉，不需要“修复”什么现场；
 * 页表（CR3）由 elf_exec 在 elf_run 返回后切回内核页目录并销毁，页框全部归还。
 * 同一条出口也用来实现 Ctrl+C：键盘中断发生在 Ring 3 时，如果刚按下了 Ctrl+C，就“杀掉”程序（死循环程序
 * 因此可以被终止）。退出码 = 128 + 向量；Ctrl+C = 130。
 */
#include "elf_run.h"
#include "../arch/isr_context.h"
#include "../mm/paging.h"
#include "../mm/aspace.h"
#include "elf.h"

extern uint32_t elf_running;
extern void     elf_sys_exit(int code, int via);
extern void     proc_on_exit(int code, int via);     /* DAY77: kernel/proc/proc.c */

elf_fault_t elf_fault_last;

static uint32_t read_cr2_(void)
{
    uint32_t v;
    __asm__ volatile ("mov %%cr2,%0" : "=r"(v));
    return v;
}

/* 异常/中断发生在 Ring 3 且确实有程序在跑吗？ */
int elf_fault_from_user(const isr_context_t *ctx)
{
    return (ctx->cs & 3u) == 3u && elf_running;
}

void elf_fault_kill(const isr_context_t *ctx, uint32_t vector)
{
    elf_fault_last.vector = vector;
    elf_fault_last.err    = (vector == 14 || vector == 13 || vector == 17 || (vector >= 10 && vector <= 12))
                            ? ctx->err_code : 0;
    elf_fault_last.addr   = (vector == 14) ? read_cr2_() : 0;
    elf_fault_last.eip    = ctx->eip;
    proc_on_exit((int)(vector == ELF_FAULT_CTRL_C ? 130 : 128 + vector), (int)ELF_VIA_KILLED);   /* DAY77 */
    elf_sys_exit((int)(vector == ELF_FAULT_CTRL_C ? 130 : 128 + vector), (int)ELF_VIA_KILLED);   /* 不返回 */
}

/* ---- 报告文字 ---- */
static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_hex(char *p, uint32_t v)
{
    static const char h[] = "0123456789abcdef";
    *p++ = '0'; *p++ = 'x';
    for (int i = 7; i >= 0; i--) *p++ = h[(v >> (i * 4)) & 0xF];
    return p;
}
static char *put_dec(char *p, uint32_t v)
{
    char t[11]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

void elf_fault_describe(const elf_run_info_t *i, char *buf, int n)
{
    char tmp[160];
    char *p = tmp;
    if (i->fault_vector == ELF_FAULT_CTRL_C) {
        p = put_str(p, "interrupted by Ctrl+C at eip ");
        p = put_hex(p, i->fault_eip);
    } else if (i->fault_vector == 14) {
        uint32_t e = i->fault_err, a = i->fault_addr;
        if (a >= ELF_USER_STACK_BASE - 0x1000u && a < ELF_USER_STACK_BASE) {
            p = put_str(p, "stack overflow (hit the guard page at ");
            p = put_hex(p, a);
            p = put_str(p, ")");
        } else {
            p = put_str(p, "page fault: ");
            p = put_str(p, (e & 16) ? "fetch of " : (e & 2) ? "write to " : "read of ");
            p = put_hex(p, a);
            p = put_str(p, (e & 1) ? " (protection violation)" : " (not mapped)");
        }
        p = put_str(p, " at eip ");
        p = put_hex(p, i->fault_eip);
    } else {
        p = put_str(p, paging_exception_name(i->fault_vector));
        p = put_str(p, " (vector ");
        p = put_dec(p, i->fault_vector);
        p = put_str(p, ") at eip ");
        p = put_hex(p, i->fault_eip);
    }
    *p = 0;
    int k = 0;
    while (tmp[k] && k < n - 1) { buf[k] = tmp[k]; k++; }
    buf[k] = 0;
}
