/* kernel/fs/fat/fat_common.h
 * LumaOS 5.0 - FAT12/16/32 Shared On-Disk Structures & Type Detection
 *
 * DAY15 (Week 3, Day 15 of the 5.0 filesystem roadmap):
 *   "重构 FAT 通用层：分离 FAT12/16/32 的共性（BPB 解析、目录项结构），
 *    创建 fat_common.c。一个 fat_mount() 根据 BPB 的 BytesPerSector 和
 *    TotalSectors 自动判断类型。"
 *
 *   Through Day 14 this project had exactly one FAT driver (FAT12,
 *   kernel/fs/fat/fat12.{h,c}), so every on-disk structure it needed —
 *   the boot-sector BPB, the 32-byte short directory entry — lived
 *   directly in fat12.h with no separation between "shared FAT family
 *   shape" and "FAT12-specific behavior". That was the right call for
 *   Day 9-14 (see fat12.h's own DAY9 comment: "adding [FAT16/32
 *   fields] here now would just be dead weight this struct doesn't
 *   use... Day 15's roadmap step is where the common vs. FAT12-only
 *   fields get properly separated"). Today is that day.
 *
 *   THIS FILE holds exactly what FAT12, FAT16, and FAT32 boot sectors
 *   and directory regions share byte-for-byte:
 *
 *     - fat_bpb_common_t: the boot-sector PREFIX every FAT variant
 *       lays out identically — BS_jmpBoot through BPB_TotSec32
 *       (offsets 0x00-0x23, 36 bytes) — PLUS the FAT12/16 EBPB tail
 *       (BS_DrvNum..BS_FilSysType, offsets 0x24-0x3D) that FAT12 and
 *       FAT16 also share byte-for-byte with each other (FAT32 alone
 *       diverges past offset 0x24 — its own BPB_FATSz32/ExtFlags/
 *       BPB_RootClus/... fields start there instead; a FAT32-specific
 *       tail struct is Day 17's job, not this file's). See the
 *       struct's own field-by-field comment below for exactly which
 *       offset range is "all three" vs. "FAT12/16 only", and why
 *       fat_detect_type()/fat_compute_geometry() below are careful to
 *       only ever read from the truly-universal 0x00-0x23 prefix.
 *     - fat_dirent_t: the classic 32-byte 8.3 short directory entry —
 *       identical on-disk shape across FAT12/16/32 (this is exactly
 *       what makes a single readdir-decode routine reusable later;
 *       Day 15 itself doesn't need a shared decode function yet since
 *       only FAT12 has a working readdir today, but the shape moves
 *       here now so Day 16/17's FAT16/32 readdir work starts from the
 *       same struct instead of a copy-pasted duplicate).
 *     - fat_type_t / fat_geometry_t / fat_classify_type() /
 *       fat_compute_geometry(): the DERIVED values (RootDirSectors,
 *       FirstDataSector, ..., and now also TotalClusters + which FAT
 *       variant this is) that used to live only in fat12.h's
 *       fat12_geometry_t, generalized so any FAT backend can reuse the
 *       exact same formulas instead of re-deriving them.
 *     - fat_mount(): Day 15's actual headline deliverable — reads a
 *       boot sector off a block_device_t, parses the common BPB
 *       prefix, computes geometry, and returns WHICH FAT variant it
 *       is (FAT12/16/32/unknown) without needing any variant-specific
 *       driver to exist yet. This is deliberately NOT a full mount
 *       (it produces geometry + a type tag, not a working filesystem_t
 *       you can readdir/open through) — FAT16's own driver is Day 16,
 *       FAT32's is Day 17; today's job is only "figure out which one
 *       this is", which is exactly what the roadmap's acceptance text
 *       asks for ("自动判断类型", not "自动挂载三种文件系统").
 *
 * DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap) — ADDITION NOTE:
 *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载一个
 *    FAT16 镜像，ls 能正确列出文件。"
 *
 *   FAT16's root directory is laid out EXACTLY like FAT12's: a
 *   fixed-size region of BPB_RootEntCnt 32-byte fat_dirent_t slots,
 *   located at geom.first_root_dir_sec, sitting before the data region
 *   — "FAT16 的根目录在数据区之前" is simply restating the same fixed-
 *   root-region shape fat12.h's Day 9 comment already described (as
 *   opposed to FAT32, where the root is an ordinary cluster chain
 *   reachable only through BPB_RootClus — Day 17's problem, not
 *   today's). This is exactly the case fat_common.h's own DAY15 note
 *   above anticipated ("FAT16 shares that same fixed-root-region
 *   shape... Day 16 can very plausibly reuse most of this verbatim").
 *
 *   So today the root-directory WALK (which used to be FAT12-only —
 *   fat12_readdir(), fat12.c) is promoted into this shared layer as
 *   fat_readdir_fixed_root() below, taking just the four things the
 *   walk actually needs (dev, mount_lba, geometry, output buffer)
 *   instead of a fat12_fs_t — so both fat12_readdir() (kernel/fs/fat/
 *   fat12.c, now a thin wrapper) and the new fat16_readdir() (kernel/
 *   fs/fat/fat16.c) call the exact same decode loop. fat_dirlist_entry_t
 *   and fat_decode_short_name() move here alongside it for the same
 *   reason (fat12_dirlist_entry_t is now a backward-compatible type
 *   alias, same pattern as bpb_12_t/fat12_geometry_t since Day 15).
 *
 *   WHAT STAYS FAT16-SPECIFIC (kernel/fs/fat/fat16.h/fat16.c, NOT
 *   moved here): the cluster-CHAIN walk. FAT16's FAT table is a plain
 *   array of 16-bit little-endian entries — no 1.5-byte odd/even
 *   packing like FAT12's fat12_next_cluster() has to handle — but that
 *   is still a real difference in the on-disk table format, not the
 *   directory-region format, so fat16_next_cluster() is its own
 *   function, the same way fat12_next_cluster() is FAT12-only. See
 *   fat16.h's own DAY16 doc comment for the full contract.
 *
 * DAY17 (Week 3, Day 17 of the 5.0 filesystem roadmap) — ADDITION NOTE:
 *   "实现 FAT32 解析（注意 BPB_RootClus，根目录是一个普通簇链，且存在
 *    FSInfo 结构）。挂载 FAT32 镜像（mkfs.fat -F 32），ls 能工作。"
 *
 *   UNLIKE Day 16, FAT32's root directory is NOT this file's fixed-
 *   region shape — fat_readdir_fixed_root() above stays FAT12/16-only.
 *   FAT32's root is an ordinary CLUSTER CHAIN starting at
 *   BPB_RootClus (a genuinely new field this file's fat_bpb_common_t
 *   does not have — FAT32's BPB diverges from the FAT12/16 EBPB tail
 *   at offset 0x24, see this file's DAY15 header note above), so
 *   walking it needs a cluster-chain walker (fat32_next_cluster(),
 *   kernel/fs/fat/fat32.c) BEFORE any directory entries can even be
 *   read — the reverse of FAT12/16, where the root directory doesn't
 *   need the cluster-chain walker at all. fat32_readdir() therefore
 *   lives entirely in fat32.c, with its own inline chain-walking loop,
 *   not shared with fat_readdir_fixed_root() here. (Flagged in
 *   fat32.c's own DAY17 comment as a candidate to later route through
 *   Day 19's general cluster-chain-walking machinery, once that exists
 *   for file *content* reads too — today it's FAT32's own one-off, the
 *   same way Day 12's fat12_readdir() was FAT12's own one-off before
 *   Day 16 gave it a second caller worth sharing with.)
 *
 *   What DOES change here on Day 17: fat_dirlist_entry_t.start_cluster
 *   widened from uint16_t to uint32_t (see its own doc comment below)
 *   so FAT32's up-to-28-bit cluster numbers fit — and
 *   fat_readdir_fixed_root()'s decode step now combines DIR_FstClusHI:
 *   DIR_FstClusLO into that wider field (a harmless no-op for FAT12/16,
 *   whose DIR_FstClusHI is always 0 per spec), so a single shared
 *   struct works for all three variants' directory listings without
 *   FAT32 needing its own parallel copy of it.
 *
 *   WHAT STAYS IN fat12.h/fat12.c (NOT moved here):
 *     - The FAT12-specific "is BS_FilSysType == 'FAT12'" string check
 *       (fat12_parse_bpb() keeps this on top of this file's generic
 *       sanity check — FAT16 images commonly read "FAT16   " there
 *       instead, and FAT32 images don't reliably fill that field with
 *       anything meaningful at all per the Microsoft spec, so a
 *       string match belongs to the FAT12 driver specifically, not
 *       the shared layer).
 *     - fat12_fs_t (a FAT12 MOUNT's state — dev + bpb + geom — is
 *       still FAT12-flavored bookkeeping, not a shared-across-variants
 *       concept; Day 16/17 will grow their own fat16_fs_t/fat32_fs_t
 *       the same way, all three built on top of this file's shared
 *       bpb/geometry types).
 *     - fat12_next_cluster() and its FAT12_CLUSTER_* constants: FAT12's
 *       12-bit odd/even nibble-packed cluster-chain format is NOT
 *       shared with FAT16 (plain 16-bit entries) or FAT32 (28-bit
 *       entries in a 32-bit slot) — each variant needs its own chain
 *       walker, so these stay FAT12-specific.
 *     - (DAY15-era note, superseded by DAY16 below) fat12_readdir()'s
 *       WALK over fat_dirent_t entries was, as of Day 15, written only
 *       for FAT12's fixed-size root directory region, not yet
 *       promoted to a shared function — "doing that split correctly is
 *       left to whichever of Day 16/17 actually needs it" (the exact
 *       words this comment used on Day 15). Day 16 is that day: see
 *       this file's DAY16 header note above and fat_readdir_fixed_root()
 *       below for the now-shared version. FAT32's root (Day 17) is
 *       still NOT this same shape (a cluster chain, not a fixed
 *       region), so fat_readdir_fixed_root() stays FAT12/16-only —
 *       FAT32 needs its own walk over a cluster chain, layered on top
 *       of Day 19's cluster-chain-walking machinery instead.
 *
 *   BACKWARD COMPATIBILITY: fat12.h keeps its old `bpb_12_t` and
 *   `fat12_geometry_t` type names ALIASED to this file's
 *   fat_bpb_common_t/fat_geometry_t (`typedef fat_bpb_common_t
 *   bpb_12_t;` etc.) rather than deleting them — every existing caller
 *   (kmain.c's DAY9/DAY9-FOLLOWUP test sections, fat12_vfs.c) keeps
 *   compiling completely unchanged. Only fat12.h/fat12.c themselves
 *   needed to change to route through this file.
 *
 * Design notes:
 *   - Field/macro names are kept IDENTICAL to what fat12.h had through
 *     Day 14 (BPB_BytesPerSec, DIR_Name, FAT_ATTR_DIRECTORY, ...) —
 *     this is a pure relocation of shapes that were already correct,
 *     not a redesign, so every existing `bpb.BPB_BytesPerSec`-style
 *     field access anywhere in the codebase keeps working without
 *     modification.
 *   - fat_parse_bpb_common()'s sanity check is deliberately narrower
 *     than old fat12.h's: it checks the 0x55AA signature plus
 *     BytesPerSec/SecPerClus/NumFATs being spec-legal values (exactly
 *     fat12_parse_bpb()'s old "fields_ok" logic, minus the
 *     FAT12-specific BS_FilSysType string match) — because those
 *     three fields are the ones the FAT spec actually constrains
 *     identically across FAT12/16/32, so this check alone is already
 *     a reliable "is this ANY kind of FAT BPB" test, before anyone
 *     asks which specific variant it is.
 */
