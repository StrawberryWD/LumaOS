/* kernel/fs/lmfs/lmfs_vfs.c
 * LumaOS 5.0 - LMFS <-> VFS Adapter
 *
 * DAY35 (第二月 · 第6周 · Day 35 of the 5.0 filesystem roadmap):
 *   "LMFS 注册进 VFS。" 验收："FAT 与 LMFS 可以同时存在。"
 *
 * 见 lmfs_vfs.h 的文件头注释：这是 fat12_vfs.c 在 LMFS 这边的对应
 * 实现，逐条设计理由都照抄那边、只把 FAT12 专属的部分换成 LMFS 的。
 *
 * DAY42 (Day 42 of the same roadmap):
 *   "多级目录与路径解析。" 下方 lmfs_vfs_create/mkdir/write/open_path/
 *   delete 五个路径入口，原来各自抄了一份"切挂载点 + 校验是 LMFS +
 *   文件名不许含 '/' + 父目录写死根目录"的代码；现在全部改成先调
 *   split_lmfs_mount() 切出 (挂载点, 挂载点内的相对路径)，再把相对路径
 *   交给 lmfs.c 的 lmfs_resolve_path()/lmfs_resolve_parent() 逐段解析。
 *   "<挂载点>/a/b/c.txt" 因此第一次成为合法路径。
 */
#include "lmfs.h"
#include "lmfs_vfs.h"
#include "../../lib/string.h"   /* memset/strchr/strcmp/memcpy, DAY38 */

/* ===== Static mount state =====
 *
 * 一次 LMFS 挂载所需的全部状态：lmfs_mount()（Day 32）填好的
 * lmfs_fs_t、Day 35 加的 filesystem_t 包装、以及每个 VFS 层调用方都
 * 会经由 fs->root / 全局 vfs_root 触达的那一个根 vfs_node_t。跟
 * fat12_vfs.c 一样用 static 而不是堆分配——理由见 lmfs_vfs.h 的设计
 * 说明。
 */
static lmfs_fs_t    g_lmfs_mount;
static filesystem_t g_lmfs_fs;
static vfs_node_t   g_lmfs_root;

/* ---------------------------------------------------------------------
 * lmfs_vfs_open(node, out_file)
 *
 * 准备 *out_file 从 node 开始读：存好节点指针，偏移量清零——符合
 * vfs_open_fn 的文档契约（kernel/fs/vfs/vfs.h）。拒绝把目录当文件
 * 打开，跟 fat12_vfs_open() 完全一样的检查。
 * --------------------------------------------------------------------- */
static int lmfs_vfs_open(vfs_node_t *node, file_t *out_file)
{
    if (!node || !out_file || node->type != VFS_TYPE_FILE)
        return -1;

    out_file->node = node;
    out_file->pos  = 0;
    return 0;
}

/* ---------------------------------------------------------------------
 * lmfs_vfs_read(file, buf, size)
 *
 * DAY40 (第二月 · 第6周 · Day 40 of the 5.0 filesystem roadmap):
 *   "实现 read()。" 验收："read bigfile.txt 正常。"
 *
 * 把 Day 40 的 lmfs_read()（inode 号 + 显式 offset 的形状）适配成
 * vfs_read_fn（file_t* + 隐式、随 file->pos 推进的形状）——DAY35 的
 * 占位桩到今天才有真正的实现：file->node->fs_private 就是
 * lmfs_vfs_mount()/lmfs_vfs_readdir()/lmfs_vfs_open_path() 一路存
 * 下来的 inode 号，file->node->fs->fs_private 是这次挂载的
 * lmfs_fs_t*，两者都已经是现成的，这里只是把它们喂给 lmfs_read()、
 * 再按读到的字节数推进 file->pos——跟 fat12_vfs_read() 当年只是
 * 转发给 fat_read() 做的事是同一个角色。
 * --------------------------------------------------------------------- */
static int lmfs_vfs_read(file_t *file, void *buf, uint32_t size)
{
    if (!file || !file->node)
        return -1;
    if (file->node->type != VFS_TYPE_FILE)
        return -1;

    lmfs_fs_t *fs = (lmfs_fs_t *)file->node->fs->fs_private;
    if (!fs)
        return -1;
    uint32_t inode_num = (uint32_t)(uintptr_t)file->node->fs_private;

    int n = lmfs_read(fs, inode_num, buf, size, file->pos);
    if (n > 0)
        file->pos += (uint32_t)n;
    return n;
}

