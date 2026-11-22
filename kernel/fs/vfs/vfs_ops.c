/* kernel/fs/vfs/vfs_ops.c - 说明见 vfs_ops.h */
#define LUMA_KERNEL_SIDE 1
#include "vfs_ops.h"
#include "../../core/errno.h"
#include "../../lib/string.h"
#include "../fat/fat_vfs.h"
#include "../lmfs/lmfs_vfs.h"
#include "../lmfs/lmfs.h"

static int is_lmfs(const filesystem_t *fs) { return fs && fs->name && strcmp(fs->name, "lmfs") == 0; }
static int is_iso (const filesystem_t *fs) { return fs && fs->name && strcmp(fs->name, "iso9660") == 0; }
static int is_fat (const filesystem_t *fs)
{
    return fs && fs->name && (strcmp(fs->name, "fat12") == 0 || strcmp(fs->name, "fat16") == 0 ||
                              strcmp(fs->name, "fat32") == 0);
}

int vfs_ops_split(const char *path, filesystem_t **fs, const char **sub)
{
    if (!path || !fs || !sub) return -LUMA_EINVAL;
    const char *sep = strchr(path, '/');
    char name[8];
    uint32_t len = sep ? (uint32_t)(sep - path) : (uint32_t)strlen(path);
    if (len == 0 || len >= sizeof(name)) return -LUMA_ENODEV;
    memcpy(name, path, len);
    name[len] = '\0';
    filesystem_t *f = vfs_lookup_mount(name);
    if (!f) return -LUMA_ENODEV;
    *fs = f;
    *sub = sep ? sep + 1 : path + len;
    return 0;
}

/* 取路径最后一个分量 */
static const char *leaf(const char *sub)
{
    const char *p = strrchr(sub, '/');
    return p ? p + 1 : sub;
}

/* FAT 8.3 名字检查：主名 1~8、扩展名 0~3、只允许字母数字和 _ - ~ $ */
static int fat_name_ok(const char *n)
{
    int base = 0, ext = 0, in_ext = 0;
    if (!*n) return -LUMA_EINVAL;
    for (; *n; n++) {
        char c = *n;
        if (c == '.') { if (in_ext || base == 0) return -LUMA_EINVAL; in_ext = 1; continue; }
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-' || c == '~' || c == '$';
        if (!ok) return -LUMA_EINVAL;
        if (in_ext) ext++; else base++;
    }
    if (base > 8 || ext > 3) return -LUMA_ENAMETOOLONG;
    return 0;
}

/* 打开已存在的节点（FAT / LMFS / ISO 通用）。返回 0 或 -errno。 */
static int open_node(filesystem_t *fs, const char *path, const char *sub, vfs_node_t *node)
{
    if (is_fat(fs)) {
        if (sub[0] == '\0' && fs->root) { *node = *fs->root; return 0; }   /* 卷根目录本身 */
        if (strchr(sub, '/')) return -LUMA_ENOENT;          /* FAT 只有根目录 */
        int e = fat_open(path, node);
        if (e == FAT_OK) return 0;
        return e == FAT_ERR_NOT_FOUND ? -LUMA_ENOENT : luma_err_from_fat(e);
    }
    if (vfs_fs_has_subdirs(fs)) {
        if (sub[0] == '\0') { *node = *fs->root; return 0; }
        return vfs_fs_open_path(fs, path, node) == 0 ? 0 : -LUMA_ENOENT;
    }
    return -LUMA_ENOSYS;
}

int vfs_ops_stat(const char *path, uint32_t *size, int *is_dir)
{
    filesystem_t *fs; const char *sub;
    int r = vfs_ops_split(path, &fs, &sub);
    if (r < 0) return r;
    vfs_node_t node;
    r = open_node(fs, path, sub, &node);
    if (r < 0) return r;
    if (size)   *size = node.size;
    if (is_dir) *is_dir = (node.type == VFS_TYPE_DIR);
    return 0;
}

