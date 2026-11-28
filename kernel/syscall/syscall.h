/* kernel/syscall/syscall.h - LumaOS 5.0
 *
 * DAY50 (Week 8, Day 50): "定义 syscall ABI。"  验收："`int 0x80` 能进入 Kernel。"
 *
 * ===================================================================================
 *  为什么需要系统调用
 * ===================================================================================
 *  Day 46~49 里，程序靠内核塞给它的一张函数表 luma_api_t 来“说话”：程序 call api->puts，
 *  就是直接调用内核里的函数。这只在程序和内核同处 Ring 0、共享地址空间时才行得通——
 *  程序知道内核函数的地址，也就能随便写内核内存。
 *
 *  Day 53~55 要给程序独立的地址空间并把它降到 Ring 3：那时程序根本看不见内核的地址，
 *  CPU 也禁止它直接 call 进去。Ring 3 的程序想让内核做事（输出、退出、读文件……），
 *  唯一合法的途径是执行一条“软件中断”指令：
 *
 *          int 0x80
 *
 *  所以今天先把这条通道的“合同”（ABI）定死：调用号放哪、参数放哪、返回值放哪、
 *  哪些寄存器被保留、出错怎么表示。以后每增加一个系统调用，只是在表里添一行。
 *
 * ===================================================================================
 *  `int 0x80` 到底发生了什么（硬件做的部分）
 * ===================================================================================
 *   1. CPU 取出 IDT[0x80]（idt.c 里装的那个“门”）。
 *   2. 权限检查：软件中断指令 `int n` 要求 门的 DPL >= 当前特权级 CPL（数值上），否则 #GP。
 *      0x80 这个门 DPL=3，所以 Ring 3 的程序允许 `int 0x80`；而 IDT 里所有别的门 DPL=0，
 *      用户程序执行 `int 14` 之类会被拦下——它不能假装发生了缺页。
 *   3. [Ring 3 -> Ring 0 时] CPU 从 TSS 取内核栈 (ss0:esp0) 并切换到它，再把用户的
 *      ss、esp 压到新栈上；[同在 Ring 0 时（今天）] 不换栈，直接压在当前栈上。
 *   4. CPU 压入 eflags、cs、eip（返回地址），再清 IF（因为这是“中断门”，不是“陷阱门”），
 *      跳到门里写的处理程序地址。
 *   5. 返回时 `iret` 把 eip/cs/eflags（以及切换过栈时的 esp/ss）弹回去，程序从 `int 0x80`
 *      的下一条指令继续，就像刚调用了一个普通函数。
 *
 *  软件做的部分：interrupts.asm 的 isr128 压入“假错误码 0 + 向量号 0x80”，进入公共代码
 *  isr_common（保存全部通用寄存器和 ds，把段寄存器换成内核数据段），调用 isr_handler()；
 *  它看到向量 0x80，调用本模块的 syscall_dispatch()。返回时公共代码把保存的寄存器原样
 *  弹回——所以分发器只要改写保存区里 eax 那一格，程序就“收到了返回值”。
 *
 *  为什么选 0x80？不是硬件规定，是 Linux/i386 的传统：8259 PIC 已被重映射到向量 32~47
 *  （pic.c），0x80 与它们和 CPU 异常（0~31）都不冲突；沿用这个编号，查资料、读别人代码最方便。
 *
 * ===================================================================================
 *  LumaOS syscall ABI（版本 1）  —— 与 Linux i386 `int 0x80` 约定相同
 * ===================================================================================
 *      eax            系统调用号（SYS_*）                      返回时：返回值
 *      ebx            参数 1
 *      ecx            参数 2
 *      edx            参数 3
 *      esi            参数 4
 *      edi            参数 5
 *      ebp            参数 6
 *
 *   - 返回值在 eax（32 位有符号）。>= 0 表示成功（含义由具体调用定义，如 write 返回字节数）；
 *     在 [-4095, -1] 之间表示出错，值是 -LUMA_Exxx（见下）。与 C 库的“返回 -1 并设 errno”
 *     不同，内核直接把错误号取负当返回值，没有 errno 全局变量（Ring 3 的程序不能共享内核变量）。
 *   - 保留寄存器：除 eax 外，所有通用寄存器（ebx ecx edx esi edi ebp esp）和段寄存器在
 *     `int 0x80` 前后保持不变；eflags 里的 IF/DF 等由 iret 恢复成调用前的值。
 *     因此调用者不需要把 ecx/edx 当“易失”寄存器保护起来（这与 cdecl 函数调用不同）。
 *   - 调用号不存在、或登记了但尚未实现 -> 返回 -LUMA_ENOSYS，内核状态不变。
 *   - 参数全部不可信：它们是程序随便填的整数。凡是“指针”，Day 51 的 sys_write 起必须先检查
 *     它指向的整块区域都在用户可访问范围内再读写（copy_from_user 的雏形）。
 *   - 调用号一经发布就不再改变含义（二进制兼容）；要改就换新号码，旧号码留空或返回 ENOSYS。
 *
 *  调用号表（今天的状态）：
 *      0  SYS_NOP      什么也不做，返回 0。用来测通道本身：进得去、出得来、寄存器没被弄坏。
 *      1  SYS_EXIT     [DAY52 已实现，见下]
 *      2  SYS_WRITE    [DAY51 已实现，见下]
 *      3  SYS_GETABI   返回 ABI 版本号（LUMA_SYSCALL_ABI_VERSION）。程序可据此判断内核支持什么。
 *      4  SYS_STRERROR [DAY70] 错误号 -> 文字，见“错误号”一节。
 *
 * ===================================================================================
 *  DAY51 (Week 8, Day 51): "sys_write。"  验收："用户程序通过 syscall 输出。"
 * ===================================================================================
 *      int32_t write(int fd, const void *buf, uint32_t len)      eax=2, ebx=fd, ecx=buf, edx=len
 *
 *   fd 1 = 标准输出（程序所在终端的默认色），fd 2 = 标准错误（同一终端，红色）；fd 0 和其它值
 *   -> -EBADF（没有标准输入可写）。返回写出的字节数；出错返回 -errno。
 *   这是第一个带指针的系统调用，所以“用户指针”的规矩从这里开始立：
 *     0. [DAY54] 除了落在用户地址区，范围里的每一页在当前页目录里还必须真的已映射
 *        （luma_user_range_ok 现在会查页表）：程序的地址空间只映射了它的段和栈，用户区里
 *        其它地址是“不存在”的，传给 write 一律 -EFAULT，不会把内核拖进缺页 panic。
 *     1. 参数是不可信的整数。先验证 [buf, buf+len) 整块都落在“用户地址区”
 *        [LUMA_USER_LO, LUMA_USER_HI) 之内（今天 = ELF 装载区 16 MB ~ 240 MB，里面有程序的段
 *        和 64KB 用户栈），整块不在里面（包括 buf 为 NULL、指向内核、buf+len 回绕、跨过上界）
 *        一律 -EFAULT，且一个字节也不输出（不会写一半）。检查在 syscall.c 的 luma_user_range_ok()，
 *        算法避免 addr+len 的 32 位溢出（先比 len <= HI-addr）。
 *     2. 不直接读用户内存，而是按块复制到内核缓冲区（copy_from_user，每块 128 字节）再输出。
 *        今天程序与内核同一地址空间，这一步看似多余；Day 54 起用户地址空间独立，内核必须
 *        通过它去读用户页，才不会被一个坏指针搞崩——现在就把调用点都写成这个形状。
 *     3. 一次最多写 LUMA_WRITE_MAX = 4096 字节，超出的部分不写，返回 4096（“短写”，与
 *        POSIX write 一致：调用者循环调用写完剩下的）。len == 0 返回 0，不检查 buf。
 *     4. 输出是按字节的：'\n' 换行、'\t' 对齐、'\b' 退格由终端处理；NUL 字节被丢弃（终端
 *        没有对应的格子），但仍计入返回的字节数。
 *   输出落到启动程序的那个终端（elf_run 的 out 参数），所以 shell 的 `run` 对系统调用输出
 *   与对 api->puts 一样“补换行/报告退出码”。当前没有程序在运行（例如内核自己 `int 0x80`）
 *   -> -EBADF：没有可写的标准输出。
 *   检查顺序固定为 fd -> len==0 -> 用户指针 -> 是否有程序，所以同一个错误输入永远得到同一个错误号。
 *
 * ===================================================================================
 *  DAY52 (Week 8, Day 52): "sys_exit。"  验收："程序可以安全结束。"
 * ===================================================================================
 *      void exit(int code)                  eax=1, ebx=code            永不返回
 *
 *   程序执行 `int 0x80`（eax=1）后，内核结束它并回到 shell；code（ebx，32 位有符号）成为
 *   退出码，`run` 照旧报告非 0 的退出码。这样程序不再依赖“入口函数 ret”或 Day 46 的
 *   api->exit 函数表：三条出口（ret / api->exit / SYS_EXIT）最后都落进同一个出口
 *   elf_leave（elf_enter.asm），elf_run_info_t.via_exit = 0 / 1 / 2 记录走了哪条。
 *
 *   “安全”体现在哪：
 *     1. 不管程序在多深的函数里、栈上压了多少东西、甚至在什么寄存器值下调用，
 *        出口只做一件事：esp 一步切回 elf_enter 保存的内核栈，并按保存的现场恢复全部寄存器、
 *        段寄存器和 eflags。程序留下的那一整叠栈（连同 `int 0x80` 的中断帧）原样作废，
 *        不需要 iret，也不需要程序“配合”。
 *     2. 中断门进入时 CPU 清了 IF；出口的 popfd 把 eflags 恢复成运行前的值（IF=1），
 *        所以退出后键盘/定时器照常工作（测试：退出后 shell 仍能输入）。
 *     3. 只在“确有程序在运行”时才跳：elf_running 标志在 elf_enter 置 1、出口清 0。内核自己
 *        `int 0x80`（sysstat -t）或程序已结束后的迟到调用 -> -ESRCH(3)，什么也不跳，
 *        否则 elf_saved_esp 是旧值，跳回去等于跳进垃圾。
 *     4. 退出后 elf_run 照常做收尾：清 g_run_out（程序不能再往终端写）、img->loaded=0、
 *        事后检查用户栈护栏（SYS_EXIT 之前栈溢出同样会被报告）。
 *   SYS_EXIT 本身不产生输出；调用号、参数、调用者地址在跳走之前就记入 sysstat 的计数，
 *   这次调用的 last_ret 记的是退出码（它不“返回”）。
 *
 *   同样今天为止没有的东西：程序死循环或崩溃时内核仍会陪着挂（Day 56 的用户态异常），
 *   没有 wait/父子进程（只有一个前台程序，退出码直接交给 elf_run 的调用者）。
 *
 * ===================================================================================
 *  今天的局限（后面几天逐个解决）
 * ===================================================================================
 *   - 程序仍在 Ring 0：`int 0x80` 同特权级进入，不换栈。内核处理函数因此运行在程序的用户栈上
 *     （这块 64KB 栈里多出约一百来字节 + C 函数的栈帧）。Day 55 降到 Ring 3 后，CPU 会按 TSS
 *     自动换成内核栈，这一点就没有了。
 *   - Day 46~49 的 luma_api_t 函数表还在；Day 51/52 把输出、退出搬到系统调用上之后，它才退役。
 *   - [Day 51 已解决] 用户指针检查与 copy_from_user 已随 sys_write 一起写好；今天的“用户地址区”
 *     只是一段地址范围（没有分页，不检查某个页是不是真的有内存），Day 54 之后才变成真正的页表检查。
 *   - 分发期间 IF=0（中断门）：键盘/鼠标/定时器不会在系统调用中途打断它。调用耗时长的操作（读盘）
 *     以后再考虑 sti 或换成陷阱门。
 */
