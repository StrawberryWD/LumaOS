/* kernel/core/panic.c
 * LumaOS 5.0 - Panic Handler
 * Displays error message and halts system
 */

#include "kernel.h"

void panic(const char *msg) {
    /* Disable interrupts to prevent further damage */
    disable_interrupts();
    
    /* Clear screen for visibility */
    console_clear();
    
    /* Display panic message */
    console_write_color("\n╔════════════════════════════════════════╗\n", COLOR_ERROR);
    console_write_color("║          KERNEL PANIC                   ║\n", COLOR_ERROR);
    console_write_color("╚════════════════════════════════════════╝\n\n", COLOR_ERROR);
    
    console_write_color("Error: ", COLOR_ERROR);
    console_write_color(msg, COLOR_ERROR);
    console_write_color("\n\n", COLOR_ERROR);
    
    console_write_color("System halted. Press power button to restart.\n", COLOR_ERROR);
    
    /* Halt forever */
    for (;;) {
        asm volatile ("cli; hlt");
    }
}
