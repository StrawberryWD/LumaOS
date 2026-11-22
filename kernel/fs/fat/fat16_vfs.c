/* kernel/fs/fat/fat16_vfs.c
 * LumaOS 5.0 - FAT16 <-> VFS Adapter
 *
 * DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
 *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载一个
 *    FAT16 镜像，ls 能正确列出文件。"
 *
 *   See fat16_vfs.h's file header comment for the full design
 *   rationale. This file mirrors fat12_vfs.c function-for-function —
 *   see that file's own DAY13 comments for the "why" behind each
 *   piece (the FAT12_VFS_ROOT_SENTINEL convention, the read() stub's
 *   0-not-error contract, the re-walk-per-call readdir cost trade-off,
 *   ...); none of that reasoning changes for FAT16, so it isn't
 *   re-derived here, only re-applied.
 */
#include "fat16.h"
#include "fat16_vfs.h"
#include "fat_vfs.h"            /* DAY19: fat_read() */
#include "../../lib/string.h"   /* memset */

/* ===== Static mount state =====
 * A single FAT16 mount's worth of state — same "no kernel heap yet,
 * one instance is enough for today's scope" reasoning as
 * fat12_vfs.c's g_fat12_mount/g_fat12_fs/g_fat12_root. */
static fat16_fs_t   g_fat16_mount;
static filesystem_t g_fat16_fs;
static vfs_node_t   g_fat16_root;

/* FAT16's root directory is a fixed region (BPB_RootEntCnt entries),
 * exactly like FAT12's — no "root directory's starting cluster"
 * concept here either (see fat12_vfs.c's own sentinel comment). */
#define FAT16_VFS_ROOT_SENTINEL  ((void *)0)

/* fat16_vfs_open(): identical contract to fat12_vfs_open() — see
 * fat12_vfs.c's DAY13 comment. */
static int fat16_vfs_open(vfs_node_t *node, file_t *out_file)
{
    if (!node || !out_file || node->type != VFS_TYPE_FILE)
        return -1;

    out_file->node = node;
    out_file->pos  = 0;
    return 0;
}

/* fat16_vfs_read(): DAY19 — the Day16 stub above is now a real
 * implementation, forwarding to fat_vfs.c's shared fat_read(), which
 * dispatches on `file->node->fs->name` ("fat16" for anything mounted
 * through this adapter — see fat16_filesystem_ops below) to the
 * FAT16-specific cluster-chain walk (fat16_next_cluster()). See
 * fat_vfs.h's doc comment on fat_read() for the full contract. */
static int fat16_vfs_read(file_t *file, void *buf, uint32_t size)
{
    return fat_read(file, buf, size);
}

/* fat16_vfs_close(): DAY21 — identical trampoline shape to
 * fat12_vfs_close() — see fat12_vfs.c's DAY21 comment. */
static int fat16_vfs_close(file_t *file)
{
    return fat_close(file);
}

/* fat16_vfs_readdir(): adapts fat16_readdir()'s "whole directory at
 * once" shape into vfs_readdir_fn's "one entry at a time, by index"
 * shape — FAT16 counterpart of fat12_vfs_readdir(). Only root-directory
 * listing is supported (subdirectories are a Day 42 concern, same as
 * FAT12). */
static int fat16_vfs_readdir(vfs_node_t *dir_node, uint32_t index,
                              vfs_node_t *out_node)
{
    if (!dir_node || !out_node || dir_node->type != VFS_TYPE_DIR)
        return -1;
    if (dir_node->fs_private != FAT16_VFS_ROOT_SENTINEL)
        return -1;   /* not the root — no subdirectory support yet (Day 42) */

    fat16_fs_t *fs = (fat16_fs_t *)dir_node->fs->fs_private;
    if (!fs)
        return -1;

    static fat16_dirlist_entry_t entries[FAT16_DIRLIST_MAX_ENTRIES];
    uint32_t count = 0;
    if (fat16_readdir(fs, entries, FAT16_DIRLIST_MAX_ENTRIES, &count) != 0)
        return -1;
    if (index >= count)
        return -1;   /* per vfs_readdir_fn's contract: "index past the last entry" */

    fat16_dirlist_entry_t *e = &entries[index];

    {
        uint32_t i = 0;
        while (e->name[i] != '\0' && i < sizeof(out_node->name) - 1) {
            out_node->name[i] = e->name[i];
            i++;
        }
        out_node->name[i] = '\0';
    }

    out_node->type = (e->attr & FAT_ATTR_DIRECTORY) ? VFS_TYPE_DIR
                                                      : VFS_TYPE_FILE;
    out_node->size = e->size;
    out_node->fs   = dir_node->fs;
    out_node->fs_private = (void *)(uintptr_t)e->start_cluster;

    return 0;
}

/* fat16_filesystem_ops — see fat16_vfs.h's doc comment. */
const filesystem_t fat16_filesystem_ops = {
    .name       = "fat16",
    .open       = fat16_vfs_open,
    .read       = fat16_vfs_read,
    .readdir    = fat16_vfs_readdir,
    .close      = fat16_vfs_close,     /* DAY21 */
    .root       = (vfs_node_t *)0,
    .fs_private = (void *)0,
};

filesystem_t *fat16_vfs_mount(block_device_t *dev, uint32_t lba)
{
    if (fat16_mount(dev, lba, &g_fat16_mount) != 0)
        return (filesystem_t *)0;

    g_fat16_fs = fat16_filesystem_ops;
    g_fat16_fs.fs_private = &g_fat16_mount;

    memset(&g_fat16_root, 0, sizeof(g_fat16_root));
    /* Root node's name: same "IDE0" LumaOS-namespace convention
     * fat12_vfs_mount() uses (see its own DAY13 comment for the full
     * rationale — no Unix-style "/", numbered device string). A FAT16
     * volume mounted this way is still reached through whichever
     * device string the caller resolved to this LBA, so the root
     * node's display name is "IDE0" here too, matching fat12_vfs_mount()
     * exactly (both adapters are only ever invoked as "the thing
     * currently mounted at IDE0", never simultaneously — see this
     * project's Day 35 note on real multi-mount support being future
     * work). */
    {
        const char *root_name = "IDE0";
        uint32_t i = 0;
        while (root_name[i] != '\0' && i < sizeof(g_fat16_root.name) - 1) {
            g_fat16_root.name[i] = root_name[i];
            i++;
        }
        g_fat16_root.name[i] = '\0';
    }
    g_fat16_root.type = VFS_TYPE_DIR;
    g_fat16_root.size = 0;
    g_fat16_root.fs = &g_fat16_fs;
    g_fat16_root.fs_private = FAT16_VFS_ROOT_SENTINEL;

    g_fat16_fs.root = &g_fat16_root;

    return &g_fat16_fs;
}