int vfs_ops_open_node(const char *path, vfs_node_t *node)
{
    filesystem_t *fs; const char *sub;
    int r = vfs_ops_split(path, &fs, &sub);
    if (r < 0) return r;
    if (!node) return -LUMA_EFAULT;
    r = open_node(fs, path, sub, node);
    if (r < 0) return r;
    if (node->type == VFS_TYPE_DIR) return -LUMA_EISDIR;
    if (!node->fs || !node->fs->open || !node->fs->read) return -LUMA_ENOSYS;
    return 0;
}

int vfs_ops_statfs(const char *path, vfs_statfs_t *out)
{
    filesystem_t *fs; const char *sub;
    int r = vfs_ops_split(path, &fs, &sub);
    if (r < 0) return r;
    if (!out) return -LUMA_EINVAL;
    memset(out, 0, sizeof(*out));
    out->fstype = fs->name;
    if (is_lmfs(fs)) {
        lmfs_fs_t *l = (lmfs_fs_t *)fs->fs_private;
        if (!l) return -LUMA_EIO;
        out->block_size   = LMFS_BLOCK_SIZE;
        out->total_blocks = l->sb.data_block_count - 1;     /* 0 号永久保留，不算容量 */
        out->free_blocks  = l->free_blocks;
        out->total_inodes = l->sb.inode_count;
        out->free_inodes  = l->free_inodes;
        out->read_only    = l->dev && !l->dev->write;
        return 0;
    }
    if (is_fat(fs)) {
        uint32_t tot, fre, bpc;
        int e = fat_vfs_statfs(fs, &tot, &fre, &bpc);
        if (e != FAT_OK) return luma_err_from_fat(e);
        out->block_size = bpc; out->total_blocks = tot; out->free_blocks = fre;
        return 0;
    }
    if (is_iso(fs)) {
        /* 光盘：只读，没有“空闲”。容量用根节点之外无从得知，这里给 0 块并标只读。 */
        out->read_only = 1;
        out->block_size = 2048;
        return 0;
    }
    return -LUMA_ENOSYS;
}

int vfs_ops_create(const char *path)
{
    filesystem_t *fs; const char *sub;
    int r = vfs_ops_split(path, &fs, &sub);
    if (r < 0) return r;
    if (is_iso(fs)) return -LUMA_EROFS;
    if (sub[0] == '\0') return -LUMA_EISDIR;
    const char *name = leaf(sub);
    if (!*name) return -LUMA_EINVAL;

    vfs_node_t tmp;
    if (open_node(fs, path, sub, &tmp) == 0) return -LUMA_EEXIST;

    if (is_lmfs(fs)) {
        if (strlen(name) > LMFS_NAME_MAX) return -LUMA_ENAMETOOLONG;
        lmfs_fs_t *l = (lmfs_fs_t *)fs->fs_private;
        if (l && l->free_inodes == 0) return -LUMA_ENOSPC;
        if (lmfs_vfs_create(path) == 0) return 0;
        /* 失败原因：父目录不存在 / 父目录不是目录 / 目录满（8 块 × 16 项）/ I/O */
        char parent[96]; uint32_t n = (uint32_t)(name - path);
        if (n >= sizeof(parent)) return -LUMA_ENAMETOOLONG;
        memcpy(parent, path, n); if (n > 0 && parent[n - 1] == '/') n--; parent[n] = '\0';
        uint32_t sz; int isdir = 0;
        int pr = vfs_ops_stat(parent, &sz, &isdir);
        if (pr < 0) return -LUMA_ENOENT;
        if (!isdir) return -LUMA_ENOTDIR;
        return -LUMA_ENOSPC;            /* 剩下最常见的原因：目录项槽位用完 */
    }
    if (is_fat(fs)) {
        if (strchr(sub, '/')) return -LUMA_ENOENT;
        int nr = fat_name_ok(name);
        if (nr < 0) return nr;
        int e = fat_create_file(path);
        if (e == FAT_OK) return 0;
        /* fat_create_file 用 INVALID_ARGS 同时表示“根目录满”和“名字不合法”；名字已检查过，
         * 所以走到这里的 INVALID_ARGS 就是根目录区没有空槽（或没有空闲簇）。 */
        if (e == FAT_ERR_INVALID_ARGS || e == FAT_ERR_NOT_FOUND) return -LUMA_ENOSPC;
        if (e == FAT_ERR_IO) {
            /* 底层把“根目录没有空槽”和“真的 I/O 错误”都折成 FAT_ERR_IO（见 fat_vfs.c fat_create_file 末尾）。
             * 区分办法：名字合法、卷里还有空闲簇、读得出根目录 -> 几乎一定是根目录满了（FAT12/16 的根目录是
             * 固定区域，FAT32 这里也只用根目录的第一个簇），报 ENOSPC；否则才是 EIO。 */
            vfs_statfs_t sf; uint32_t cnt; char slot[16]; uint32_t k = 0;
            while (path[k] && path[k] != '/' && k < sizeof(slot) - 1) { slot[k] = path[k]; k++; }
            slot[k] = '\0';                          /* 卷根目录 = 槽名本身，如 "IDE2" */
            if (vfs_ops_statfs(slot, &sf) == 0 && sf.free_blocks > 0 && vfs_ops_dir_count(slot, &cnt) == 0)
                return -LUMA_ENOSPC;
        }
        return luma_err_from_fat(e);
    }
    return -LUMA_ENOSYS;
}