#ifndef LUMA_FAT_COMMON_H
#define LUMA_FAT_COMMON_H

#include <stdint.h>
#include "../../drivers/block/block.h"   /* block_device_t, used by fat_mount() */

/* ===== BIOS Parameter Block — shared prefix + FAT12/16 EBPB tail =====
 *
 * Offsets 0x00-0x23 (BS_jmpBoot through BPB_TotSec32, 36 bytes) are
 * laid out IDENTICALLY on every FAT12, FAT16, and FAT32 boot sector —
 * this is the one part of the struct fat_detect_type()/
 * fat_compute_geometry() below are allowed to read, since it's true
 * regardless of which variant is actually on disk.
 *
 * Offsets 0x24-0x3D (BS_DrvNum through BS_FilSysType, the "EBPB" —
 * Extended BPB) are shared between FAT12 and FAT16 ONLY. A real FAT32
 * boot sector has DIFFERENT fields at these same byte offsets
 * (BPB_FATSz32, BPB_ExtFlags, BPB_FSVer, BPB_RootClus, BPB_FSInfo,
 * BPB_BkBootSec, ... — Day 17's job to define). This struct still
 * includes the FAT12/16 EBPB tail (rather than splitting it into a
 * third struct) purely for continuity with fat12.h's pre-Day-15 shape
 * (see this file's header comment on backward compatibility) — code
 * that only cares about the universal 0x00-0x23 prefix (this file's
 * own fat_detect_type()/fat_compute_geometry()) simply never reads
 * BS_DrvNum onward, so parsing a real FAT32 sector into this struct
 * is harmless (those trailing fields just end up holding whatever
 * FAT32-specific bytes happen to sit there — unused garbage from this
 * struct's point of view, never read, never acted on).
 */