#ifndef LUMA_SYSCALL_H
#define LUMA_SYSCALL_H

#include <stdint.h>

/* ---- 入口 ---- */
#define LUMA_SYSCALL_VECTOR       0x80u
#define LUMA_SYSCALL_ABI_VERSION  1

/* IDT 门的 type_attr：Present | DPL=3 | 32 位中断门 */
#define LUMA_SYSCALL_GATE_ATTR    0xEEu

/* ---- 调用号 ---- */
#define SYS_NOP      0
#define SYS_EXIT     1      /* DAY52: void exit(int code)，永不返回 */
#define SYS_WRITE    2      /* DAY51: ssize_t write(int fd, const void *buf, size_t len) */
#define SYS_GETABI   3
/* SYS_STRERROR = 4 见下方“错误号”一节（DAY70） */
#define LUMA_SYSCALL_MAX  32   /* 调用号 >= 它一律 -ENOSYS；表在 syscall.c */

/* ---- DAY51: sys_write ---- */
#define LUMA_STDIN     0
#define LUMA_STDOUT    1
#define LUMA_STDERR    2
#define LUMA_WRITE_MAX 4096u         /* 一次最多写这么多字节，超出的返回短写 */
#define LUMA_USER_LO   0x01000000u   /* 用户指针必须落在 [LO, HI) 内（= elf.h 的 ELF_LOAD_MIN/MAX_ADDR） */
#define LUMA_USER_HI   0x0F000000u

