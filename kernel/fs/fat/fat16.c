/* kernel/fs/fat/fat16.c
 * LumaOS 5.0 - FAT16 Mount, Cluster-Chain Walk & Root Directory Listing
 *
 * DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
 *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载一个
 *    FAT16 镜像（可以用 mkfs.fat -F 16 制作），ls 能正确列出文件。"
 *
 *   See fat16.h for the full contract of every function here. This
 *   file is the arithmetic + I/O, following the exact same shape
 *   fat12.c established: fat16_mount() is a thin wrapper around
 *   fat_common.c's shared fat_parse_bpb_common()/fat_compute_geometry()
 *   plus the one FAT16-specific BS_FilSysType check; fat16_next_cluster()
 *   is the one genuinely FAT16-specific piece of arithmetic (plain
 *   16-bit little-endian FAT entries — no odd/even nibble packing, see
 *   fat16.h's own comment for why this is actually simpler than FAT12's
 *   version); fat16_readdir() is a thin wrapper around fat_common.c's
 *   shared fat_readdir_fixed_root(), since FAT16's root directory region
 *   is byte-for-byte the same shape as FAT12's.
 */
#include "fat16.h"
#include "fat_cache.h"      /* DAY65: FAT table sector cache */
#include "../../core/kernel.h"   /* memcpy, memset, memcmp */

int fat16_mount(block_device_t *dev, uint32_t lba, fat16_fs_t *out_fs)
{
    /* Zero the whole handle up front, same "never hand back a
     * partially-populated fs on failure" discipline fat12_mount()
     * uses. */
    memset(out_fs, 0, sizeof(fat16_fs_t));

    if (!dev || !dev->read)
        return -1;

    static uint8_t boot_sector[512];
    if (dev->read(dev, lba, 1, boot_sector) != 0)
        return -1;

    fat_bpb_common_t bpb;
    int common_ok = (fat_parse_bpb_common(boot_sector, &bpb) == 0);

    /* The one check fat_parse_bpb_common() deliberately leaves out —
     * see fat_common.h's doc comment on that function. This is Day
     * 16's own acceptance-criteria check, the FAT16 counterpart of
     * fat12_parse_bpb()'s "BS_FilSysType == FAT12" comparison. */
    int fstype_ok = (memcmp(bpb.BS_FilSysType, "FAT16", 5) == 0);

    if (!common_ok || !fstype_ok)
        return -1;

    fat_geometry_t geom;
    fat_compute_geometry(&bpb, &geom);   /* always 0 — pure arithmetic */

    out_fs->dev       = dev;
    out_fs->mount_lba = lba;
    out_fs->bpb       = bpb;
    out_fs->geom      = geom;

    return 0;
}

int32_t fat16_next_cluster(fat16_fs_t *fs, uint16_t cluster)
{
    /* Byte offset of this cluster's 16-bit entry within the FAT
     * region (FAT #1 — same "never consult the backup FAT copy"
     * choice fat12_next_cluster() makes). No odd/even branch needed:
     * every FAT16 entry is a plain, byte-aligned 2-byte value. */
    uint32_t fat_byte_offset = (uint32_t)cluster * 2;

    uint32_t bytes_per_sec = fs->bpb.BPB_BytesPerSec;
    if (bytes_per_sec == 0)
        bytes_per_sec = 512;

    uint32_t sector_index_in_fat = fat_byte_offset / bytes_per_sec;
    uint32_t offset_in_sector    = fat_byte_offset % bytes_per_sec;

    /* Same volume-relative -> disk-absolute LBA fix fat12_next_cluster()'s
     * DAY11 bugfix documents: geom.first_fat_sector is relative to this
     * volume's own boot sector, so fs->mount_lba must be added back in. */
    uint32_t lba = fs->mount_lba + fs->geom.first_fat_sector + sector_index_in_fat;

    /* Unlike fat12_next_cluster(), a single sector is always enough —
     * see fat16.h's doc comment for why a 16-bit entry (even byte
     * offset, since BLOCK_SECTOR_SIZE is even) can never straddle a
     * 512-byte sector boundary. */
    static uint8_t fat_buf[512];
    if (fatc_read(fs->dev, lba, 1, fat_buf) != 0)
        return -1;

    uint8_t b0 = fat_buf[offset_in_sector];
    uint8_t b1 = fat_buf[offset_in_sector + 1];

    uint16_t value = (uint16_t)b0 | (uint16_t)((uint16_t)b1 << 8);

    return (int32_t)value;
}

/* DAY23 (Week 4, Day 23 of the 5.0 filesystem roadmap):
 *   "实现 fat_alloc_cluster()：分配一个新簇，并更新 FAT 表（写回磁盘）。"
 *
 * See fat16.h's doc comment above the declaration for the full
 * contract. Same offset math as fat16_next_cluster() above; writes
 * `value` little-endian into the 2 bytes at that offset instead of
 * decoding them.
 */
int fat16_set_cluster_value(fat16_fs_t *fs, uint16_t cluster, uint16_t value)
{
    if (!fs || !fs->dev || !fs->dev->read || !fs->dev->write)
        return -1;

    uint32_t fat_byte_offset = (uint32_t)cluster * 2;

    uint32_t bytes_per_sec = fs->bpb.BPB_BytesPerSec;
    if (bytes_per_sec == 0)
        bytes_per_sec = 512;

    uint32_t sector_index_in_fat = fat_byte_offset / bytes_per_sec;
    uint32_t offset_in_sector    = fat_byte_offset % bytes_per_sec;
    uint32_t lba = fs->mount_lba + fs->geom.first_fat_sector + sector_index_in_fat;

    static uint8_t fat_buf[512];
    if (fatc_read(fs->dev, lba, 1, fat_buf) != 0)
        return -1;

    fat_buf[offset_in_sector]     = (uint8_t)(value & 0xFF);
    fat_buf[offset_in_sector + 1] = (uint8_t)((value >> 8) & 0xFF);

    if (fatc_write(fs->dev, lba, 1, fat_buf) != 0)
        return -1;

    return 0;
}

/* fat16_readdir(): thin wrapper around fat_common.h's shared
 * fat_readdir_fixed_root() — see fat_common.h's DAY16 header note and
 * fat16.h's own doc comment for why FAT16's root directory needs no
 * FAT16-specific walk logic at all (identical fixed-region shape to
 * FAT12's). Behavior/contract is otherwise identical to
 * fat12_readdir(). */
int fat16_readdir(fat16_fs_t *fs, fat16_dirlist_entry_t *out_entries,
                   uint32_t max_entries, uint32_t *out_count)
{
    return fat_readdir_fixed_root(fs->dev, fs->mount_lba, &fs->geom,
                                   out_entries, max_entries, out_count);
}