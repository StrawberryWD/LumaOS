/* kernel/fs/fat/fat12.h
 * LumaOS 5.0 - FAT12 On-Disk Structures & BPB Parser
 *
 * DAY9 (Week 2, Day 9 of the 5.0 filesystem roadmap):
 *   "创建 kernel/fs/fat/fat12.h。定义 BPB 结构体（bpb_12_t）和目录项
 *    结构（fat_dirent_t）。写一个 fat12_parse_bpb(buf)，能算出 RootDir
 *    扇区数、Data 起始扇区。"
 *
 *   This is the FAT12-specific counterpart to Day 5's mbr.h/mbr.c: it
 *   defines the raw on-disk layout of a FAT12 boot sector (the BIOS
 *   Parameter Block, BPB) and a 32-byte root/subdirectory entry, then
 *   provides fat12_parse_bpb() to turn a raw 512-byte sector buffer
 *   (as already read by disk->read(dev, lba, 1, buf) — the Day 6
 *   block_device_t interface) into a populated bpb_12_t plus the two
 *   derived geometry values Day 9's acceptance check asks for:
 *   RootDirSectors (how many sectors the root directory region
 *   occupies) and the first Data Region sector (where cluster 2, the
 *   first real data cluster, begins).
 *
 *   Nothing in this file mounts a filesystem, walks a directory, or
 *   touches the VFS layer from Day 8 (kernel/fs/vfs/vfs.h) yet — that
 *   starts Day 10 ("fat12_mount(block_dev)... 挂载成功返回 0") and
 *   Day 13 ("将 FAT12 挂接到 VFS"). Day 9's scope is strictly the
 *   struct shapes plus the arithmetic that every later FAT12 step
 *   (mount, cluster-chain walk, readdir, open, read) depends on, so
 *   getting the RootDirSectors/Data-region math right here first
 *   means Day 10+ can build on it without re-deriving it.
 *
 * Design notes:
 *   - bpb_12_t is laid out exactly as it appears in a FAT12 boot
 *     sector (offset 0, 512-byte sector 0 of a FAT12-formatted
 *     volume/partition), `packed` for the same reason
 *     mbr_partition_entry_t is in kernel/drivers/block/mbr.h: so it
 *     can be pointer-cast directly over bytes already read into a
 *     sector buffer, with x86 tolerating the resulting unaligned
 *     multi-byte field accesses natively. Field names follow the
 *     conventional Microsoft FAT spec naming (BS_jmpBoot, BPB_
 *     BytesPerSec, BPB_RootEntCnt, ...) so anyone cross-referencing
 *     the spec (or Day 16/17's FAT16/FAT32 work, which extends this
 *     same boot-sector prefix) can match fields by name.
 *   - Only the fields Day 9-14's roadmap steps actually need are
 *     included: enough to compute FAT/root-dir/data-region geometry
 *     (Day 9), verify the filesystem-type string (Day 10: "检查
 *     BS_FileSysType 是否为 FAT12"), and locate the root directory
 *     region for a later readdir (Day 12). Fields specific to
 *     FAT16/32 boot sectors (e.g. FAT32's BPB_FATSz32/BPB_RootClus)
 *     are deliberately left out of *this* struct — Day 15's roadmap
 *     step ("重构 FAT 通用层...创建 fat_common.c") is where the
 *     common vs. FAT12-only fields get properly separated; adding
 *     them here now would just be dead weight this struct doesn't
 *     use.
 *   - fat_dirent_t is the classic 32-byte 8.3 directory entry, exact
 *     on-disk layout, `packed` for the same pointer-cast reason as
 *     above. Day 12 ("遍历每 32 字节条目...提取文件名、属性、起始簇、
 *     文件大小") is what will actually walk an array of these; this
 *     header just defines the shape so that step doesn't also need to
 *     work out the byte layout from scratch.
 *   - fat12_parse_bpb() returns a small result struct (fat12_geometry_t)
 *     rather than several out-parameters, both because Day 9 asks for
 *     two related numbers (RootDirSectors and the Data region's first
 *     sector) that are logically one "layout" result, and because
 *     mbr_parse()'s int-return/out-array split isn't a good fit here:
 *     unlike the 4 fixed MBR entries, there's exactly one BPB per
 *     call, so a single struct return is simpler than an out-param.
 *
 * DAY15 (Week 3, Day 15 of the 5.0 filesystem roadmap) — REFACTOR NOTE:
 *   "重构 FAT 通用层：分离 FAT12/16/32 的共性（BPB 解析、目录项结构），
 *    创建 fat_common.c。"
 *
 *   Everything this file's Day 9 comment above describes as "the
 *   struct shapes" — bpb_12_t's field layout, fat_dirent_t, the
 *   FAT_ATTR_*, FAT_DIRENT_*, and FAT_BOOT_SIG_* constants, and the
 *   RootDirSectors/FirstDataSector geometry formulas — has MOVED to
 *   the new kernel/fs/fat/fat_common.{h,c} (shared by FAT12, and by
 *   Day 16/17's FAT16/FAT32 once they exist). See fat_common.h's own
 *   header comment for the full rationale of what moved and why.
 *
 *   `bpb_12_t` and `fat12_geometry_t` below are now TYPE ALIASES onto
 *   fat_common.h's fat_bpb_common_t/fat_geometry_t rather than
 *   independent struct definitions — every field name is unchanged
 *   (BPB_BytesPerSec, DIR_Name, ...), so every existing caller in this
 *   codebase (kmain.c's DAY9-DAY14 test sections, fat12_vfs.c) keeps
 *   compiling and behaving exactly as before, with zero changes on
 *   their end. What's still genuinely FAT12-SPECIFIC and stays defined
 *   in THIS file: fat12_fs_t (a FAT12 mount's own state), the 12-bit
 *   cluster-chain constants/walker (fat12_next_cluster(), FAT12_
 *   CLUSTER_*), and the root-directory walk (fat12_readdir(),
 *   fat12_dirlist_entry_t, FAT12_DIRLIST_MAX_ENTRIES) — none of these
 *   are shared byte-for-byte with FAT16/32 the way the BPB/dirent
 *   shapes are (12-bit vs. 16-bit vs. 28-bit cluster entries in
 *   particular are a hard divergence, not just a naming one).
 */
