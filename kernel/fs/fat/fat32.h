/* kernel/fs/fat/fat32.h
 * LumaOS 5.0 - FAT32 Mount, FSInfo, Cluster-Chain Walk & Root Directory Listing
 *
 * DAY17 (Week 3, Day 17 of the 5.0 filesystem roadmap):
 *   "实现 FAT32 解析（注意 BPB_RootClus，根目录是一个普通簇链，且存在
 *    FSInfo 结构）。挂载 FAT32 镜像（mkfs.fat -F 32），ls 能工作。"
 *
 *   FAT32's counterpart to fat12.h/fat16.h — but genuinely less code
 *   can be shared this time. What's shared (fat_common.h, unchanged
 *   except for one widened field — see its own DAY17 note):
 *   fat_dirent_t (the 32-byte directory entry shape is identical),
 *   fat_dirlist_entry_t/fat_decode_short_name() (name decoding is
 *   identical), and the universal 0x00-0x23 BPB prefix (BS_jmpBoot
 *   through BPB_TotSec32) via fat_bpb_common_t.
 *
 *   What's NOT shared, and is this file's real job:
 *
 *     - The BPB tail past offset 0x24 is COMPLETELY DIFFERENT on
 *       FAT32 — fat_bpb_common_t's BS_DrvNum..BS_FilSysType fields
 *       (which are correct for FAT12/16) do not apply; FAT32 has
 *       BPB_FATSz32, BPB_ExtFlags, BPB_FSVer, BPB_RootClus,
 *       BPB_FSInfo, BPB_BkBootSec, ... instead. fat32_bpb_ext_t below
 *       parses that FAT32-specific tail as its own struct, read
 *       directly off the raw boot-sector bytes rather than overlaid
 *       onto fat_bpb_common_t (which would silently misinterpret
 *       these bytes as FAT12/16 EBPB fields).
 *     - fat_common.c's shared fat_compute_geometry() is DELIBERATELY
 *       NOT used for FAT32's real geometry: that function computes
 *       first_data_sector using BPB_FATSz16, which is always 0 on a
 *       FAT32 volume (the real per-FAT sector count lives in
 *       BPB_FATSz32 instead — see fat32_bpb_ext_t below). Using the
 *       shared formula would silently compute the WRONG
 *       first_data_sector (short by NumFATs * FATSz32 sectors — on
 *       this project's own FAT32 fixture, short by 2*539 = 1078
 *       sectors) even though it happens to still classify total
 *       sector COUNT as "big enough to be FAT32" by a wide margin
 *       (fat_mount()'s Day 15 classification path uses the shared
 *       formula only to tell FAT12/16/32 apart via total_clusters,
 *       which stays roughly correct despite the offset error — but a
 *       real mount that actually needs to read the data region cannot
 *       tolerate being off by 1078 sectors). fat32_mount() below
 *       therefore computes first_data_sector/total_clusters itself,
 *       using the real BPB_FATSz32 value.
 *     - fat32_fs_t: FAT32's own mount-state handle — dev + mount_lba +
 *       the shared BPB prefix + fat32_bpb_ext_t + this file's own
 *       correctly-computed geometry fields (no fat_geometry_t reuse,
 *       for the same FATSz32-vs-FATSz16 reason above).
 *     - fat32_fsinfo_t / fat32_read_fsinfo(): FAT32 is the first FAT
 *       variant with an FSInfo sector at all (BPB_FSInfo points to it,
 *       usually sector 1) — a hint structure (free cluster count,
 *       next free cluster) that real FAT32 drivers use to speed up
 *       allocation. This project doesn't allocate space yet (that's
 *       LMFS's job starting Day 30+, and even a future FAT32 writer
 *       is out of this roadmap's scope), so fat32_read_fsinfo() only
 *       reads and signature-validates it for diagnostic purposes
 *       today — "且存在 FSInfo 结构" (this Day's own acceptance text)
 *       asks for awareness of it, not that anything consumes the
 *       free-space hints yet.
 *     - FAT32_CLUSTER_* constants and fat32_next_cluster(): FAT32's FAT
 *       table entries are 32-bit slots, but only the low 28 bits are
 *       meaningful (the top 4 bits are reserved and must be preserved
 *       on write, ignored on read) — a third distinct on-disk cluster-
 *       chain format, so this needs its own walker, same as FAT16's
 *       needed its own despite superficially "just being wider" than
 *       FAT12's.
 *     - fat32_readdir(): FAT32's root directory is an ordinary CLUSTER
 *       CHAIN starting at BPB_RootClus (see fat_common.h's DAY17 note
 *       for why this can't reuse fat_readdir_fixed_root() — that
 *       function is fundamentally about a FIXED region, and FAT32's
 *       root isn't one). This file's own inline chain-walking loop is
 *       used instead, calling fat32_next_cluster() to advance.
 */
