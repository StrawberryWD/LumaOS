/* kernel/fs/vfs/vfs_ops.h - DAY64/68/70: 路径级文件操作的统一入口（FAT / LMFS / ISO9660 一视同仁）
 *
 * 为什么需要它
 * ------------
 * Day 24~42 的 create / write / rm / read / mkdir 命令各自带着一套“这是 FAT 还是 LMFS”的分支
 * （见 write.c 的 write_fs_*() 辅助函数、create.c、rm.c……），每个命令都在重复同一份分派逻辑，
 * 返回值也各有各的约定（FAT 返回 fat_error_t，LMFS 返回 0/-1）。Day 64 的压力测试需要在任意卷上
 * “建文件、写、读回、删”几百次，Day 68 的 `df` 需要任意卷的容量，Day 70 要求统一错误码——
 * 三件事都需要同一个东西：一层薄薄的、按路径工作的、返回 -LUMA_Exxx 的适配层。
 *
 * 约定
 * ----
 *   * path 是 shell_resolve_path() 的输出："IDE3/docs/a.txt"（挂载槽名 + 卷内路径）。
 *   * 返回值：成功 >= 0（vfs_ops_write 返回写入字节数，vfs_ops_read 返回读到的字节数），
 *     失败 -LUMA_Exxx（errno.h）。
 *   * 全部是“尽力而为的前置检查 + 底层调用”：底层各自的校验（FAT 的 8.3 名字规则、LMFS 的目录项
 *     上限……）仍然在底层，这里负责把失败原因翻译成统一错误码（见 vfs_ops.c 里每个函数的注释）。
 *   * 不缓存任何东西，不持有任何状态；线程安全问题不存在（单核、无抢占）。
 */
#ifndef LUMA_VFS_OPS_H
#define LUMA_VFS_OPS_H

#include <stdint.h>
#include "vfs.h"

typedef struct {
    const char *fstype;        /* "fat12" / "fat16" / "fat32" / "lmfs" / "iso9660" */
    int         read_only;     /* 1 = 只读介质（ISO9660 光盘） */
    uint32_t    block_size;    /* 分配单位：FAT = 簇字节数，LMFS = 1024，ISO = 2048 */
    uint32_t    total_blocks;  /* 数据区总块数 */
    uint32_t    free_blocks;   /* 空闲块数 */
    uint32_t    total_inodes;  /* LMFS: inode 总数；其余 0 */
    uint32_t    free_inodes;   /* LMFS: 空闲 inode；其余 0 */
} vfs_statfs_t;

int vfs_ops_create(const char *path);                               /* 0 / -errno */
int vfs_ops_mkdir(const char *path);                                /* 0 / -errno（FAT：-ENOSYS） */
int vfs_ops_write(const char *path, const void *buf, uint32_t n);   /* 整个文件覆盖写；文件必须已存在；返回 n / -errno */
int vfs_ops_read(const char *path, void *buf, uint32_t cap);        /* 读整个文件；文件比 cap 大 -> -ERANGE；返回字节数 / -errno */
int vfs_ops_delete(const char *path);                               /* 0 / -errno（目录 -> -EISDIR） */
int vfs_ops_stat(const char *path, uint32_t *size, int *is_dir);    /* 0 / -errno */
int vfs_ops_dir_count(const char *path, uint32_t *count);           /* 目录项数（不含 . ..） */
int vfs_ops_statfs(const char *path, vfs_statfs_t *out);            /* 0 / -errno；path 只用来选卷（"IDE3" 即可） */

/* DAY74: 按路径找到文件的 vfs_node（供“打开文件”用，不读内容）。成功 0 并填 *node；
 * 不存在 -ENOENT，是目录 -EISDIR，卷不存在 -ENODEV。之后用 node->fs->open/read/close 随机读（file_t.pos 就是偏移）。 */
int vfs_ops_open_node(const char *path, vfs_node_t *node);

/* 把 "IDE3/docs/a" 拆成卷 + 卷内路径；找不到卷 -> -LUMA_ENODEV */
int vfs_ops_split(const char *path, filesystem_t **fs, const char **sub);

#endif
