/* kernel/fs/fat/fat16.h
 * LumaOS 5.0 - FAT16 Mount, Cluster-Chain Walk & Root Directory Listing
 *
 * DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
 *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载一个
 *    FAT16 镜像（可以用 mkfs.fat -F 16 制作），ls 能正确列出文件。"
 *
 *   This is FAT16's counterpart to kernel/fs/fat/fat12.h — the same
 *   role fat12.h has played since Day 9, just for the second FAT
 *   variant. Everything that is byte-for-byte IDENTICAL between FAT12
 *   and FAT16 already lives in kernel/fs/fat/fat_common.h (the BPB
 *   prefix, the 32-byte directory entry, the geometry formulas, AND —
 *   new as of today — the fixed-root-region directory WALK itself,
 *   fat_readdir_fixed_root(); see fat_common.h's DAY16 header note for
 *   why that walk is safe to share verbatim). What's left for THIS
 *   file is exactly what genuinely differs:
 *
 *     - fat16_fs_t: FAT16's own mount-state handle (dev + mount_lba +
 *       bpb + geom), built directly on fat_common.h's shared
 *       fat_bpb_common_t/fat_geometry_t types — unlike fat12_fs_t,
 *       there is no legacy bpb_12_t-style alias to preserve here (FAT16
 *       is new code as of today, nothing pre-Day-16 ever referenced a
 *       "bpb_16_t"), so this struct uses the shared types directly.
 *     - fat16_mount(): reads the boot sector, runs it through
 *       fat_parse_bpb_common() (Day 15's shared sanity check) PLUS the
 *       one FAT16-specific check (BS_FilSysType == "FAT16   "), mirrors
 *       fat12_mount()'s Day 10 shape exactly.
 *     - FAT16_CLUSTER_* constants and fat16_next_cluster(): FAT16's FAT
 *       table is a plain array of 16-bit little-endian entries — no
 *       1.5-byte odd/even nibble packing like FAT12's 12-bit entries
 *       need (see fat12.h's DAY11 comment for that mess) — so this is
 *       actually SIMPLER than fat12_next_cluster(), not shared with it,
 *       since the on-disk table format itself genuinely differs.
 *     - fat16_dirlist_entry_t / FAT16_DIRLIST_MAX_ENTRIES: backward/
 *       forward-compatible aliases onto fat_common.h's shared
 *       fat_dirlist_entry_t / FAT_DIRLIST_MAX_ENTRIES (same alias
 *       pattern fat12.h uses for fat12_dirlist_entry_t as of today's
 *       Day 16 refactor there).
 *     - fat16_readdir(): a thin wrapper around fat_common.h's
 *       fat_readdir_fixed_root(), unpacking this file's own
 *       fat16_fs_t instead of fat12_fs_t — see fat_common.h's DAY16
 *       header note for why FAT16's root directory (a fixed region of
 *       BPB_RootEntCnt entries, located before the data region — "FAT16
 *       的根目录在数据区之前") is exactly the same shape as FAT12's, and
 *       therefore needs no FAT16-specific walk logic at all.
 *
 * Design notes:
 *   - No bpb_16_t alias: fat12.h's bpb_12_t exists purely for backward
 *     compatibility with pre-Day-15 callers (kmain.c's DAY9 test
 *     sections). FAT16 has no such history — fat16_fs_t below just
 *     embeds fat_bpb_common_t/fat_geometry_t directly, which is the
 *     cleaner shape Day 15's refactor was building toward in the first
 *     place.
 *   - Cluster numbering: FAT16 cluster numbers are 16-bit (0..0xFFFF)
 *     the same width as fat12_next_cluster()'s `cluster` parameter, so
 *     fat16_next_cluster() reuses the identical uint16_t parameter
 *     type — no widening needed despite FAT16's larger address space
 *     (a FAT16 volume tops out at 65524 clusters, comfortably inside
 *     uint16_t's 0..65535 range).
 */
#ifndef LUMA_FAT16_H
#define LUMA_FAT16_H

#include <stdint.h>
#include "fat_common.h"                  /* fat_bpb_common_t, fat_geometry_t,
                                             fat_dirent_t, fat_dirlist_entry_t,
                                             FAT_ATTR_*, FAT_DIRENT_*,
                                             fat_parse_bpb_common(),
                                             fat_compute_geometry(),
                                             fat_readdir_fixed_root() */
#include "../../drivers/block/block.h"   /* block_device_t */

