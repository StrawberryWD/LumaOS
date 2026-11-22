/* kernel/arch/arch.h
 * LumaOS 5.0 - Architecture definitions and low-level I/O
 * Merged from 3.5 and 2.2 versions
 */
#ifndef LUMA_ARCH_H
#define LUMA_ARCH_H

#include <stdint.h>

/* ===== Port I/O ===== */
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outl(uint16_t port, uint32_t val) {
    asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ===== Interrupt control ===== */
static inline void enable_interrupts(void)  { asm volatile ("sti"); }
static inline void disable_interrupts(void) { asm volatile ("cli"); }

/* ===== IDT structures ===== */
struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* ===== PIC ===== */
void pic_init(void);
void pic_eoi(uint8_t irq);
void pic_enable_irq(uint8_t irq);
void pic_disable_irq(uint8_t irq);

/* ===== GDT / IDT init ===== */
void gdt_init(void);
void idt_init(void);

#endif /* LUMA_ARCH_H */