/* ---- 错误号（返回 -LUMA_Exxx；数值沿用 Linux，方便对照资料） ----
 * DAY70: 以前这里只有 5 个号（ESRCH/EBADF/EFAULT/EINVAL/ENOSYS）。现在全系统统一一张表，
 * 在 kernel/core/errno.h（不依赖任何内核头文件，用户程序也可以直接包含）；这里 include 进来，
 * 原来的名字和数值一个都没变，所以老代码照常编译。 */
#include "../core/errno.h"

/* DAY70: 4  SYS_STRERROR(errnum, buf, len)：把错误号 errnum（带不带符号都行）的英文描述写进用户缓冲区 buf，
 *        最多 len 字节（含结尾 NUL）。返回 描述的字节数（不含 NUL）；
 *        len == 0 -> -EINVAL；buf 不在用户可写范围 -> -EFAULT；
 *        缓冲区放不下 -> 写入被截断的、仍以 NUL 结尾的前 len-1 字节，并返回 -ERANGE。
 *        没登记的错误号得到 "Unknown error"（不是错误）。 */
#define SYS_STRERROR 4

/* ===================================================================================
 *  DAY73: SYS_BRK(6) —— 用户堆。  int32 brk(uint32 addr)      eax=6, ebx=addr
 * ===================================================================================
 *   程序没有堆：它的地址空间只有 ELF 段 + 64KB 栈（Day 54）。用户态 malloc 需要“向内核要更多页”，
 *   这就是 brk（“program break”，程序数据段的末端）。堆区 = [heap_lo, heap_hi)：
 *     heap_lo = 最高 PT_LOAD 段末尾向上取整到页；heap_hi = min(heap_lo + 8MB, 栈护栏页下沿 / 用户区上界)。
 *   addr == 0：不改变任何东西，返回当前 break（Linux 的 brk(0) 习惯）。
 *   addr 在 [heap_lo, heap_hi]：把 break 移到 addr（增长 = 内核给新增的整页映射新页框，已清零；
 *     缩小 = 多出的整页还给页框池）。成功返回新 break（= addr）。
 *   addr < heap_lo        -> -EINVAL（不能把堆缩到程序自己的段里）
 *   addr > heap_hi、页框池用完 -> -ENOMEM，break 不变（增长一半失败会回滚）。
 *   没有程序在运行、或这个地址空间没有堆 -> -ENOMEM / -ESRCH，什么也不改。
 *   返回值是地址（< 0x0F000000），不会和 [-4095,-1] 的错误区间混淆。
 *   程序退出时 aspace_destroy 回收所有页，所以堆不会泄漏给下一个程序。
 *   libluma 的 sbrk()/malloc() 建立在它上面（user/lib/malloc.c）。 */
