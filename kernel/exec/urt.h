/* kernel/exec/urt.h - LumaOS 5.0 - DAY55 用户态运行时页（原理见 urt.asm，Ring 3 见 aspace.h）
 *
 * 每个程序的地址空间里，用户栈正上方（LUMA_URT_BASE = 0x0A000000）有一页：
 *   只读（用户写它 -> #PF 保护违例）、用户可读可执行、内容是 urt.asm 的几段 stub + 填好的 luma_api_t 表。
 * 程序入口收到的 api 指针指向这页里的表（用户地址！），api->puts 等是这页里的 Ring 3 代码。
 * ELF 的段不得压在栈和这页上（elf.c 的 check_phdrs 拒绝，ELF_ERR_SEG_STACK）。
 */
#ifndef LUMA_URT_H
#define LUMA_URT_H

#include <stdint.h>
#include "../mm/aspace.h"

#define LUMA_URT_BASE  0x0A000000u      /* = ELF_USER_STACK_TOP：栈顶之上紧挨着的一页 */
#define LUMA_URT_SIZE  0x00001000u

/* 在 as 里映射这一页、拷入 stub、填 api 表、改成只读。池满返回 -1。 */
int      urt_install(aspace_t *as);
/* 以下两个查“当前页目录”：页没映射（例如在内核页目录下）返回 0。 */
uint32_t urt_api_va(void);
uint32_t urt_ret_stub_va(void);
/* SYS_EXIT 时从调用者的 eip 判断退出方式：0 = 入口函数 ret，1 = api->exit，2 = 其它 */
int      urt_exit_via(uint32_t eip);

#endif