#ifndef LUMA_FAT12_H
#define LUMA_FAT12_H

#include <stdint.h>
#include "fat_common.h"                  /* DAY15: fat_bpb_common_t, fat_dirent_t,
                                            FAT_ATTR_*, FAT_DIRENT_*, fat_geometry_t;
                                            see this file's DAY15 header note above. */
#include "../../drivers/block/block.h"   /* block_device_t, used by fat12_mount() (DAY10) */

/* ===== DAY15 backward-compatible type aliases =====
 * bpb_12_t/fat12_geometry_t used to be independent struct definitions
 * in this file (Day 9); they are now the exact same shapes as
 * fat_common.h's fat_bpb_common_t/fat_geometry_t, just kept under
 * their original Day-9 names so nothing else in this codebase has to
 * change. See fat_common.h for the authoritative field-by-field
 * layout/comments — this file no longer duplicates them. */
typedef fat_bpb_common_t bpb_12_t;
typedef fat_geometry_t   fat12_geometry_t;

/* ===== BIOS Parameter Block, directory entry, and geometry shapes =====
 *
 * DAY15: bpb_12_t/fat_dirent_t/fat12_geometry_t and the FAT_ATTR_*,
 * FAT_DIRENT_*, and boot-signature constants that used to be defined
 * directly in this section have all moved to kernel/fs/fat/
 * fat_common.h (included above) — see this file's DAY15 header note
 * for the full rationale. bpb_12_t/fat12_geometry_t are now the type
 * aliases declared just above this comment; fat_dirent_t and the
 * FAT_ATTR_* and FAT_DIRENT_* constants are used here (in
 * fat12_readdir()'s declaration and fat12_dirlist_entry_t below) exactly as before,
 * just pulled in via fat_common.h instead of defined locally.
 */

