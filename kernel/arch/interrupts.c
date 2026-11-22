/* kernel/arch/interrupts.c - LumaOS 5.0 Interrupt Dispatch
 *
 * DAY50: the frame struct now lives in isr_context.h and its field ORDER was corrected (ds is the LOWEST slot,
 * not the one after eax); the "Stack layout" table below is the old, wrong one - see isr_context.h.
 *
 * ISR-3 (CRITICAL FIX): isr_context_t was missing the 'ds' field.
 *   The asm stub pushes DS (as 32-bit) between eax and int_no, so
 *   without that field ctx->int_no silently read the DS value (0x10)
 *   and every switch() fell through to default, masking all faults.
 *
 * Stack layout built by interrupts.asm (low → high):
 *   [esp+ 0] edi
 *   [esp+ 4] esi
 *   [esp+ 8] ebp
 *   [esp+12] esp_snapshot
 *   [esp+16] ebx
 *   [esp+20] edx
 *   [esp+24] ecx
 *   [esp+28] eax
 *   [esp+32] ds           ← was missing
 *   [esp+36] int_no
 *   [esp+40] err_code
 *   [esp+44] eip  (CPU)
 *   [esp+48] cs   (CPU)
 *   [esp+52] eflags (CPU)
 */
#include <stdint.h>
#include "isr_context.h"          /* DAY50: the struct moved here so syscall.c can share it */

extern void pic_eoi(uint8_t irq);
extern void syscall_dispatch(isr_context_t *ctx);   /* DAY50: kernel/syscall/syscall.c */
extern void paging_page_fault(uint32_t err, uint32_t eip, uint32_t cs);                   /* DAY53 */
extern void paging_exception_panic(uint32_t vec, uint32_t err, uint32_t eip, uint32_t cs); /* DAY53 */

static uint32_t interrupt_count = 0;

extern int  elf_fault_from_user(const isr_context_t *ctx);     /* kernel/exec/elf_fault.c (DAY56) */
extern void elf_fault_kill(const isr_context_t *ctx, uint32_t vector);
#define ELF_FAULT_CTRL_C_VEC 0x102u                                  /* = ELF_FAULT_CTRL_C (elf_run.h) */
extern int  keyboard_take_ctrl_c(void);                         /* kernel/drivers/keyboard.c (DAY56) */

void isr_handler(isr_context_t *ctx)
{
    interrupt_count++;
    switch (ctx->int_no) {
    case 0x80:                       /* DAY50: `int 0x80` = system call (kernel/syscall/syscall.h) */
        syscall_dispatch(ctx);
        break;
    /* DAY53: CPU exceptions used to be ignored here (`break`), which for a FAULT means `iret` re-executes
     * the same instruction forever. Faults now stop the system with a message. Vectors 1 (debug), 2 (NMI)
     * and 3 (breakpoint) are traps/interrupts that can resume, so they still just return. */
    /* DAY56: 出错的若是 Ring 3 的用户程序，杀掉它、回到 shell（elf_fault_kill 不返回）；
     * 若是内核自己（CPL 0）出错，仍然 panic。 */
    case 14:
        if (elf_fault_from_user(ctx)) elf_fault_kill(ctx, 14);
        paging_page_fault(ctx->err_code, ctx->eip, ctx->cs);
        break;
    case 1: case 2: case 3:
        break;
    case 0: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
    case 12: case 13: case 15: case 16: case 17: case 18: case 19: case 20:
    case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28:
    case 29: case 30: case 31:
        if (elf_fault_from_user(ctx)) elf_fault_kill(ctx, ctx->int_no);
        paging_exception_panic(ctx->int_no, ctx->err_code, ctx->eip, ctx->cs);
        break;
    default: break;
    }
}

extern void keyboard_irq_handler(void);
extern void mouse_irq_handler(void);

void irq_handler(isr_context_t *ctx)
{
    uint32_t irq = ctx->int_no - 32;
    interrupt_count++;
    switch (irq) {
    case 0:  break;
    case 1:  keyboard_irq_handler();
             /* DAY56: Ctrl+C 且被打断的是 Ring 3 的用户程序 -> 终止它。先给 PIC 发 EOI，否则键盘
              * 中断不会再来（elf_fault_kill 不返回，下面的 pic_eoi 走不到）。 */
             if (keyboard_take_ctrl_c() && elf_fault_from_user(ctx)) {
                 pic_eoi((uint8_t)irq);
                 elf_fault_kill(ctx, ELF_FAULT_CTRL_C_VEC);
             }
             break;
    case 2:  break;
    case 12: mouse_irq_handler();    break;
    default: break;
    }
    pic_eoi((uint8_t)irq);
}

void     enable_interrupts(void)  { __asm__ volatile ("sti"); }
void     disable_interrupts(void) { __asm__ volatile ("cli"); }
uint32_t get_interrupt_count(void)  { return interrupt_count; }
void     reset_interrupt_count(void){ interrupt_count = 0; }

uint32_t interrupts_enabled(void)
{
    uint32_t f;
    __asm__ volatile ("pushf; pop %0" : "=r"(f));
    return (f >> 9) & 1;
}