#define SYS_BRK      6
#define LUMA_HEAP_MAX 0x00800000u

/* ===================================================================================
 *  DAY74: 文件系统调用（fd 表见 fd.h）
 * ===================================================================================
 *   7  SYS_OPEN   int open(const char *path, int flags)             ebx=path, ecx=flags（今天只有 O_RDONLY=0）
 *                 成功返回 fd（>= 3）。-ENOENT 不存在/空路径，-EISDIR 是目录，-EMFILE 表满，-ENAMETOOLONG 路径 >= 160，
 *                 -EFAULT 路径指针坏，-ENODEV 卷不存在，-ENOSYS 想写/创建。
 *   8  SYS_READ   ssize_t read(int fd, void *buf, size_t len)       ebx=fd, ecx=buf, edx=len
 *                 返回读到的字节数；0 = 文件尾；一次最多 65536 字节（短读）。fd 不对（含 0/1/2）-EBADF；buf 不是整块
 *                 可写的用户内存 -EFAULT（此时不读、不移动位置）；底层读盘失败 -EIO。
 *   9  SYS_CLOSE  int close(int fd)                                 成功 0；fd 不对（含 0/1/2、重复 close）-EBADF
 *  10  SYS_LSEEK  off_t lseek(int fd, int off, int whence)          返回新的偏移；whence = SEEK_SET 0 / CUR 1 / END 2；
 *                 结果 < 0 或 > 2GB-1 -> -EINVAL，非法 whence -EINVAL；越过文件尾合法（之后 read 返回 0）。
 *   为什么不是 5：5 一直留作“未分配调用号”（Day 50 的测试靠它验证 -ENOSYS）。
 *   程序结束时内核关闭它忘记关的所有文件。 */
