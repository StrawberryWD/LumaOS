/* kernel/fs/fat/fat32_vfs.h
 * LumaOS 5.0 - FAT32 <-> VFS Adapter
 *
 * DAY17 (Week 3, Day 17 of the 5.0 filesystem roadmap):
 *   "实现 FAT32 解析（注意 BPB_RootClus，根目录是一个普通簇链，且存在
 *    FSInfo 结构）。挂载 FAT32 镜像（mkfs.fat -F 32），ls 能工作。"
 *
 *   The FAT32 counterpart of fat16_vfs.h — same role, adapting
 *   fat32.h's on-disk-shaped API (fat32_fs_t, fat32_readdir(),
 *   fat32_dirlist_entry_t) to vfs.h's generic vfs_open_fn/
 *   vfs_read_fn/vfs_readdir_fn signatures, so "ls 能工作" (this Day's
 *   own acceptance text) needs zero changes to kernel/shell/cmd/ls.c —
 *   it only ever calls through vfs_root->fs->readdir(). See
 *   fat16_vfs.h's header comment for the full design rationale
 *   (separate translation unit, single static mount instance, why
 *   read() is still a Day 18/19 stub); it applies here unchanged.
 */
#ifndef LUMA_FAT32_VFS_H
#define LUMA_FAT32_VFS_H

#include "../../drivers/block/block.h"   /* block_device_t */
#include "../vfs/vfs.h"                  /* filesystem_t, vfs_node_t */

/* fat32_filesystem_ops — FAT32 counterpart of fat16_vfs.h's
 * fat16_filesystem_ops. `.name` is "fat32" for diagnostics. */
extern const filesystem_t fat32_filesystem_ops;

/* fat32_vfs_mount(dev, lba)
 *
 * Mounts a FAT32 volume at `lba` on `dev` (via fat32_mount(), Day 17)
 * and wraps it as a VFS `filesystem_t` whose `.root` is a
 * VFS_TYPE_DIR node representing the FAT32 root directory (walked as
 * a cluster chain starting at BPB_RootClus — see fat32.h).
 *
 * Returns a pointer to the populated, statically-allocated
 * `filesystem_t` on success. Returns NULL if fat32_mount() itself
 * fails — not a valid FAT32 BPB at `lba`, or the underlying
 * dev->read() failed.
 */
filesystem_t *fat32_vfs_mount(block_device_t *dev, uint32_t lba);

#endif /* LUMA_FAT32_VFS_H */