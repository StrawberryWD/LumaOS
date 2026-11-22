/* kernel/shell/cmd/version.c
 * LumaOS 5.0
 *
 * BUGFIX (reported: "gui里面的是LumaOS 5.0啊"): this file was still
 * printing "LumaOS 4.1" — stale from before the Day 9-14 filesystem
 * work (kernel/fs/, kernel/drivers/block/, kernel/shell/cmd/ls.c) was
 * versioned as LumaOS 5.0 (see e.g. fat12.h/vfs.h/ls.c's own file
 * header comments). `version` is the one command whose entire job is
 * to report this number accurately, so it's corrected here to match.
 */
#include "../../core/kernel.h"
#include "../../core/version.h"
#include "../../core/bootinfo.h"
#include "../../gui/gui.h"
#include "../shell.h"

void cmd_version(terminal_t *t, int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Reviewer feedback: leave a blank line between the "IDE0>
     * version" echo and the version block below it, for readability. */
    terminal_newline(t);
    terminal_puts(t, LUMAOS_FULLNAME "\n",                  TERM_COL_INFO);
    terminal_puts(t, "  UI      : " LUMAOS_UI "\n",          TERM_COL_DEFAULT);
    terminal_puts(t, "  Arch    : i386 32-bit Protected Mode\n", TERM_COL_DEFAULT);
    {   /* DAY58b: real backend + current resolution (selectable at run time) */
        char buf[64];
        snprintf(buf, sizeof(buf), "  Display : %s %dx%d\n", vga_gfx_backend_name(),
                 vga_gfx_width(), vga_gfx_height());
        terminal_puts(t, buf, TERM_COL_DEFAULT);
    }
    {   /* DAY61: where this system was booted from (loader's boot info block) */
        char buf[64];
        snprintf(buf, sizeof(buf), "  Boot    : %s\n", boot_media_name());
        terminal_puts(t, buf, TERM_COL_DEFAULT);
    }
    terminal_puts(t, "  Kernel  : 0x00010000\n",              TERM_COL_DEFAULT);
    terminal_puts(t, "  Stack   : 0x00200000\n",              TERM_COL_DEFAULT);
    terminal_puts(t, "  BkBuf   : 0x00300000 (up to 1 MB)\n",    TERM_COL_DEFAULT);
    terminal_puts(t, "  LFB     : 0xE0000000 (BGA)\n",       TERM_COL_DEFAULT);
    terminal_puts(t, "  Compile : GCC -m32 -O2\n",            TERM_COL_DEFAULT);
    terminal_puts(t, "  Drivers : KB(E0)+Mouse IRQ\n",        TERM_COL_DEFAULT);
    terminal_puts(t, "  Power   : QEMU+ACPI+CF9\n",           TERM_COL_DEFAULT);
}