typedef struct __attribute__((packed)) {
    uint8_t  BS_jmpBoot[3];      /* 0x00: x86 jump to boot code (ignored — not our loader) */
    uint8_t  BS_OEMName[8];      /* 0x03: OEM name string, not NUL-terminated */
    uint16_t BPB_BytesPerSec;    /* 0x0B: bytes per sector, almost always 512 */
    uint8_t  BPB_SecPerClus;     /* 0x0D: sectors per cluster (power of 2, >=1) */
    uint16_t BPB_RsvdSecCnt;     /* 0x0E: reserved sectors before the first FAT (incl. boot sector) */
    uint8_t  BPB_NumFATs;        /* 0x10: number of FAT copies, almost always 2 */
    uint16_t BPB_RootEntCnt;     /* 0x11: max root-directory entries (FAT12/16 only; 0 on FAT32) */
    uint16_t BPB_TotSec16;       /* 0x13: total sectors, if it fits in 16 bits (else 0, use TotSec32) */
    uint8_t  BPB_Media;          /* 0x15: media descriptor byte (0xF8 = fixed disk) */
    uint16_t BPB_FATSz16;        /* 0x16: sectors per FAT (FAT12/16; 0 on FAT32, which uses FATSz32 instead) */
    uint16_t BPB_SecPerTrk;      /* 0x18: sectors per track (CHS geometry, legacy) */
    uint16_t BPB_NumHeads;       /* 0x1A: number of heads (CHS geometry, legacy) */
    uint32_t BPB_HiddSec;        /* 0x1C: hidden sectors preceding this volume */
    uint32_t BPB_TotSec32;       /* 0x20: total sectors, if TotSec16 was 0 */
    /* ---- FAT12/16 EBPB tail (offsets 0x24-0x3D) — NOT valid on FAT32 ---- */
    uint8_t  BS_DrvNum;          /* 0x24: INT 13h drive number (0x80 = first fixed disk) */
    uint8_t  BS_Reserved1;       /* 0x25: reserved, must be 0 */
    uint8_t  BS_BootSig;         /* 0x26: extended boot signature, 0x29 if the next 3 fields are valid */
    uint32_t BS_VolID;           /* 0x27: volume serial number */
    uint8_t  BS_VolLab[11];      /* 0x2B: volume label, space-padded, not NUL-terminated */
    uint8_t  BS_FilSysType[8];   /* 0x36: e.g. "FAT12   "/"FAT16   " — advisory only per spec */
} fat_bpb_common_t;

/* Boot sector signature bytes — present at the very end of every FAT
 * boot sector regardless of FAT12/16/32 variant (same 0x55AA
 * convention as MBR's MBR_SIGNATURE_OFFSET/BYTE0/BYTE1). */
#define FAT_BOOT_SIG_OFFSET  0x1FE  /* 510: 2-byte 0x55AA signature */
#define FAT_BOOT_SIG_BYTE0   0x55
#define FAT_BOOT_SIG_BYTE1   0xAA

/* ===== Directory entry (root directory & subdirectories, 32 bytes) =====
 *
 * The classic short (8.3) FAT directory entry — identical on-disk
 * shape across FAT12/16/32. See fat12.h's original DAY12 doc comment
 * (still there, describing the decode/skip rules a *reader* of these
 * entries follows) for the full walking contract; this struct only
 * defines the raw byte layout.
 */
typedef struct __attribute__((packed)) {
    uint8_t  DIR_Name[11];        /* 0x00: 8.3 name, space-padded, no dot stored */
    uint8_t  DIR_Attr;            /* 0x0B: attribute bits, see FAT_ATTR_* below */
    uint8_t  DIR_NTRes;           /* 0x0C: reserved for Windows NT, must be 0 */
    uint8_t  DIR_CrtTimeTenth;    /* 0x0D: creation time, tenths of a second */
    uint16_t DIR_CrtTime;         /* 0x0E: creation time */
    uint16_t DIR_CrtDate;         /* 0x10: creation date */
    uint16_t DIR_LstAccDate;      /* 0x12: last access date */
    uint16_t DIR_FstClusHI;       /* 0x14: high 16 bits of first cluster (0 on FAT12/16) */
    uint16_t DIR_WrtTime;         /* 0x16: last write time */
    uint16_t DIR_WrtDate;         /* 0x18: last write date */
    uint16_t DIR_FstClusLO;       /* 0x1A: low 16 bits of first cluster (the whole cluster # on FAT12) */
    uint32_t DIR_FileSize;        /* 0x1C: file size in bytes, 0 for directories */
} fat_dirent_t;

/* DIR_Name[0] sentinel values. */
#define FAT_DIRENT_FREE       0xE5  /* entry is free (deleted); rest of entry is stale */
#define FAT_DIRENT_END        0x00  /* entry is free AND no further entries exist in this directory */
#define FAT_DIRENT_DOT_FLAG   0x2E  /* '.' — this or the following byte starts "." / ".." */

/* DIR_Attr bits (standard FAT attribute byte). */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
/* Long-file-name entries mark all four of these bits. */
#define FAT_ATTR_LONG_NAME  (FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | \
                              FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)

/* ===== Decoded directory listing entry (Day 16's promoted concept) =====
 *
 * DAY16: the small, already-decoded result of walking one 32-byte
 * fat_dirent_t — originally fat12_dirlist_entry_t (Day 12, defined
 * directly in fat12.h), moved here now that fat_readdir_fixed_root()
 * below is shared between FAT12 and FAT16. See fat12.h's original
 * DAY12 doc comment (still there) for the full field-by-field
 * rationale (name decoding rules, which entries get skipped, ...) —
 * this is a pure relocation of an already-correct shape, not a
 * redesign, exactly like fat_bpb_common_t/fat_geometry_t's Day 15
 * move. `fat12_dirlist_entry_t` (fat12.h) and `fat16_dirlist_entry_t`
 * (fat16.h) are both now backward/forward-compatible type aliases
 * onto this same struct.
 */
