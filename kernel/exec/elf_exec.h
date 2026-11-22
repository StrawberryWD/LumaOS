/* kernel/exec/elf_exec.h
 * LumaOS 5.0 - elf_exec(): “按路径运行一个 .sw 程序”的完整流水线
 *
 * DAY48 (Week 7, Day 48): "Shell 集成 `run`。"  验收："`run hello.sw` 成功。"
 *
 * 原理见 elf.h 的 DAY48 注释。这是 Day 44~47 四个函数的“组合”：
 *     elf_load()           Day 44  打开 + 验证
 *     elf_load_segments()  Day 45  段装入 p_vaddr
 *     elf_run()            Day 46/47  在用户栈上跳到入口，统一出口回来
 * shell 的 `run` 命令只需要这一个调用；调试命令 elfinfo/elfload/elfmap/elfrun
 * 仍然各自只走其中一段（elfrun 因为要在“装入”与“运行”之间打印入口地址，保留自己的
 * 三步写法）。
 */
#ifndef LUMA_ELF_EXEC_H
#define LUMA_ELF_EXEC_H

#include <stdint.h>
#include "elf.h"
#include "elf_run.h"
#include "../gui/gui.h"

/* 失败发生在哪一步（成功时为 ELF_STAGE_DONE）。 */
#define ELF_STAGE_LOAD   1    /* elf_load() 失败：文件打不开 / 不是可运行的 ELF；什么都没动 */
#define ELF_STAGE_MAP    2    /* elf_load_segments() 失败：段没能装入内存；没有执行任何指令   */
#define ELF_STAGE_RUN    3    /* elf_run() 拒绝或跑完后发现问题（见 rc）                       */
#define ELF_STAGE_DONE   0

typedef struct {
    int            stage;     /* ELF_STAGE_*：成功为 ELF_STAGE_DONE，失败为出错的那一步       */
    int            rc;        /* ELF_OK 或 ELF_ERR_*（ELF_ERR_STACK_OVERFLOW 时程序其实已跑完）*/
    int            ran;       /* 1 = 程序真的执行过（含栈溢出这种“事后发现”的情况）           */
    int            code;      /* 程序的退出码（ran == 1 时有效）                              */
    uint32_t       entry;     /* 入口地址（load 成功之后有效）                                */
    elf_run_info_t info;      /* 运行报告（ran == 1 时有效）                                  */
    /* DAY54：程序的独立地址空间（建好之后有效；运行结束即销毁，这里只留统计）               */
    uint32_t       as_frames;     /* 一共占用的物理页框数（页目录+页表+数据页）               */
    uint32_t       as_pages;      /* 其中映射到用户区的数据页数（段 + 栈）                    */
    uint32_t       as_pd;         /* 页目录的物理地址                                         */
    uint32_t       as_entry_pa;   /* 入口地址在该地址空间里对应的物理地址                     */
    uint32_t       as_stack_pa;   /* 用户栈最低一页（ELF_USER_STACK_BASE）对应的物理页框       */
    uint32_t       as_stack_top_pa; /* 用户栈最高一页对应的物理页框                            */
} elf_exec_result_t;

/* DAY54: 装入之后、跳进程序之前的回调（elfrun 用它在程序输出前打印 "running ..."）。 */
typedef void (*elf_exec_hook_t)(void *ctx, const elf_image_t *img, const elf_exec_result_t *res);
int elf_exec_args_hook(const char *path, terminal_t *out, int argc, const char *const *argv,
                       elf_exec_result_t *res, elf_exec_hook_t hook, void *ctx);

/* elf_exec(path, out, res)
 *
 * path : 已经过 shell_resolve_path() 规范化的完整路径（"IDE3/hello.sw"、"IDE0/HELLO.SW"）。
 * out  : 程序输出到哪个终端。
 * res  : 输出（必须非 NULL）。
 * 返回 res->rc。内部用静态 elf_image_t：不可重入（今天没有任何会重入的场合）。 */
int elf_exec(const char *path, terminal_t *out, elf_exec_result_t *res);

/* elf_exec_args(path, out, argc, argv, res)                          [DAY49]
 * 同 elf_exec()，程序拿到 argc/argv（规则见 elf_run.h 的 elf_run_args）。
 * 参数不合格 -> res->stage == ELF_STAGE_RUN、res->rc == ELF_ERR_ARGS、ran == 0。 */
int elf_exec_args(const char *path, terminal_t *out, int argc, const char *const *argv,
                  elf_exec_result_t *res);

#endif /* LUMA_ELF_EXEC_H */