/* fat12_parse_bpb(buf, out_bpb, out_geom)
 *
 * Reads a raw 512-byte FAT12 boot-sector buffer (as already read via
 * block_device_t's disk->read(dev, lba, 1, buf) — same pattern Day 3/
 * 5 use for LBA 0) and:
 *
 *   1. Copies it into *out_bpb (a straight struct-sized memcpy, since
 *      bpb_12_t is packed to match the on-disk layout exactly — same
 *      technique mbr_parse() uses for mbr_partition_entry_t).
 *   2. Computes the Day 9 acceptance-criteria values into *out_geom:
 *        RootDirSectors  = ((BPB_RootEntCnt * 32) + (BPB_BytesPerSec - 1))
 *                           / BPB_BytesPerSec
 *        FirstRootDirSec = BPB_RsvdSecCnt + (BPB_NumFATs * BPB_FATSz16)
 *        FirstDataSector = FirstRootDirSec + RootDirSectors
 *      (the standard Microsoft FAT spec formulas), plus the
 *      first-FAT-sector, total-sector-count, and bytes-per-cluster
 *      values later steps will also need.
 *
 * `out_bpb` and/or `out_geom` may be NULL if the caller only wants
 * the other one populated.
 *
 * Returns 0 if the buffer looks like a genuine FAT12 boot sector: the
 * 0x55AA signature at FAT_BOOT_SIG_OFFSET (DAY15: renamed from
 * FAT12_BOOT_SIG_OFFSET when this check moved into fat_common.c's
 * fat_parse_bpb_common(), see this file's DAY15 header note) is
 * present AND BPB_BytesPerSec/BPB_SecPerClus/BPB_NumFATs all hold
 * spec-legal values AND BS_FilSysType reads "FAT12" (see fat12.c's
 * DAY9 comment for why 0x55AA alone isn't sufficient — every bootable
 * x86 sector, FAT or not, ends in 0x55AA). Returns -1 otherwise;
 * *out_geom is still filled in on a -1 return (mirroring mbr_parse()'s
 * "entries still copied out, treat with suspicion" contract) so a
 * caller doing diagnostic dumping can inspect what was actually on
 * the sector.
 */
int fat12_parse_bpb(const uint8_t buf[512], bpb_12_t *out_bpb,
                     fat12_geometry_t *out_geom);

/* ===== Mount state (Day 10's actual deliverable) =====
 *
 * DAY10 (Week 2, Day 10 of the 5.0 filesystem roadmap):
 *   "实现 fat12_mount(block_dev)：读取引导扇区，检查 BS_FileSysType
 *    是否为 FAT12。挂载成功返回 0，失败返回 -1，在终端打印
 *    "FAT12 mounted"。"
 *
 *   fat12_fs_t is the small piece of mount state a successful
 *   fat12_mount() call produces: which block_device_t the volume
 *   lives on, plus the BPB and derived geometry fat12_parse_bpb()
 *   already knows how to compute (Day 9). This is deliberately NOT
 *   wired into kernel/fs/vfs/vfs.h's `struct filesystem` yet — that
 *   plug-in ("注册 fat12_filesystem_ops") is Day 13's job. Until
 *   then, fat12_fs_t is a standalone handle a caller (today: kmain.c;
 *   Day 11+: the cluster-chain walk and readdir code) can hold and
 *   pass around on its own.
 */
typedef struct {
    block_device_t    *dev;        /* the volume's underlying block device */
    uint32_t           mount_lba;  /* DAY11: disk-absolute LBA of this volume's boot
                                     * sector (the `lba` fat12_mount() was called
                                     * with) — see DAY11 note on fat12_geometry_t
                                     * below for why this has to be remembered. */
    bpb_12_t           bpb;        /* parsed boot sector, see fat12_parse_bpb() */
    fat12_geometry_t   geom;       /* derived geometry, see fat12_parse_bpb() */
} fat12_fs_t;

/* DAY11 NOTE ON fat12_geometry_t's *_sector FIELDS BEING VOLUME-RELATIVE:
 * fat12_parse_bpb() (Day 9) computes first_fat_sector/first_root_dir_sec/
 * first_data_sector purely from BPB fields — they are sector offsets
 * *within the volume* (sector 0 = this volume's own boot sector),
 * exactly matching the FAT spec's own formulas. They are NOT
 * disk-absolute LBAs whenever the volume doesn't start at LBA 0 of
 * the underlying disk (e.g. this project's Day 9-followup/Day 10 test
 * fixture at disk LBA 2500, or Day 11's own fixture at disk LBA 5000).
 *
 * Through Day 10 this distinction never mattered in practice: no code
 * in this codebase ever issued a dev->read() using one of these
 * fields as if it were disk-absolute — fat12_mount() only reads the
 * single boot-sector LBA it's explicitly given, and Day 9/10's kmain.c
 * tests only ever print geom's fields, never dereference them. Day 11's
 * fat12_next_cluster() is the FIRST code path to actually turn
 * first_fat_sector into a dev->read() LBA — and doing that with just
 * first_fat_sector alone (as an early version of this function did,
 * before this was caught by a live QEMU run showing garbage cluster
 * values at LBA 5000 despite a passing host cross-validation test that
 * happened to mount at LBA 0) silently reads the wrong sector on any
 * volume not sitting at LBA 0 of the disk. Fixed by having
 * fat12_mount() remember mount_lba above, and fat12_next_cluster()
 * add it back in: the correct disk-absolute LBA is always
 * `fs->mount_lba + fs->geom.first_fat_sector + <sectors into the FAT>`.
 */

