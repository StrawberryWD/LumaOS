/* kernel/proc/proc.h - LumaOS 5.0 DAY77: fork 雏形——进程表 + 协作式切换
 *
 * 目标（计划.md Day 77: "fork 雏形，为多任务做准备"）：一个用户程序可以调用 fork()，变成两个几乎一样的进程，
 * 各有自己的地址空间、内核栈、fd 表；wait() 等子进程结束并取退出码。今天还没有时钟抢占的调度器——
 * 进程只在三个点让出 CPU：wait()（子进程还没结束时）、yield()、exit()。所以输出顺序是确定的，便于测试；
 * Day 78 之后的“多任务”才会加上时钟中断驱动的抢占。
 *
 * 一个进程是什么：
 *   - pid / ppid / 状态（FREE 空槽、RUNNING 正在跑、READY 可运行、WAITING 在 wait() 里、ZOMBIE 已退出等人收尸）
 *   - 用户地址空间 aspace_t（段、堆、栈、运行时页）——fork 时整份复制（没有写时复制，页框池 8 MB，够用；COW 以后再做）
 *   - 自己的内核栈（静态数组，8 KB）和切走时保存的 esp——进程在内核里（系统调用中）被切走，回来时从原处继续
 *   - 自己的 fd 表（fork 时复制：两边各有一份独立的读写位置——和 Unix 的“共享偏移”不同，今天先这样）
 * “根进程”是 shell 的 `run` 启动的那个程序（pid 1）。它直接用原来的内核栈和原来的 aspace_t 对象，
 * 所以没有调用过 fork 的程序完全不受影响（proc_count == 0 时所有钩子都是空操作）。
 *
 * fork 怎么让“一次调用、两次返回”：
 *   int 0x80 进内核时，CPU 和 isr_common 已经把用户态现场（寄存器 + eip/cs/eflags/esp/ss）压在父进程内核栈顶。
 *   sys_fork 把这份现场原样拷到子进程内核栈顶，把里面的 eax 槽改成 0，再在它下面摆一个假的 proc_switch 帧，
 *   “返回地址”指向 proc_child_start（elf_enter.asm：isr_common 出口那串 pop + iretd）。
 *   以后调度器第一次切到子进程时，proc_switch 弹出假寄存器、ret 进 proc_child_start、iretd——子进程就像刚从
 *   int 0x80 返回一样回到用户态，返回值 0。父进程则正常返回子进程的 pid。
 *
 * 退出：
 *   子进程 exit：关 fd、销毁自己的地址空间、变 ZOMBIE、唤醒在 wait 的父进程，切走，永不回来。
 *   根进程 exit：先把还没结束的子孙都跑完（它们在它前面“收尸”），然后才真正走 elf_sys_exit 回到 shell。
 *   孤儿（父进程先退出）并给根进程。
 */
#ifndef LUMA_PROC_H
#define LUMA_PROC_H

#include <stdint.h>

#define PROC_MAX    8          /* 进程表大小（含根进程） */
#define PROC_KSTACK 8192       /* 每个子进程的内核栈 */

int  proc_fork(void);                        /* SYS_FORK：成功返回子 pid（父进程里），-errno 失败 */
int  proc_wait(uint32_t ustatus);            /* SYS_WAIT：返回已结束子进程的 pid，*ustatus = 状态；-ECHILD / -EFAULT */
int  proc_getpid(void);                      /* SYS_GETPID：没有 fork 过也能用（返回 1） */
void proc_yield(void);                       /* SYS_YIELD */
void proc_on_exit(int code, int via);        /* 所有“程序结束”的地方在 elf_sys_exit 之前调用；子进程不返回 */
void proc_reset(void);                       /* 新程序开始前（elf_exec）清空进程表 */
int  proc_count(void);                       /* 当前进程数（0 = 没有人 fork 过） */

/* 状态值，proc 测试命令也用 */
enum { PS_FREE = 0, PS_RUNNING, PS_READY, PS_WAITING, PS_ZOMBIE };

#endif