typedef struct {
    char      name[13];   /* "8.3" decoded, NUL-terminated, spaces
                              stripped, at most "12345678.123\0" */
    uint8_t   attr;        /* raw DIR_Attr byte, see FAT_ATTR_* */
    uint32_t  start_cluster; /* DAY17: widened from uint16_t to uint32_t —
                                 DIR_FstClusHI:DIR_FstClusLO combined. On
                                 FAT12/16 this is a pure no-op (FstClusHI
                                 is always 0 there, per spec, so the value
                                 never exceeds 16 bits) — widened now so
                                 the SAME struct also correctly holds a
                                 FAT32 cluster number (which needs the
                                 full 28 bits), instead of fat32.c/
                                 fat32_readdir() needing a parallel
                                 almost-identical struct just for this one
                                 field. See fat_common.c's DAY17 note on
                                 fat_readdir_fixed_root() for where the
                                 hi:lo combine happens (still harmless for
                                 FAT12/16 callers). */
    uint32_t  size;        /* DIR_FileSize, 0 for directories */
} fat_dirlist_entry_t;

/* DAY16: shared cap on how many entries a single fat_readdir_fixed_root()
 * call decodes — was FAT12_DIRLIST_MAX_ENTRIES (fat12.h, Day 12), moved
 * here since the walk itself is now shared. fat12.h/fat16.h both alias
 * their own *_DIRLIST_MAX_ENTRIES names to this one. */
#define FAT_DIRLIST_MAX_ENTRIES  32

/* ===== FAT variant identification (Day 15's new concept) ===== */
typedef enum {
    FAT_TYPE_UNKNOWN = 0,  /* not a recognizable FAT BPB at all */
    FAT_TYPE_FAT12   = 12,
    FAT_TYPE_FAT16   = 16,
    FAT_TYPE_FAT32   = 32,
} fat_type_t;

/* ===== Derived geometry (generalizes fat12.h's old fat12_geometry_t) =====
 *
 * Everything fat12_geometry_t already held, PLUS `total_clusters`
 * (the one extra number fat_classify_type() needs that Day 9-14 never
 * had to compute) and `type` (this file's classification result,
 * folded into the same struct so a single fat_compute_geometry() call
 * gives a caller both the sector-layout numbers AND "which FAT is
 * this" in one shot).
 */
typedef struct {
    uint32_t root_dir_sectors;   /* sectors occupied by the root directory region */
    uint32_t first_root_dir_sec; /* first sector (volume-relative) of the root directory region */
    uint32_t first_data_sector;  /* first sector (volume-relative) of the data region (cluster 2) */
    uint32_t first_fat_sector;   /* first sector (volume-relative) of FAT #1 */
    uint32_t total_sectors;      /* volume's total sector count (TotSec16 or TotSec32, whichever is set) */
    uint32_t bytes_per_cluster;  /* BPB_BytesPerSec * BPB_SecPerClus, for convenience */
    uint32_t total_clusters;     /* DAY15: (total_sectors - first_data_sector) / SecPerClus —
                                   * the number fat_classify_type() uses to tell FAT12/16/32 apart */
    fat_type_t type;             /* DAY15: fat_classify_type(total_clusters) */
} fat_geometry_t;

/* fat_parse_bpb_common(buf, out_bpb)
 *
 * Copies the raw 512-byte boot-sector buffer into *out_bpb (always,
 * regardless of validity — same "fill anyway, caller checks the
 * return value" contract fat12_parse_bpb() has used since Day 9) and
 * sanity-checks the fields that are constrained IDENTICALLY across
 * every FAT variant:
 *
 *   - the 0x55AA signature at FAT_BOOT_SIG_OFFSET (necessary but NOT
 *     sufficient by itself — see fat12.c's original DAY9 comment for
 *     why: any bootable x86 sector ends in 0x55AA, FAT or not)
 *   - BPB_BytesPerSec is one of the four spec-legal sector sizes
 *     (512/1024/2048/4096)
 *   - BPB_SecPerClus is a power of two (1..128)
 *   - BPB_NumFATs is 1 or 2
 *
 * Deliberately does NOT check BS_FilSysType (that string means
 * different, variant-specific things on FAT12 vs FAT16 vs FAT32 — see
 * this file's header comment) — a caller that needs a stronger,
 * variant-specific check layers it on top, the way fat12_parse_bpb()
 * already does.
 *
 * `out_bpb` may be NULL if the caller only wants the return value.
 * Returns 0 if all the checks above pass, -1 otherwise.
 */
int fat_parse_bpb_common(const uint8_t buf[512], fat_bpb_common_t *out_bpb);

/* fat_classify_type(total_clusters)
 *
 * The actual FAT12-vs-16-vs-32 boundary rule, straight from the
 * Microsoft FAT spec (fatgen103, section "FAT Type Determination"):
 * the ONLY reliable way to tell the three variants apart is the
 * volume's total cluster count, evaluated in this exact order:
 *
 *     total_clusters <  4085   -> FAT12
 *     total_clusters < 65525   -> FAT16
 *     otherwise                -> FAT32
 *
 * NOTE ON THE ROADMAP TEXT ("根据 BPB 的 BytesPerSector 和
 * TotalSectors 自动判断类型"): total_clusters isn't read directly out
 * of the BPB — it's DERIVED from BPB_BytesPerSec, BPB_SecPerClus, and
 * TotalSectors (BPB_TotSec16/32) together via fat_compute_geometry()
 * below (total_clusters = (TotalSectors - FirstDataSector) /
 * SecPerClus). This is read the same way fat12.h's own DAY11/DAY12
 * comments read similar roadmap shorthand elsewhere in this project:
 * "BytesPerSector 和 TotalSectors" names the two BPB fields a human
 * would point to first when explaining how automatic detection works,
 * not a claim that a bare comparison of those two raw fields alone is
 * the whole algorithm — the real spec formula (which this function
 * implements) necessarily also folds in SecPerClus and the root/FAT
 * region sizes to get from "sectors" to "clusters" before the
 * 12/16/32 boundary check means anything. A naive BytesPerSec/
 * TotalSectors-only heuristic would misclassify plenty of real
 * volumes (e.g. a large-cluster-size FAT16 volume with a big
 * TotalSectors count could be mistaken for FAT32); the cluster-count
 * method is the one that's actually correct, and is what every real
 * FAT implementation (including Microsoft's own) uses.
 */