int vfs_ops_mkdir(const char *path)
{
    filesystem_t *fs; const char *sub;
    int r = vfs_ops_split(path, &fs, &sub);
    if (r < 0) return r;
    if (is_iso(fs)) return -LUMA_EROFS;
    if (!is_lmfs(fs)) return -LUMA_ENOSYS;          /* FAT 只有根目录 */
    if (sub[0] == '\0') return -LUMA_EEXIST;
    vfs_node_t tmp;
    if (open_node(fs, path, sub, &tmp) == 0) return -LUMA_EEXIST;
    if (strlen(leaf(sub)) > LMFS_NAME_MAX) return -LUMA_ENAMETOOLONG;
    lmfs_fs_t *l = (lmfs_fs_t *)fs->fs_private;
    if (l && (l->free_inodes == 0 || l->free_blocks == 0)) return -LUMA_ENOSPC;
    if (lmfs_vfs_mkdir(path) == 0) return 0;
    return -LUMA_ENOENT;                            /* 最常见：父目录不存在 */
}

int vfs_ops_write(const char *path, const void *buf, uint32_t n)
{
    filesystem_t *fs; const char *sub;
    int r = vfs_ops_split(path, &fs, &sub);
    if (r < 0) return r;
    if (!buf && n) return -LUMA_EFAULT;
    if (is_iso(fs)) return -LUMA_EROFS;
    vfs_node_t node;
    r = open_node(fs, path, sub, &node);
    if (r < 0) return r;
    if (node.type == VFS_TYPE_DIR) return -LUMA_EISDIR;

    if (is_lmfs(fs)) {
        if (n > LMFS_MAX_FILE_SIZE) return -LUMA_EFBIG;
        lmfs_fs_t *l = (lmfs_fs_t *)fs->fs_private;
        /* 保守估计需要的块数（数据块 + 可能的间接索引块），已有内容的块会先被释放，所以要加上 */
        uint32_t need = (n + LMFS_BLOCK_SIZE - 1) / LMFS_BLOCK_SIZE + (n > 8u * LMFS_BLOCK_SIZE ? 1 : 0);
        uint32_t have = (node.size + LMFS_BLOCK_SIZE - 1) / LMFS_BLOCK_SIZE + (node.size > 8u * LMFS_BLOCK_SIZE ? 1 : 0);
        if (l && need > l->free_blocks + have) return -LUMA_ENOSPC;
        if (lmfs_vfs_write(path, buf, n) == 0) return (int)n;
        return -LUMA_EIO;
    }
    if (is_fat(fs)) {
        int w = fat_write(path, buf, n);
        if (w >= 0) return w;
        if (w == FAT_ERR_NOT_FOUND || w == FAT_ERR_INVALID_ARGS) {
            /* fat_write 的这两个码在“没有空闲簇”时也会出现：用空闲簇数确认 */
            vfs_statfs_t st;
            if (vfs_ops_statfs(path, &st) == 0 && st.free_blocks * st.block_size < n)
                return -LUMA_ENOSPC;
        }
        return luma_err_from_fat(w);
    }
    return -LUMA_ENOSYS;
}