/* fat12_mount(dev, lba, out_fs)
 *
 * Reads the boot sector at `lba` (volume-relative — normally 0 for a
 * whole-disk FAT12 volume, or an MBR partition's start_lba per Day 5's
 * mbr_find_first_active_fat() for a partitioned one) off `dev` via
 * dev->read(dev, lba, 1, buf) — the Day 6 block_device_t interface,
 * same call kmain.c already makes directly — then hands the buffer to
 * fat12_parse_bpb() (Day 9) and additionally checks BS_FilSysType
 * reads exactly "FAT12   " (8 bytes, space-padded), the Day 10
 * acceptance criterion's specific check on top of Day 9's broader
 * sanity check (which already includes this same comparison — see
 * fat12_parse_bpb()'s contract above — so this is a second look at
 * the same field for the sake of Day 10's own explicit "check
 * BS_FileSysType" requirement, not a materially different test).
 *
 * On success, populates *out_fs (dev pointer, parsed bpb, derived
 * geometry) and returns 0. On failure — dev->read() itself fails, OR
 * the sector doesn't parse as a valid FAT12 BPB per
 * fat12_parse_bpb()'s contract — returns -1; *out_fs is left
 * zero-initialized in that case (unlike fat12_parse_bpb()'s "fill
 * anyway" contract: a failed mount has no sensible fs handle to hand
 * back, so callers must check the return value before using *out_fs).
 *
 * `out_fs` must not be NULL.
 */
int fat12_mount(block_device_t *dev, uint32_t lba, fat12_fs_t *out_fs);

/* ===== Cluster-chain values (Day 11's actual deliverable) =====
 *
 * FAT12 reserves the low end of the cluster-number space for
 * bookkeeping rather than real data clusters — a FAT[cluster] table
 * entry doesn't always hold "the next cluster in a chain", it can
 * also hold one of these sentinel meanings. Every later step that
 * walks a chain (Day 11 here; Day 12's readdir indirectly, via the
 * DIR_FstClusLO it hands to Day 11's walker; Day 19's file read) has
 * to recognize these before treating a FAT entry as "keep going".
 *
 *   0x000            - free cluster (not allocated to any file)
 *   0x001             - reserved, never used by real data (mirrors
 *                        clusters 0/1 both being reserved FAT
 *                        entries per the spec — see fat12.c's DAY9
 *                        FirstDataSector comment)
 *   0xFF7             - marks a cluster the OS has flagged "bad
 *                        sector, do not allocate"
 *   0xFF8 - 0xFFF      - end-of-chain. IMPORTANT: this is a *range*,
 *                        not a single value — a real mkfs.fat/format
 *                        tool is free to write any value in
 *                        0xFF8..0xFFF to terminate a chain (0xFFF is
 *                        the most common in practice, but treating
 *                        ONLY 0xFFF as "end" and anything else in
 *                        that range as "a real next-cluster number"
 *                        is a bug — this project's own test fixture,
 *                        built with `mkfs.fat -F 12`, terminates its
 *                        test file's chain at 0xFFF, but code that
 *                        hardcodes that single value instead of the
 *                        >= 0xFF8 range would silently break against
 *                        other formatters).
 */
#define FAT12_CLUSTER_FREE        0x000
#define FAT12_CLUSTER_RESERVED    0x001  /* clusters 0 and 1 are both reserved */
#define FAT12_CLUSTER_BAD         0xFF7
#define FAT12_CLUSTER_EOC_MIN     0xFF8  /* >= this value means "end of chain" */
#define FAT12_CLUSTER_EOC_MAX     0xFFF  /* largest legal 12-bit value */