fat_type_t fat_classify_type(uint32_t total_clusters);

/* fat_compute_geometry(bpb, out_geom)
 *
 * Applies the standard Microsoft FAT spec geometry formulas to a
 * parsed fat_bpb_common_t — the same formulas fat12.c's original
 * fat12_parse_bpb() computed inline through Day 14, now shared:
 *
 *   RootDirSectors  = ((BPB_RootEntCnt * 32) + (BPB_BytesPerSec - 1))
 *                      / BPB_BytesPerSec
 *   FirstRootDirSec = BPB_RsvdSecCnt + (BPB_NumFATs * BPB_FATSz16)
 *   FirstDataSector = FirstRootDirSec + RootDirSectors
 *   TotalSectors    = BPB_TotSec16 if nonzero, else BPB_TotSec32
 *   TotalClusters   = (TotalSectors - FirstDataSector) / BPB_SecPerClus
 *                      (DAY15: new — feeds fat_classify_type() below)
 *
 * `out_geom->type` is filled via fat_classify_type(total_clusters) —
 * so a single call to this function gives both the sector-layout
 * geometry AND the detected FAT variant.
 *
 * Divide-by-zero guards: if BPB_BytesPerSec or BPB_SecPerClus is 0
 * (a malformed/non-FAT sector, e.g. LumaOS's own boot/mbr.asm
 * reinterpreted as a BPB — see fat12.c's original DAY9 comment for
 * why that happens and is expected), this function substitutes a
 * safe fallback of 512/1 respectively so the arithmetic below never
 * divides by zero — mirroring fat12_parse_bpb()'s original "fields_ok
 * ? bpb.BPB_BytesPerSec : 512" fallback, generalized to SecPerClus
 * too since TotalClusters now also divides by it. The geometry is
 * still computed and returned even when these fallbacks kick in
 * (same "fill anyway, diagnostic value" spirit as
 * fat_parse_bpb_common() and every Day 9+ parse function before it) —
 * callers that need to know whether the BPB was actually trustworthy
 * check fat_parse_bpb_common()'s own return value, not this
 * function's.
 *
 * `bpb` must not be NULL. `out_geom` may be NULL if the caller only
 * wants... nothing (this function has no other side effect); passing
 * NULL is harmless but pointless. Always returns 0 (pure arithmetic
 * over already-in-memory fields; there is no I/O here to fail).
 */
int fat_compute_geometry(const fat_bpb_common_t *bpb, fat_geometry_t *out_geom);

/* ===== DAY15's headline deliverable ===== */

/* fat_mount(dev, lba, out_bpb, out_geom)
 *
 * DAY15: "一个 fat_mount() 根据 BPB 的 BytesPerSector 和 TotalSectors
 *  自动判断类型。"
 *
 *   Reads the boot sector at `lba` off `dev` (via dev->read(dev, lba,
 *   1, buf) — the Day 6 block_device_t interface, same call every
 *   fat12_mount()-style function already uses), parses it through
 *   fat_parse_bpb_common() and fat_compute_geometry() above, and
 *   returns WHICH FAT VARIANT the volume at `lba` appears to be —
 *   without needing a FAT16 or FAT32 driver to exist yet (Day 16/17).
 *
 *   This is intentionally a TYPE-DETECTION function, not a full
 *   mount: it never returns a filesystem_t or a working vfs_node_t
 *   tree (compare fat12_vfs_mount(), which does — Day 13's VFS
 *   adapter). Once FAT16/32 drivers exist, the natural place for a
 *   caller to use fat_mount()'s result is a small dispatcher (e.g.
 *   inside vfs.c, or a future generic "fat_vfs_mount()") that calls
 *   this function first, then routes to fat12_mount()/fat16_mount()/
 *   fat32_mount() based on the returned fat_type_t — not built today
 *   since only fat12_mount() exists, but this function's shape is
 *   exactly what that dispatch will need.
 *
 * `out_bpb`/`out_geom` are populated on a successful boot-sector read
 * regardless of whether the sector turns out to be a valid FAT BPB
 * (same "fill anyway" contract as fat_parse_bpb_common()/
 * fat_compute_geometry() above) — a caller doing diagnostic dumping
 * (kmain.c's DAY15 test section) can inspect them even on a
 * FAT_TYPE_UNKNOWN result. Both may be NULL if the caller only wants
 * the fat_type_t return value.
 *
 * Returns FAT_TYPE_FAT12/FAT_TYPE_FAT16/FAT_TYPE_FAT32 on a
 * successful read of a plausible FAT BPB (per
 * fat_parse_bpb_common()'s sanity check), classified via
 * fat_classify_type(). Returns FAT_TYPE_UNKNOWN if `dev`/`dev->read`
 * is NULL, the underlying dev->read() call fails, or the sector
 * doesn't pass fat_parse_bpb_common()'s sanity check at all (e.g.
 * LumaOS's own LBA 0 mbr.asm bootloader — expected on this project's
 * own stock image, same as every Day 9-14 test against LBA 0).
 */
fat_type_t fat_mount(block_device_t *dev, uint32_t lba,
                      fat_bpb_common_t *out_bpb, fat_geometry_t *out_geom);

/* ===== DAY16's headline deliverables ===== */

/* fat_decode_short_name(raw11, out13)
 *
 * DAY16: moved here verbatim from fat12.c's static
 * fat12_decode_short_name() (Day 12) — see fat12.c's original DAY12
 * comment for the full byte-layout rationale (why base/extension
 * trailing spaces are stripped separately, the DIR_Name[0]==0x05
 * special case, ...). No longer `static`/file-local: fat16_readdir()
 * (kernel/fs/fat/fat16.c) needs the exact same decode fat12_readdir()
 * already relied on, and there is nothing FAT12-specific about an
 * 11-byte 8.3 name's on-disk shape (identical across FAT12/16/32,
 * same as fat_dirent_t itself).
 *
 * Decodes one raw 11-byte DIR_Name (8-byte base + 3-byte extension,
 * space-padded) into a NUL-terminated "BASE.EXT" string in out13
 * (must have room for 13 bytes: 8 + '.' + 3 + '\0').
 */