#ifndef LUMA_FAT32_H
#define LUMA_FAT32_H

#include <stdint.h>
#include "fat_common.h"                  /* fat_bpb_common_t, fat_dirent_t,
                                             fat_dirlist_entry_t, FAT_ATTR_*,
                                             FAT_DIRENT_*, fat_parse_bpb_common(),
                                             fat_decode_short_name() */
#include "../../drivers/block/block.h"   /* block_device_t */

/* ===== FAT32-specific BPB tail (offsets 0x24 onward) =====
 *
 * Only the fields this driver actually needs are named; the ones in
 * between (BPB_ExtFlags, BPB_FSVer, BPB_Reserved, BS_DrvNum,
 * BS_Reserved1, BS_BootSig, BS_VolID, BS_VolLab) are real spec fields
 * but this project has no current use for them, so they're absorbed
 * into the `_reservedN[]` padding arrays below purely to keep the
 * struct's byte offsets correct when overlaid on a real boot sector —
 * same "only name what's used, pad the rest" approach fat_bpb_common_t
 * itself takes with BS_jmpBoot/BS_OEMName.
 */
typedef struct __attribute__((packed)) {
    uint32_t BPB_FATSz32;        /* 0x24: sectors per FAT (the REAL one on FAT32 — BPB_FATSz16 is always 0) */
    uint16_t BPB_ExtFlags;       /* 0x28: FAT mirroring flags (ignored — this project never writes) */
    uint16_t BPB_FSVer;          /* 0x2A: FAT32 version, must be 0x0000 */
    uint32_t BPB_RootClus;       /* 0x2C: root directory's FIRST CLUSTER — usually 2, but not guaranteed */
    uint16_t BPB_FSInfo;         /* 0x30: sector number (volume-relative) of the FSInfo structure, usually 1 */
    uint16_t BPB_BkBootSec;      /* 0x32: sector number of the backup boot sector, usually 6 */
    uint8_t  _reserved1[12];     /* 0x34: BPB_Reserved, must be 0 */
    uint8_t  BS_DrvNum;          /* 0x40: INT 13h drive number */
    uint8_t  BS_Reserved1;       /* 0x41: reserved, must be 0 */
    uint8_t  BS_BootSig;         /* 0x42: extended boot signature, 0x29 if next 3 fields are valid */
    uint32_t BS_VolID;           /* 0x43: volume serial number */
    uint8_t  BS_VolLab[11];      /* 0x47: volume label, space-padded */
    uint8_t  BS_FilSysType[8];   /* 0x52: usually "FAT32   " — advisory only per spec, same caveat
                                     as fat_bpb_common_t's own BS_FilSysType field */
} fat32_bpb_ext_t;

/* ===== FSInfo structure (Day 17's other new concept) =====
 *
 * "且存在 FSInfo 结构" — this project's read-only view of it: the two
 * documented signatures (used to sanity-check that BPB_FSInfo really
 * does point at a real FSInfo sector) plus the two hint fields a real
 * allocator would use. See fat32_read_fsinfo()'s doc comment for why
 * this project doesn't act on FSI_Free_Count/FSI_Nxt_Free yet.
 */
typedef struct __attribute__((packed)) {
    uint32_t FSI_LeadSig;        /* 0x000: must be 0x41615252 ("RRaA") */
    uint8_t  _reserved1[480];    /* 0x004: FSI_Reserved1, must be 0 */
    uint32_t FSI_StrucSig;       /* 0x1E4: must be 0x61417272 ("rrAa") */
    uint32_t FSI_Free_Count;     /* 0x1E8: last known free cluster count, 0xFFFFFFFF if unknown */
    uint32_t FSI_Nxt_Free;       /* 0x1EC: hint: first cluster to start a free-cluster search from */
    uint8_t  _reserved2[12];     /* 0x1F0: FSI_Reserved2, must be 0 */
    uint32_t FSI_TrailSig;       /* 0x1FC: must be 0xAA550000 */
} fat32_fsinfo_t;

