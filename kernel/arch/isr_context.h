/* kernel/arch/isr_context.h - LumaOS 5.0 (DAY50)
 *
 * 中断/异常/系统调用入口汇编（interrupts.asm 的 isr_common / irq_common）在栈上建好的
 * “现场”，C 代码拿到的就是指向它的指针。以前这个结构只写在 interrupts.c 里；Day 50 起
 * syscall.c 也要读写它（系统调用号和参数在 eax/ebx/ecx/...，返回值要写回 eax），所以
 * 挪到这个头文件，两边共用同一份定义。
 *
 * 布局（低地址 -> 高地址）= interrupts.asm 里 push 顺序的反过来（后压的在低地址）：
 *   ds                                         <- 最后压入，所以在最低处（被中断代码的 ds，
 *                                                 入口处随即换成内核数据段 0x10）
 *   edi esi ebp esp_snapshot ebx edx ecx eax   <- 先于 ds 压入的通用寄存器（eax 最先压，最高）
 *   int_no err_code                            <- 向量号、错误码（无错误码的向量压 0）
 *   eip cs eflags                              <- CPU 自己压的
 * 从 Ring 3 进来时 CPU 还会压 user_esp、user_ss（Day 55）；今天一切都在 Ring 0，没有这两项。
 *
 * DAY50 更正：Day 4 的 "ISR-3" 把 ds 写在 eax 之后（注释说汇编把 ds 压在 eax 与 int_no 之间）。
 * 实际汇编是 `push edi` 之后才 `push ds`，ds 在最低处。旧结构里 int_no/err_code/eip/cs 的偏移
 * 碰巧对得上，所以 isr_handler/irq_handler 一直工作；但通用寄存器全错位了一格（ctx->eax 其实是
 * ecx，ctx->ds 其实是 eax）——没人读它们，直到系统调用要读 eax、写 eax。现已改成真实布局，
 * tests/day50_test_syscall.py 里的“寄存器保留 / 返回值”检查会钉住它。
 *
 * DAY56: ds 之下又多了 gs/fs/es（最后压入，gs 在最低处）：Ring 3 程序的 es/fs/gs 可以和 ds 不同，
 * 中断返回必须原样还给它们。
 *
 * 出口时汇编把这些槽原样 pop 回寄存器——所以 C 代码改 ctx->eax，就等于改了
 * “iret 之后程序看到的 eax”。系统调用靠这一点把返回值交给调用者。 */
#ifndef LUMA_ISR_CONTEXT_H
#define LUMA_ISR_CONTEXT_H

#include <stdint.h>

typedef struct isr_context {
    uint32_t gs, fs, es;              /* DAY56: 最后压入，在最低处；被中断代码的 es/fs/gs，返回时原样恢复 */
    uint32_t ds;
    uint32_t edi, esi, ebp, esp;      /* esp = 压栈前的快照，仅供诊断，出口时丢弃 */
    uint32_t ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} __attribute__((packed)) isr_context_t;

#endif