void fat_decode_short_name(const uint8_t raw11[11], char out13[13]);

/* fat_readdir_fixed_root(dev, mount_lba, geom, out_entries, max_entries, out_count)
 *
 * DAY16: "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。
 *  挂载一个 FAT16 镜像，ls 能正确列出文件。"
 *
 *   The promoted version of fat12_readdir()'s Day 12 walk — reads the
 *   FIXED-SIZE root directory region (`geom->root_dir_sectors` sectors
 *   starting at disk-absolute LBA `mount_lba + geom->first_root_dir_sec`
 *   — same volume-relative-vs-disk-absolute distinction fat12.h's
 *   DAY11 note documents) and decodes each 32-byte fat_dirent_t slot
 *   into `out_entries[]`, using exactly fat12.c's original DAY12
 *   skip/decode rules (0x00 end-marker stops the whole walk without
 *   error, 0xE5 deleted entries are skipped, VFAT long-name fragments
 *   and the volume-label entry are skipped, fat_decode_short_name()
 *   above does the "去掉空格" name decode).
 *
 *   This works UNCHANGED for both FAT12 and FAT16 because both
 *   variants share the identical fixed-root-region shape (see this
 *   file's DAY16 header note) — it takes `dev`/`mount_lba`/`geom`
 *   directly instead of a `fat12_fs_t *`/`fat16_fs_t *` specifically
 *   so it doesn't need to know which variant-specific mount-state
 *   struct it's being called from; fat12_readdir() and fat16_readdir()
 *   are now both thin wrappers that just unpack their own fs_t and
 *   forward here. NOT valid for FAT32 (Day 17): FAT32's root is a
 *   cluster chain, not a fixed region, so it needs its own walk.
 *
 * On success, writes the number of entries actually decoded into
 * `*out_count` (0 is legal: an empty root directory) and returns 0.
 * Returns -1 if the underlying dev->read(...) call fails partway
 * through the region; *out_count is left however many entries were
 * successfully decoded before the failing read (same "don't discard
 * partial progress" spirit as fat12_readdir()'s original contract).
 *
 * `dev`, `geom`, `out_entries`, and `out_count` must not be NULL.
 * `out_entries` must have room for at least `max_entries` slots.
 */
int fat_readdir_fixed_root(block_device_t *dev, uint32_t mount_lba,
                            const fat_geometry_t *geom,
                            fat_dirlist_entry_t *out_entries,
                            uint32_t max_entries, uint32_t *out_count);

/* ===== DAY24's headline deliverables ===== */

/* fat_encode_short_name(name, out11)
 *
 * DAY24 (Week 4, Day 24 of the 5.0 filesystem roadmap):
 *   "实现 fat_create_file(name)：在根目录区找一个空条目（首字节 0x00 或
 *    0xE5），填入文件名、起始簇 0、大小 0。"
 *
 *   The WRITE-side mirror of fat_decode_short_name() (Day 16): turns a
 *   NUL-terminated "BASE.EXT" (or extension-less "BASE") display name
 *   — e.g. "TEST.TXT", exactly what a Shell user types after `create`
 *   — into the raw, space-padded 11-byte DIR_Name[11] a fat_dirent_t
 *   actually stores on disk (8 bytes base + 3 bytes extension, no
 *   dot). This is the one new piece of one-way logic Day 24 needs
 *   that no earlier day already built — every prior FAT day only ever
 *   READ names off disk (fat_decode_short_name(), Day 16) or compared
 *   an already-decoded name against a caller-supplied string
 *   (fat_open()'s strcmp(), Day 18) — nothing before today ever had to
 *   go the other direction and manufacture a NEW raw DIR_Name.
 *
 *   ENCODING RULES (the exact inverse of fat_decode_short_name()'s
 *   decode rules — see that function's own doc comment for the
 *   decode-side rationale this mirrors):
 *     - Split `name` at the LAST '.' (so "A.B.TXT" splits into base
 *       "A.B" / ext "TXT", matching how a real FAT8.3 name has at
 *       most one dot in its DECODED form — this project's own 8.3-only
 *       scope, see fat12.h's DAY9 file comment, means Day 24 never
 *       has to handle a long-filename '.'-in-the-middle case beyond
 *       "take the last dot" the way a full VFAT implementation would
 *       for long names). A `name` with no '.' at all is encoded with
 *       an empty (all-space) 3-byte extension field, matching how
 *       fat_decode_short_name() renders an empty extension field back
 *       as no dot at all (Day 12/16's "must not have the base's
 *       trailing spaces bleed into where the dot belongs... an
 *       unconditional dot would wrongly render those as README." —
 *       the same round-trip guarantee this function is the other half
 *       of).
 *     - Both the base (up to 8 bytes) and extension (up to 3 bytes)
 *       parts are UPPER-CASED (fat_decode_short_name() never
 *       lower-cases anything it reads — conventional FAT8.3 storage is
 *       upper-case, matching fat_open()'s own doc comment: "Case-
 *       sensitive; FAT8.3 names are conventionally stored/decoded
 *       upper-case, so callers should pass the filename upper-case
 *       too" — this function now does that upper-casing itself,
 *       rather than pushing the requirement onto every caller, since
 *       Shell users typing `create test.txt` should not need to know
 *       or care about this on-disk convention).
 *     - Both parts are then SPACE-PADDED on the right to their full
 *       width (8 and 3 bytes respectively) — the exact reverse of
 *       fat_decode_short_name()'s "strip trailing spaces" — so
 *       out11[0..7] and out11[8..10] are always fully populated,
 *       never left holding stale/uninitialized bytes.
 *
 * Returns 0 on success. Returns -1 if `name`/`out11` is NULL, `name`
 * is empty, the base part is empty (a name that's ALL extension, e.g.
 * ".TXT" — not a legal 8.3 short name, same as a bare "." having no
 * real base) or exceeds 8 bytes, or the extension part exceeds 3
 * bytes — i.e. `name` doesn't fit this project's 8.3-only scope. This
 * project's own Shell-typed test names (Day 24's own acceptance text,
 * "test.txt") always fit comfortably within these limits; a caller
 * with a longer real-world name is expected to shorten it itself
 * (this function does not truncate silently — silently truncating a
 * user-typed filename to something shorter than what they asked for
 * is a worse failure mode than a clear "name doesn't fit" rejection).
 */
int fat_encode_short_name(const char *name, uint8_t out11[11]);

