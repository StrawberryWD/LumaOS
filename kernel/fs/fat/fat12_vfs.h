/* kernel/fs/fat/fat12_vfs.h
 * LumaOS 5.0 - FAT12 <-> VFS Adapter
 *
 * DAY13 (Week 2, Day 13 of the 5.0 filesystem roadmap):
 *   "将 FAT12 挂接到 VFS：注册 fat12_filesystem_ops。实现
 *    vfs_mount("IDE0")。调用 vfs_mount 后，vfs_root 指针指向 FAT
 *    根目录。"
 *
 *   Day 8 (kernel/fs/vfs/vfs.h) defined the filesystem-independent
 *   shapes (`struct vfs_node`, `struct file`, `struct filesystem`)
 *   deliberately without touching FAT12 at all. Day 9-12
 *   (kernel/fs/fat/fat12.{h,c}) built a complete, working FAT12
 *   driver deliberately without touching the VFS at all — its
 *   functions take a `fat12_fs_t *` and, for readdir, produce
 *   `fat12_dirlist_entry_t` results, neither of which VFS code knows
 *   about. Day 13 is the day these two finally meet: this file is
 *   the *adapter* layer, translating between fat12.h's on-disk-shaped
 *   API and vfs.h's generic vfs_open_fn/vfs_read_fn/vfs_readdir_fn
 *   signatures — mirroring how kernel/drivers/block/block.c's
 *   ide0_block_device() adapts the IDE driver to the generic
 *   block_device_t interface.
 *
 *   This adapter deliberately lives in its OWN file (fat12_vfs.c),
 *   not inside fat12.c itself: fat12.c/fat12.h's Day 9 design notes
 *   explicitly kept the FAT12 driver ignorant of the VFS layer ("No
 *   block_device_t include here on purpose" was vfs.h's version of
 *   the same principle, applied in the other direction). Keeping the
 *   glue in a separate translation unit means fat12.c still compiles
 *   and is testable with zero knowledge of vfs.h, and the FAT16/32
 *   work in Week 3 (Day 15's fat_common.c) can follow the exact same
 *   "driver file + adapter file" split instead of everything getting
 *   tangled into fat12.c.
 *
 * Design notes:
 *   - fat12_vfs_mount(dev, lba) is this file's one entry point: it
 *     calls fat12_mount() (Day 10) to do the actual FAT12 mount, then
 *     wraps the result in a `filesystem_t` (ops = fat12_filesystem_ops,
 *     fs_private = the fat12_fs_t mount state) plus a root `vfs_node_t`
 *     (type VFS_TYPE_DIR, name "IDE0" — see fat12_vfs.c's mount
 *     comment for why this uses the numbered device name rather than
 *     a Unix-style "/"). Returns the populated
 *     `filesystem_t *` on success, NULL if the underlying
 *     fat12_mount() call itself fails — mirroring fat12_mount()'s own
 *     "NULL/-1 means nothing usable was produced" contract, just
 *     translated to a pointer return since vfs_mount() (Day 13's
 *     other deliverable, kernel/fs/vfs/vfs.c) is what turns this back
 *     into the roadmap's requested "vfs_mount() sets vfs_root" shape.
 *   - Backing storage (the fat12_fs_t mount state, the filesystem_t,
 *     and the root vfs_node_t) is static inside fat12_vfs.c, not
 *     heap-allocated — this project has no kernel heap yet (that's
 *     Day 73's "malloc/free", still 60 days out), and Day 13/14's
 *     scope is a single mounted volume, matching how kmain.c's own
 *     Day 9-12 test sections already use `static fat12_fs_t` locals
 *     for the same reason. Day 35 ("FAT 与 LMFS 可以同时存在") is
 *     where real multi-mount support becomes necessary; this file is
 *     not trying to solve that early.
 *   - `fat12_filesystem_ops` is exposed (not `static`) so
 *     kernel/fs/vfs/vfs.c or a future caller could in principle
 *     inspect/reuse just the ops table without going through
 *     fat12_vfs_mount() — but nothing does that yet; vfs_mount() only
 *     ever calls fat12_vfs_mount() as of Day 13.
 *   - open()/read() are wired with real, working bodies for exactly
 *     what Day 13 needs them to do (open() sets up a `file_t` at
 *     offset 0; readdir() walks the root directory via fat12_readdir()
 *     and produces one child vfs_node_t per call) — Day 13's own
 *     acceptance text only asks for readdir/mount to work
 *     ("vfs_root 指针指向 FAT 根目录"), but a `struct filesystem` with
 *     a NULL `open`/`read` would violate vfs.h's own "open/read
 *     required" contract, so open() is implemented for real now
 *     rather than stubbed. read() itself is intentionally a stub that
 *     always reports "0 bytes read, no error" (a real read needs
 *     Day 18's fat_open()-by-path + Day 19's fat_read()'s cluster-
 *     chain walk, neither of which exists yet) — see fat12_vfs.c's
 *     DAY13 comment on fat12_vfs_read() for why 0-not-error is the
 *     correct stub return value per vfs_read_fn's own documented
 *     "0 == EOF" contract, rather than -1 ("error").
 */
#ifndef LUMA_FAT12_VFS_H
#define LUMA_FAT12_VFS_H

#include "../../drivers/block/block.h"   /* block_device_t */
#include "../vfs/vfs.h"                  /* filesystem_t, vfs_node_t */

/* fat12_filesystem_ops
 *
 * The FAT12 driver's VFS operations table: .open/.read/.readdir point
 * at this file's adapter functions (fat12_vfs.c). `.name` is "fat12"
 * for diagnostics. `.root`/`.fs_private` are left NULL here — those
 * are per-MOUNT state, filled in per-instance by fat12_vfs_mount()
 * below, not part of the shared ops table itself (mirroring how a
 * Unix filesystem_type's operations vector is one shared thing while
 * each mounted instance gets its own superblock).
 */
extern const filesystem_t fat12_filesystem_ops;

/* fat12_vfs_mount(dev, lba)
 *
 * Mounts a FAT12 volume at `lba` on `dev` (via fat12_mount(), Day 10)
 * and wraps it as a VFS `filesystem_t` whose `.root` is a
 * VFS_TYPE_DIR node representing the FAT12 root directory region.
 *
 * Returns a pointer to the populated, statically-allocated
 * `filesystem_t` on success. Returns NULL if fat12_mount() itself
 * fails (not a valid FAT12 BPB at `lba`, or the underlying
 * dev->read() failed) — same failure conditions fat12_mount()
 * documents, just surfaced as NULL instead of -1.
 */
filesystem_t *fat12_vfs_mount(block_device_t *dev, uint32_t lba);

#endif /* LUMA_FAT12_VFS_H */