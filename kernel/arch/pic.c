/* kernel/arch/pic.c
 * LumaOS 5.0 - Programmable Interrupt Controller (PIC)
 * Initializes 8259A PIC chips in cascade mode
 * Maps IRQ 0-7 to ISR 32-39, IRQ 8-15 to ISR 40-47
 */

#include <stdint.h>

/* I/O port addresses for 8259A PIC */
#define PIC1_CMD  0x20    /* Master PIC command port */
#define PIC1_DATA 0x21    /* Master PIC data (IMR) port */
#define PIC2_CMD  0xA0    /* Slave PIC command port */
#define PIC2_DATA 0xA1    /* Slave PIC data (IMR) port */

/* ICW1 - Initialization Command Word 1 */
#define ICW1_ICW4     0x01    /* ICW4 needed */
#define ICW1_SINGLE   0x02    /* Single (vs cascade) mode */
#define ICW1_INTERVAL 0x04    /* Call interval 4 (vs 8) */
#define ICW1_LEVEL    0x08    /* Level-triggered (vs edge) */
#define ICW1_INIT     0x10    /* Initialization */

/* ICW4 - Initialization Command Word 4 */
#define ICW4_8086     0x01    /* 8086/8088 mode */
#define ICW4_AUTO     0x02    /* Auto (vs normal) EOI */
#define ICW4_BUF_SLAVE 0x08   /* Buffered mode, slave */
#define ICW4_BUF_MASTER 0x0C  /* Buffered mode, master */
#define ICW4_SFNM     0x10    /* Special fully nested */

/* Inline I/O helpers */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Small delay for PIC operations */
static inline void io_wait() {
    asm volatile ("jmp 1f; 1: jmp 1f; 1:");
}

/* Initialize PIC in cascade mode */
void pic_init() {
    /* Save current interrupt masks */
    uint8_t pic1_mask = inb(PIC1_DATA);
    uint8_t pic2_mask = inb(PIC2_DATA);

    /* ICW1: Start initialization in cascade mode */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: Set interrupt vector offset (base) */
    /* Master: IRQ 0 -> ISR 32 */
    outb(PIC1_DATA, 32);
    io_wait();
    /* Slave: IRQ 8 -> ISR 40 */
    outb(PIC2_DATA, 40);
    io_wait();

    /* ICW3: Configure cascade */
    /* Master: IRQ2 connected to slave (0000_0100) */
    outb(PIC1_DATA, 4);
    io_wait();
    /* Slave: Connected to master IRQ2 (0000_0010) */
    outb(PIC2_DATA, 2);
    io_wait();

    /* ICW4: Set mode */
    /* 8086 mode, normal EOI */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Restore interrupt masks (enable all for now) */
    outb(PIC1_DATA, 0x00);  /* Enable all IRQs on master */
    io_wait();
    outb(PIC2_DATA, 0x00);  /* Enable all IRQs on slave */
    io_wait();
}

/* Send End-Of-Interrupt (EOI) signal */
void pic_eoi(uint8_t irq) {
    if (irq >= 8) {
        /* Slave PIC */
        outb(PIC2_CMD, 0x20);
    }
    /* Master PIC (always) */
    outb(PIC1_CMD, 0x20);
}

/* Disable specific IRQ */
void pic_disable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
        irq = irq;
    } else {
        port = PIC2_DATA;
        irq = irq - 8;
    }

    value = inb(port) | (1 << irq);
    outb(port, value);
}

/* Enable specific IRQ */
void pic_enable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
        irq = irq;
    } else {
        port = PIC2_DATA;
        irq = irq - 8;
    }

    value = inb(port) & ~(1 << irq);
    outb(port, value);
}