/* ---------------------------------------------------------------------
 * lmfs_vfs_close(file)
 *
 * 跟 vfs_close_fn 自己文档里的默认契约完全一致：把 file->node 清成
 * NULL，让任何后续不小心对同一个（已经"关闭"的）file_t 再调用
 * fs->read() 会因为 file->node 是 NULL 而明显失败，而不是悄悄继续
 * 读一个调用者以为已经关掉的文件。LMFS 目前没有需要释放的每次打开
 * 状态（没有内核堆，也没有额外分配的资源），所以这里不需要像
 * fat_close() 那样转发到一个共享实现，直接原地做这件事就够。
 * --------------------------------------------------------------------- */
static int lmfs_vfs_close(file_t *file)
{
    if (!file || !file->node)
        return -1;

    file->node = 0;
    return 0;
}

/* ---------------------------------------------------------------------
 * lmfs_vfs_readdir(dir_node, index, out_node)
 *
 * 把 lmfs_readdir()（Day 34，"一次性给整个数组"的形状）适配成
 * vfs_readdir_fn（"按 index 一次给一条"的形状）——跟
 * fat12_vfs_readdir() 对 fat12_readdir() 做的事完全一样的适配方向。
 *
 * dir_node 必须是 VFS_TYPE_DIR，其 fs_private 是这个目录的 inode 号
 * （lmfs_vfs_mount() 给根目录填的是 LMFS_ROOT_INODE_NUM；子目录还
 * 没有任何代码产出，Day 42 的多级目录会用到这条路径，读法完全一样，
 * 不需要改这个函数）。
 *
 * 关于按 index 单条调用却每次都重新跑一遍 lmfs_readdir() 全量扫描：
 * 跟 fat12_vfs_readdir() 的 NOTE ON RE-WALKING PER CALL 是同一个
 * 权衡——LMFS_DIRLIST_MAX_ENTRIES(32) 把这个代价限制得足够小，现在
 * 优化是在解决一个还不存在的性能问题。
 * --------------------------------------------------------------------- */
static int lmfs_vfs_readdir(vfs_node_t *dir_node, uint32_t index,
                             vfs_node_t *out_node)
{
    if (!dir_node || !out_node || dir_node->type != VFS_TYPE_DIR)
        return -1;

    lmfs_fs_t *fs = (lmfs_fs_t *)dir_node->fs->fs_private;
    if (!fs)
        return -1;

    uint32_t dir_inode_num = (uint32_t)(uintptr_t)dir_node->fs_private;

    /* DAY67: 不再用 32 项的静态数组 + lmfs_readdir() 全量扫描（那会让 `ls` 看不到第 33 项以后的
     * 文件，Day 42 文档里的“已知限制”，Day 64 的压力测试第一个撞上它）。改成按序号取第 index 个
     * 有效项：lmfs_dir_entry_at() 返回 1 表示“index 超出末尾”，0 = 取到了，-1 = 读盘错。 */
    static lmfs_dirlist_entry_t entry_buf;
    int rc = lmfs_dir_entry_at(fs, dir_inode_num, index, &entry_buf);
    if (rc != 0)
        return -1;   /* 按 vfs_readdir_fn 的约定："index 超出末尾"就停 */

    lmfs_dirlist_entry_t *e = &entry_buf;

    /* lmfs_dirlist_entry_t 的 name[56]（LMFS_NAME_MAX+1）比
     * vfs_node_t 的 name[32] 还大——见 vfs.h 里 name[32] 的注释：
     * 这是 Day 8 定的、原本按 FAT 8.3 场景设计的大小，LMFS 的长
     * 文件名可能超出它。这里按 vfs_node_t 的容量截断拷贝（宁可显示
     * 被截断的名字，也不要越界写），这个截断本身是 vfs_node_t 这个
     * 通用结构体的固有限制，不是 lmfs_readdir() 的问题——e->name
     * 在 entries[] 里本身是完整、未截断的。 */
    {
        uint32_t i = 0;
        while (e->name[i] != '\0' && i < sizeof(out_node->name) - 1) {
            out_node->name[i] = e->name[i];
            i++;
        }
        out_node->name[i] = '\0';
    }

    out_node->type = (e->type == LMFS_TYPE_DIR) ? VFS_TYPE_DIR
                                                  : VFS_TYPE_FILE;
    out_node->size = e->size;
    out_node->fs   = dir_node->fs;
    out_node->fs_private = (void *)(uintptr_t)e->inode_num;

    return 0;
}