/* fat12_next_cluster(fs, cluster)
 *
 * DAY11 (Week 2, Day 11 of the 5.0 filesystem roadmap):
 *   "实现 FAT12 簇链遍历函数 fat12_next_cluster(cluster)（处理 1.5
 *    字节对齐，注意奇偶簇）。给定根目录的起始簇（通常为 0），能遍历
 *    打印出前 10 个簇号。"
 *
 *   NOTE ON THE ROADMAP TEXT: strictly speaking FAT12/16's root
 *   directory is NOT a cluster chain — it's a fixed-size region
 *   (BPB_RootEntCnt entries) located by fat12_geometry_t's
 *   first_root_dir_sec, described here in fat12.h's Day 9 doc
 *   comment. There is no "root directory's starting cluster" to feed
 *   this function on FAT12/16 (that concept only exists for FAT32's
 *   BPB_RootClus, a Day 17 concern). The roadmap's acceptance text is
 *   read here as shorthand for "exercise the cluster-chain walker
 *   end-to-end, printing 10 cluster numbers" rather than a literal
 *   instruction to chain-walk the root directory — see kmain.c's
 *   DAY11 section for how the test harness resolves this (it walks
 *   cluster 2 onward, the first real data cluster, using a test file
 *   built specifically to have a chain >= 10 clusters long, plus a
 *   sanity call on cluster 0/1 to confirm the reserved values print
 *   without the walker crashing or looping).
 *
 *   `cluster` is the CURRENT cluster number; this function looks up
 *   FAT[cluster] — the table entry describing what comes after it —
 *   and returns that raw 12-bit value: either a real next-cluster
 *   number (2..0xFEF), FAT12_CLUSTER_FREE/RESERVED/BAD, or a value
 *   >= FAT12_CLUSTER_EOC_MIN meaning "chain ends here". Callers walk
 *   a whole chain by repeatedly feeding each return value back in as
 *   the next `cluster` argument, stopping once the return is negative
 *   (I/O error) or >= FAT12_CLUSTER_EOC_MIN (legitimate end).
 *
 *   ---- The 1.5-byte-per-entry packing, and why it must be handled
 *   this way (this is the step the roadmap calls out by name as the
 *   easiest one to get backwards) ----
 *
 *   A FAT12 table entry is only 12 bits, not a clean 8 or 16 — so two
 *   consecutive cluster numbers (one even-numbered, one odd-numbered)
 *   are packed together into 3 bytes (24 bits = 2*12 bits) instead of
 *   each getting its own byte-aligned slot. The byte offset of
 *   cluster N's entry within the FAT is `N + N/2` (equivalently
 *   `N*3/2`, but `N + N/2` is used here specifically because it never
 *   needs a 3x multiply and, more importantly, makes it visually
 *   obvious this is "N whole bytes, plus a half-byte-pair correction"
 *   rather than looking like an arbitrary scaling factor — the
 *   integer-division truncation of N/2 is exactly what's wanted here,
 *   since a pair of clusters (2*(N/2), 2*(N/2)+1) always shares one
 *   fixed 3-byte group starting at byte 3*(N/2)).
 *
 *   Once you have the 2 bytes at that offset (call them b0 = low
 *   byte, b1 = high byte of the 16-bit little-endian read), which 12
 *   bits belong to cluster N depends on whether N is even or odd,
 *   because the packing alternates which half of the 3-byte group
 *   each cluster owns:
 *
 *     - N even:  N's 12 bits are the FIRST (low) 12 bits of the
 *                3-byte group -> all of b0, plus the low nibble of
 *                b1: value = b0 | ((b1 & 0x0F) << 8)
 *     - N odd:   N's 12 bits are the LAST (high) 12 bits of the
 *                3-byte group -> the high nibble of b0, plus all of
 *                b1: value = (b0 >> 4) | (b1 << 4)
 *
 *   Concretely: bytes [B0 B1 B2] hold clusters N (even) and N+1 (odd)
 *   packed as nibbles [b0_lo b0_hi | b1_lo b1_hi | b2_lo b2_hi] =
 *   [N_0 N_1 | N_2 N0+1_0 | N+1_1 N+1_2] — cluster N owns B0's full
 *   byte plus B1's low nibble; cluster N+1 owns B1's high nibble plus
 *   B2's full byte. Getting the even/odd branches swapped is the
 *   classic FAT12 bug this doc comment (and the roadmap text) warns
 *   about: it silently produces a plausible-looking but wrong cluster
 *   number instead of an obvious crash, since both branches produce
 *   values in the same 0-4095 range.
 *
 *   ---- Sector-boundary crossing ----
 *
 *   The 2 bytes an entry needs can straddle a sector boundary: if
 *   `N + N/2` mod BPB_BytesPerSec == BPB_BytesPerSec - 1, byte b0 is
 *   the last byte of one FAT sector and b1 is the first byte of the
 *   next one. fat12.c's implementation always reads two consecutive
 *   FAT sectors into one buffer before indexing into it, specifically
 *   so this case never needs special-casing separately from the
 *   common case — see fat12.c's DAY11 comment for why that's the
 *   chosen approach over a boundary-only fallback path.
 *
 * Returns the raw 12-bit FAT entry value (0..0xFFF, always
 * non-negative) on success. Returns -1 if the underlying
 * dev->read(...) call fails (mirrors this project's "negative means
 * failure" convention used throughout — see fat12_mount()'s
 * comment) — this is unambiguous because a real 12-bit entry can
 * never itself be negative.
 */
