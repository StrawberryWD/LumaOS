/* kernel/exec/elf_run.h
 * LumaOS 5.0 - elf_run(): 跳到已装入内存的 ELF 入口点并执行
 *
 * DAY46 (Week 7, Day 46): "实现 elf_run()。"  验收："hello.sw 可以执行。"
 *
 * DAY47 (Week 7, Day 47): "增加用户态栈和退出路径。" 验收："程序执行后返回 Shell。"
 *   程序现在在自己的用户栈上运行（elf.h 的 ELF_USER_STACK_*），并有统一出口：
 *   入口函数 ret，或调用 api->exit(code)，都回到 elf_run()，再回到 shell。
 *
 * DAY49 (Week 7, Day 49): "argc/argv。" 验收："`run hello.sw luma` 能读取参数。"
 *   入口函数多了两个参数：int _start(const luma_api_t *api, int argc, char **argv)。
 *   参数字符串被复制进程序的用户栈顶（elf.h 的 DAY49 注释有完整布局）。
 *
 * 原理见 elf.h 的 DAY46 / DAY47 / DAY49 注释；汇编跳板见 elf_enter.asm。
 *
 * 加载 → 运行 的完整流程（今天第一次走通）：
 *     elf_load()           Day 44  打开 + 验证      -> elf_image_t（loaded=0）
 *     elf_load_segments()  Day 45  段装入 p_vaddr   -> loaded=1
 *     elf_run()            Day 46  跳到 e_entry     -> 程序 ret 回来，得到退出码
 */
#ifndef LUMA_ELF_RUN_H
#define LUMA_ELF_RUN_H

#include <stdint.h>
#include "elf.h"
#include "../gui/gui.h"

/* ---------------------------------------------------------------------
 * 临时的“内核服务表” luma_api_t（Day 46 ~ Day 50 的过渡品）
 * ---------------------------------------------------------------------
 * 程序跑起来之后总得“说点什么”才看得出它跑过。可系统调用（int 0x80，
 * Day 50~51）今天还没有，所以内核先把一张函数表的地址作为唯一参数传给
 * 程序入口：
 *       int _start(const luma_api_t *api);
 * 程序调用 api->puts("...") 就等于直接调用内核函数——这只有在程序和内核
 * 同处 Ring 0、共享地址空间的今天才成立（也因此程序可以随便写内核内存，
 * 今天没有任何保护；Day 55 才降到 Ring 3）。Day 50 之后这张表会整个消失，
 * 换成 int 0x80 + 系统调用号，程序源码里的 api->puts(s) 变成 sys_write()。
 *
 * 表的布局就是 ABI：新增字段只能加在末尾，version 同步 +1。
 * 所有函数都是 cdecl（参数从右到左压栈，调用者清栈，返回值在 eax）。 */
#define LUMA_API_MAGIC    0x4950414Cu    /* 'L','A','P','I'（小端）*/
#define LUMA_API_VERSION  2u               /* DAY47: 1 -> 2（末尾新增 exit）      */

typedef struct luma_api {
    uint32_t magic;                      /* LUMA_API_MAGIC：程序据此确认是 LumaOS 在调用它 */
    uint32_t version;                    /* LUMA_API_VERSION                               */
    void (*puts)(const char *s);         /* 向启动它的终端输出以 NUL 结尾的字符串（不自动换行） */
    void (*putchar)(int c);              /* 输出一个字符                                    */
    void (*exit)(int code);              /* DAY47（version 2）：立即结束程序，永不返回；
                                          * code 成为退出码。可在任意嵌套深度调用。        */
} luma_api_t;

/* DAY47: elf_run() 的可选“运行报告”。 */
typedef struct {
    uint32_t via_exit;      /* 0 = 入口函数 ret；1 = api->exit()；2 = SYS_EXIT 系统调用(Day 52) */
    uint32_t stack_top;     /* 用户栈顶（不含）                                  */
    uint32_t stack_size;    /* 用户栈大小                                        */
    uint32_t stack_used;    /* 栈最深用到了多少字节（栈顶到最低被改过的字节）    */
    uint32_t overflow;      /* 1 = 栈底护栏被改写（elf_run 同时返回 ELF_ERR_STACK_OVERFLOW）*/
    uint32_t out_total;     /* DAY48: 程序通过 api->puts/putchar 一共输出的字符数          */
    uint32_t out_last;      /* DAY48: 最后输出的那个字符（out_total == 0 时为 0）          */
    /* DAY56: via_exit == 3 表示程序被内核杀掉（用户态异常 / Ctrl+C），下面四项描述原因。退出码 = 128 + 向量
     * （和 Unix 里“被信号杀死”的 128+N 同一个习惯）；Ctrl+C 的向量记为 ELF_FAULT_CTRL_C，退出码 130。 */
    uint32_t fault_vector;  /* CPU 异常向量（0~31），或 ELF_FAULT_CTRL_C                    */
    uint32_t fault_err;     /* 异常的错误码（没有则 0）                                     */
    uint32_t fault_addr;    /* #PF：CR2（出错的地址）；其它：0                              */
    uint32_t fault_eip;     /* 出错指令的地址                                               */
} elf_run_info_t;

