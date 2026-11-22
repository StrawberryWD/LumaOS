/* kernel/core/live.c - DAY62: live-CD resource loading. See live.h. */
#include "live.h"
#include "bootinfo.h"
#include "kernel.h"
#include "../lib/string.h"
#include "../drivers/block/atapi.h"
#include "../drivers/block/ramdisk.h"
#include "../fs/lmfs/lmfs_vfs.h"
#include "../fs/vfs/vfs.h"
#include "../fs/iso9660/iso9660.h"
#include "config.h"
#include "../gui/gui.h"

#define RAMDISK_BASE 0x00600000u        /* free hole: 0x500000 terminal pool ends 0x58A000, PMM starts 0x800000 */
#define RAMDISK_MAX  0x00200000u        /* 2 MB */

static int      g_cd = -1;
static int      g_ram_ok = 0;
static int      g_cd_ok = 0;
static uint32_t g_ram_bytes = 0;

int live_cd_index(void)    { return g_cd; }
int live_ram_mounted(void) { return g_ram_ok; }
int live_cd_mounted(void)  { return g_cd_ok; }
uint32_t live_ram_bytes(void) { return g_ram_bytes; }

/* PVD check: LBA 16 = type 1, "CD001", volume space size (LE u32 at +80) */
static int is_boot_cd(int idx, uint32_t want_sectors)
{
    static uint8_t sec[ATAPI_SECTOR];
    if (atapi_read(idx, 16, 1, sec) != 0) return 0;
    if (sec[0] != 1 || sec[1] != 'C' || sec[2] != 'D' || sec[3] != '0' || sec[4] != '0' || sec[5] != '1') return 0;
    uint32_t vs = (uint32_t)sec[80] | ((uint32_t)sec[81] << 8) | ((uint32_t)sec[82] << 16) | ((uint32_t)sec[83] << 24);
    return want_sectors == 0 || vs == want_sectors;
}

int live_init(void)
{
    int n = atapi_probe();
    printf("[live] ATAPI drives found: %d\n", n);
    for (int i = 0; i < n; i++)
        printf("[live]   cd%d: %s  \"%s\"\n", i, atapi_position(i), atapi_model(i));
    if (n == 0) { console_write_color("[live] FAIL: no CD-ROM drive found\n", COLOR_ERROR); return -1; }

    for (int i = 0; i < n && g_cd < 0; i++)
        if (is_boot_cd(i, g_boot.iso_sectors)) g_cd = i;
    if (g_cd < 0) { console_write_color("[live] FAIL: none of the drives holds the boot ISO\n", COLOR_ERROR); return -2; }
    printf("[live] Boot CD = cd%d (ISO9660, %u sectors)\n", g_cd, (unsigned)g_boot.iso_sectors);

    /* ---- Day 63: the ISO 9660 volume itself, read only, as "CD0" ---- */
    config_defaults();
    {
        filesystem_t *iso = iso9660_vfs_mount(atapi_block_device(g_cd));
        if (!iso) {
            console_write_color("[live] FAIL: ISO 9660 mount of the boot CD\n", COLOR_ERROR);
        } else {
            char slot[8];
            vfs_register_mount_named(iso, "CD0", slot);
            g_cd_ok = 1;
            printf("[live] ISO 9660 volume \"%s\" (%u blocks) mounted as %s (read only)\n",
                   iso9660_volume_name(), (unsigned)iso9660_volume_blocks(), slot);

            /* resources: the font ... */
            static unsigned char fbuf[1024];
            int fn = iso9660_read_file("FONTS/SYS8X8.FNT", fbuf, sizeof fbuf);
            if (fn < 0) {
                console_write_color("[live] font: FONTS/SYS8X8.FNT not on the CD - built-in font stays\n", COLOR_WARNING);
            } else if (font_load(fbuf, (unsigned)fn, "CD0/FONTS/SYS8X8.FNT") != 0) {
                console_write_color("[live] FAIL: FONTS/SYS8X8.FNT is not a valid LFNT font - built-in font stays\n", COLOR_ERROR);
            } else {
                printf("[live] font: loaded %d bytes from CD0/FONTS/SYS8X8.FNT (checksum 0x%08X)\n", fn, font_checksum());
            }
            /* ... and the configuration */
            static char cbuf[2048];
            int cn = iso9660_read_file("ETC/LUMAOS.CFG", cbuf, sizeof cbuf);
            if (cn < 0) {
                console_write_color("[live] config: ETC/LUMAOS.CFG not on the CD - defaults\n", COLOR_WARNING);
            } else {
                config_defaults();
                int keys = config_parse(cbuf, (unsigned)cn);
                g_cfg.loaded = 1;
                strcpy(g_cfg.source, "CD0/ETC/LUMAOS.CFG");
                printf("[live] config: %d keys from CD0/ETC/LUMAOS.CFG (%d lines, %d errors)\n",
                       keys, g_cfg.lines, g_cfg.errors);
            }
        }
    }

    /* ---- Day 62: LMFS system image -> RAM disk -> mount "RAM0" (registered LAST: it becomes the current root) ---- */
    uint32_t bytes = g_boot.lmfs_bytes;
    if (bytes == 0 || bytes > RAMDISK_MAX) {
        printf("[live] FAIL: LMFS image size %u not usable (max %u)\n", (unsigned)bytes, (unsigned)RAMDISK_MAX);
        return -3;
    }
    uint32_t secs = (bytes + ATAPI_SECTOR - 1) / ATAPI_SECTOR;
    if (atapi_read(g_cd, g_boot.lmfs_lba, secs, (void *)RAMDISK_BASE) != 0) {
        console_write_color("[live] FAIL: reading BOOT/LMFS.IMG from the CD\n", COLOR_ERROR);
        return -4;
    }
    printf("[live] BOOT/LMFS.IMG: %u bytes copied from CD LBA %u to RAM 0x%08X\n",
           (unsigned)bytes, (unsigned)g_boot.lmfs_lba, (unsigned)RAMDISK_BASE);
    block_device_t *rd = ramdisk_create("ram0", (void *)RAMDISK_BASE, bytes);
    filesystem_t *fs = rd ? lmfs_vfs_mount(rd, 0) : 0;
    if (!fs) { console_write_color("[live] FAIL: LMFS mount of the RAM disk\n", COLOR_ERROR); return -5; }
    char slot[8];
    if (vfs_register_mount_named(fs, "RAM0", slot) != 0) return -6;
    g_ram_ok = 1; g_ram_bytes = bytes;
    printf("[live] LMFS system image mounted as %s (RAM-backed, writable until reboot)\n", slot);
    return 0;
}

/* DAY63: called once the desktop exists. Resolution first (it re-fits the windows), then theme, then icons. */
void live_apply_config(void)
{
    if (!g_cfg.loaded) return;
    if (g_cfg.width != CFG_UNSET && g_cfg.height != CFG_UNSET) {
        if (gui_set_resolution(g_cfg.width, g_cfg.height) != 0)
            printf("[live] config: resolution %dx%d is not a supported mode - ignored\n", g_cfg.width, g_cfg.height);
    }
    if (g_cfg.theme != CFG_UNSET) gui_apply_theme(g_cfg.theme);
    if (g_cfg.desktop_icons != CFG_UNSET) g_show_desktop_icons = g_cfg.desktop_icons;
}