/* lmfs_filesystem_ops — 见 lmfs_vfs.h 的文档注释。`.root`/`.fs_private`
 * 是每次挂载各自的状态（由 lmfs_vfs_mount() 填入），这张表本身是
 * 共享的，留 NULL/0。 */
const filesystem_t lmfs_filesystem_ops = {
    .name       = "lmfs",
    .open       = lmfs_vfs_open,
    .read       = lmfs_vfs_read,
    .readdir    = lmfs_vfs_readdir,
    .close      = lmfs_vfs_close,
    .root       = (vfs_node_t *)0,
    .fs_private = (void *)0,
};

filesystem_t *lmfs_vfs_mount(block_device_t *dev, uint32_t lba)
{
    if (lmfs_mount(dev, lba, &g_lmfs_mount) != 0)
        return (filesystem_t *)0;

    /* 从共享操作表出发，再填这一次挂载自己的 root/fs_private——
     * g_lmfs_fs 是"实例"，lmfs_filesystem_ops 是它复制函数指针的
     * "模板"，跟 fat12_vfs_mount() 的 g_fat12_fs 是同一个套路。 */
    g_lmfs_fs = lmfs_filesystem_ops;
    g_lmfs_fs.fs_private = &g_lmfs_mount;

    memset(&g_lmfs_root, 0, sizeof(g_lmfs_root));

    /* 根节点自己的 .name 字段：跟 fat12/16/32_vfs_mount() 一样先写
     * 死成 "IDE0"——这个字段本来就不是显示用的权威来源（见 vfs.c
     * 里 vfs_mount_name_of() 那段"BUGFIX / WHY THIS EXISTS"的完整
     * 说明：多次挂载会共享同一个 static 根节点，后写的会覆盖先写的
     * .name，所以显示层一律改查 vfs_mounts[] 表而不是这个字段），
     * 这里写 "IDE0"只是跟其他三个适配器保持字面一致，不代表这个卷
     * 真的会被分到 "IDE0" 这个槽位——真正的槽位名是
     * vfs_register_mount() 分配、调用方从它的 out_name 参数里拿到的
     * 那个。 */
    {
        const char *root_name = "IDE0";
        uint32_t i = 0;
        while (root_name[i] != '\0' && i < sizeof(g_lmfs_root.name) - 1) {
            g_lmfs_root.name[i] = root_name[i];
            i++;
        }
        g_lmfs_root.name[i] = '\0';
    }
    g_lmfs_root.type = VFS_TYPE_DIR;
    g_lmfs_root.size = 0;
    g_lmfs_root.fs   = &g_lmfs_fs;
    g_lmfs_root.fs_private = (void *)(uintptr_t)LMFS_ROOT_INODE_NUM;

    g_lmfs_fs.root = &g_lmfs_root;

    return &g_lmfs_fs;
}

/* split_lmfs_mount(path, &rest)
 *
 * DAY42: 五个路径入口共用的第一步。把 "<挂载点>/<挂载点内路径>" 在第一个
 * '/' 处切开：前半截查 vfs_lookup_mount()，必须是一个 LMFS 卷（跟
 * Day 38 起各入口里"防御性的第二道防线"是同一条检查，只是现在只写
 * 一遍）；后半截（可以是空串，也可以含任意多个 '/'）通过 *rest 返回，
 * 它是"相对于这个卷根目录"的路径，交给 lmfs_resolve_*() 解析。
 *   "IDE3/a/b.txt" -> rest = "a/b.txt"
 *   "IDE3/"        -> rest = ""   （卷根目录本身）
 *   "IDE3"         -> rest = ""   （同上，没有 '/' 也接受）
 * 挂载点不存在、或不是 LMFS 卷，返回 NULL。 */
static filesystem_t *split_lmfs_mount(const char *path, const char **rest)
{
    if (!path || !rest)
        return (filesystem_t *)0;

    const char *sep = strchr(path, '/');
    uint32_t len = (sep != (char *)0) ? (uint32_t)(sep - path)
                                      : (uint32_t)strlen(path);

    char mount_name[8];
    if (len == 0 || len >= sizeof(mount_name))
        return (filesystem_t *)0;
    memcpy(mount_name, path, len);
    mount_name[len] = '\0';

    filesystem_t *fs = vfs_lookup_mount(mount_name);
    if (!fs || !fs->name || strcmp(fs->name, "lmfs") != 0 || !fs->fs_private)
        return (filesystem_t *)0;

    *rest = (sep != (char *)0) ? sep + 1 : path + len;
    return fs;
}

