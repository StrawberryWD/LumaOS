/* kernel/fs/fat/fat_common.c
 * LumaOS 5.0 - FAT12/16/32 Shared BPB Parser, Geometry & Type Detection
 *
 * DAY15 (Week 3, Day 15 of the 5.0 filesystem roadmap):
 *   "重构 FAT 通用层：分离 FAT12/16/32 的共性（BPB 解析、目录项结构），
 *    创建 fat_common.c。一个 fat_mount() 根据 BPB 的 BytesPerSector 和
 *    TotalSectors 自动判断类型。"
 *
 *   See fat_common.h for the full design rationale: which structs/
 *   macros moved here from fat12.h, which stayed FAT12-specific, and
 *   why. This file is the arithmetic + I/O: fat_parse_bpb_common() is
 *   fat12_parse_bpb()'s old sanity-check logic minus the FAT12-only
 *   BS_FilSysType string match; fat_compute_geometry() is its old
 *   geometry-formula block, generalized with the new TotalClusters/
 *   type fields; fat_classify_type() is genuinely new (Day 9-14 never
 *   needed to tell FAT variants apart, since only FAT12 existed);
 *   fat_mount() is the new top-level entry point that ties all three
 *   together against a real block_device_t, mirroring fat12_mount()'s
 *   old read-then-parse shape but without assuming FAT12 specifically.
 */
#include "fat_common.h"
#include "../../core/kernel.h"   /* memcpy, memset, strlen */
#include "../../lib/string.h"    /* DAY24: strrchr(), strupper() —
                                     kernel.h only hand-declares a
                                     subset of string.c's functions,
                                     and doesn't include strrchr/
                                     strupper at all; identical
                                     redeclarations of the functions
                                     kernel.h DOES already declare
                                     (strlen/strcpy/strcmp/strchr/
                                     memcpy/memset) are legal C, so
                                     this include is purely additive. */

int fat_parse_bpb_common(const uint8_t buf[512], fat_bpb_common_t *out_bpb)
{
    fat_bpb_common_t bpb;
    memcpy(&bpb, buf, sizeof(fat_bpb_common_t));

    if (out_bpb)
        *out_bpb = bpb;

    /* Same three-field-plus-signature check fat12_parse_bpb() used to
     * do inline through Day 14 (see this file's header comment and
     * fat_common.h's doc comment on fat_parse_bpb_common() for why
     * these specific fields/values are the ones the FAT spec actually
     * constrains identically across FAT12/16/32) — just without the
     * FAT12-only BS_FilSysType == "FAT12" comparison, which stays in
     * fat12.c since it's a FAT12-specific check, not a shared one. */
    int sig_ok = (buf[FAT_BOOT_SIG_OFFSET]     == FAT_BOOT_SIG_BYTE0 &&
                  buf[FAT_BOOT_SIG_OFFSET + 1] == FAT_BOOT_SIG_BYTE1);

    int bytes_per_sec_ok = (bpb.BPB_BytesPerSec == 512  ||
                             bpb.BPB_BytesPerSec == 1024 ||
                             bpb.BPB_BytesPerSec == 2048 ||
                             bpb.BPB_BytesPerSec == 4096);

    int sec_per_clus_ok = (bpb.BPB_SecPerClus == 1  || bpb.BPB_SecPerClus == 2  ||
                            bpb.BPB_SecPerClus == 4  || bpb.BPB_SecPerClus == 8  ||
                            bpb.BPB_SecPerClus == 16 || bpb.BPB_SecPerClus == 32 ||
                            bpb.BPB_SecPerClus == 64 || bpb.BPB_SecPerClus == 128);

    int num_fats_ok = (bpb.BPB_NumFATs == 1 || bpb.BPB_NumFATs == 2);

    return (sig_ok && bytes_per_sec_ok && sec_per_clus_ok && num_fats_ok) ? 0 : -1;
}

fat_type_t fat_classify_type(uint32_t total_clusters)
{
    /* Microsoft FAT spec (fatgen103, "FAT Type Determination") — see
     * fat_common.h's doc comment on this function for the full
     * rationale, including why this is NOT simply "compare
     * BytesPerSec/TotalSectors directly". Order matters: check the
     * smallest boundary first. */
    if (total_clusters < 4085)
        return FAT_TYPE_FAT12;
    if (total_clusters < 65525)
        return FAT_TYPE_FAT16;
    return FAT_TYPE_FAT32;
}

