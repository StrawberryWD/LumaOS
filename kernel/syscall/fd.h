/* kernel/syscall/fd.h - DAY74: 每个程序一张“文件描述符表”（fd table）
 *
 * ===================================================================================
 *  为什么需要它
 * ===================================================================================
 *  内核的 VFS 早就能读文件（fs->open / fs->read / fs->close，file_t.pos 是当前偏移），但那是内核自己调用的。
 *  用户程序想读文件，必须通过系统调用，而且不能把内核里的指针（vfs_node_t *、file_t *）交给程序——
 *  程序拿到指针就能改写内核数据，而且 Ring 3 的程序根本看不见内核的地址。
 *  UNIX 的经典解法：内核自己保管“已打开文件”，只给程序一个小整数——文件描述符 fd。程序以后所有操作
 *  （read / lseek / close）都带着这个整数，内核用它在表里找到真正的对象；表只有内核能碰。
 *  fd 是“不可伪造的句柄”：程序随便编一个数，顶多得到 -EBADF，不会读到别人的文件，更不会崩内核。
 *
 *  表的形状：
 *      fd 0 / 1 / 2     预留给 stdin / stdout / stderr（0 目前没有输入可读；1、2 由 SYS_WRITE 特判）。
 *                       open 永远不会返回 0~2，close(0~2) 返回 -EBADF（不让程序把标准流关掉）。
 *      fd 3 ... 3+LUMA_FD_MAX-1   普通文件。每项 = 一份 vfs_node_t（文件的元数据）+ 一个 file_t（node 指针 + pos）。
 *  open 返回“最小的空闲号”（POSIX 的规矩，所以 close(3) 之后下一个 open 又得到 3）。
 *  表满了 -> -EMFILE。每个程序一张表：程序开始前清空，结束后（无论正常退出、崩溃、被杀）全部关闭——
 *  所以程序忘了 fclose 也不会把“打开的文件”泄漏给下一个程序。
 *  今天只能只读打开（O_RDONLY）。写文件要求 VFS 有“按偏移写/追加”，现在的 vfs_ops_write 是整文件覆盖，留给后面。
 *
 *  路径：程序传来的字符串先拷进内核（逐字节检查每一页都映射、最长 LUMA_PATH_MAX-1 个字符，没有 NUL ->
 *  -ENAMETOOLONG），再交给 shell_resolve_path() ——和 shell 命令完全同一套规则：相对路径按当前目录
 *  （`cd` 设置的）解析，"IDE3/docs/a.txt" 是全名，"/a" 是当前卷的绝对路径。
 */
#ifndef LUMA_FD_H
#define LUMA_FD_H
#include <stdint.h>

#define LUMA_FD_FIRST     3        /* 第一个可分配的 fd */
#define LUMA_FD_MAX       16       /* 每个程序最多同时打开的普通文件数 */
#define LUMA_PATH_MAX     160u     /* 含结尾 NUL；与 shell_resolve_path 的内部缓冲一致 */
#define LUMA_READ_MAX     65536u   /* 一次 read 最多读这么多（短读，调用者循环）*/

/* open 的 flags：今天只有只读 */
#define LUMA_O_RDONLY     0
/* lseek 的 whence */
#define LUMA_SEEK_SET     0
#define LUMA_SEEK_CUR     1
#define LUMA_SEEK_END     2

void    fd_reset(void);                              /* 关闭所有打开的文件（程序开始前和结束后各调一次）*/
int     fd_open_count(void);                         /* 当前打开了几个（测试/调试用）*/
int32_t fd_open(const char *upath, uint32_t flags);  /* 用户指针；返回 fd 或 -errno */
int32_t fd_read(int32_t fd, uint32_t ubuf, uint32_t len);
int32_t fd_close(int32_t fd);
int32_t fd_lseek(int32_t fd, int32_t off, uint32_t whence);

/* DAY77: 进程切换时保存/恢复整张 fd 表（每个进程一张）。buf 至少 fd_state_size() 字节；fd_fork_state 不碰当前表。 */
#define LUMA_FD_STATE_MAX 16384
unsigned fd_state_size(void);
void fd_state_save(void *buf);
void fd_state_load(const void *buf);

#endif