int lmfs_vfs_create(const char *path)
{
    const char *rest;
    filesystem_t *fs = split_lmfs_mount(path, &rest);
    if (!fs)
        return -1;
    lmfs_fs_t *lfs = (lmfs_fs_t *)fs->fs_private;

    uint32_t parent;
    char name[LMFS_NAME_MAX + 1];
    if (lmfs_resolve_parent(lfs, LMFS_ROOT_INODE_NUM, rest, &parent,
                             name) != 0)
        return -1;   /* 父目录不存在/不是目录，或者最后一段为空 */

    uint32_t new_inode_num;
    return lmfs_create(lfs, parent, name, &new_inode_num);
}

int lmfs_vfs_mkdir(const char *path)
{
    const char *rest;
    filesystem_t *fs = split_lmfs_mount(path, &rest);
    if (!fs)
        return -1;
    lmfs_fs_t *lfs = (lmfs_fs_t *)fs->fs_private;

    uint32_t parent;
    char name[LMFS_NAME_MAX + 1];
    if (lmfs_resolve_parent(lfs, LMFS_ROOT_INODE_NUM, rest, &parent,
                             name) != 0)
        return -1;

    return lmfs_mkdir(lfs, parent, name, (uint32_t *)0);
}

int lmfs_vfs_write(const char *path, const void *buf, uint32_t size)
{
    const char *rest;
    filesystem_t *fs = split_lmfs_mount(path, &rest);
    if (!fs)
        return -1;
    lmfs_fs_t *lfs = (lmfs_fs_t *)fs->fs_private;

    uint32_t inode_num;
    if (lmfs_resolve_path(lfs, LMFS_ROOT_INODE_NUM, rest, &inode_num) != 0)
        return -1;   /* 文件必须已经存在——跟 fat_write() 一样，本函数
                        不会顺便新建目录项 */

    return lmfs_write(lfs, inode_num, buf, size);   /* 目录会被它拒绝 */
}

int lmfs_vfs_open_path(const char *path, vfs_node_t *out_node)
{
    if (!out_node)
        return -1;

    const char *rest;
    filesystem_t *fs = split_lmfs_mount(path, &rest);
    if (!fs)
        return -1;
    lmfs_fs_t *lfs = (lmfs_fs_t *)fs->fs_private;

    uint32_t inode_num;
    if (lmfs_resolve_path(lfs, LMFS_ROOT_INODE_NUM, rest, &inode_num) != 0)
        return -1;

    lmfs_inode_t ino;
    if (lmfs_read_inode(lfs, inode_num, &ino) != 0)
        return -1;

    /* 节点的 .name：取路径最后一段（忽略结尾 '/'）；路径就是卷根目录
     * 本身（rest 为空）时用挂载点名字——跟 lmfs_vfs_mount() 给根节点
     * 起名字是同一个思路。vfs_node_t::name 只有 32 字节，超长截断，
     * 理由同 lmfs_vfs_readdir()。 */
    const char *name = rest;
    {
        uint32_t end = (uint32_t)strlen(rest);
        while (end > 0 && rest[end - 1] == '/')
            end--;
        uint32_t start = end;
        while (start > 0 && rest[start - 1] != '/')
            start--;
        name = rest + start;
        memset(out_node, 0, sizeof(*out_node));
        uint32_t n = end - start;
        if (n == 0) {
            const char *mn = vfs_mount_name_of(fs);
            if (mn) {
                while (mn[n] != '\0' && n < sizeof(out_node->name) - 1) {
                    out_node->name[n] = mn[n];
                    n++;
                }
            }
        } else {
            if (n > sizeof(out_node->name) - 1)
                n = sizeof(out_node->name) - 1;
            memcpy(out_node->name, name, n);
        }
        out_node->name[n] = '\0';
    }
    out_node->type = (ino.type == LMFS_TYPE_DIR) ? VFS_TYPE_DIR
                                                   : VFS_TYPE_FILE;
    out_node->size = ino.file_size;
    out_node->fs = fs;
    out_node->fs_private = (void *)(uintptr_t)inode_num;

    return 0;
}

int lmfs_vfs_delete(const char *path)
{
    const char *rest;
    filesystem_t *fs = split_lmfs_mount(path, &rest);
    if (!fs)
        return -1;
    lmfs_fs_t *lfs = (lmfs_fs_t *)fs->fs_private;

    uint32_t parent;
    char name[LMFS_NAME_MAX + 1];
    if (lmfs_resolve_parent(lfs, LMFS_ROOT_INODE_NUM, rest, &parent,
                             name) != 0)
        return -1;

    return lmfs_delete(lfs, parent, name);
}