int32_t fat12_next_cluster(fat12_fs_t *fs, uint16_t cluster);

/* fat12_set_cluster_value(fs, cluster, value)
 *
 * DAY23 (Week 4, Day 23 of the 5.0 filesystem roadmap):
 *   "实现 fat_alloc_cluster()：分配一个新簇，并更新 FAT 表（写回磁盘）。
 *    分配成功后，再次读取 FAT 对应位置值变为 0xFFF（FAT12）或 0xFFFF。"
 *
 *   The write-side mirror of fat12_next_cluster() above: instead of
 *   reading FAT[cluster] and returning its raw 12-bit value, this
 *   WRITES `value` (a raw 12-bit quantity, 0..0xFFF — same range
 *   fat12_next_cluster() returns) into FAT[cluster] and persists it
 *   to disk via fs->dev->write(). fat_alloc_cluster() (kernel/fs/fat/
 *   fat_vfs.c) is this function's only caller today: it finds a free
 *   cluster via Day 22's fat_find_free_cluster(), then calls this
 *   function with `value = FAT12_CLUSTER_EOC_MAX` (0xFFF) to claim it
 *   as a new, single-cluster file's terminating cluster.
 *
 *   SAME READ-MODIFY-WRITE SHAPE AS THE READ SIDE, on purpose: this
 *   function computes the exact same `fat_byte_offset`/`lba`/
 *   `offset_in_sector` fat12_next_cluster() does (see that function's
 *   own doc comment for the full "N + N/2, two-sector read to dodge
 *   the sector-boundary-straddle case" rationale — none of that
 *   changes for a write), reads the same 2-sector window off disk
 *   first, MODIFIES only the 12 bits belonging to `cluster` within
 *   it (the neighboring cluster packed into the other nibble of the
 *   shared byte is read back unchanged and written back byte-for-byte
 *   identical — see the even/odd branches below), then writes that
 *   same 2-sector window back. A cluster's 12 bits are never
 *   byte-aligned on their own (see fat12_next_cluster()'s packing
 *   comment), so writing them in isolation without first reading the
 *   surrounding byte would silently corrupt whichever neighboring
 *   cluster shares that byte — this read-modify-write shape is not
 *   optional the way it might be for FAT16/32's byte-aligned entries.
 *
 *   FAT #1 ONLY: same "never touch the second/backup FAT copy" choice
 *   fat12_next_cluster() documents (this project's mount/read paths
 *   never consult it either, so a backup-FAT copy that silently goes
 *   stale after this function runs is consistent with how the rest of
 *   this codebase already treats it — not a new limitation introduced
 *   here).
 *
 * Returns 0 on success. Returns -1 if `fs`/`fs->dev` is NULL,
 * `fs->dev->write` is NULL (a read-only backend — see block.h's
 * doc comment on block_write_fn), the underlying dev->read() call
 * (needed to preserve the neighboring cluster's nibble) fails, or the
 * underlying dev->write() call fails.
 */
int fat12_set_cluster_value(fat12_fs_t *fs, uint16_t cluster, uint16_t value);

