/* kernel/fs/fat/fat12_vfs.c
 * LumaOS 5.0 - FAT12 <-> VFS Adapter
 *
 * DAY13 (Week 2, Day 13 of the 5.0 filesystem roadmap):
 *   "将 FAT12 挂接到 VFS：注册 fat12_filesystem_ops。实现
 *    vfs_mount("IDE0")。调用 vfs_mount 后，vfs_root 指针指向 FAT
 *    根目录。"
 *
 *   See fat12_vfs.h's file header comment for the full design
 *   rationale (why this is a separate translation unit from fat12.c,
 *   why the mount state is static, why open/readdir are real but
 *   read() is a documented stub). This file is strictly the
 *   IMPLEMENTATION of the three adapter functions plus
 *   fat12_vfs_mount(); the "why" lives in the header.
 */
#include "fat12.h"
#include "fat12_vfs.h"
#include "fat_vfs.h"            /* DAY19: fat_read() */
#include "../../lib/string.h"   /* memset */

/* ===== Static mount state =====
 *
 * A single FAT12 mount's worth of state: the fat12_fs_t Day 10's
 * fat12_mount() fills in, the filesystem_t wrapper Day 13 adds around
 * it, and the one root vfs_node_t every VFS-level caller reaches
 * through fs->root / the global vfs_root (kernel/fs/vfs/vfs.c). See
 * fat12_vfs.h's design notes for why this is static rather than
 * heap-allocated (no kernel heap exists yet) and why one instance is
 * enough for Day 13/14's scope (single mounted volume; Day 35 is
 * where real multi-mount support is planned).
 */
static fat12_fs_t   g_fat12_mount;
static filesystem_t g_fat12_fs;
static vfs_node_t   g_fat12_root;

/* ===== fs_private sentinel for directory nodes =====
 *
 * FAT12/16's root directory is a fixed region (BPB_RootEntCnt
 * entries), NOT a cluster chain — there is no "root directory's
 * starting cluster" (see fat12.h's DAY11 note on fat12_next_cluster()
 * for the same point made about the roadmap's Day 11 text). So the
 * root vfs_node_t's fs_private is set to this sentinel (NULL) to mean
 * "list the fixed root region via fat12_readdir(), not a cluster
 * chain". A non-root directory (a subdirectory) would instead need
 * fs_private set to its own starting cluster number so a future
 * readdir on *that* node knows where to walk — subdirectories aren't
 * produced by anything yet (fat12_vfs_readdir() below only ever lists
 * the root), so that path is documented here for Day 42's multi-level
 * directories to build on, not exercised today.
 */
#define FAT12_VFS_ROOT_SENTINEL  ((void *)0)

/* ---------------------------------------------------------------------
 * fat12_vfs_open(node, out_file)
 *
 * Prepares `*out_file` to read from `node`: stashes the node pointer
 * and resets the byte offset to 0, per vfs_open_fn's documented
 * contract (kernel/fs/vfs/vfs.h). Refuses to "open" a directory as a
 * file (VFS_TYPE_DIR), matching that same doc comment's example of a
 * legitimate error case.
 *
 * This is a real, working implementation (not a Day-13 stub) because
 * populating `file->pos = 0` correctly now is what lets Day 20's
 * "支持多次读取" step build on a stable file_t contract later, rather
 * than changing this function's behavior out from under Day 19/20's
 * work.
 * --------------------------------------------------------------------- */
static int fat12_vfs_open(vfs_node_t *node, file_t *out_file)
{
    if (!node || !out_file || node->type != VFS_TYPE_FILE)
        return -1;

    out_file->node = node;
    out_file->pos  = 0;
    return 0;
}

/* ---------------------------------------------------------------------
 * fat12_vfs_read(file, buf, size)
 *
 * DAY19: the Day13 stub above is now a real implementation — this
 * just forwards to fat_vfs.c's shared fat_read(), which dispatches on
 * `file->node->fs->name` ("fat12" for anything mounted through this
 * adapter — see fat12_filesystem_ops below) to the FAT12-specific
 * cluster-chain walk (fat12_next_cluster()). See fat_vfs.h's doc
 * comment on fat_read() for the full contract (return value, EOF,
 * error cases) — this wrapper adds nothing beyond the forward.
 * --------------------------------------------------------------------- */
static int fat12_vfs_read(file_t *file, void *buf, uint32_t size)
{
    return fat_read(file, buf, size);
}

/* ---------------------------------------------------------------------
 * fat12_vfs_close(file)
 *
 * DAY21: one-line trampoline onto fat_vfs.c's shared fat_close() — same
 * "adapter forwards to the shared implementation" shape fat12_vfs_read()
 * already established for fat_read(). See vfs_close_fn's doc comment
 * (kernel/fs/vfs/vfs.h) and fat_close()'s own doc comment
 * (kernel/fs/fat/fat_vfs.h) for the full contract; nothing FAT12-
 * specific happens here, which is exactly why fat_close() itself lives
 * in fat_vfs.c rather than being triplicated per variant.
 * --------------------------------------------------------------------- */
static int fat12_vfs_close(file_t *file)
{
    return fat_close(file);
}