/* 读整个文件：FAT / LMFS / ISO 都走 node.fs->open + read 这条通路（read.c 同款） */
int vfs_ops_read(const char *path, void *buf, uint32_t cap)
{
    filesystem_t *fs; const char *sub;
    int r = vfs_ops_split(path, &fs, &sub);
    if (r < 0) return r;
    if (!buf && cap) return -LUMA_EFAULT;
    vfs_node_t node;
    r = open_node(fs, path, sub, &node);
    if (r < 0) return r;
    if (node.type == VFS_TYPE_DIR) return -LUMA_EISDIR;
    if (node.size > cap) return -LUMA_ERANGE;
    if (node.size == 0) return 0;
    if (!node.fs || !node.fs->open || !node.fs->read) return -LUMA_ENOSYS;
    file_t file;
    if (node.fs->open(&node, &file) != 0) return -LUMA_EIO;
    uint32_t got = 0;
    while (got < node.size) {
        int n = node.fs->read(&file, (uint8_t *)buf + got, node.size - got);
        if (n < 0) { if (node.fs->close) node.fs->close(&file); return luma_err_from_fat(n); }
        if (n == 0) break;
        got += (uint32_t)n;
    }
    if (node.fs->close) node.fs->close(&file);
    return (int)got;
}

int vfs_ops_delete(const char *path)
{
    filesystem_t *fs; const char *sub;
    int r = vfs_ops_split(path, &fs, &sub);
    if (r < 0) return r;
    if (is_iso(fs)) return -LUMA_EROFS;
    vfs_node_t node;
    r = open_node(fs, path, sub, &node);
    if (r < 0) return r;
    if (node.type == VFS_TYPE_DIR) return -LUMA_EISDIR;
    if (is_lmfs(fs))
        return lmfs_vfs_delete(path) == 0 ? 0 : -LUMA_EIO;
    if (is_fat(fs)) {
        int e = fat_delete_file(path);
        return e == FAT_OK ? 0 : luma_err_from_fat(e);
    }
    return -LUMA_ENOSYS;
}

int vfs_ops_dir_count(const char *path, uint32_t *count)
{
    filesystem_t *fs; const char *sub;
    int r = vfs_ops_split(path, &fs, &sub);
    if (r < 0) return r;
    if (!count) return -LUMA_EINVAL;
    vfs_node_t dir;
    if (is_fat(fs)) {
        if (sub[0] != '\0') return -LUMA_ENOTDIR;
        dir = (vfs_root && vfs_root->fs == fs) ? *vfs_root : *fs->root;
    } else {
        r = open_node(fs, path, sub, &dir);
        if (r < 0) return r;
    }
    if (dir.type != VFS_TYPE_DIR) return -LUMA_ENOTDIR;
    if (!dir.fs || !dir.fs->readdir) return -LUMA_ENOSYS;
    vfs_node_t child;
    uint32_t idx = 0, n = 0;
    while (dir.fs->readdir(&dir, idx, &child) == 0) {
        idx++;
        if (strcmp(child.name, ".") == 0 || strcmp(child.name, "..") == 0) continue;
        n++;
    }
    *count = n;
    return 0;
}
