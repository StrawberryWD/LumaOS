/* kernel/fs/iso9660/iso9660.h - DAY63: read-only ISO 9660 driver + VFS adapter.
 *
 * What the kernel needs from the CD itself (the ISO is both the boot medium and a resource store):
 *   - mount the volume: PVD at LBA 16 ("CD001", type 1, logical block size 2048), root directory record
 *     at PVD+156 (34 bytes: length, ext-attr length, extent LBA @2, data length @10, flags @25, name @33)
 *   - walk directories: records are packed back to back inside 2048-byte sectors; a record never crosses a
 *     sector - a length byte of 0 means "padding, continue at the next sector". Entries 0x00 ("."), 0x01
 *     ("..") are skipped. Level-1 names look like "LUMAOS.CFG;1": the ";version" is cut off here.
 *   - read files: extent LBA + byte length -> consecutive sectors.
 * Name lookup is case-INSENSITIVE ("cd CD0/etc" works although the disc stores "ETC"). Paths use '/'.
 * Only plain ISO 9660 level 1 (what tools/mkiso.py writes); no Joliet / Rock Ridge / multi-extent files. */
#ifndef LUMA_ISO9660_H
#define LUMA_ISO9660_H
#include <stdint.h>
#include "../vfs/vfs.h"
#include "../../drivers/block/block.h"

typedef struct {
    char     name[32];     /* converted name ("LUMAOS.CFG"), NUL-terminated */
    int      is_dir;
    uint32_t lba;          /* extent */
    uint32_t size;         /* bytes */
} iso_entry_t;

/* Mount: dev is a 2048-byte-sector device (atapi_block_device). Returns the static filesystem_t or NULL. */
filesystem_t *iso9660_vfs_mount(block_device_t *dev);

/* Look up "ETC/LUMAOS.CFG" (no mount prefix) in the mounted volume. 0 = found. */
int  iso9660_lookup(const char *rel_path, iso_entry_t *out);
/* Read up to n bytes at byte offset pos of a file entry; returns bytes read (0 at EOF) or -1. */
int  iso9660_read(const iso_entry_t *e, uint32_t pos, void *buf, uint32_t n);
/* Convenience: whole file into buf (max bytes). Returns the file size, or -1 (not found / too big). */
int  iso9660_read_file(const char *rel_path, void *buf, uint32_t max);

/* "CD0/ETC/LUMAOS.CFG" (mount prefix + path, as the shell passes it) -> vfs node. 0 = ok. */
int  iso9660_vfs_open_path(const char *path, vfs_node_t *out);
const char *iso9660_volume_name(void);
uint32_t    iso9660_volume_blocks(void);
#endif