int fat_compute_geometry(const fat_bpb_common_t *bpb, fat_geometry_t *out_geom)
{
    /* ---- Standard Microsoft FAT spec geometry formulas ----
     * Identical to fat12_parse_bpb()'s original inline computation
     * through Day 14 — see fat12.c's original DAY9 comment for the
     * formula-by-formula rationale, reproduced here since this is now
     * where the arithmetic actually lives. */

    uint32_t bytes_per_sec = (bpb->BPB_BytesPerSec != 0) ? bpb->BPB_BytesPerSec : 512;
    uint32_t sec_per_clus  = (bpb->BPB_SecPerClus  != 0) ? bpb->BPB_SecPerClus  : 1;

    uint32_t root_dir_sectors =
        ((uint32_t)bpb->BPB_RootEntCnt * 32 + (bytes_per_sec - 1)) / bytes_per_sec;

    uint32_t first_fat_sector = bpb->BPB_RsvdSecCnt;
    uint32_t first_root_dir_sec =
        bpb->BPB_RsvdSecCnt + ((uint32_t)bpb->BPB_NumFATs * bpb->BPB_FATSz16);

    uint32_t first_data_sector = first_root_dir_sec + root_dir_sectors;

    uint32_t total_sectors =
        bpb->BPB_TotSec16 != 0 ? (uint32_t)bpb->BPB_TotSec16 : bpb->BPB_TotSec32;

    /* DAY15: the one genuinely new number — TotalClusters, the value
     * fat_classify_type() actually keys off of. Guarded against
     * total_sectors < first_data_sector (a bogus/tiny BPB, e.g. the
     * all-zero or garbage buffers fat_parse_bpb_common() already
     * rejected on sanity grounds, but this function computes geometry
     * "anyway" per its documented contract) so the subtraction never
     * wraps around a uint32_t into a huge bogus cluster count. */
    uint32_t data_sectors =
        (total_sectors > first_data_sector) ? (total_sectors - first_data_sector) : 0;
    uint32_t total_clusters = data_sectors / sec_per_clus;

    if (out_geom) {
        out_geom->root_dir_sectors   = root_dir_sectors;
        out_geom->first_root_dir_sec = first_root_dir_sec;
        out_geom->first_data_sector  = first_data_sector;
        out_geom->first_fat_sector   = first_fat_sector;
        out_geom->total_sectors      = total_sectors;
        out_geom->bytes_per_cluster  = bytes_per_sec * sec_per_clus;
        out_geom->total_clusters     = total_clusters;
        out_geom->type               = fat_classify_type(total_clusters);
    }

    return 0;
}

fat_type_t fat_mount(block_device_t *dev, uint32_t lba,
                      fat_bpb_common_t *out_bpb, fat_geometry_t *out_geom)
{
    if (!dev || !dev->read)
        return FAT_TYPE_UNKNOWN;

    /* static scratch buffer — same convention fat12_mount() already
     * uses for its own boot-sector read (see fat12.c's DAY10
     * comment): avoids putting 512 bytes on what may be a small
     * kernel stack. */
    static uint8_t boot_sector[512];
    if (dev->read(dev, lba, 1, boot_sector) != 0)
        return FAT_TYPE_UNKNOWN;

    fat_bpb_common_t bpb;
    int bpb_ok = (fat_parse_bpb_common(boot_sector, &bpb) == 0);
    if (out_bpb)
        *out_bpb = bpb;

    fat_geometry_t geom;
    fat_compute_geometry(&bpb, &geom);   /* always 0 — pure arithmetic, see its own doc comment */
    if (out_geom)
        *out_geom = geom;

    if (!bpb_ok)
        return FAT_TYPE_UNKNOWN;

    return geom.type;
}

/* DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
 *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载一个
 *    FAT16 镜像，ls 能正确列出文件。"
 *
 *   fat_decode_short_name() and fat_readdir_fixed_root() below are the
 *   promoted, FAT12/16-shared versions of fat12.c's original DAY12
 *   fat12_decode_short_name() (was `static`) and fat12_readdir()'s
 *   decode loop. The logic is UNCHANGED from Day 12 — this is a pure
 *   relocation/generalization (fat12_fs_t's `dev`/`mount_lba`/`geom`
 *   fields become explicit parameters instead of struct members) so
 *   fat16_readdir() (kernel/fs/fat/fat16.c) can call the exact same
 *   code fat12_readdir() (now a thin wrapper, see fat12.c's own DAY16
 *   note) already relies on. See fat_common.h's DAY16 header note for
 *   the full "why this is safe to share" rationale (FAT12 and FAT16
 *   have byte-for-byte identical fixed-root-region layouts).
 */
void fat_decode_short_name(const uint8_t raw11[11], char out13[13])
{
    /* DIR_Name[0] == 0x05 is a documented special case in the FAT
     * spec: byte 0x05 stands in for a *literal* leading 0xE5 byte in
     * a filename (0xE5 itself is reserved for FAT_DIRENT_FREE, so a
     * real filename that happens to start with 0xE5 is stored with
     * this substitution to avoid being misread as a deleted entry).
     * Not expected to be exercised by this project's own fixtures
     * (mtools never generates it for ordinary ASCII names like
     * BIGFILE.BIN/SMALL.BIN), but handled here rather than silently
     * mis-decoding a real-world volume that does use it. */
    uint8_t base[9];
    uint8_t ext[4];
    int i;

    for (i = 0; i < 8; i++)
        base[i] = raw11[i];
    base[0] = (raw11[0] == 0x05) ? 0xE5 : raw11[0];
    base[8] = '\0';
    for (i = 7; i >= 0 && base[i] == ' '; i--)
        base[i] = '\0';

    for (i = 0; i < 3; i++)
        ext[i] = raw11[8 + i];
    ext[3] = '\0';
    for (i = 2; i >= 0 && ext[i] == ' '; i--)
        ext[i] = '\0';

    size_t base_len = strlen((const char *)base);
    size_t ext_len  = strlen((const char *)ext);

    memcpy(out13, base, base_len);
    if (ext_len > 0) {
        out13[base_len] = '.';
        memcpy(out13 + base_len + 1, ext, ext_len);
        out13[base_len + 1 + ext_len] = '\0';
    } else {
        out13[base_len] = '\0';
    }
}

int fat_readdir_fixed_root(block_device_t *dev, uint32_t mount_lba,
                            const fat_geometry_t *geom,
                            fat_dirlist_entry_t *out_entries,
                            uint32_t max_entries, uint32_t *out_count)
{
    uint32_t count = 0;
    uint32_t sec;

    /* Root directory region is BLOCK_SECTOR_SIZE(512)-byte sectors,
     * same fixed-512 assumption every other disk-facing routine in
     * this project already makes (see fat12.c's original DAY11
     * comment). 16 entries of 32 bytes each fit in one 512-byte
     * sector. */
    static uint8_t sector_buf[512];
    const uint32_t entries_per_sector = 512 / sizeof(fat_dirent_t);

    for (sec = 0; sec < geom->root_dir_sectors && count < max_entries; sec++) {
        /* Same volume-relative -> disk-absolute LBA fix fat12.h's
         * DAY11 note documents for first_fat_sector, applied here to
         * first_root_dir_sec: it is relative to this volume's own
         * boot sector, so `mount_lba` must be added back in. */
        uint32_t lba = mount_lba + geom->first_root_dir_sec + sec;

        if (dev->read(dev, lba, 1, sector_buf) != 0) {
            if (out_count)
                *out_count = count;
            return -1;
        }

        fat_dirent_t *dirents = (fat_dirent_t *)sector_buf;
        uint32_t i;

        for (i = 0; i < entries_per_sector && count < max_entries; i++) {
            fat_dirent_t *de = &dirents[i];

            if (de->DIR_Name[0] == FAT_DIRENT_END) {
                /* Spec-defined "no more entries past this point" —
                 * stop the whole walk, not just this sector. */
                if (out_count)
                    *out_count = count;
                return 0;
            }

            if (de->DIR_Name[0] == FAT_DIRENT_FREE)
                continue;   /* deleted entry — skip, keep scanning */

            if (de->DIR_Attr == FAT_ATTR_LONG_NAME)
                continue;   /* VFAT long-name fragment — 8.3-only for now */

            if (de->DIR_Attr & FAT_ATTR_VOLUME_ID)
                continue;   /* root directory's volume-label entry */

            fat_dirlist_entry_t *out = &out_entries[count];
            fat_decode_short_name(de->DIR_Name, out->name);
            out->attr          = de->DIR_Attr;
            /* DAY17: combine DIR_FstClusHI:DIR_FstClusLO — a no-op for
             * FAT12/16 (FstClusHI is always 0 there per spec), but this
             * is what lets the same fat_dirlist_entry_t/this same
             * function's shape stay meaningful if a future caller ever
             * points it at a region containing FAT32-sourced entries
             * (it doesn't today — fat32_readdir(), kernel/fs/fat/
             * fat32.c, has its own cluster-chain walk and does this
             * same combine itself, see its DAY17 comment). */
            out->start_cluster = ((uint32_t)de->DIR_FstClusHI << 16) |
                                  (uint32_t)de->DIR_FstClusLO;
            out->size          = de->DIR_FileSize;
            count++;
        }
    }

    if (out_count)
        *out_count = count;
    return 0;
}