/* ===== Root directory listing (Day 12's actual deliverable) =====
 *
 * DAY12 (Week 2, Day 12 of the 5.0 filesystem roadmap):
 *   "实现 fat12_readdir(root_node)：读取根目录区（Root Directory
 *    Region），遍历每 32 字节条目。提取文件名（去掉空格）、属性、起始
 *    簇、文件大小。在终端列出根目录下所有文件名（至少能看到 KERNEL
 *    BIN 和 LOADER BIN）。"
 *
 *   NOTE ON "root_node" AND "KERNEL BIN"/"LOADER BIN": the roadmap's
 *   acceptance text is phrased as if this project's own lumaos-5.0.img
 *   were itself a FAT12 volume whose root directory holds KERNEL.BIN
 *   and LOADER.BIN — but LBA 0 of this image is LumaOS's own custom
 *   mbr.asm bootloader, not a FAT12 boot sector (same point already
 *   made in fat12.h's DAY9 comment and kmain.c's DAY9 comment), and
 *   kernel.bin/loader.bin are dd'd onto raw disk-absolute sectors by
 *   build.sh, never stored as named files inside a filesystem. There
 *   is no FAT12 volume anywhere in this project that literally
 *   contains a "KERNEL.BIN" or "LOADER.BIN" directory entry, and
 *   Day 8/13's `struct vfs_node` (kernel/fs/vfs/vfs.h) isn't wired to
 *   FAT12 until Day 13 ("将 FAT12 挂接到 VFS"), so `fat12_readdir()`
 *   below intentionally takes a `fat12_fs_t *` (the same standalone
 *   mount handle fat12_mount()/fat12_next_cluster() already use), not
 *   a `vfs_node_t *`. This is read the same way Day 11's header
 *   comment reads "根目录的起始簇（通常为 0）": as shorthand for
 *   "list whatever real, verifiable files the current fixture
 *   actually has", not a literal KERNEL.BIN/LOADER.BIN requirement.
 *   kmain.c's DAY12 section lists tests/fat12-with-file.img's root
 *   directory (Day 11's own fixture, already embedded at LBA 5000),
 *   which for exactly this reason contains BIGFILE.BIN and SMALL.BIN
 *   instead — two real, independently-verifiable files this project
 *   already cross-validated in Day 11's header comment — rather than
 *   inventing a new fixture just to match the roadmap text's literal
 *   two example filenames.
 *
 *   `fat12_dirlist_entry_t` is the small, already-decoded result of
 *   walking one 32-byte fat_dirent_t: a NUL-terminated display name
 *   with trailing 8.3 padding spaces stripped (Day 12's "去掉空格"),
 *   plus the attribute byte, starting cluster, and file size the
 *   roadmap explicitly asks to extract. This is deliberately a
 *   separate, smaller struct from fat_dirent_t itself (which stays
 *   the raw on-disk layout, Day 9's struct) rather than reusing
 *   fat_dirent_t directly for the decoded result — the space-padded
 *   11-byte DIR_Name isn't a C string, and callers (kmain.c's test
 *   harness today; Day 14's `cmd_ls` eventually) want a printable
 *   name, not raw on-disk bytes.
 *
 *   NAME DECODING ("去掉空格" — strip the spaces): a raw DIR_Name[11]
 *   is 8 bytes of base name + 3 bytes of extension, space-padded on
 *   the right within each part (e.g. "BIGFILE BIN" as the literal 11
 *   bytes for "BIGFILE.BIN", "SMALL   BIN" for "SMALL.BIN"). Decoding
 *   trims trailing spaces from the 8-byte base separately from the
 *   3-byte extension (an entry with a short base name but a full
 *   3-char extension, e.g. "A       TXT", must not have the base's
 *   trailing spaces bleed into where the dot belongs), then joins
 *   them with "." only if the extension part is non-empty (many real
 *   FAT volumes have extension-less entries, e.g. a subdirectory or a
 *   file literally named "README" with no extension — an unconditional
 *   dot would wrongly render those as "README."). A directory entry's
 *   FAT_ATTR_DIRECTORY-flagged name (e.g. "." or ".." synthetic
 *   entries once subdirectories exist, not exercised by this project
 *   until Day 42) follows this same 8+3 decode; nothing here special-
 *   cases "." / ".." differently.
 *
 *   ENTRIES SKIPPED BY THE WALK (none of these count toward
 *   `out_count`/get returned to the caller):
 *     - DIR_Name[0] == FAT_DIRENT_END (0x00): per the FAT spec this
 *       marks not just "this entry is free" but "every entry from
 *       here to the end of the region is also free" — so encountering
 *       it is the walk's normal, expected stop condition, not an
 *       error (mirrors fat12_next_cluster()'s treatment of
 *       FAT12_CLUSTER_EOC_MIN..MAX as "legitimate end", not failure).
 *     - DIR_Name[0] == FAT_DIRENT_FREE (0xE5): a deleted entry — the
 *       rest of the 32 bytes are stale leftovers from whatever file
 *       used to be there, so it's skipped but the walk continues
 *       (unlike 0x00, a deleted slot doesn't imply every entry after
 *       it is also free — a later create could have reused a
 *       different, non-adjacent free slot).
 *     - DIR_Attr == FAT_ATTR_LONG_NAME (the four bits all set): a
 *       VFAT long-filename fragment, not a real 8.3 entry. This
 *       roadmap is 8.3-only through Day 14 (see fat12.h's DAY9 file
 *       comment), so these are skipped rather than decoded — the
 *       fixture this project mounts (tests/fat12-with-file.img, an
 *       mtools `mcopy` of two files whose names are already legal
 *       8.3 short names) never actually produces one, but real-world
 *       FAT volumes commonly do, and skipping instead of misreading
 *       a fragment as a garbled short name is the only safe option.
 *     - DIR_Attr & FAT_ATTR_VOLUME_ID: the root directory's volume
 *       label entry (if present) isn't a file or a directory; skipped
 *       for the same reason.
 *
 * DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap) — REFACTOR NOTE:
 *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。"
 *
 *   fat12_dirlist_entry_t and FAT12_DIRLIST_MAX_ENTRIES below are now
 *   TYPE/MACRO ALIASES onto fat_common.h's fat_dirlist_entry_t /
 *   FAT_DIRLIST_MAX_ENTRIES, and fat12_readdir() itself is now a thin
 *   wrapper around fat_common.h's fat_readdir_fixed_root() — same
 *   "backward-compatible alias, zero caller changes" pattern Day 15
 *   already used for bpb_12_t/fat12_geometry_t. See fat_common.h's
 *   DAY16 header note for the full rationale (FAT16 shares this exact
 *   fixed-root-region shape byte-for-byte, so the walk moved to the
 *   shared layer instead of being copy-pasted into fat16.c).
 */