#define SYS_OPEN     7
#define SYS_READ     8
#define SYS_CLOSE    9
#define SYS_LSEEK   10

/* ===================================================================================
 *  DAY75: 目录与状态（kernel/syscall/fsops.c）——让 ls / explorer 这类程序能“看”文件系统
 * ===================================================================================
 *  11 SYS_READDIR  int readdir(const char *path, uint32_t index, luma_dirent_t *out)   ebx=path, ecx=index, edx=out
 *       把 path 这个目录的第 index 个条目（0 起）写进 *out，返回 1；index 已越过最后一项返回 0（循环结束的信号）；
 *       出错 -errno（-ENOENT 不存在，-ENOTDIR 是文件，-ENODEV 卷不存在，-EFAULT 指针坏）。
 *       "." 和 ".." 不算条目（内核跳过，和 shell 的 ls 一致），所以 index 数的是“看得见的”条目。
 *       path == ""（空串）是特殊的：列出“所有已挂载的卷”（RAM0 CD0 IDE0 ...），类型都是目录。
 *       每次调用都重新解析路径，所以循环 readdir(path, 0..n) 是 O(n^2)——目录只有几十到几百项，可接受；
 *       以后若要 O(n)，可以像 open 一样给“目录 fd”。
 *  12 SYS_STAT     int stat(const char *path, luma_stat_t *out)                       ebx=path, ecx=out
 *       成功 0，写入类型和大小（目录大小 0）。-ENOENT / -ENODEV / -EFAULT / -ENAMETOOLONG。
 *  13 SYS_GETCWD   int getcwd(char *buf, size_t size)                                 ebx=buf, ecx=size
 *       把当前目录写成 "卷/子目录"（如 "IDE3/" 或 "RAM0/docs"），返回长度（不含 NUL）；放不下 -ERANGE。
 *       当前目录是 shell 的（`cd` 设置的），程序不能改它（没有 chdir：还没必要）。
 * 路径解析与 open 完全相同（相对路径按 cwd，"IDE3/x" 全名）。 */
#define SYS_READDIR 11
#define SYS_STAT    12
#define SYS_GETCWD  13

