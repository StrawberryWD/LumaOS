/* kernel/fs/fat/fat32_vfs.c
 * LumaOS 5.0 - FAT32 <-> VFS Adapter
 *
 * DAY17 (Week 3, Day 17 of the 5.0 filesystem roadmap):
 *   See fat32_vfs.h's file header comment. Mirrors fat16_vfs.c
 *   function-for-function — see fat12_vfs.c's original DAY13 comments
 *   for the "why" behind each piece (none of that reasoning changes
 *   here, only re-applied against fat32_readdir() instead of
 *   fat_readdir_fixed_root()).
 */
#include "fat32.h"
#include "fat32_vfs.h"
#include "fat_vfs.h"            /* DAY19: fat_read() */
#include "../../lib/string.h"   /* memset */

static fat32_fs_t   g_fat32_mount;
static filesystem_t g_fat32_fs;
static vfs_node_t   g_fat32_root;

#define FAT32_VFS_ROOT_SENTINEL  ((void *)0)

static int fat32_vfs_open(vfs_node_t *node, file_t *out_file)
{
    if (!node || !out_file || node->type != VFS_TYPE_FILE)
        return -1;

    out_file->node = node;
    out_file->pos  = 0;
    return 0;
}

/* DAY19 — the Day17 stub above is now a real implementation,
 * forwarding to fat_vfs.c's shared fat_read(), which dispatches on
 * `file->node->fs->name` ("fat32" for anything mounted through this
 * adapter — see fat32_filesystem_ops below) to the FAT32-specific
 * cluster-chain walk (fat32_next_cluster()) — the SAME walker
 * fat32_vfs_readdir() above already uses for the root directory's own
 * chain, just now applied to a FILE's start_cluster instead. See
 * fat_vfs.h's doc comment on fat_read() for the full contract. */
static int fat32_vfs_read(file_t *file, void *buf, uint32_t size)
{
    return fat_read(file, buf, size);
}

/* fat32_vfs_close(): DAY21 — identical trampoline shape to
 * fat12_vfs_close() — see fat12_vfs.c's DAY21 comment. */
static int fat32_vfs_close(file_t *file)
{
    return fat_close(file);
}

static int fat32_vfs_readdir(vfs_node_t *dir_node, uint32_t index,
                              vfs_node_t *out_node)
{
    if (!dir_node || !out_node || dir_node->type != VFS_TYPE_DIR)
        return -1;
    if (dir_node->fs_private != FAT32_VFS_ROOT_SENTINEL)
        return -1;   /* not the root — no subdirectory support yet (Day 42) */

    fat32_fs_t *fs = (fat32_fs_t *)dir_node->fs->fs_private;
    if (!fs)
        return -1;

    static fat32_dirlist_entry_t entries[FAT32_DIRLIST_MAX_ENTRIES];
    uint32_t count = 0;
    if (fat32_readdir(fs, entries, FAT32_DIRLIST_MAX_ENTRIES, &count) != 0)
        return -1;
    if (index >= count)
        return -1;

    fat32_dirlist_entry_t *e = &entries[index];

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

const filesystem_t fat32_filesystem_ops = {
    .name       = "fat32",
    .open       = fat32_vfs_open,
    .read       = fat32_vfs_read,
    .readdir    = fat32_vfs_readdir,
    .close      = fat32_vfs_close,     /* DAY21 */
    .root       = (vfs_node_t *)0,
    .fs_private = (void *)0,
};

filesystem_t *fat32_vfs_mount(block_device_t *dev, uint32_t lba)
{
    if (fat32_mount(dev, lba, &g_fat32_mount) != 0)
        return (filesystem_t *)0;

    g_fat32_fs = fat32_filesystem_ops;
    g_fat32_fs.fs_private = &g_fat32_mount;

    memset(&g_fat32_root, 0, sizeof(g_fat32_root));
    {
        const char *root_name = "IDE0";
        uint32_t i = 0;
        while (root_name[i] != '\0' && i < sizeof(g_fat32_root.name) - 1) {
            g_fat32_root.name[i] = root_name[i];
            i++;
        }
        g_fat32_root.name[i] = '\0';
    }
    g_fat32_root.type = VFS_TYPE_DIR;
    g_fat32_root.size = 0;
    g_fat32_root.fs = &g_fat32_fs;
    g_fat32_root.fs_private = FAT32_VFS_ROOT_SENTINEL;

    g_fat32_fs.root = &g_fat32_root;

    return &g_fat32_fs;
}