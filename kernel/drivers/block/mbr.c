/* kernel/drivers/block/mbr.c
 * LumaOS 5.0 - MBR Partition Table Parser
 *
 * DAY5 (Week 1, Day 5 of the 5.0 filesystem roadmap):
 *   "解析 MBR（LBA 0），扫描分区表（偏移 0x1BE）。识别第一个活动分区
 *    （Type 0x0C = FAT32 LBA，0x06 = FAT16，0x01 = FAT12）。在终端
 *    打印出分区起始 LBA 和大小。"
 *
 *   See mbr.h for the full layout/design rationale. This file just
 *   walks the 4-entry, 16-byte-per-entry table at offset 0x1BE of a
 *   sector buffer already in memory (kmain() supplies it via the
 *   Day 3 ide_read_sectors(0, 1, buf) call) — no disk I/O happens in
 *   this file itself, keeping the block-device and partition-table
 *   concerns separate per the roadmap's block_device abstraction
 *   planned for Day 6.
 */
#include "mbr.h"
#include "../../core/kernel.h"   /* console_write_color, printf, memcpy */

int mbr_parse(const uint8_t sector[512], mbr_partition_entry_t out[MBR_PARTITION_COUNT])
{
    const uint8_t *table = sector + MBR_PARTITION_TABLE_OFFSET;

    for (int i = 0; i < MBR_PARTITION_COUNT; i++) {
        const uint8_t *entry = table + (i * MBR_PARTITION_ENTRY_SIZE);
        memcpy(&out[i], entry, sizeof(mbr_partition_entry_t));
    }

    if (sector[MBR_SIGNATURE_OFFSET]     != MBR_SIGNATURE_BYTE0 ||
        sector[MBR_SIGNATURE_OFFSET + 1] != MBR_SIGNATURE_BYTE1) {
        return -1; /* not a valid MBR boot signature */
    }

    return 0;
}

int mbr_is_fat_type(uint8_t type)
{
    switch (type) {
        case MBR_PART_TYPE_FAT12:
        case MBR_PART_TYPE_FAT16:
        case MBR_PART_TYPE_FAT32_LBA:
            return 1;
        default:
            return 0;
    }
}

const char *mbr_type_name(uint8_t type)
{
    switch (type) {
        case MBR_PART_TYPE_EMPTY:     return "empty";
        case MBR_PART_TYPE_FAT12:     return "FAT12";
        case MBR_PART_TYPE_FAT16:     return "FAT16";
        case MBR_PART_TYPE_FAT32_LBA: return "FAT32(LBA)";
        default:                      return "unknown";
    }
}

const mbr_partition_entry_t *mbr_find_first_active_fat(
    const mbr_partition_entry_t entries[MBR_PARTITION_COUNT], int *out_index)
{
    for (int i = 0; i < MBR_PARTITION_COUNT; i++) {
        if (entries[i].status == MBR_STATUS_ACTIVE &&
            mbr_is_fat_type(entries[i].type)) {
            if (out_index)
                *out_index = i;
            return &entries[i];
        }
    }

    return NULL;
}

void mbr_dump(const mbr_partition_entry_t entries[MBR_PARTITION_COUNT])
{
    console_write_color("[mbr]  Partition table:\n", COLOR_INFO);

    int any = 0;
    for (int i = 0; i < MBR_PARTITION_COUNT; i++) {
        const mbr_partition_entry_t *e = &entries[i];

        if (e->type == MBR_PART_TYPE_EMPTY)
            continue;

        any = 1;
        uint32_t size_kb = e->sector_count / 2; /* 512-byte sectors -> KB */

        printf("  [%d] %s  type=0x%02x (%s)  start_lba=%u  sectors=%u (%u KB)\n",
               i,
               (e->status == MBR_STATUS_ACTIVE) ? "active  " : "inactive",
               e->type, mbr_type_name(e->type),
               e->lba_start, e->sector_count, size_kb);
    }

    if (!any) {
        console_write_color("  (no partitions found — table is all-empty)\n",
                             COLOR_WARNING);
    }
}