/* ---- DAY76: execve ----
 *  14 SYS_EXECVE   int execv(const char *path, char *const argv[])                 ebx=path, ecx=argv
 *      用 path 指向的 .sw 程序替换当前程序（同一个“进程”：同一个终端、同一个退出出口）。
 *      argv 是以 NULL 结尾的指针数组（最多 32 个、字符串合计 2048 字节）；ecx==0 等价于 argc=0。
 *      成功：不返回——新程序从入口开始运行，它的退出码就是整个 run 的退出码。旧程序的地址空间、堆、栈全部销毁，
 *            打开的文件全部关闭（今天没有“跨 exec 保留 fd”）。
 *      失败：返回 -errno，当前程序原样继续（失败发生在“不可回头”之前）：
 *            -EFAULT 坏指针；-ENAMETOOLONG；-ENOENT 找不到；-EISDIR；-ENOEXEC 不是合法 ELF；
 *            -E2BIG 参数太多/太长；-ENOMEM 页框不够；-EIO 读盘失败。 */
#define SYS_EXECVE  14

/* ---- DAY77: 进程（kernel/proc/proc.c，设计见 proc.h） ----
 *  15 SYS_FORK     int fork(void)          父进程得到子 pid，子进程得到 0；复制整个地址空间和 fd 表。-EAGAIN 进程表满 / -ENOMEM
 *  16 SYS_WAIT     int wait(int *status)   等任意一个子进程结束，返回它的 pid；status（可为 NULL）= 退出码<<8（被杀死则低 7 位非 0）。
 *                                          没有子进程 -ECHILD；status 是坏指针 -EFAULT
 *  17 SYS_GETPID   int getpid(void)        没 fork 过的程序是 1
 *  18 SYS_YIELD    int yield(void)         把 CPU 让给别的可运行进程（今天没有时钟抢占，进程只在 wait/yield/exit 时切换）
 *  exit 的语义不变；父进程先退出时，它还活着的子孙会先跑完再回到 shell。 */
#define SYS_FORK    15
#define SYS_WAIT    16
#define SYS_GETPID  17
#define SYS_YIELD   18

/* ---- DAY77a: 标准输入 ----
 *  read(0, buf, n) 读键盘（SYS_READ 的 fd 0，见 stdin.h：行编辑/原始两种模式）。
 *  19 SYS_TERMMODE  int termmode(int mode)   0=COOKED（默认）1=RAW 2=RAW_NB；返回旧模式，-EINVAL。每个程序开始时都是 COOKED。 */
#define SYS_TERMMODE 19

/* ---- DAY77b: 写文件 ----
 *  open(path, O_WRONLY|O_RDWR [|O_CREAT|O_EXCL|O_TRUNC|O_APPEND])（SYS_OPEN 的 flags，见 fd.h）、write(fd>=3, ...)（SYS_WRITE）、
 *  close 写回（SYS_CLOSE 可能返回 -ENOSPC/-EIO）、lseek 对写 fd 同样可用。
 *  20 SYS_UNLINK   int unlink(const char *path)                      ebx=path。目录 -EISDIR，光盘 -EROFS
 *  21 SYS_MKDIR    int mkdir(const char *path)                       ebx=path。已存在 -EEXIST，FAT -ENOSYS
 *  22 SYS_RENAME   int rename(const char *old, const char *new)      ebx=old ecx=new。只支持文件，可跨卷；新路径已有文件则覆盖；非原子
 *  23 SYS_FSYNC    int fsync(int fd)                                 把写缓冲写回文件系统 */
