/* kernel/syscall/fsops.h - DAY75: SYS_READDIR / SYS_STAT / SYS_GETCWD 的内核实现（调用号和结构见 syscall.h） */
#ifndef LUMA_FSOPS_H
#define LUMA_FSOPS_H
#include <stdint.h>
int32_t fsops_readdir(const char *upath, uint32_t index, uint32_t uout);
int32_t fsops_stat(const char *upath, uint32_t uout);
int32_t fsops_getcwd(uint32_t ubuf, uint32_t size);
#endif