/* ===================================================================
 * DAY24 (Week 4, Day 24 of the 5.0 filesystem roadmap):
 *   "实现 fat_create_file(name)：在根目录区找一个空条目（首字节 0x00
 *    或 0xE5），填入文件名、起始簇 0、大小 0。"
 *
 * See fat_common.h's own doc comments above fat_encode_short_name()'s
 * and fat_create_entry_fixed_root()'s declarations for the full
 * contract and rationale. This is just the implementation.
 * =================================================================== */

int fat_encode_short_name(const char *name, uint8_t out11[11])
{
    if (!name || !out11 || name[0] == '\0')
        return -1;

    /* Split at the LAST '.' — see fat_encode_short_name()'s own doc
     * comment for why "last", not "first" (mirrors how a real 8.3
     * short name only ever has one dot in its decoded form). No '.'
     * at all means the whole string is the base, extension empty. */
    const char *dot = strrchr(name, '.');

    const char *base_start = name;
    size_t      base_len;
    const char *ext_start;
    size_t      ext_len;

    if (dot) {
        base_len  = (size_t)(dot - name);
        ext_start = dot + 1;
        ext_len   = strlen(ext_start);
    } else {
        base_len  = strlen(name);
        ext_start = "";
        ext_len   = 0;
    }

    /* Base must be non-empty and fit 8 bytes; extension (if any) must
     * fit 3 bytes — this project's 8.3-only scope (see this function's
     * own doc comment for exactly why a too-long name is REJECTED
     * rather than silently truncated). */
    if (base_len == 0 || base_len > 8 || ext_len > 3)
        return -1;

    uint8_t base[9];
    uint8_t ext[4];
    size_t  i;

    for (i = 0; i < base_len; i++)
        base[i] = (uint8_t)base_start[i];
    base[base_len] = '\0';
    strupper((char *)base);

    for (i = 0; i < ext_len; i++)
        ext[i] = (uint8_t)ext_start[i];
    ext[ext_len] = '\0';
    strupper((char *)ext);

    /* Space-pad both parts out to their full on-disk width — the
     * exact reverse of fat_decode_short_name()'s trailing-space
     * strip. */
    for (i = 0; i < 8; i++)
        out11[i] = (i < base_len) ? base[i] : (uint8_t)' ';
    for (i = 0; i < 3; i++)
        out11[8 + i] = (i < ext_len) ? ext[i] : (uint8_t)' ';

    return 0;
}

