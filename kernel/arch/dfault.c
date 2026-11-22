/* kernel/arch/dfault.c - LumaOS 5.0 - DAY54 双重故障（#DF）处理：任务门 + 独立栈
 *
 * 为什么今天需要它：程序在 Ring 0 运行，esp 在用户栈上。用户栈下面那一页故意不映射（aspace.h 的
 * “护栏页”），栈溢出时 push 会缺页（#PF）。CPU 要把 #PF 的异常帧压到“当前 esp”——可 esp 正指向
 * 那个不存在的页，于是压栈本身再次出错，升级成双重故障（#DF，向量 8）；#DF 的处理再压栈又失败，
 * CPU 就“三重故障”直接复位——屏幕上什么都看不到。
 * 解决办法：把向量 8 做成“任务门”，指向一个有自己的栈、自己的 cr3 的 TSS。CPU 进入 #DF 时做任务切换，
 * 不再依赖那根坏掉的 esp，于是能打出一屏明确的 panic。
 * DAY55: 程序改在 Ring 3 运行后，从 Ring 3 进内核（#PF、#GP、int 0x80、IRQ）CPU 本来就会按 TSS.esp0 换到
 * 内核栈，用户栈溢出变成一次普通的 #PF（panic 屏上写着 user program (Ring 3)），不再升级成 #DF。
 * 这个任务门留着做最后的保险：内核自己的栈坏了、或者在 Ring 0 里发生的栈问题仍然靠它显示 DOUBLE FAULT。
 * 同一个文件还放着 kernel_tss（当前任务的 TSS）：esp0 由 elf_enter 在每次进用户态前设置。
 */
#include <stdint.h>
#include "../core/kernel.h"
#include "../mm/paging.h"

extern void gdt_set_tss(int n, uint32_t base, uint32_t limit);
extern void idt_set_task_gate(int num, uint16_t tss_selector);

typedef struct {
    uint32_t link, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap;
} __attribute__((packed)) tss_t;

static tss_t df_tss;
tss_t kernel_tss;                 /* DAY55: 当前任务的 TSS；Ring 3 -> Ring 0 时 CPU 从这里取 ss0/esp0 */
#define cur_tss kernel_tss
static uint8_t df_stack[4096] __attribute__((aligned(16)));

static void df_handler(void)
{
    panic("DOUBLE FAULT\n  the stack is unusable: most likely the user stack overflowed\n"
          "  into the unmapped guard page below it");
    for (;;) __asm__ volatile ("cli; hlt");
}

void dfault_init(void)
{
    df_tss.cr3 = paging_kernel_pd_phys();
    df_tss.eip = (uint32_t)df_handler;
    df_tss.eflags = 0x2;
    df_tss.esp = (uint32_t)(df_stack + sizeof(df_stack));
    df_tss.cs = 0x08;
    df_tss.ss = df_tss.ds = df_tss.es = df_tss.fs = df_tss.gs = 0x10;
    df_tss.iomap = sizeof(tss_t);
    cur_tss.iomap = sizeof(tss_t);
    cur_tss.ss0  = 0x10;                                      /* DAY55: Ring 3 进内核时用内核数据段做栈 */
    cur_tss.esp0 = 0x200000;                                  /* 初值；elf_enter 每次进用户态前会改成当前内核栈位置 */
    gdt_set_tss(5, (uint32_t)&df_tss,  sizeof(tss_t) - 1);
    gdt_set_tss(6, (uint32_t)&cur_tss, sizeof(tss_t) - 1);
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)0x30));     /* 当前任务 = GDT[6]，任务切换时在这里保存旧现场 */
    idt_set_task_gate(8, 0x28);                              /* 向量 8 -> GDT[5] */
}