#define SYS_UNLINK   20
#define SYS_MKDIR    21
#define SYS_RENAME   22
#define SYS_FSYNC    23
/* DAY77c: 24 SYS_RMDIR int rmdir(const char *path)   ebx=path。只删空目录：非空 -ENOTEMPTY，不是目录 -ENOTDIR，卷根 -EBUSY，光盘 -EROFS，FAT -ENOSYS */
#define SYS_RMDIR    24
/* DAY77c: 环境变量（设计见 kernel/env/env.h；全系统共享一份，程序改了 shell 立刻看到）
 *  25 SYS_GETENV   int getenv(const char *name, char *buf, size_t size)   ebx=name ecx=buf edx=size；返回值的长度；没有 -ENOENT；buf 小 -ERANGE
 *  26 SYS_SETENV   int setenv(const char *name, const char *value)        名字非法 -EINVAL，值太长 -E2BIG，表满 -ENOSPC
 *  27 SYS_UNSETENV int unsetenv(const char *name)                         不存在也返回 0
 *  28 SYS_ENVAT    int envat(unsigned i, char *buf, size_t size)          把第 i 个变量以 "NAME=VALUE" 写进 buf，返回长度；i 越界 -ENOENT */
#define SYS_GETENV   25
#define SYS_SETENV   26
#define SYS_UNSETENV 27
#define SYS_ENVAT    28

#define LUMA_DT_FILE 1
#define LUMA_DT_DIR  2
typedef struct { uint32_t size; uint32_t type; char name[32]; } luma_dirent_t;   /* name 与 vfs_node_t.name 一样长 */
typedef struct { uint32_t size; uint32_t type; } luma_stat_t;    /* 每个程序的堆最大 8MB（也受页框池 8MB 总量限制）*/

/* ---- 程序一侧的调用封装（内核自测、测试程序都用它）----
 * 程序是 -fno-pic 的静态可执行文件，ebx 可以直接做输入约束。 */
static inline int32_t luma_syscall0(uint32_t nr)
{
    int32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(nr) : "memory");
    return r;
}
static inline void luma_exit(int code)
{
    __asm__ volatile ("int $0x80" : : "a"(1), "b"(code) : "memory");
    for (;;) { }                    /* 永不返回（内核无程序在跑时才会返回，调用者不该这样用） */
}
static inline int32_t luma_write(int fd, const void *buf, uint32_t len)
{
    int32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(2), "b"(fd), "c"(buf), "d"(len) : "memory");
    return r;
}
static inline int32_t luma_syscall3(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3)
{
    int32_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(nr), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return r;
}

#ifdef LUMA_KERNEL_SIDE
/* ---- 以下只给内核用 ---- */

/* 最近一次系统调用的记录 + 总数（sysstat 命令显示；测试靠它证明“确实进过内核”）。 */
typedef struct {
    uint32_t calls;          /* 进入 syscall_dispatch 的总次数 */
    uint32_t bad_calls;      /* 其中返回 -ENOSYS 的次数 */
    uint32_t last_nr;        /* 最近一次的调用号 */
    int32_t  last_ret;       /* 最近一次的返回值 */
    uint32_t last_args[6];   /* 最近一次的 6 个参数 */
    uint32_t last_eip;       /* 调用者执行 `int 0x80` 的下一条指令地址 */
    uint32_t last_cs;        /* 调用者的代码段（今天 0x08；Ring 3 时低 2 位 = 3） */
} syscall_stats_t;

typedef int32_t (*syscall_fn_t)(uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6);

struct isr_context;
void syscall_dispatch(struct isr_context *ctx);       /* 由 isr_handler 在向量 0x80 时调用 */
const syscall_stats_t *syscall_get_stats(void);
void syscall_reset_stats(void);

/* DAY51: 用户指针检查。[addr, addr+len) 整块都在用户地址区内返回 1，否则 0（len==0 视为通过）。 */
int luma_user_range_ok(uint32_t addr, uint32_t len);
/* DAY51: 把用户内存 [src, src+n) 复制到内核缓冲区 dst。范围检查失败返回 -LUMA_EFAULT 且不复制。 */
int luma_copy_from_user(void *dst, uint32_t src, uint32_t n);

/* 读 IDTR/IDT，取出 0x80 门的内容（诊断用）。 */
typedef struct { uint32_t handler; uint16_t selector; uint8_t type_attr; } syscall_gate_t;
void syscall_read_gate(syscall_gate_t *g);
#endif

#endif /* LUMA_SYSCALL_H */