#define FAT32_FSINFO_LEAD_SIG    0x41615252u
#define FAT32_FSINFO_STRUC_SIG   0x61417272u
#define FAT32_FSINFO_TRAIL_SIG   0xAA550000u

/* ===== Mount state ===== */
typedef struct {
    block_device_t     *dev;         /* the volume's underlying block device */
    uint32_t             mount_lba;  /* disk-absolute LBA of this volume's boot sector */
    fat_bpb_common_t      bpb;        /* parsed universal 0x00-0x23 BPB prefix */
    fat32_bpb_ext_t       ext;        /* parsed FAT32-specific tail, 0x24 onward */

    /* Correctly-computed geometry — see this file's header comment
     * for why fat_common.c's shared fat_compute_geometry() is NOT
     * used here (it assumes BPB_FATSz16, always 0 on FAT32). */
    uint32_t first_fat_sector;    /* volume-relative first sector of FAT #1 */
    uint32_t first_data_sector;   /* volume-relative first sector of cluster 2's data region */
    uint32_t total_sectors;       /* TotSec16 or TotSec32, whichever is set */
    uint32_t total_clusters;      /* (total_sectors - first_data_sector) / SecPerClus */
    uint32_t bytes_per_cluster;   /* BPB_BytesPerSec * BPB_SecPerClus */
} fat32_fs_t;

/* fat32_mount(dev, lba, out_fs)
 *
 * Reads the boot sector at `lba` off `dev`, runs it through
 * fat_parse_bpb_common() (Day 15's shared 0x55AA + BytesPerSec/
 * SecPerClus/NumFATs sanity check — still valid for FAT32, since that
 * check only ever reads the universal 0x00-0x23 prefix), separately
 * parses the FAT32-specific tail into fat32_bpb_ext_t, checks
 * BS_FilSysType reads "FAT32   ", then computes this file's own
 * correct geometry (see header comment) into *out_fs.
 *
 * On success returns 0 with *out_fs fully populated. On failure — I/O
 * error, fat_parse_bpb_common() rejects the sector, or BS_FilSysType
 * doesn't say "FAT32" — returns -1 and zeroes *out_fs, same "no
 * meaningful handle on failure" discipline as fat12_mount()/
 * fat16_mount().
 *
 * `out_fs` must not be NULL.
 */
int fat32_mount(block_device_t *dev, uint32_t lba, fat32_fs_t *out_fs);

/* fat32_read_fsinfo(fs, out_fsinfo)
 *
 * Reads the FSInfo sector at disk-absolute LBA `fs->mount_lba +
 * fs->ext.BPB_FSInfo` and validates its two signatures. Returns 0 and
 * populates *out_fsinfo if the read succeeds AND both signatures
 * match (a real FSInfo structure) — the free-count/next-free hints
 * inside are then valid to READ (this project's ls/readdir don't need
 * them, but a future allocator could). Returns -1 on I/O failure or a
 * signature mismatch (`fs->ext.BPB_FSInfo` pointed somewhere that
 * isn't actually a valid FSInfo sector — some very old/nonstandard
 * FAT32 formatters can leave this 0xFFFF, meaning "no FSInfo"; this
 * project's own mkfs.fat-generated fixture always has a real one, but
 * this function doesn't assume that).
 *
 * `fs` and `out_fsinfo` must not be NULL. This project's own driver
 * does not currently allocate space, so nothing besides diagnostics
 * (kmain.c's DAY17 test section) calls this today — see this file's
 * header comment.
 */
int fat32_read_fsinfo(fat32_fs_t *fs, fat32_fsinfo_t *out_fsinfo);