int fat_create_entry_fixed_root(block_device_t *dev, uint32_t mount_lba,
                                 const fat_geometry_t *geom,
                                 const char *name)
{
    if (!dev || !geom || !name)
        return -1;

    uint8_t raw_name[11];
    if (fat_encode_short_name(name, raw_name) != 0)
        return -1;   /* name doesn't fit this project's 8.3-only scope */

    if (!dev->write)
        return -1;   /* read-only backend — creation is impossible, see doc comment */

    /* Same 512-byte-sector, entries-per-sector shape
     * fat_readdir_fixed_root() above already uses. */
    static uint8_t sector_buf[512];
    const uint32_t entries_per_sector = 512 / sizeof(fat_dirent_t);
    uint32_t sec;

    for (sec = 0; sec < geom->root_dir_sectors; sec++) {
        uint32_t lba = mount_lba + geom->first_root_dir_sec + sec;

        if (dev->read(dev, lba, 1, sector_buf) != 0)
            return -1;

        fat_dirent_t *dirents = (fat_dirent_t *)sector_buf;
        uint32_t i;

        for (i = 0; i < entries_per_sector; i++) {
            fat_dirent_t *de = &dirents[i];

            /* "首字节 0x00 或 0xE5" — either sentinel marks a slot this
             * function is free to claim (see fat_create_entry_fixed_
             * root()'s own doc comment for the FAT_DIRENT_END vs.
             * FAT_DIRENT_FREE distinction; both are equally reusable
             * here, unlike fat_readdir_fixed_root()'s walk, where only
             * FREE lets the scan continue past it). */
            if (de->DIR_Name[0] == FAT_DIRENT_END ||
                de->DIR_Name[0] == FAT_DIRENT_FREE) {
                memset(de, 0, sizeof(*de));
                memcpy(de->DIR_Name, raw_name, 11);
                de->DIR_Attr      = FAT_ATTR_ARCHIVE;
                de->DIR_FstClusHI = 0;
                de->DIR_FstClusLO = 0;   /* "起始簇 0" */
                de->DIR_FileSize  = 0;   /* "大小 0" */

                if (dev->write(dev, lba, 1, sector_buf) != 0)
                    return -1;

                return 0;
            }
        }
    }

    /* Scanned the entire fixed root region without finding a reusable
     * slot — the root directory is full (see this function's own doc
     * comment for why this can't just grow another sector on FAT12/16). */
    return -1;
}

/* ===================================================================
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "实现 fat_write(file, buf, size)：处理簇链扩展（如果当前簇链不够，
 *    追加新簇）。"
 *
 * See fat_common.h's doc comment above the declaration for the full
 * "why find-and-update, not delete-and-recreate" rationale. This is
 * just the implementation: walk the fixed root region exactly like
 * fat_readdir_fixed_root() does (same skip rules for 0x00/0xE5/VFAT/
 * volume-label), but instead of decoding into a fat_dirlist_entry_t,
 * compare each live entry's decoded name against `name` and, on the
 * first match, overwrite its cluster/size fields in place.
 * =================================================================== */
/* fat_name_matches_ci(decoded, name)
 *
 * DAY26 bugfix helper — see fat_common.h's DAY26 header note above
 * fat_delete_entry_fixed_root() for the full "why case-insensitive"
 * rationale. `decoded` (fat_decode_short_name()'s output) is always
 * upper-case; this compares it against `name` (whatever case the
 * caller/Shell-user supplied) one character at a time, upper-casing
 * only `name`'s side of the comparison. No heap/extra buffer needed
 * (unlike a strupper()-into-a-local-copy approach), so it works
 * unchanged regardless of how long `name` is. */
static int fat_name_matches_ci(const char *decoded, const char *name)
{
    while (*decoded != '\0' && *name != '\0') {
        char a = *decoded;
        char b = *name;
        if (b >= 'a' && b <= 'z')
            b = (char)(b - 'a' + 'A');
        if (a != b)
            return 0;
        decoded++;
        name++;
    }
    return (*decoded == '\0' && *name == '\0');
}