#define ELF_FAULT_CTRL_C  0x102u      /* 130 = 128 + 2（Unix 的 SIGINT）                    */
#define ELF_VIA_KILLED    3u

/* 内核在 Ring 3 异常/Ctrl+C 时填写，elf_run 复制到 elf_run_info_t。 */
typedef struct { uint32_t vector, err, addr, eip; } elf_fault_t;
extern elf_fault_t elf_fault_last;

/* DAY56: 把“被杀死”的原因写成一行英文（<80 列，不含换行）。buf 至少 128 字节。 */
void elf_fault_describe(const elf_run_info_t *info, char *buf, int n);

/* elf_run(img, out, exit_code, info)
 *
 * img       : elf_load() 验证、并由 elf_load_segments() 装入内存后的映像。
 *             必须 img->loaded == 1，否则返回 ELF_ERR_NOT_LOADED，一条指令也不执行。
 * out       : 程序调用 api->puts/putchar 时输出到哪个终端（不能为 NULL）。
 * exit_code : 可选输出（可传 NULL）：程序入口函数返回时的 eax，或 api->exit() 的参数。
 * info      : 可选输出（可传 NULL）：怎么退出的、栈用量、是否溢出（DAY47）。
 *
 * 做的事：
 *   1. 参数检查，loaded 必须为 1；
 *   2. 不信任 img：对其中的 Header + Program Header 重新校验一遍，并确认
 *      入口点仍落在可执行 PT_LOAD 的文件部分里（防止 img 在装载后被改过）；
 *   3. [Day 47] 准备用户栈：整块填 0xA5 并读回校验（无 RAM -> ELF_ERR_NO_STACK，
 *      此时一条指令都没执行）；
 *   4. 把内核现场存起来，切到用户栈，跳到 e_entry（elf_enter.asm）；程序 ret 或调用
 *      api->exit() 都从统一出口回来、恢复内核现场；
 *   5. [Day 47] 事后检查栈：算出最深用量，护栏被改写 -> ELF_ERR_STACK_OVERFLOW
 *      （此时 *exit_code 仍是程序给出的值，img->loaded 已清零）；
 *   6. 把 img->loaded 置 0：程序运行会修改自己的 .data/.bss，内存里的映像
 *      已经不是“刚装载好的样子”，再跑一次必须重新 elf_load_segments()。
 * 返回 ELF_OK（程序返回了，*exit_code 是它的退出码）或 ELF_ERR_*。
 *
 * 局限（后续几天逐个解决）：程序若不返回（死循环）或崩溃，内核会跟着挂——
 * Day 52 sys_exit，Day 56 用户态异常；栈只能事后检查，Day 54 分页后才有硬件保护。 */
int elf_run(elf_image_t *img, terminal_t *out, int *exit_code, elf_run_info_t *info);

/* elf_run_args(img, out, argc, argv, exit_code, info)                [DAY49]
 *
 * 与 elf_run() 相同，但程序拿到 argc/argv：
 *   argc, argv : argv[0..argc-1] 是 NUL 结尾的字符串（不能为 NULL 指针）。个数 <=
 *                ELF_ARGS_MAX_COUNT、字符串总字节数（含各自的 NUL）<= ELF_ARGS_MAX_BYTES，
 *                否则返回 ELF_ERR_ARGS（程序没跑）。argc == 0 时 argv 可为 NULL。
 * 字符串被复制进用户栈，程序看到的 argv 是栈里那份拷贝，argv[argc] == NULL。
 * 其余语义（返回值、info、loaded 清零……）同 elf_run()；elf_run() 就是 argc = 0 的特例。 */
int elf_run_args(elf_image_t *img, terminal_t *out, int argc, const char *const *argv,
                 int *exit_code, elf_run_info_t *info);

/* elf_run_write(buf, n, to_stderr)                                    [DAY51]
 * SYS_WRITE 的输出后端（kernel/syscall/syscall.c 调用）：把 buf[0..n) 打印到“正在运行的程序”的终端；
 * to_stderr != 0 时用错误色（红）。NUL 字节被丢弃。buf 必须已经由调用者复制到内核内存。
 * 返回 0；当前没有程序在运行（g_run_out 为空）返回 -1。输出同样计入 elf_run_info_t 的
 * out_total/out_last，所以 `run` 的“补换行”对系统调用输出同样有效。 */
int elf_run_write(const char *buf, uint32_t n, int to_stderr);

#endif /* LUMA_ELF_RUN_H */