#define FAT12_DIRLIST_MAX_ENTRIES  FAT_DIRLIST_MAX_ENTRIES

typedef fat_dirlist_entry_t fat12_dirlist_entry_t;

/* fat12_readdir(fs, out_entries, max_entries, out_count)
 *
 * Reads `fs`'s root directory region (fs->geom.root_dir_sectors
 * sectors at disk-absolute LBA fs->mount_lba + fs->geom.
 * first_root_dir_sec) and decodes each 32-byte fat_dirent_t slot into
 * `out_entries[]`, per the DAY12 doc comment above for exactly which
 * entries are skipped and how names are decoded. Stops early, without
 * error, at the first FAT_DIRENT_END (0x00) entry (spec-defined "no
 * more entries past this point") or once `max_entries` decoded
 * entries have been written, whichever comes first.
 *
 * On success, writes the number of entries actually decoded into
 * `*out_count` (0 is a legal result: an empty root directory) and
 * returns 0. Returns -1 if the underlying dev->read(...) call fails
 * partway through the region — mirrors fat12_next_cluster()'s
 * "negative means I/O failure" convention; *out_count is left
 * whatever it was last set to (entries already decoded before the
 * failing read are still valid, same "don't discard partial progress
 * on a later error" spirit as fat12_mount() zeroing *out_fs only on
 * its *own* first read failing, not a later one).
 *
 * `fs`, `out_entries`, and `out_count` must not be NULL. `out_entries`
 * must have room for at least `max_entries` fat12_dirlist_entry_t
 * slots.
 *
 * DAY16: implemented as a thin wrapper around fat_common.h's
 * fat_readdir_fixed_root(fs->dev, fs->mount_lba, &fs->geom, ...) — see
 * this file's DAY16 refactor note above.
 */
int fat12_readdir(fat12_fs_t *fs, fat12_dirlist_entry_t *out_entries,
                   uint32_t max_entries, uint32_t *out_count);

#endif /* LUMA_FAT12_H */