/* ===== Cluster-chain values =====
 *
 * FAT32 FAT entries are 32-bit slots but only the low 28 bits carry
 * real cluster-chain information — the FAT32_CLUSTER_MASK below
 * exists specifically to strip the reserved top 4 bits before
 * comparing against any of these sentinels, per spec ("Software that
 * accesses FAT32 volumes should not use these bits, since they are
 * not part of the FAT32 cluster number"). Counterparts of
 * FAT12_CLUSTER_* / FAT16_CLUSTER_* — same meanings, just wider values.
 */
#define FAT32_CLUSTER_MASK        0x0FFFFFFFu  /* strip the reserved top 4 bits */
#define FAT32_CLUSTER_FREE        0x00000000u
#define FAT32_CLUSTER_RESERVED    0x00000001u
#define FAT32_CLUSTER_BAD         0x0FFFFFF7u
#define FAT32_CLUSTER_EOC_MIN     0x0FFFFFF8u  /* >= this (after masking) means "end of chain" */
#define FAT32_CLUSTER_EOC_MAX     0x0FFFFFFFu

/* fat32_next_cluster(fs, cluster)
 *
 * Looks up FAT[cluster] (FAT #1, same "never consult the backup FAT
 * copy" choice fat12_next_cluster()/fat16_next_cluster() make) and
 * returns its masked 28-bit value: either a real next-cluster number,
 * FAT32_CLUSTER_FREE/RESERVED/BAD, or a value >= FAT32_CLUSTER_EOC_MIN
 * meaning "chain ends here". `cluster` is uint32_t here (unlike
 * fat16_next_cluster()'s uint16_t) because FAT32 cluster numbers
 * genuinely need up to 28 bits.
 *
 * Each entry is 4 bytes; unlike FAT16's always-even offsets, a FAT32
 * entry's byte offset (`cluster * 4`) can still never straddle a
 * 512-byte sector boundary either (4 divides 512 evenly, same
 * reasoning fat16_next_cluster()'s doc comment gives for 2), so a
 * single sector read is always enough here too.
 *
 * Returns the masked 32-bit value (0..0x0FFFFFFF, always
 * non-negative) on success as int64_t (wide enough that even the
 * largest legal masked value can never collide with -1). Returns -1
 * if the underlying dev->read(...) call fails.
 */
int64_t fat32_next_cluster(fat32_fs_t *fs, uint32_t cluster);

/* fat32_set_cluster_value(fs, cluster, value)
 *
 * DAY23 (Week 4, Day 23 of the 5.0 filesystem roadmap):
 *   "实现 fat_alloc_cluster()：分配一个新簇，并更新 FAT 表（写回磁盘）。"
 *
 *   The write-side mirror of fat32_next_cluster() above. A FAT32 entry
 *   is a byte-aligned 4-byte slot, but unlike FAT16's 2-byte entry,
 *   only its low 28 bits (FAT32_CLUSTER_MASK) are real cluster-chain
 *   data — the top 4 bits are reserved and, per spec, software must
 *   leave them exactly as it found them rather than assuming they're
 *   0. So this function DOES need a genuine read-modify-write (unlike
 *   fat16_set_cluster_value()'s "no neighbor to preserve" case): it
 *   reads the existing 4-byte raw value, replaces only the bits
 *   FAT32_CLUSTER_MASK covers with `value`'s low 28 bits, and leaves
 *   the top 4 reserved bits exactly as they were read — the write-side
 *   counterpart of fat32_next_cluster()'s own `raw & FAT32_CLUSTER_MASK`
 *   masking on the way out.
 *
 *   Same "FAT #1 only" scope as fat32_next_cluster().
 *
 * `value` is a raw cluster-chain value (only its low 28 bits are used
 * — pass FAT32_CLUSTER_EOC_MAX or a real cluster number, not a value
 * with meaningful bits above FAT32_CLUSTER_MASK).
 *
 * Returns 0 on success. Returns -1 if `fs`/`fs->dev` is NULL,
 * `fs->dev->write` is NULL (read-only backend), or the underlying
 * dev->read()/dev->write() call fails.
 */
int fat32_set_cluster_value(fat32_fs_t *fs, uint32_t cluster, uint32_t value);

/* ===== Root directory listing =====
 *
 * fat32_dirlist_entry_t / FAT32_DIRLIST_MAX_ENTRIES: same alias
 * pattern as fat16.h's — see fat_common.h's DAY17 note on why
 * fat_dirlist_entry_t.start_cluster was widened to fit a FAT32
 * cluster number.
 */
