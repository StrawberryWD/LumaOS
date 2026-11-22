/* kernel/core/bootinfo.h - DAY59/61: what the loader tells the kernel.
 *
 * Both loaders (hard disk: boot/loader.asm, CD: loader.asm -DCDBOOT) leave a 32-byte block at
 * physical 0x0600 (conventional memory below the loader, untouched by the kernel image at 0x10000):
 *     +0  "LBI1"        magic
 *     +4  u8  drive     BIOS drive number the loader booted from (0x80 = first HDD, 0xE0.. = CD)
 *     +5  u8  media     0 = hard disk, 1 = CD-ROM (El Torito No-Emulation)
 *     +8  u32 kernel_lba, +12 kernel_bytes    where KERNEL.BIN is on the ISO (2048-byte sectors)
 *     +16 u32 lmfs_lba,   +20 lmfs_bytes      the LMFS system-resource image on the ISO
 *     +24 u32 iso_sectors                     size of the whole ISO
 * All CD fields are 0 on a hard-disk boot. boot_info_init() copies the block ONCE (before anything
 * can overwrite low memory) so the rest of the kernel uses g_boot, not the raw address. */
#ifndef LUMAOS_BOOTINFO_H
#define LUMAOS_BOOTINFO_H
#include <stdint.h>

#define BOOT_MEDIA_HD  0
#define BOOT_MEDIA_CD  1

typedef struct {
    int      valid;          /* magic found */
    uint8_t  drive;
    uint8_t  media;
    uint8_t  chs;            /* DAY69: 1 = loader had no INT 13h extensions, kernel was read via CHS */
    uint32_t kernel_lba, kernel_bytes;
    uint32_t lmfs_lba, lmfs_bytes;
    uint32_t iso_sectors;
} boot_info_t;

extern boot_info_t g_boot;

void        boot_info_init(void);
int         boot_used_chs(void);              /* DAY69 */
int         boot_is_cd(void);                 /* live-CD mode (no hard disk assumed) */
const char *boot_media_name(void);            /* "CD-ROM (El Torito)" / "Hard disk (BIOS)" / "BIOS" */
#endif