/* ===== Mount state =====
 *
 * FAT16's mount handle: which block_device_t the volume lives on, the
 * disk-absolute LBA its boot sector was read from (same reason
 * fat12_fs_t needs mount_lba — see fat12.h's DAY11 note: geometry's
 * *_sector fields are volume-relative, not disk-absolute), plus the
 * parsed BPB and derived geometry.
 */
typedef struct {
    block_device_t    *dev;        /* the volume's underlying block device */
    uint32_t           mount_lba;  /* disk-absolute LBA of this volume's boot sector */
    fat_bpb_common_t   bpb;        /* parsed boot sector, see fat16_mount() */
    fat_geometry_t     geom;       /* derived geometry, see fat16_mount() */
} fat16_fs_t;

/* fat16_mount(dev, lba, out_fs)
 *
 * Reads the boot sector at `lba` off `dev` via dev->read(dev, lba, 1,
 * buf) — same block_device_t call fat12_mount() uses — then checks it
 * through fat_parse_bpb_common() (Day 15's shared sanity check: 0x55AA
 * signature + spec-legal BytesPerSec/SecPerClus/NumFATs) PLUS the one
 * check that's genuinely FAT16-specific: BS_FilSysType reads exactly
 * "FAT16   " (8 bytes, space-padded) — mirrors fat12_mount()'s own
 * "shared sanity check + one variant-specific string compare" shape
 * (see fat12.c's DAY15 comment on fat12_parse_bpb() for why that split
 * exists: BS_FilSysType means different things per variant, so it
 * can't live in the shared layer).
 *
 * On success, populates *out_fs (dev pointer, disk-absolute mount_lba,
 * parsed bpb, derived geometry) and returns 0. On failure — dev->read()
 * itself fails, the sector doesn't pass fat_parse_bpb_common()'s sanity
 * check, or BS_FilSysType isn't "FAT16" — returns -1 and zeroes
 * *out_fs (same "no meaningful handle on failure" discipline as
 * fat12_mount()).
 *
 * `out_fs` must not be NULL.
 */
int fat16_mount(block_device_t *dev, uint32_t lba, fat16_fs_t *out_fs);

/* ===== Cluster-chain values =====
 *
 * FAT16's FAT table entries are a plain, byte-aligned 16-bit
 * little-endian array — no odd/even packing (contrast fat12.h's
 * FAT12_CLUSTER_* comment for the 12-bit case this does NOT need to
 * deal with). Sentinel ranges straight from the Microsoft FAT spec,
 * the FAT16 counterparts of fat12.h's FAT12_CLUSTER_* values:
 *
 *   0x0000            - free cluster (not allocated to any file)
 *   0x0001             - reserved, never used by real data
 *   0xFFF7             - marks a cluster flagged "bad sector"
 *   0xFFF8 - 0xFFFF     - end-of-chain (a RANGE, same "don't hardcode
 *                         only 0xFFFF" caution as FAT12's EOC range —
 *                         see fat12.h's DAY11 comment for why a real
 *                         formatter is free to write any value in this
 *                         range to terminate a chain).
 */
#define FAT16_CLUSTER_FREE        0x0000
#define FAT16_CLUSTER_RESERVED    0x0001
#define FAT16_CLUSTER_BAD         0xFFF7
#define FAT16_CLUSTER_EOC_MIN     0xFFF8  /* >= this value means "end of chain" */
#define FAT16_CLUSTER_EOC_MAX     0xFFFF  /* largest legal 16-bit value */

/* fat16_next_cluster(fs, cluster)
 *
 * Looks up FAT[cluster] — the table entry describing what comes after
 * `cluster` in a chain — and returns its raw 16-bit value: either a
 * real next-cluster number, one of the FAT16_CLUSTER_FREE/RESERVED/BAD
 * sentinels, or a value >= FAT16_CLUSTER_EOC_MIN meaning "chain ends
 * here". Callers walk a whole chain the same way fat12_next_cluster()'s
 * callers do: repeatedly feed each return value back in as the next
 * `cluster` argument, stopping on a negative return (I/O error) or a
 * value >= FAT16_CLUSTER_EOC_MIN (legitimate end).
 *
 * UNLIKE fat12_next_cluster(), there is no odd/even branch and no
 * 1.5-byte-per-entry packing to worry about: cluster N's entry is
 * simply the 2 bytes at byte offset `N * 2` within the FAT region,
 * read little-endian. Because BLOCK_SECTOR_SIZE (512) is even, `N * 2`
 * mod 512 is always even, so the 2-byte read never straddles a sector
 * boundary the way a FAT12 entry sometimes does — a single sector read
 * is always enough (contrast fat12_next_cluster()'s deliberate
 * always-read-2-sectors simplicity trade-off, which FAT16 doesn't need
 * in the first place).
 *
 * Returns the raw 16-bit FAT entry value (0..0xFFFF, always
 * non-negative) on success. Returns -1 if the underlying
 * dev->read(...) call fails (same "negative means failure" convention
 * as fat12_next_cluster()) — unambiguous because a real 16-bit entry
 * can never itself be negative once returned as int32_t.
 */