typedef fat_dirlist_entry_t fat32_dirlist_entry_t;
#define FAT32_DIRLIST_MAX_ENTRIES  FAT_DIRLIST_MAX_ENTRIES

/* fat32_readdir(fs, out_entries, max_entries, out_count)
 *
 * Lists `fs`'s root directory by walking the cluster chain starting
 * at `fs->ext.BPB_RootClus` (via fat32_next_cluster()), decoding every
 * 32-byte fat_dirent_t slot in each cluster with the same skip/decode
 * rules fat_readdir_fixed_root() uses (FAT_DIRENT_END stops the WHOLE
 * walk, FAT_DIRENT_FREE/LFN/volume-label entries are skipped,
 * fat_decode_short_name() decodes the 8.3 name) — just driven by a
 * chain walk instead of a fixed sector range, since FAT32's root has
 * no fixed region at all.
 *
 * Stops at whichever comes first: a FAT_DIRENT_END marker, the chain
 * reaching end-of-chain (fat32_next_cluster() returning a masked value
 * >= FAT32_CLUSTER_EOC_MIN), `max_entries` decoded entries, or (as a
 * corruption guard against a chain that cycles back on itself) more
 * cluster hops than `fs->total_clusters` — a well-formed chain can
 * never legitimately need more hops than the volume has clusters.
 *
 * On success, writes the number of entries decoded into `*out_count`
 * (0 is legal) and returns 0. Returns -1 if a dev->read(...) call
 * (either a directory-cluster sector or a FAT sector via
 * fat32_next_cluster()) fails partway through — *out_count is left
 * however many entries were successfully decoded first, same
 * "don't discard partial progress" contract as fat12_readdir()/
 * fat16_readdir().
 *
 * `fs`, `out_entries`, and `out_count` must not be NULL. `out_entries`
 * must have room for at least `max_entries` slots.
 */
int fat32_readdir(fat32_fs_t *fs, fat32_dirlist_entry_t *out_entries,
                   uint32_t max_entries, uint32_t *out_count);

/* fat32_create_entry_root(fs, name)
 *
 * DAY24 (Week 4, Day 24 of the 5.0 filesystem roadmap):
 *   "实现 fat_create_file(name)：在根目录区找一个空条目（首字节 0x00
 *    或 0xE5），填入文件名、起始簇 0、大小 0。"
 *
 *   The FAT32 counterpart to fat_common.h's fat_create_entry_fixed_
 *   root() (DAY24) — same "claim the first free 32-byte slot, encode
 *   the name via fat_encode_short_name(), zero the rest, write the
 *   one changed sector back" shape, but driven by a CLUSTER-CHAIN walk
 *   starting at `fs->ext.BPB_RootClus` instead of a fixed sector range
 *   — the same root-directory-shape divergence fat32_readdir() (Day
 *   17) already has from fat_readdir_fixed_root() (see this file's own
 *   header comment, and fat_common.h's DAY17 header note, for the full
 *   "FAT32's root is an ordinary cluster chain, not a fixed region"
 *   rationale this function's walk mirrors).
 *
 *   UNLIKE fat_create_entry_fixed_root(): if every cluster currently in
 *   the root's chain is scanned without finding a free slot, this
 *   function does NOT attempt to extend the chain with a freshly
 *   fat32_set_cluster_value()-allocated cluster — growing a directory's
 *   cluster chain is exactly the same "如果当前簇链不够，追加新簇"
 *   operation Day 25's roadmap text describes for FILE writes (see
 *   fat_vfs.h's doc comment on fat_read()), just not yet built for
 *   directories either. Day 24's own acceptance text only asks to find
 *   an existing free ENTRY, not to grow the directory itself — every
 *   fixture this project mounts (a freshly mkfs.fat-formatted volume
 *   with 2-3 files in its root) has far more free entries in its
 *   FIRST root cluster alone than Day 24 will ever create in one test
 *   run, so this limitation is not expected to be hit in practice
 *   today. A full "chain too short" root directory returns -1 the same
 *   way fat_create_entry_fixed_root()'s "root region full" case does.
 *
 * Returns 0 on success. Returns -1 if `fs`/`name` is NULL,
 * `fat_encode_short_name(name, ...)` itself fails (out-of-8.3-scope
 * name), `fs->dev->write` is NULL (read-only backend), a
 * dev->read()/dev->write() call fails while walking the chain or
 * claiming a slot, or the chain reaches its own end-of-chain marker
 * without a free slot ever being found (see the paragraph above).
 */
