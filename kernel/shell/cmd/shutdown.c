/* kernel/shell/cmd/shutdown.c
 * LumaOS 5.0 - System Shutdown / QEMU power-off
 *
 * Shutdown methods (in order of preference):
 *   1. ACPI PM1a control register: write SLP_TYP S5 + SLP_EN → powers off.
 *      QEMU emulates this at port 0x0604 (default ACPI PIIX4 PM base).
 *   2. QEMU isa-debug-exit: writing any value to port 0x0501 makes QEMU
 *      exit (requires -device isa-debug-exit,iobase=0x501,iosize=0x1).
 *   0. [DAY70+] 真正的 ACPI 关机：读固件的 ACPI 表（kernel/core/acpi.c），用表里给出的 PM1a 端口和 S5 睡眠类型。
 *      QEMU、VMware、VirtualBox 都认；老的“写死 0x604”只对 QEMU 有效。
 *   3. PS/2 reset as last resort (causes reboot not clean shutdown,
 *      but better than hanging).
 */
#include "../../core/kernel.h"
#include "../../core/acpi.h"
#include "../../gui/gui.h"
#include "../shell.h"

#define ACPI_PM1a_CNT   0x0604   /* QEMU PIIX4 ACPI PM1a control */
#define ACPI_SLP_EN     0x2000   /* SLP_EN bit  */
#define ACPI_S5_SLP_TYP 0x1C00  /* SLP_TYP = 7 (S5) for QEMU */
#define QEMU_EXIT_PORT  0x0501   /* isa-debug-exit port */

static void spin(int n)
{
    for (volatile int i = 0; i < n; i++)
        asm volatile("pause");
}

static void do_shutdown(void)
{
    disable_interrupts();

    /* --- Method 0: 解析 ACPI 表再关机（VMware / VirtualBox / 真机 / QEMU 都适用）--- */
    (void)acpi_poweroff();

    /* --- Method 1: ACPI S5 (power off) --- */
    /* QEMU maps PM1a_CNT_BLK at 0x0604.
     * SLP_TYP for S5 in QEMU's DSDT is 0 (bits 12:10 = 0b111 = 0x1C00
     * in some builds) but writing 0x2000 (SLP_EN alone) also works on
     * most QEMU releases.  We try both: first the combined value, then
     * just SLP_EN.                                                        */
    outw(ACPI_PM1a_CNT, (uint16_t)(ACPI_S5_SLP_TYP | ACPI_SLP_EN));
    spin(100000);
    outw(ACPI_PM1a_CNT, (uint16_t)ACPI_SLP_EN);
    spin(100000);

    /* --- Method 2: QEMU isa-debug-exit port --- */
    outb(QEMU_EXIT_PORT, 0x00);
    spin(100000);

    /* --- Method 3: Halt forever (graceful degradation) ---
     * 用 `sti; hlt` 而不是 `cli; hlt`：VMware 看到“关中断后 HLT”会弹窗 “The CPU has been disabled by the
     * guest operating system”（它认为客户机把 CPU 永久停了）；开着中断的 HLT 只是普通的空闲。 */
    for (;;) asm volatile("sti; hlt");
}

void sys_shutdown(void) { do_shutdown(); }

void cmd_shutdown(terminal_t *t, int argc, char **argv)
{
    (void)argc; (void)argv;
    terminal_puts(t, "Shutting down LumaOS 5.0...\n", TERM_COL_INFO);
    terminal_puts(t, "Goodbye!\n", TERM_COL_INFO);
    vga_flip();
    spin(8000000);
    do_shutdown();
}