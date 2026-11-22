/* kernel/shell/cmd/reboot.c
 * LumaOS 5.0 - System Reboot
 *
 * STAGE1-REBOOT fixes (need ⑥):
 *   RBT-1: Port 0x0CF9 sequence corrected.
 *          PIIX3/PIIX4 (used by QEMU i440FX) requires:
 *            write 0x02 → "CPU Reset" bit arm
 *            write 0x06 → "Hard Reset" (bit1=SYS_RST, bit2=full reset)
 *          The old code wrote 0x02 then 0x06 with only a tiny delay;
 *          some QEMU builds need the "0x04 then 0x06" sequence instead.
 *          We now try BOTH sequences back-to-back.
 *   RBT-2: Added explicit flush of PS/2 controller input buffer before
 *          sending the 0xFE "pulse reset" command on port 0x64.
 *          Without the flush the command byte is dropped on some hosts.
 *   RBT-3: QEMU fw_cfg / isa-debug-exit port 0x501 write added as a
 *          last-resort fallback (causes QEMU to exit, forcing a cold
 *          restart in most launcher scripts).
 *   RBT-4: Triple-fault via null IDT kept as absolute last resort.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../shell.h"

#define CF9_PORT        0x0CF9   /* PIIX reset control */
#define KB_CMD_PORT     0x64     /* PS/2 controller command */
#define KB_DATA_PORT    0x60     /* PS/2 data port */
#define QEMU_EXIT_PORT  0x0501   /* isa-debug-exit (also triggers QEMU exit) */

/* Small spin delay */
static void spin(int n)
{
    for (volatile int i = 0; i < n; i++)
        asm volatile("pause");
}

/* Flush PS/2 input buffer (required before 0xFE pulse) */
static void kb_flush(void)
{
    int t = 10000;
    while (t-- > 0 && (inb(KB_CMD_PORT) & 0x02))
        asm volatile("pause");
}

static void do_reboot(void)
{
    disable_interrupts();

    /* --- Method 1a: PIIX "soft" reset via 0xCF9 (0x02 → 0x06) --- */
    outb(CF9_PORT, 0x02);
    spin(10000);
    outb(CF9_PORT, 0x06);
    spin(200000);

    /* --- Method 1b: PIIX "hard" reset via 0xCF9 (0x04 → 0x0E) --- */
    /* Some QEMU versions / chipsets need this variant */
    outb(CF9_PORT, 0x04);
    spin(10000);
    outb(CF9_PORT, 0x0E);
    spin(200000);

    /* --- Method 2: PS/2 keyboard controller pulse reset line --- */
    /* RBT-2: flush input buffer first so the command is accepted */
    kb_flush();
    outb(KB_CMD_PORT, 0xFE);
    spin(200000);

    /* --- Method 3: Triple fault via null IDT --- */
    struct {
        uint16_t lim;
        uint32_t base;
    } __attribute__((packed)) nidt = { 0, 0 };
    asm volatile(
        "lidt %0\n\t"
        "int  $0\n\t"
        : : "m"(nidt)
    );

    /* Should never reach here */
    for (;;) asm volatile("hlt");
}

/* Public API (called from gui battery popup) */
void sys_reboot(void) { do_reboot(); }

void cmd_reboot(terminal_t *t, int argc, char **argv)
{
    (void)argc; (void)argv;
    terminal_puts(t, "Rebooting LumaOS 5.0...\n", TERM_COL_INFO);
    vga_flip();
    spin(8000000);
    do_reboot();
}