int fat32_create_entry_root(fat32_fs_t *fs, const char *name);

/* fat32_update_entry_root(fs, name, new_start_cluster, new_size)
 *
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "实现 fat_write(file, buf, size)：处理簇链扩展（如果当前簇链不够，
 *    追加新簇）。"
 *
 *   The FAT32 counterpart to fat_common.h's fat_update_entry_fixed_
 *   root() (DAY25) — same "find the live entry named `name`, overwrite
 *   its DIR_FstClusHI/LO + DIR_FileSize fields in place, write back
 *   just that one sector" shape, but driven by fat32_create_entry_
 *   root()'s own cluster-chain walk (starting at `fs->ext.BPB_RootClus`)
 *   instead of a fixed sector range — the same root-directory-shape
 *   divergence every other FAT32 root-directory operation in this
 *   project already has from its FAT12/16 fixed-region counterpart.
 *
 * Returns 0 on success. Returns -1 if `fs`/`name` is NULL,
 * `fs->dev->write` is NULL (read-only backend), a dev->read()/
 * dev->write() call fails while walking the chain or updating the
 * entry, or the chain is walked to its end (or the corruption-guard
 * hop limit fat32_create_entry_root() itself uses is hit) without
 * finding a live entry named `name` — nothing to update. The caller
 * is expected to have already confirmed the file exists (e.g. via
 * fat_open()) before calling this — see fat_write()'s own doc comment
 * in fat_vfs.h for exactly where that happens.
 */
int fat32_update_entry_root(fat32_fs_t *fs, const char *name,
                             uint32_t new_start_cluster, uint32_t new_size);

/* fat32_delete_entry_root(fs, name)
 *
 * DAY26 (Week 4, Day 26 of the 5.0 filesystem roadmap):
 *   "实现 fat_delete_file(name)：找到目录项，标记为已删除（首字节设为
 *    0xE5），释放其占用的簇链。"
 *
 *   The FAT32 counterpart to fat_common.h's fat_delete_entry_fixed_
 *   root() (DAY26) — same "find the live entry named `name` and set
 *   DIR_Name[0] to FAT_DIRENT_FREE, write back just that one sector"
 *   shape fat32_update_entry_root() above already follows for FAT32's
 *   cluster-chain root, this time ending in a one-byte delete marker
 *   instead of an in-place field overwrite. As with fat_delete_entry_
 *   fixed_root(), this function ONLY ever touches the directory-entry
 *   byte — freeing the FAT-table cluster chain itself is
 *   fat_delete_file()'s job (kernel/fs/fat/fat_vfs.c/fat_vfs.h), which
 *   calls fat32_next_cluster()/fat32_set_cluster_value() directly
 *   (both already exist, DAY17/23) BEFORE calling this function, for
 *   the same "the entry still needs to record the real start_cluster
 *   to free, so free the chain first" ordering fat_delete_file()'s own
 *   doc comment explains.
 *
 *   Name matching is case-insensitive, the same DAY26 bugfix applied
 *   to fat32_update_entry_root() below and to fat_open()/fat_common.c
 *   — see fat_common.h's DAY26 header note above fat_delete_entry_
 *   fixed_root() for the full "why" (a Shell-typed lower/mixed-case
 *   name must still match the always-upper-case on-disk short name).
 *
 * Returns 0 on success. Returns -1 if `fs`/`name` is NULL,
 * `fs->dev->write` is NULL (read-only backend), a dev->read()/
 * dev->write() call fails while walking the chain or deleting the
 * entry, or the chain is walked to its end (or the corruption-guard
 * hop limit is hit) without finding a live entry named `name` —
 * nothing to delete.
 */
int fat32_delete_entry_root(fat32_fs_t *fs, const char *name);

#endif /* LUMA_FAT32_H */