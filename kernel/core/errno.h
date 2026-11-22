/* kernel/core/errno.h - DAY70: LumaOS 统一错误码
 *
 * 为什么要有这个文件
 * ------------------
 * Day 63 之前，每个子系统各说各话：
 *   - FAT 返回 fat_error_t（FAT_ERR_NOT_FOUND = -2 ...），
 *   - LMFS 的所有失败都是 -1（找不到、没空间、重名、I/O 错误分不出来），
 *   - ISO9660 / elf_exec / shell 命令各自打印一句话，
 *   - 系统调用返回 -LUMA_Exxx（Day 50 起），只有 5 个号。
 * 用户程序拿到一个 -1 什么也做不了：不知道是“文件不存在”（可以去创建它）还是
 * “磁盘满了”（该告诉用户）还是“参数错了”（程序自己有 bug）。
 *
 * 约定（全系统一致，也是 Linux/POSIX 的约定，所以数值沿用 Linux，查资料方便）
 * ---------------------------------------------------------------------------
 *   1. 错误号是正数，名字 LUMA_Exxx，例如 LUMA_ENOENT = 2。
 *   2. “返回错误”的函数返回 -LUMA_Exxx（负数）；成功返回 0 或一个非负的结果
 *      （读到的字节数、文件大小……）。所以调用者只需要写 `if (r < 0)`，
 *      想知道原因就看 -r。
 *   3. 系统调用沿用 Day 50 的规则：eax 在 [-4095, -1] 之间就是错误。
 *   4. luma_strerror(e) / luma_errname(e) 把错误号变成人能读的文字，
 *      同一张表也被 SYS_STRERROR 系统调用暴露给用户程序（见 syscall.h）。
 *
 * 这个头文件不依赖内核任何其它头文件，用户程序（.sw）也可以直接 #include 它，
 * 或者把下面的 #define 抄走：它们就是 ABI 的一部分，数值不会再变。
 */
#ifndef LUMA_ERRNO_H
#define LUMA_ERRNO_H

#define LUMA_EOK            0     /* 没有错误 */
#define LUMA_EPERM          1     /* 操作不被允许 */
#define LUMA_ENOENT         2     /* 文件或目录不存在 */
#define LUMA_ESRCH          3     /* 没有这样的进程（Day 52：SYS_EXIT 时没有程序在运行） */
#define LUMA_EIO            5     /* 底层读写（磁盘/光驱）出错 */
#define LUMA_E2BIG          7     /* 参数/路径列表太长 */
#define LUMA_ENOEXEC        8     /* 不是可执行文件（ELF 校验失败） */
#define LUMA_EBADF          9     /* 文件描述符不对 */
#define LUMA_ENOMEM        12     /* 内存不够 */
#define LUMA_EACCES        13     /* 没有权限 */
#define LUMA_EFAULT        14     /* 指针指向不可访问的内存 */
#define LUMA_EBUSY         16     /* 资源正忙（例如已经有程序在运行） */
#define LUMA_EEXIST        17     /* 已经存在 */
#define LUMA_ENODEV        19     /* 没有这个设备/卷 */
#define LUMA_ENOTDIR       20     /* 不是目录 */
#define LUMA_EISDIR        21     /* 是目录（想当文件用） */
#define LUMA_EINVAL        22     /* 参数不合法 */
#define LUMA_EMFILE        24     /* 打开的文件太多 */
#define LUMA_EFBIG         27     /* 文件太大（超过该文件系统的上限） */
#define LUMA_ENOSPC        28     /* 磁盘（或目录、inode）已满 */
#define LUMA_EROFS         30     /* 只读文件系统 */
#define LUMA_ERANGE        34     /* 结果超出范围（缓冲区太小） */
#define LUMA_ENAMETOOLONG  36     /* 名字或路径太长 */
#define LUMA_ENOSYS        38     /* 没有这个系统调用/这个文件系统不支持该操作 */
#define LUMA_ENOTEMPTY     39     /* 目录不为空 */
#define LUMA_EUCLEAN      117     /* 文件系统结构损坏（例如 FAT 簇链断裂） */

#define LUMA_ERRNO_MAX    117     /* 已定义的最大错误号 */

#ifdef LUMA_KERNEL_SIDE
/* 以下只在内核里有实现（kernel/core/errno.c）；用户程序走 SYS_STRERROR。 */

/* 错误号 -> 一句话描述（"No such file or directory"）。e 可以带符号：
 * -2 和 2 给同一个答案。没登记的号返回 "Unknown error"。永不返回 NULL。 */
const char *luma_strerror(int e);

/* 错误号 -> 符号名（"ENOENT"）。没登记的返回 "E?"。永不返回 NULL。 */
const char *luma_errname(int e);

/* 反查：符号名（"ENOENT"，不区分大小写，EXXX 的 "E" 可省略）-> 错误号，找不到返回 -1。 */
int luma_errno_from_name(const char *name);

/* 遍历登记表：第 i 项的错误号（i 从 0 开始），超出返回 -1。errno 命令用它列表。 */
int luma_errno_nth(int i);

/* 把各子系统的老式错误码折算成统一错误码，返回值总是负数（-LUMA_Exxx）：
 *   fat_error_t（FAT_ERR_*，见 fat_vfs.h）：  luma_err_from_fat(rc)
 * LMFS 没有自己的错误码（成功 0 / 失败 -1），由 vfs_ops 在失败点根据上下文判断，
 * 见 kernel/fs/vfs/vfs_ops.c。 */
int luma_err_from_fat(int fat_rc);
#endif

#endif /* LUMA_ERRNO_H */