int fat_update_entry_fixed_root(block_device_t *dev, uint32_t mount_lba,
                                 const fat_geometry_t *geom,
                                 const char *name,
                                 uint32_t new_start_cluster,
                                 uint32_t new_size)
{
    if (!dev || !geom || !name)
        return -1;

    if (!dev->write)
        return -1;   /* read-only backend — update is impossible, see doc comment */

    static uint8_t sector_buf[512];
    const uint32_t entries_per_sector = 512 / sizeof(fat_dirent_t);
    uint32_t sec;

    for (sec = 0; sec < geom->root_dir_sectors; sec++) {
        uint32_t lba = mount_lba + geom->first_root_dir_sec + sec;

        if (dev->read(dev, lba, 1, sector_buf) != 0)
            return -1;

        fat_dirent_t *dirents = (fat_dirent_t *)sector_buf;
        uint32_t i;

        for (i = 0; i < entries_per_sector; i++) {
            fat_dirent_t *de = &dirents[i];

            if (de->DIR_Name[0] == FAT_DIRENT_END)
                return -1;   /* spec-defined end-of-directory — no more
                               * live entries past this point, so `name`
                               * cannot appear later in this region either */

            if (de->DIR_Name[0] == FAT_DIRENT_FREE)
                continue;   /* deleted entry — skip, keep scanning */

            if (de->DIR_Attr == FAT_ATTR_LONG_NAME)
                continue;   /* VFAT long-name fragment — 8.3-only for now */

            if (de->DIR_Attr & FAT_ATTR_VOLUME_ID)
                continue;   /* root directory's volume-label entry */

            char decoded[13];
            fat_decode_short_name(de->DIR_Name, decoded);

            /* DAY26 bugfix: case-insensitive match — see this file's
             * fat_name_matches_ci() doc comment / fat_common.h's DAY26
             * header note above fat_delete_entry_fixed_root(). */
            if (!fat_name_matches_ci(decoded, name))
                continue;   /* not the entry we're looking for */

            /* Found it — overwrite cluster + size in place (see this
             * function's own doc comment for why in-place update, not
             * delete-and-recreate), then persist this one sector. */
            de->DIR_FstClusHI = (uint16_t)(new_start_cluster >> 16);
            de->DIR_FstClusLO = (uint16_t)(new_start_cluster & 0xFFFFu);
            de->DIR_FileSize  = new_size;

            if (dev->write(dev, lba, 1, sector_buf) != 0)
                return -1;

            return 0;
        }
    }

    /* Scanned the entire fixed root region without finding a live
     * entry named `name` — nothing to update. */
    return -1;
}

/* ===================================================================
 * DAY26 (Week 4, Day 26 of the 5.0 filesystem roadmap):
 *   "实现 fat_delete_file(name)：找到目录项，标记为已删除（首字节设为
 *    0xE5），释放其占用的簇链。"
 *
 * See fat_common.h's doc comment above the declaration for the full
 * contract. Implementation is fat_update_entry_fixed_root()'s own scan
 * loop with a one-field-different ending: instead of overwriting
 * DIR_FstClus and DIR_FileSize, this just sets DIR_Name[0] to
 * FAT_DIRENT_FREE and writes the sector back.
 * =================================================================== */
int fat_delete_entry_fixed_root(block_device_t *dev, uint32_t mount_lba,
                                 const fat_geometry_t *geom,
                                 const char *name)
{
    if (!dev || !geom || !name)
        return -1;

    if (!dev->write)
        return -1;   /* read-only backend — deletion is impossible, same as create/update */

    static uint8_t sector_buf[512];
    const uint32_t entries_per_sector = 512 / sizeof(fat_dirent_t);
    uint32_t sec;

    for (sec = 0; sec < geom->root_dir_sectors; sec++) {
        uint32_t lba = mount_lba + geom->first_root_dir_sec + sec;

        if (dev->read(dev, lba, 1, sector_buf) != 0)
            return -1;

        fat_dirent_t *dirents = (fat_dirent_t *)sector_buf;
        uint32_t i;

        for (i = 0; i < entries_per_sector; i++) {
            fat_dirent_t *de = &dirents[i];

            if (de->DIR_Name[0] == FAT_DIRENT_END)
                return -1;   /* spec-defined end-of-directory — nothing named
                               * `name` can appear later in this region either */

            if (de->DIR_Name[0] == FAT_DIRENT_FREE)
                continue;   /* already-deleted entry — skip, keep scanning */

            if (de->DIR_Attr == FAT_ATTR_LONG_NAME)
                continue;   /* VFAT long-name fragment — 8.3-only for now */

            if (de->DIR_Attr & FAT_ATTR_VOLUME_ID)
                continue;   /* root directory's volume-label entry */

            char decoded[13];
            fat_decode_short_name(de->DIR_Name, decoded);

            if (!fat_name_matches_ci(decoded, name))
                continue;   /* not the entry we're looking for */

            /* Found it — the WHOLE deletion, as far as this fixed-root
             * layer is concerned, is this one byte (see this function's
             * doc comment in fat_common.h for why the rest of the entry
             * is deliberately left untouched/stale). */
            de->DIR_Name[0] = FAT_DIRENT_FREE;

            if (dev->write(dev, lba, 1, sector_buf) != 0)
                return -1;

            return 0;
        }
    }

    /* Scanned the entire fixed root region without finding a live
     * entry named `name` — nothing to delete. */
    return -1;
}