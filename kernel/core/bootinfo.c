/* kernel/core/bootinfo.c - DAY59/61: see bootinfo.h */
#include "bootinfo.h"

boot_info_t g_boot;

void boot_info_init(void)
{
    const volatile uint8_t *p = (const volatile uint8_t *)0x600;
    uint32_t magic = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    g_boot.valid = (magic == 0x3149424Cu);                    /* "LBI1" */
    if (!g_boot.valid) { g_boot.media = BOOT_MEDIA_HD; return; }
    g_boot.drive = p[4];
    g_boot.media = p[5] == BOOT_MEDIA_CD ? BOOT_MEDIA_CD : BOOT_MEDIA_HD;
    g_boot.chs = (g_boot.media == BOOT_MEDIA_HD && p[6] == 1) ? 1 : 0;   /* DAY69 */
#define U32(o) ((uint32_t)p[o] | ((uint32_t)p[(o)+1] << 8) | ((uint32_t)p[(o)+2] << 16) | ((uint32_t)p[(o)+3] << 24))
    g_boot.kernel_lba = U32(8);  g_boot.kernel_bytes = U32(12);
    g_boot.lmfs_lba   = U32(16); g_boot.lmfs_bytes   = U32(20);
    g_boot.iso_sectors = U32(24);
#undef U32
}

int boot_used_chs(void) { return g_boot.valid && g_boot.chs; }
int boot_is_cd(void) { return g_boot.valid && g_boot.media == BOOT_MEDIA_CD; }

const char *boot_media_name(void)
{
    if (!g_boot.valid) return "BIOS";
    if (g_boot.media == BOOT_MEDIA_CD) return "CD-ROM (El Torito)";
    return g_boot.chs ? "Hard disk (BIOS, CHS fallback)" : "Hard disk (BIOS)";
}
