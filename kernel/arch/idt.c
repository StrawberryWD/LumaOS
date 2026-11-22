/* kernel/arch/idt.c
 * LumaOS 5.0 - Interrupt Descriptor Table
 * Sets up 256 interrupt gates for exception and IRQ handlers
 */

#include <stdint.h>

/* IDT Entry Structure */
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  reserved;
    uint8_t  type_attr;
    uint16_t offset_high;
} __attribute__((packed)) idt_entry_t;

/* IDT Descriptor (IDTR register format) */
typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

/* Static IDT (256 entries) */
#define IDT_ENTRIES 256
static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t idt_ptr;

/* Forward declarations for interrupt handlers */
extern void isr0();   extern void isr1();   extern void isr2();   extern void isr3();
extern void isr4();   extern void isr5();   extern void isr6();   extern void isr7();
extern void isr8();   extern void isr9();   extern void isr10();  extern void isr11();
extern void isr12();  extern void isr13();  extern void isr14();  extern void isr15();

extern void isr16();  extern void isr17();  extern void isr18();  extern void isr19();
extern void isr20();  extern void isr21();  extern void isr22();  extern void isr23();
extern void isr24();  extern void isr25();  extern void isr26();  extern void isr27();
extern void isr28();  extern void isr29();  extern void isr30();  extern void isr31();   /* DAY56 */
extern void isr128();   /* DAY50: int 0x80 = system call */

extern void irq0();   extern void irq1();   extern void irq2();   extern void irq3();
extern void irq4();   extern void irq5();   extern void irq6();   extern void irq7();
extern void irq8();   extern void irq9();   extern void irq10();  extern void irq11();
extern void irq12();  extern void irq13();  extern void irq14();  extern void irq15();

/* Create an IDT entry */
static void idt_set_entry(int num, uint32_t handler, uint16_t selector, uint8_t type_attr) {
    if (num >= IDT_ENTRIES) return;

    idt[num].offset_low = (handler & 0xFFFF);
    idt[num].offset_high = (handler >> 16) & 0xFFFF;
    idt[num].selector = selector;
    idt[num].type_attr = type_attr;
    idt[num].reserved = 0;
}

/* DAY54: 任务门（type 0x85, DPL0）：CPU 收到这个向量时切换到 selector 指向的 TSS 任务——
 * 整套寄存器（含 esp、cr3）都换成 TSS 里的值，所以即使异常发生时栈已经坏了也能运行处理程序。 */
void idt_set_task_gate(int num, uint16_t tss_selector)
{
    if (num >= IDT_ENTRIES) return;
    idt[num].offset_low = 0; idt[num].offset_high = 0;
    idt[num].selector = tss_selector;
    idt[num].reserved = 0;
    idt[num].type_attr = 0x85;
}

/* Initialize IDT */
void idt_init() {
    /* IDT Pointer */
    idt_ptr.limit = (sizeof(idt_entry_t) * IDT_ENTRIES) - 1;
    idt_ptr.base = (uint32_t)&idt;

    /* Clear all entries */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_entry(i, 0, 0, 0);
    }

    /* Set up exception handlers (ISR 0-15) */
    /* Type 0x8E = Present, DPL=0, Type=1110 (32-bit interrupt gate) */
    idt_set_entry(0, (uint32_t)isr0, 0x08, 0x8E);
    idt_set_entry(1, (uint32_t)isr1, 0x08, 0x8E);
    idt_set_entry(2, (uint32_t)isr2, 0x08, 0x8E);
    idt_set_entry(3, (uint32_t)isr3, 0x08, 0x8E);
    idt_set_entry(4, (uint32_t)isr4, 0x08, 0x8E);
    idt_set_entry(5, (uint32_t)isr5, 0x08, 0x8E);
    idt_set_entry(6, (uint32_t)isr6, 0x08, 0x8E);
    idt_set_entry(7, (uint32_t)isr7, 0x08, 0x8E);
    idt_set_entry(8, (uint32_t)isr8, 0x08, 0x8E);
    idt_set_entry(9, (uint32_t)isr9, 0x08, 0x8E);
    idt_set_entry(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_entry(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_entry(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_entry(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_entry(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_entry(15, (uint32_t)isr15, 0x08, 0x8E);
    /* DAY56: 向量 16~31 */
    static void (*const isr_hi[16])() = { isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
                                          isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31 };
    for (int v = 16; v < 32; v++)
        idt_set_entry(v, (uint32_t)isr_hi[v - 16], 0x08, 0x8E);

    /* Set up IRQ handlers (IRQ 0-15, mapped to ISR 32-47) */
    idt_set_entry(32, (uint32_t)irq0, 0x08, 0x8E);
    idt_set_entry(33, (uint32_t)irq1, 0x08, 0x8E);
    idt_set_entry(34, (uint32_t)irq2, 0x08, 0x8E);
    idt_set_entry(35, (uint32_t)irq3, 0x08, 0x8E);
    idt_set_entry(36, (uint32_t)irq4, 0x08, 0x8E);
    idt_set_entry(37, (uint32_t)irq5, 0x08, 0x8E);
    idt_set_entry(38, (uint32_t)irq6, 0x08, 0x8E);
    idt_set_entry(39, (uint32_t)irq7, 0x08, 0x8E);
    idt_set_entry(40, (uint32_t)irq8, 0x08, 0x8E);
    idt_set_entry(41, (uint32_t)irq9, 0x08, 0x8E);
    idt_set_entry(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_entry(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_entry(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_entry(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_entry(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_entry(47, (uint32_t)irq15, 0x08, 0x8E);

    /* DAY50: system-call gate, vector 0x80.
     * type_attr 0xEE = Present(1) | DPL=3(11) | 0 | type 1110 (32-bit INTERRUPT gate).
     * DPL=3 is the whole point: `int n` from Ring 3 is only allowed through a gate whose DPL >= the
     * caller's CPL; every other gate above is DPL=0, so a user program can't fake an IRQ/exception with `int 14`
     * but CAN `int 0x80`.  (Everything still runs in Ring 0 today; Day 55 moves programs to Ring 3.)
     * "Interrupt" (not "trap") gate: the CPU clears IF on entry, so the dispatcher runs without being
     * interrupted by the keyboard/timer.  See kernel/syscall/syscall.h. */
    idt_set_entry(0x80, (uint32_t)isr128, 0x08, 0xEE);

    /* Load IDT into IDTR register */
    asm volatile ("lidt %0" : : "m"(idt_ptr));
}