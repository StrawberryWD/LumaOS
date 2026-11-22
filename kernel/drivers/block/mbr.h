/* kernel/drivers/block/mbr.h
 * LumaOS 5.0 - MBR Partition Table Parser
 *
 * DAY5 (Week 1, Day 5 of the 5.0 filesystem roadmap):
 *   "解析 MBR（LBA 0），扫描分区表（偏移 0x1BE）。识别第一个活动分区
 *    （Type 0x0C = FAT32 LBA，0x06 = FAT16，0x01 = FAT12）。在终端
 *    打印出分区起始 LBA 和大小。"
 *
 *   Adds the classic DOS/MBR partition-table layout on top of the
 *   Day 3/4 ide_read_sectors() primitive: a 512-byte MBR sector holds
 *   a 4-entry partition table starting at byte offset 0x1BE (446),
 *   16 bytes per entry, terminated by the 0x55AA boot signature at
 *   offset 0x1FE.
 *
 *   mbr_parse() copies the four raw 16-byte entries out of a sector
 *   buffer (as already read by ide_read_sectors(0, 1, buf)) into an
 *   array of mbr_partition_entry_t, and mbr_find_first_active_fat()
 *   scans that array for the first *active* (status byte 0x80)
 *   entry whose partition type is one of the three FAT variants this
 *   roadmap targets (FAT12/FAT16/FAT32-LBA). mbr_dump() prints every
 *   non-empty entry (status, type, start LBA, sector count) to the
 *   text console for eyeballing during boot, matching the Day 5
 *   acceptance criteria ("在终端打印出分区起始 LBA 和大小").
 */
#ifndef LUMA_MBR_H
#define LUMA_MBR_H

#include <stdint.h>

/* ===== MBR sector layout constants ===== */
#define MBR_PARTITION_TABLE_OFFSET  0x1BE  /* 446: start of the 4 partition entries */
#define MBR_PARTITION_ENTRY_SIZE    16     /* bytes per entry */
#define MBR_PARTITION_COUNT         4      /* classic MBR: exactly 4 primary entries */
#define MBR_SIGNATURE_OFFSET        0x1FE  /* 510: 2-byte 0x55AA boot signature */
#define MBR_SIGNATURE_BYTE0         0x55
#define MBR_SIGNATURE_BYTE1         0xAA

/* ===== Partition status byte (offset 0 of each entry) ===== */
#define MBR_STATUS_ACTIVE    0x80  /* bootable / active flag */
#define MBR_STATUS_INACTIVE  0x00

/* ===== Partition type bytes we recognize for this roadmap ===== */
#define MBR_PART_TYPE_EMPTY      0x00  /* unused entry */
#define MBR_PART_TYPE_FAT12      0x01  /* FAT12, CHS or LBA */
#define MBR_PART_TYPE_FAT16      0x06  /* FAT16, CHS or LBA (>32MB) */
#define MBR_PART_TYPE_FAT32_LBA  0x0C  /* FAT32, LBA (INT 13h extensions) */

/* One 16-byte MBR partition-table entry, laid out exactly as it
 * appears on disk. Packed so the struct can be pointer-cast directly
 * over bytes read from LBA 0 without compiler-inserted padding; x86
 * tolerates the resulting unaligned uint32_t accesses natively. */
typedef struct __attribute__((packed)) {
    uint8_t  status;        /* 0x80 = active/bootable, 0x00 = inactive */
    uint8_t  chs_start[3];  /* CHS start (legacy, ignored — we use LBA) */
    uint8_t  type;          /* partition type byte, see MBR_PART_TYPE_* */
    uint8_t  chs_end[3];    /* CHS end (legacy, ignored) */
    uint32_t lba_start;     /* first sector of the partition, LBA */
    uint32_t sector_count;  /* partition size in 512-byte sectors */
} mbr_partition_entry_t;

/* mbr_parse(sector, out)
 *
 * Copies the four 16-byte partition entries out of a raw 512-byte MBR
 * sector buffer (as read by ide_read_sectors(0, 1, sector)) into
 * out[0..3], reading them from MBR_PARTITION_TABLE_OFFSET (0x1BE).
 *
 * Returns 0 if the sector's boot signature (0x55AA at offset 0x1FE)
 * is present, -1 otherwise (sector is not a valid MBR — entries are
 * still copied out on a -1 return so callers may inspect them, but
 * should treat them with suspicion).
 */
int mbr_parse(const uint8_t sector[512], mbr_partition_entry_t out[MBR_PARTITION_COUNT]);

/* mbr_is_fat_type(type)
 *
 * Returns 1 if `type` is one of the three FAT partition-type bytes
 * this roadmap step recognizes (0x01/0x06/0x0C), 0 otherwise.
 */
int mbr_is_fat_type(uint8_t type);

/* mbr_type_name(type)
 *
 * Returns a short human-readable label for a partition type byte
 * (e.g. "FAT32(LBA)", "FAT16", "FAT12", "empty", or "unknown") for
 * use in diagnostic printouts. Never returns NULL.
 */
const char *mbr_type_name(uint8_t type);

/* mbr_find_first_active_fat(entries, out_index)
 *
 * Scans `entries` (MBR_PARTITION_COUNT entries, as filled in by
 * mbr_parse()) for the first entry that is both active
 * (status == MBR_STATUS_ACTIVE) and a recognized FAT type
 * (mbr_is_fat_type() true). If found, writes its index (0-3) to
 * *out_index (when out_index is non-NULL) and returns a pointer to
 * that entry. Returns NULL if no such entry exists.
 */
const mbr_partition_entry_t *mbr_find_first_active_fat(
    const mbr_partition_entry_t entries[MBR_PARTITION_COUNT], int *out_index);

/* mbr_dump(entries)
 *
 * Prints every non-empty entry in `entries` to the text console:
 * index, active/inactive, type byte + name, start LBA, and sector
 * count (both in sectors and the equivalent KB). Purely diagnostic —
 * this is what satisfies the Day 5 acceptance criteria of printing
 * partition start LBA and size to the terminal.
 */
void mbr_dump(const mbr_partition_entry_t entries[MBR_PARTITION_COUNT]);

#endif /* LUMA_MBR_H */