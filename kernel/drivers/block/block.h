/* kernel/drivers/block/block.h
 * LumaOS 5.0 - Generic Block Device Layer
 *
 * DAY6 (Week 1, Day 6 of the 5.0 filesystem roadmap):
 *   "封装块设备层：struct block_device 包含 read(lba, count, buf) 函数
 *    指针。上层代码不再直接调用 ide 函数，统一走设备结构体。"
 *
 *   This header defines the device-independent interface that every
 *   concrete block driver (today: IDE PIO; tomorrow: AHCI, a RAM disk,
 *   whatever) is expected to implement. From Day 6 onward, code above
 *   this layer (MBR parsing, and eventually the VFS/FAT/LMFS work in
 *   Weeks 2+) must go through a `struct block_device *` and its
 *   `read`/`write` function pointers — it must never call
 *   ide_read_sectors()/ide_write_sectors() (or any other driver-
 *   specific function) directly. That keeps the filesystem code fully
 *   decoupled from *which* disk controller is underneath, which is the
 *   whole point of the abstraction: swapping IDE for AHCI later should
 *   mean writing one new block_device backend, not touching MBR/VFS/
 *   FAT/LMFS at all.
 *
 * Design notes:
 *   - `read`/`write` mirror ide_read_sectors()/ide_write_sectors()'s
 *     signature exactly (lba, count, buf) so the IDE backend below is
 *     just a thin pointer-forwarding wrapper — no translation logic
 *     needed for Day 6's minimal single-disk case.
 *   - Both function pointers return int (0 on success, -1 on error),
 *     matching the existing ide_* convention so callers migrating from
 *     direct ide_* calls don't need to change their error handling.
 *   - `write` may be NULL for a read-only backend (not needed yet,
 *     but documented so future backends — e.g. a CD-ROM block device
 *     in the Week 9 ISO work — know it's a legal state). Callers must
 *     check for NULL before invoking it if they can't guarantee the
 *     backend is writable.
 *   - `name` and `sector_size` are metadata only (diagnostics /
 *     future multi-device dispatch); no code depends on them yet.
 *   - `dev_private` is reserved for a backend to stash driver-specific
 *     state (e.g. channel/drive select for a future secondary-channel
 *     or slave-drive IDE instance) without polluting this generic
 *     struct. The IDE backend doesn't need it yet (single primary-
 *     master assumption, same as ide.h) but it's here so adding a
 *     second IDE instance later doesn't require changing this struct's
 *     layout.
 */
#ifndef LUMA_BLOCK_H
#define LUMA_BLOCK_H

#include <stdint.h>

/* Standard sector size assumed everywhere in this codebase so far
 * (MBR, IDE PIO transfers, the Day 4 write/readback test). FAT/LMFS
 * work in later weeks may introduce filesystem block sizes that are
 * a multiple of this, but the underlying disk sector size itself
 * stays 512 for the IDE/ATA backend. */
#define BLOCK_SECTOR_SIZE  512

typedef struct block_device block_device_t;

/* block_read_fn / block_write_fn
 *
 * Same contract as ide_read_sectors()/ide_write_sectors(): read or
 * write `count` BLOCK_SECTOR_SIZE-byte sectors starting at 28-bit LBA
 * `lba`, to/from `buf` (which must hold at least count*512 bytes).
 * The `dev` parameter is the owning block_device_t so a backend that
 * supports multiple instances (e.g. IDE primary vs secondary channel,
 * or master vs slave) can recover its own dev_private state instead
 * of relying on file-scope globals — the current IDE backend doesn't
 * need this yet, but the signature is future-proofed for when it
 * does.
 *
 * Returns 0 on success, -1 on error (mirrors the ide_* convention).
 */
typedef int (*block_read_fn)(block_device_t *dev, uint32_t lba,
                              uint8_t count, void *buf);
typedef int (*block_write_fn)(block_device_t *dev, uint32_t lba,
                               uint8_t count, const void *buf);

/* struct block_device
 *
 * The device-independent handle every upper layer (MBR today; VFS/
 * FAT/LMFS starting Week 2) is expected to hold and call through,
 * instead of reaching into a specific driver's functions.
 */
struct block_device {
    const char     *name;         /* diagnostic label, e.g. "ide0" */
    uint32_t        sector_size;  /* bytes per sector; 512 for IDE */
    block_read_fn   read;         /* required: dev->read(dev, lba, count, buf) */
    block_write_fn  write;        /* optional: NULL if read-only */
    void           *dev_private;  /* backend-specific state, opaque here */
};

/* ide0_block_device(void)
 *
 * Returns a pointer to the single, statically-allocated block_device_t
 * that wraps the primary-channel IDE driver (kernel/drivers/block/
 * ide.c). This is the Day 6 "封装" step applied to the disk this
 * roadmap has been using since Day 2: instead of kmain()/mbr.c calling
 * ide_read_sectors()/ide_write_sectors() by name, they call
 * ide0_block_device()->read(...)/->write(...) through this generic
 * struct.
 *
 * The returned pointer is always valid (points to static storage) and
 * is safe to call before any explicit "init" step, since the IDE
 * driver itself requires no setup beyond what ide_test() already
 * probes at boot.
 */
block_device_t *ide0_block_device(void);

#endif /* LUMA_BLOCK_H */