/* fat_create_entry_fixed_root(dev, mount_lba, geom, name)
 *
 * DAY24: the WRITE-side counterpart to fat_readdir_fixed_root() above
 * — same "shared between FAT12 and FAT16, both have this exact
 * fixed-size root-region shape" split fat_common.h's own DAY16 header
 * note already established for the read side (see that note for why
 * FAT32's root, a cluster chain rather than a fixed region, needs its
 * own separate function instead — fat32_create_entry_root(), kernel/
 * fs/fat/fat32.c/fat32.h, NOT this one).
 *
 * Reads the fixed-size root directory region sector by sector (same
 * `geom->root_dir_sectors` sectors starting at disk-absolute LBA
 * `mount_lba + geom->first_root_dir_sec` fat_readdir_fixed_root()
 * already reads), looking for the FIRST 32-byte fat_dirent_t slot
 * whose DIR_Name[0] is FAT_DIRENT_END (0x00, "no entry has ever lived
 * here, and everything from here on is also free — spec-defined
 * legal to reuse the first such slot") or FAT_DIRENT_FREE (0xE5, "a
 * PREVIOUSLY used, now-deleted slot — also legal to reuse", the exact
 * "首字节 0x00 或 0xE5" the roadmap's own acceptance text names by
 * both hex values). Once found, encodes `name` via
 * fat_encode_short_name() above, zero-fills the rest of the entry
 * (DIR_Attr = FAT_ATTR_ARCHIVE — a plain file, not a directory/
 * volume-label/read-only/hidden/system entry; DIR_FstClusHI/LO = 0,
 * "起始簇 0" per the roadmap text — Day 25's fat_write() is what will
 * later replace this 0 with a real allocated cluster the first time
 * something is written into the file; DIR_FileSize = 0, "大小 0" —
 * every other DIR_* timestamp field is also left 0, this project
 * having no real-time clock driver yet, same "0 is a legal, if
 * uninformative, timestamp" choice every mount/readdir path already
 * makes implicitly by never reading or acting on those fields), and
 * writes that ONE 32-byte-updated sector back to disk —
 * the smallest possible write that claims the slot, mirroring
 * fat12_set_cluster_value()'s own "read the whole unit, modify only
 * the bytes that changed, write the whole unit back" discipline (see
 * that function's doc comment) applied to a directory sector instead
 * of a FAT sector.
 *
 * NAME COLLISION: this function does NOT check whether `name` already
 * exists elsewhere in the root directory before claiming a free slot
 * — Day 24's own acceptance text only asks for "找一个空条目...填入
 * 文件名", not "reject a duplicate name". A caller that cares whether
 * `name` already exists (kernel/shell/cmd/create.c does, precisely to
 * give a friendlier "already exists" Shell message instead of quietly
 * creating a second, unreachable-by-fat_open() entry with the same
 * name — fat_open()'s own linear readdir()-then-strcmp() walk always
 * matches the FIRST entry with a given name, so a duplicate second
 * entry would simply become permanently unreachable, not a crash, but
 * still worth warning about) is expected to call fat_open()/readdir()
 * itself FIRST and only call this function once it's confirmed there
 * is no existing match — see fat_vfs.c's DAY24 fat_create_file()
 * dispatcher for exactly where that check happens.
 *
 * Returns 0 on success. Returns -1 if `dev`/`geom`/`name` is NULL,
 * `fat_encode_short_name(name, ...)` itself fails (an out-of-8.3-scope
 * name — see that function's own doc comment for the exact limits),
 * `dev->write` is NULL (a read-only backend, exactly the same
 * "allocation is fundamentally impossible, not a transient failure"
 * case fat_alloc_cluster()'s own doc comment already documents for
 * FAT-table writes, now applying to a directory-region write instead),
 * the underlying dev->read()/dev->write() call fails while scanning
 * for or claiming a slot, or the ENTIRE root region was scanned
 * without finding a free slot (the root directory is full — a FAT12/
 * 16 volume's root has a fixed BPB_RootEntCnt entry ceiling, unlike a
 * FAT32 subdirectory or FAT32's own cluster-chain root, which can
 * simply grow another cluster; growing the FIXED root past its
 * BPB_RootEntCnt ceiling is not a legal FAT12/16 operation at all, so
 * "root is full" is reported as a plain failure here rather than
 * silently trying to extend anything).
 */
int fat_create_entry_fixed_root(block_device_t *dev, uint32_t mount_lba,
                                 const fat_geometry_t *geom,
                                 const char *name);

/* fat_update_entry_fixed_root(dev, mount_lba, geom, name,
 *                              new_start_cluster, new_size)
 *
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "实现 fat_write(file, buf, size)：处理簇链扩展（如果当前簇链不够，
 *    追加新簇）。在 Shell 中输入 write test.txt，写入内容，show 能看到。"
 *
 *   fat_create_entry_fixed_root() (Day 24) can only ever WRITE A FRESH
 *   entry — it scans for a free slot and claims it; it has no notion
 *   of "find the entry that's already there and change it". That is
 *   exactly what's missing for `write` to work: fat_create_file()
 *   always creates a file with "起始簇 0...大小 0" (Day 24's own
 *   words), and the very first fat_write() into that file needs to go
 *   back and give that SAME directory entry a real starting cluster
 *   and a nonzero size — not create a second entry. This function is
 *   that missing "find by name, update in place" operation — the
 *   WRITE-side sibling of fat_readdir_fixed_root()'s READ walk and
 *   fat_create_entry_fixed_root()'s "find a FREE slot" walk, this time
 *   "find an OCCUPIED slot whose name matches, and rewrite two of its
 *   fields".
 *
 *   Reads the fixed-size root directory region sector by sector,
 *   exactly the same region fat_readdir_fixed_root()/fat_create_entry_
 *   fixed_root() already read (`geom->root_dir_sectors` sectors
 *   starting at disk-absolute LBA `mount_lba + geom->first_root_dir_
 *   sec`), decoding each live entry's name via fat_decode_short_name()
 *   (skipping 0x00/0xE5/VFAT-long-name/volume-label slots exactly the
 *   same way fat_readdir_fixed_root()'s walk already does — reusing
 *   that same skip logic here rather than inventing a second version
 *   of it). The FIRST entry whose decoded name matches `name`
 *   (case-sensitive strcmp, same convention fat_open()'s own doc
 *   comment already documents for name matching) has its
 *   DIR_FstClusLO/DIR_FstClusHI fields overwritten with
 *   `new_start_cluster` (low 16 bits / high 16 bits respectively —
 *   the exact reverse of fat_readdir_fixed_root()'s own "combine
 *   DIR_FstClusHI:DIR_FstClusLO" decode) and its DIR_FileSize field
 *   overwritten with `new_size`, then that ONE 32-byte-updated sector
 *   is written back to disk — same "read the whole unit, modify only
 *   the bytes that changed, write the whole unit back" discipline
 *   fat_create_entry_fixed_root() itself already uses for a directory
 *   sector.
 *
 *   WHY NOT JUST DELETE-AND-RECREATE: fat_create_entry_fixed_root()
 *   always claims whichever free slot comes FIRST in the region, which
 *   is not necessarily the slot the file used to occupy — a delete-
 *   then-recreate would move the entry around the directory region on
 *   every write and would also require a separate "find and delete"
 *   step this file doesn't otherwise need. Updating in place is both
 *   simpler and preserves the entry's position, matching how a real
 *   FAT filesystem driver updates DIR_FstClus/DIR_FileSize on every
 *   write rather than reallocating the directory entry itself.
 *
 * Returns 0 on success. Returns -1 if `dev`/`geom`/`name` is NULL,
 * `dev->write` is NULL (a read-only backend — same "allocation/update
 * is fundamentally impossible, not transient" case fat_alloc_cluster()
 * and fat_create_entry_fixed_root() already document), the underlying
 * dev->read()/dev->write() call fails while scanning for or updating
 * the entry, or the ENTIRE root region was scanned without finding a
 * live entry named `name` (nothing to update — the caller is expected
 * to have already confirmed the file exists, e.g. via fat_open(),
 * before calling this; see fat_write()'s own doc comment in
 * fat_vfs.h for exactly where that happens).
 */
