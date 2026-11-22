/* kernel/drivers/block/block.c
 * LumaOS 5.0 - Generic Block Device Layer
 *
 * DAY6 (Week 1, Day 6 of the 5.0 filesystem roadmap):
 *   "封装块设备层：struct block_device 包含 read(lba, count, buf) 函数
 *    指针。上层代码不再直接调用 ide 函数，统一走设备结构体。"
 *
 *   This file is the *only* place in the kernel (besides ide.c itself)
 *   that is allowed to call ide_read_sectors()/ide_write_sectors()
 *   directly. It wraps them behind the generic block_read_fn/
 *   block_write_fn signatures from block.h and exposes the result as
 *   a single static block_device_t via ide0_block_device().
 *
 *   Everything above this layer (mbr.c's caller in kmain.c today;
 *   VFS/FAT/LMFS from Week 2 onward) goes through that block_device_t
 *   and its ->read/->write function pointers instead of naming the
 *   IDE driver's functions itself.
 */
#include "block.h"
#include "ide.h"
#include "../../fs/fat/fat_cache.h"   /* DAY65: fatc_note_write() — keep the FAT cache coherent */

/* ide0_read() / ide0_write()
 *
 * Thin adapters from the generic block_read_fn/block_write_fn
 * signature (dev, lba, count, buf) to ide_read_sectors()/
 * ide_write_sectors()'s (lba, count, buf). `dev` is unused today
 * (single primary-master assumption, same as ide.h) but is accepted
 * so the function pointers match block.h's typedefs exactly; a future
 * multi-instance IDE backend would read dev->dev_private here to pick
 * the right channel/drive instead of always targeting the primary
 * master.
 */
static int ide0_read(block_device_t *dev, uint32_t lba, uint8_t count, void *buf)
{
    (void)dev; /* unused: single primary-master IDE instance for now */
    return ide_read_sectors(lba, count, buf);
}

static int ide0_write(block_device_t *dev, uint32_t lba, uint8_t count, const void *buf)
{
    (void)dev; /* unused: single primary-master IDE instance for now */
    /* DAY65: whoever writes the disk (FAT code, `write`, the self-tests...), the FAT-table cache must never
     * serve stale sectors afterwards: drop every cached sector overlapping [lba, lba+count) and bump the
     * generation counter that the fat_read() chain cursor checks. Done on success AND failure (a failed
     * write may still have changed some sectors). */
    int rc = ide_write_sectors(lba, count, buf);
    fatc_note_write(lba, count);
    return rc;
}

/* The single block_device_t instance for this roadmap's one disk.
 * Static storage: no heap allocator exists yet (that's Week 11's
 * malloc/sbrk work), and one disk is all run.sh's `-drive ... if=ide`
 * exposes today anyway. dev_private is left NULL — the IDE backend
 * doesn't need any per-instance state yet (see block.h's comment on
 * why the field still exists). */
static block_device_t g_ide0 = {
    .name        = "ide0",
    .sector_size = BLOCK_SECTOR_SIZE,
    .read        = ide0_read,
    .write       = ide0_write,
    .dev_private = 0,
};

block_device_t *ide0_block_device(void)
{
    return &g_ide0;
}