/* ---------------------------------------------------------------------
 * fat12_vfs_readdir(dir_node, index, out_node)
 *
 * The Day 13 payoff: adapts fat12_readdir()'s "whole directory at
 * once, into an array" shape (Day 12) into vfs_readdir_fn's
 * "one entry at a time, by index" shape (Day 8). Only root-directory
 * listing is supported (dir_node must be `fs->root`, i.e. fs_private
 * == FAT12_VFS_ROOT_SENTINEL) — subdirectories are a Day 42 concern,
 * see the sentinel comment above.
 *
 * NOTE ON RE-WALKING PER CALL: this re-runs the full fat12_readdir()
 * (Day 12) scan on every single call rather than caching the decoded
 * list across calls, so listing an N-entry directory one index at a
 * time costs O(N^2) fat12_readdir() work instead of O(N). This is
 * deliberately not optimized yet: FAT12_DIRLIST_MAX_ENTRIES (32, see
 * fat12.h) bounds N tightly enough that the cost is invisible in
 * practice through Day 14's planned `cmd_ls` use, and adding a cached
 * iterator now would be solving a performance problem Day 13/14 don't
 * actually have — a real fix (if this ever becomes visible) belongs
 * to whichever later step first needs it, not a speculative one here.
 * --------------------------------------------------------------------- */
static int fat12_vfs_readdir(vfs_node_t *dir_node, uint32_t index,
                              vfs_node_t *out_node)
{
    if (!dir_node || !out_node || dir_node->type != VFS_TYPE_DIR)
        return -1;
    if (dir_node->fs_private != FAT12_VFS_ROOT_SENTINEL)
        return -1;   /* not the root — no subdirectory support yet (Day 42) */

    fat12_fs_t *fs = (fat12_fs_t *)dir_node->fs->fs_private;
    if (!fs)
        return -1;

    static fat12_dirlist_entry_t entries[FAT12_DIRLIST_MAX_ENTRIES];
    uint32_t count = 0;
    if (fat12_readdir(fs, entries, FAT12_DIRLIST_MAX_ENTRIES, &count) != 0)
        return -1;
    if (index >= count)
        return -1;   /* per vfs_readdir_fn's contract: "index past the last entry" */

    fat12_dirlist_entry_t *e = &entries[index];

    /* fat12_dirlist_entry_t's name[13] ("8.3\0" max) always fits in
     * vfs_node_t's name[32] with room to spare — a plain byte copy
     * (fat12_dirlist_entry_t's name is already NUL-terminated, see
     * fat12.h's DAY12 doc comment on fat12_decode_short_name()) is
     * enough, no truncation logic needed. */
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

    /* Not yet meaningful (no Day-18 path-based open uses this), but
     * stashing the starting cluster now — rather than leaving it 0 —
     * means Day 18's fat_open()-by-path can start from a readdir'd
     * vfs_node_t without this adapter needing to change. */
    out_node->fs_private = (void *)(uintptr_t)e->start_cluster;

    return 0;
}

/* fat12_filesystem_ops — see fat12_vfs.h's doc comment. `.root` and
 * `.fs_private` are per-mount state (filled in by fat12_vfs_mount()
 * below), left NULL/0 here since this table itself is shared, not
 * per-instance. */
const filesystem_t fat12_filesystem_ops = {
    .name       = "fat12",
    .open       = fat12_vfs_open,
    .read       = fat12_vfs_read,
    .readdir    = fat12_vfs_readdir,
    .close      = fat12_vfs_close,     /* DAY21 */
    .root       = (vfs_node_t *)0,
    .fs_private = (void *)0,
};

filesystem_t *fat12_vfs_mount(block_device_t *dev, uint32_t lba)
{
    if (fat12_mount(dev, lba, &g_fat12_mount) != 0)
        return (filesystem_t *)0;

    /* Start from the shared ops table, then fill in this mount's own
     * root/fs_private — g_fat12_fs is the "instance", fat12_filesystem_ops
     * is the "class"/template it copies its function pointers from. */
    g_fat12_fs = fat12_filesystem_ops;
    g_fat12_fs.fs_private = &g_fat12_mount;

    memset(&g_fat12_root, 0, sizeof(g_fat12_root));
    /* Root node's own name: LumaOS's namespace has no Unix-style "/"
     * concept (see vfs.c's header comment) — the root of what
     * vfs_mount("IDE0") mounted IS named "IDE0", matching the numbered
     * device string it was mounted through (see vfs.c's header
     * comment, part 2, for why the device name is numbered rather
     * than plain "IDE" — it mirrors block.c's ide0_block_device()/
     * future ide1_block_device() numbering), not a borrowed "/"
     * convention. strcpy is safe here: "IDE0" (5 bytes incl. NUL)
     * always fits vfs_node_t's name[32].
     *
     * "IDE0" is hardcoded here rather than passed in as a parameter
     * because this file's whole design is single-mount/single-caller
     * through Day 13/14 (see this file's "Static mount state" comment
     * above) — vfs.c is the ONE caller, and it only ever mounts
     * "IDE0" today. Once a second physical drive is real (block.c
     * grows ide1_block_device(), vfs.c grows an "IDE1" branch),
     * fat12_vfs_mount()'s signature is where that generalization
     * belongs — it should take the device name as a parameter instead
     * of assuming "IDE0", rather than this file guessing which of
     * several possible root names to hardcode. Not needed yet: no
     * second physical drive exists to mount. */
    {
        const char *root_name = "IDE0";
        uint32_t i = 0;
        while (root_name[i] != '\0' && i < sizeof(g_fat12_root.name) - 1) {
            g_fat12_root.name[i] = root_name[i];
            i++;
        }
        g_fat12_root.name[i] = '\0';
    }
    g_fat12_root.type = VFS_TYPE_DIR;
    g_fat12_root.size = 0;
    g_fat12_root.fs = &g_fat12_fs;
    g_fat12_root.fs_private = FAT12_VFS_ROOT_SENTINEL;

    g_fat12_fs.root = &g_fat12_root;

    return &g_fat12_fs;
}