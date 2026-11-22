/* kernel/shell/cmd/livecd.c - DAY62: `mounts` and `cdinfo`.
 *   mounts   the VFS mount table: slot name -> filesystem type (IDE0..IDE3 on a hard-disk boot,
 *            RAM0 (+CD0 from Day 63) on a live CD)
 *   cdinfo   boot medium, the BIOS boot info block, every ATAPI drive found, which one is the boot CD */
#include "../../core/kernel.h"
#include "../../core/bootinfo.h"
#include "../../core/live.h"
#include "../../core/config.h"
#include "../../drivers/block/atapi.h"
#include "../../fs/vfs/vfs.h"
#include "../shell.h"

void cmd_mounts(terminal_t *t, int argc, char **argv)
{
    (void)argc; (void)argv;
    char b[96];
    terminal_newline(t);
    if (vfs_mount_count == 0) { terminal_puts(t, "(nothing mounted)\n", TERM_COL_DEFAULT); return; }
    for (uint32_t i = 0; i < vfs_mount_count; i++) {
        char nm[16], fn[16];       /* the kernel snprintf has no %-6s padding: pad by hand */
        int k = 0;
        for (const char *p = vfs_mounts[i].name; *p && k < 6; p++) nm[k++] = *p;
        while (k < 6) nm[k++] = ' ';
        nm[k] = 0;
        k = 0;
        for (const char *p = vfs_mounts[i].fs->name; *p && k < 8; p++) fn[k++] = *p;
        while (k < 8) fn[k++] = ' ';
        fn[k] = 0;
        snprintf(b, sizeof b, "  %s  %s%s\n", nm, fn, vfs_mounts[i].fs->root == vfs_root ? "  (current root)" : "");
        terminal_puts(t, b, TERM_COL_DEFAULT);
    }
}

void cmd_cdinfo(terminal_t *t, int argc, char **argv)
{
    (void)argc; (void)argv;
    char b[112];
    terminal_newline(t);
    snprintf(b, sizeof b, "Boot medium : %s\n", boot_media_name());
    terminal_puts(t, b, TERM_COL_INFO);
    if (g_boot.valid) {
        snprintf(b, sizeof b, "BIOS drive  : 0x%02X\n", (unsigned)g_boot.drive);
        terminal_puts(t, b, TERM_COL_DEFAULT);
        if (g_boot.media == BOOT_MEDIA_CD) {
            snprintf(b, sizeof b, "ISO         : %u sectors of 2048 B\n", (unsigned)g_boot.iso_sectors);
            terminal_puts(t, b, TERM_COL_DEFAULT);
            snprintf(b, sizeof b, "Kernel      : LBA %u, %u bytes\n", (unsigned)g_boot.kernel_lba, (unsigned)g_boot.kernel_bytes);
            terminal_puts(t, b, TERM_COL_DEFAULT);
            snprintf(b, sizeof b, "LMFS image  : LBA %u, %u bytes%s\n", (unsigned)g_boot.lmfs_lba, (unsigned)g_boot.lmfs_bytes,
                     live_ram_mounted() ? "  -> RAM0" : "");
            terminal_puts(t, b, TERM_COL_DEFAULT);
        }
    }
    int n = atapi_count();
    snprintf(b, sizeof b, "ATAPI drives: %d\n", n);
    terminal_puts(t, b, TERM_COL_DEFAULT);
    for (int i = 0; i < n; i++) {
        snprintf(b, sizeof b, "  cd%d  %s  \"%s\"%s\n", i, atapi_position(i), atapi_model(i),
                 i == live_cd_index() ? "  <- boot CD" : "");
        terminal_puts(t, b, TERM_COL_DEFAULT);
    }
}

/* `font`: where the glyphs in use came from (+ checksum, the same number tools/mkfont.py prints) */
void cmd_font(terminal_t *t, int argc, char **argv)
{
    (void)argc; (void)argv;
    char b[96];
    terminal_newline(t);
    snprintf(b, sizeof b, "Font source : %s\n", font_source());
    terminal_puts(t, b, TERM_COL_INFO);
    snprintf(b, sizeof b, "Glyphs      : 95 (ASCII 32..126), 8x8\n");
    terminal_puts(t, b, TERM_COL_DEFAULT);
    snprintf(b, sizeof b, "Checksum    : 0x%08X\n", font_checksum());
    terminal_puts(t, b, TERM_COL_DEFAULT);
}

/* `config`: what ETC/LUMAOS.CFG said */
void cmd_config(terminal_t *t, int argc, char **argv)
{
    (void)argc; (void)argv;
    char b[96];
    terminal_newline(t);
    snprintf(b, sizeof b, "Config source: %s%s\n", g_cfg.source, g_cfg.loaded ? "" : "  (no file)");
    terminal_puts(t, b, TERM_COL_INFO);
    snprintf(b, sizeof b, "  theme         : %s\n", g_cfg.theme == CFG_UNSET ? "(unset)" : g_cfg.theme ? "light" : "dark");
    terminal_puts(t, b, TERM_COL_DEFAULT);
    if (g_cfg.width == CFG_UNSET) snprintf(b, sizeof b, "  resolution    : (unset)\n");
    else snprintf(b, sizeof b, "  resolution    : %dx%d\n", g_cfg.width, g_cfg.height);
    terminal_puts(t, b, TERM_COL_DEFAULT);
    snprintf(b, sizeof b, "  desktop_icons : %s\n", g_cfg.desktop_icons == CFG_UNSET ? "(unset)" : g_cfg.desktop_icons ? "1" : "0");
    terminal_puts(t, b, TERM_COL_DEFAULT);
    snprintf(b, sizeof b, "  hostname      : %s\n", g_cfg.hostname[0] ? g_cfg.hostname : "(unset)");
    terminal_puts(t, b, TERM_COL_DEFAULT);
    snprintf(b, sizeof b, "  lines %d, keys %d, errors %d\n", g_cfg.lines, g_cfg.keys, g_cfg.errors);
    terminal_puts(t, b, TERM_COL_DEFAULT);
    for (int i = 0; i < g_cfg.n_unknown; i++) {
        snprintf(b, sizeof b, "  unknown key   : %s\n", g_cfg.unknown[i]);
        terminal_puts(t, b, TERM_COL_ERROR);
    }
}