int fat_update_entry_fixed_root(block_device_t *dev, uint32_t mount_lba,
                                 const fat_geometry_t *geom,
                                 const char *name,
                                 uint32_t new_start_cluster,
                                 uint32_t new_size);

/* ===================================================================
 * DAY26 (Week 4, Day 26 of the 5.0 filesystem roadmap):
 *   "实现 fat_delete_file(name)：找到目录项，标记为已删除（首字节设为
 *    0xE5），释放其占用的簇链（在 FAT 表中标记为空闲）。"
 *
 *   fat_update_entry_fixed_root() (Day 25) walks the fixed root region
 *   looking for a live entry whose name matches and overwrites two of
 *   its fields; this function is the same walk with a different
 *   ending — instead of rewriting DIR_FstClus and DIR_FileSize, it just
 *   sets DIR_Name[0] to FAT_DIRENT_FREE (0xE5) and writes the one
 *   changed sector back. That single byte is the WHOLE deletion as far
 *   as this fixed-root layer is concerned: fat_readdir_fixed_root()'s
 *   own walk already treats FAT_DIRENT_FREE as "skip, keep scanning"
 *   (same skip rule fat_update_entry_fixed_root() above reuses), so a
 *   deleted entry is immediately and correctly invisible to `ls`/
 *   fat_open() without this function needing to touch any other byte
 *   of the 32-byte entry (the stale name/cluster/size bytes are simply
 *   never looked at again — same "rest of entry is stale" contract
 *   FAT_DIRENT_FREE's own #define comment already documents, and the
 *   exact reverse of fat_create_entry_fixed_root()'s own "首字节 0x00
 *   或 0xE5" free-slot scan, which is what lets a deleted slot be
 *   reused by a later `create`/`write`).
 *
 *   Freeing the FAT-table cluster CHAIN itself (the other half of this
 *   day's roadmap text, "释放其占用的簇链") is NOT this function's job
 *   — it only ever touches the directory-entry byte. That's because
 *   this file (fat_common.c) is deliberately variant-agnostic (its own
 *   DAY15 header note: "no #include of fat12.h/fat16.h") and freeing a
 *   chain needs a *_next_cluster()/*_set_cluster_value() pair, which
 *   only exist per-variant. fat_delete_file() (kernel/fs/fat/fat_vfs.c,
 *   fat_vfs.h) is the caller that does both steps in the right order:
 *   walk and free the chain FIRST (while the directory entry still
 *   records the real start_cluster to free), THEN call this function
 *   to mark the entry itself deleted — see fat_delete_file()'s own doc
 *   comment for exactly why that order matters and for the FAT32
 *   sibling of this function, fat32_delete_entry_root() (kernel/fs/
 *   fat/fat32.h/fat32.c), which does the identical one-byte-marker
 *   trick against FAT32's own cluster-chain root instead of this fixed
 *   region.
 *
 *   DAY26 BUGFIX (applies to fat_update_entry_fixed_root() above too,
 *   not just this new function): name matching is now CASE-INSENSITIVE
 *   instead of a raw strcmp(). fat_decode_short_name() always produces
 *   an upper-case decoded name (short names are stored upper-cased by
 *   fat_encode_short_name()'s own strupper() call), but a Shell user
 *   typing e.g. `rm test.txt` supplies `name` in whatever case they
 *   typed — a plain strcmp("TEST.TXT", "test.txt") never matches. Left
 *   unfixed, this meant `write test.txt` run a second time couldn't
 *   find the entry `write` itself had just created the first time
 *   either (fat_update_entry_fixed_root() hit the very same bug) and
 *   silently fell through to fat_create_file() creating a SECOND
 *   "TEST.TXT" entry instead of overwriting the first — exactly the
 *   duplicate-`ls`-listing symptom this day's roadmap text calls out
 *   ("大小写文件名问题"). fat_open() (kernel/fs/fat/fat_vfs.c) has the
 *   identical fix applied for the same reason.
 *
 * Returns 0 on success. Returns -1 if `dev`/`geom`/`name` is NULL,
 * `dev->write` is NULL (read-only backend), the underlying dev->read()/
 * dev->write() call fails while scanning for or deleting the entry, or
 * the entire root region was scanned without finding a live entry
 * named `name` (nothing to delete — same "caller is expected to have
 * already confirmed the file exists via fat_open()" contract
 * fat_update_entry_fixed_root() documents; fat_delete_file() is that
 * caller).
 */
int fat_delete_entry_fixed_root(block_device_t *dev, uint32_t mount_lba,
                                 const fat_geometry_t *geom,
                                 const char *name);

#endif /* LUMA_FAT_COMMON_H */