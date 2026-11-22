/* kernel/fs/fat/fat16_vfs.h
 * LumaOS 5.0 - FAT16 <-> VFS Adapter
 *
 * DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
 *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载一个
 *    FAT16 镜像，ls 能正确列出文件。"
 *
 *   The FAT16 counterpart of kernel/fs/fat/fat12_vfs.h — same role
 *   Day 13's fat12_vfs.h has played for FAT12 since Day 13, adapting
 *   fat16.h's on-disk-shaped API (fat16_fs_t, fat16_readdir(),
 *   fat16_dirlist_entry_t) to kernel/fs/vfs/vfs.h's generic
 *   vfs_open_fn/vfs_read_fn/vfs_readdir_fn signatures. "ls 能正确列出
 *   文件" (this Day's own acceptance text) is exactly what this
 *   adapter makes possible: kernel/shell/cmd/ls.c (Day 14) only ever
 *   calls through vfs_root->fs->readdir() — it has no idea FAT16
 *   exists, or that this file does either — so once a FAT16 volume is
 *   mounted through this adapter and vfs_root points at its root, the
 *   EXACT SAME cmd_ls that already lists FAT12 volumes lists a FAT16
 *   one too, with zero changes to ls.c.
 *
 *   Design notes are identical to fat12_vfs.h's (separate translation
 *   unit so fat16.c stays VFS-ignorant, single static mount instance
 *   since this project has no kernel heap yet, open()/readdir() are
 *   real while read() is a documented Day-18/19 stub) — see
 *   fat12_vfs.h's own header comment for the full rationale, which
 *   applies here unchanged.
 */
#ifndef LUMA_FAT16_VFS_H
#define LUMA_FAT16_VFS_H

#include "../../drivers/block/block.h"   /* block_device_t */
#include "../vfs/vfs.h"                  /* filesystem_t, vfs_node_t */

/* fat16_filesystem_ops
 *
 * The FAT16 driver's VFS operations table — FAT16 counterpart of
 * fat12_vfs.h's fat12_filesystem_ops. `.name` is "fat16" for
 * diagnostics. `.root`/`.fs_private` are per-MOUNT state, filled in by
 * fat16_vfs_mount() below.
 */
extern const filesystem_t fat16_filesystem_ops;

/* fat16_vfs_mount(dev, lba)
 *
 * Mounts a FAT16 volume at `lba` on `dev` (via fat16_mount(), Day 16)
 * and wraps it as a VFS `filesystem_t` whose `.root` is a
 * VFS_TYPE_DIR node representing the FAT16 root directory region.
 *
 * Returns a pointer to the populated, statically-allocated
 * `filesystem_t` on success. Returns NULL if fat16_mount() itself
 * fails (not a valid FAT16 BPB at `lba`, or the underlying
 * dev->read() failed) — same failure conditions fat16_mount()
 * documents, just surfaced as NULL instead of -1 (same convention
 * fat12_vfs_mount() already uses).
 */
filesystem_t *fat16_vfs_mount(block_device_t *dev, uint32_t lba);

#endif /* LUMA_FAT16_VFS_H */