int32_t fat16_next_cluster(fat16_fs_t *fs, uint16_t cluster);

/* fat16_set_cluster_value(fs, cluster, value)
 *
 * DAY23 (Week 4, Day 23 of the 5.0 filesystem roadmap):
 *   "实现 fat_alloc_cluster()：分配一个新簇，并更新 FAT 表（写回磁盘）。"
 *
 *   The write-side mirror of fat16_next_cluster() above. Unlike
 *   FAT12's 12-bit packed entries (fat12_set_cluster_value(), which
 *   MUST read-modify-write to avoid corrupting a neighboring cluster
 *   packed into the same byte), a FAT16 entry is a plain, byte-aligned
 *   2-byte slot that belongs to exactly one cluster — so this function
 *   could in principle just overwrite those 2 bytes directly. It still
 *   reads the sector first anyway (same shape as fat12_set_cluster_
 *   value(), just because there's genuinely nothing to preserve here)
 *   purely so both variants' set_cluster_value() have an identical
 *   "read sector, modify in memory, write sector back" shape for
 *   fat_vfs.c's fat_alloc_cluster() to call through — see fat_vfs.c's
 *   DAY23 comment for why that symmetry (not a performance need)
 *   is what actually justifies it here.
 *
 *   Same "FAT #1 only" scope as fat16_next_cluster() (this project
 *   never reads or writes the backup FAT copy).
 *
 * `value` is a raw 16-bit quantity (0..0xFFFF, same range
 * fat16_next_cluster() returns) written verbatim, little-endian, into
 * FAT[cluster].
 *
 * Returns 0 on success. Returns -1 if `fs`/`fs->dev` is NULL,
 * `fs->dev->write` is NULL (read-only backend), or the underlying
 * dev->read()/dev->write() call fails.
 */
int fat16_set_cluster_value(fat16_fs_t *fs, uint16_t cluster, uint16_t value);

/* ===== Root directory listing =====
 *
 * DAY16 alias note: fat16_dirlist_entry_t / FAT16_DIRLIST_MAX_ENTRIES
 * are the FAT16-side names for fat_common.h's shared
 * fat_dirlist_entry_t / FAT_DIRLIST_MAX_ENTRIES — see fat_common.h's
 * DAY16 header note for why FAT16's root directory needs no walk logic
 * of its own (it's the exact same fixed-region shape FAT12's already
 * is).
 */
typedef fat_dirlist_entry_t fat16_dirlist_entry_t;
#define FAT16_DIRLIST_MAX_ENTRIES  FAT_DIRLIST_MAX_ENTRIES

/* fat16_readdir(fs, out_entries, max_entries, out_count)
 *
 * Lists `fs`'s root directory — same contract as fat12_readdir()
 * (kernel/fs/fat/fat12.h): decodes up to `max_entries` fat_dirent_t
 * slots into `out_entries[]`, skipping free/deleted/LFN/volume-label
 * entries and stopping (without error) at the first end-of-directory
 * marker. Implemented as a thin wrapper around fat_common.h's
 * fat_readdir_fixed_root(fs->dev, fs->mount_lba, &fs->geom, ...) — see
 * fat16.c's DAY16 comment.
 *
 * `fs`, `out_entries`, and `out_count` must not be NULL. `out_entries`
 * must have room for at least `max_entries` fat16_dirlist_entry_t
 * slots. Returns 0 on success (*out_count = entries decoded, 0 is
 * legal), -1 if the underlying dev->read(...) fails partway through.
 */
int fat16_readdir(fat16_fs_t *fs, fat16_dirlist_entry_t *out_entries,
                   uint32_t max_entries, uint32_t *out_count);

#endif /* LUMA_FAT16_H */