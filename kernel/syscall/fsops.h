/* kernel/syscall/fsops.h - DAY75: SYS_READDIR / SYS_STAT / SYS_GETCWD 的内核实现（调用号和结构见 syscall.h） */
#ifndef LUMA_FSOPS_H
#define LUMA_FSOPS_H
#include <stdint.h>
int32_t fsops_readdir(const char *upath, uint32_t index, uint32_t uout);
int32_t fsops_stat(const char *upath, uint32_t uout);
int32_t fsops_getcwd(uint32_t ubuf, uint32_t size);
/* DAY77b */
int32_t fsops_unlink(const char *upath);                      /* 删文件；目录 -EISDIR */
int32_t fsops_mkdir(const char *upath);                       /* 建目录（LMFS；FAT -ENOSYS） */
int32_t fsops_rmdir(const char *upath);                        /* DAY77c: 删空目录 */
int32_t fsops_rename(const char *uold, const char *unew);     /* 只支持文件：读 -> 写新 -> 删旧（非原子）；新路径已有文件则覆盖 */
#endif
