/* kernel/shell/cmd/elfrun.c
 * LumaOS 5.0 — DAY46 (Week 7, Day 46): "实现 elf_run()。"
 *   验收："hello.sw 可以执行。"
 *
 * 用法:  elfrun [-v] <file> [args...]   例如  elfrun IDE3/hello.sw   （LMFS）  [DAY55: 可带参数]
 *                                elfrun IDE0/HELLO.SW   （FAT）
 *                                cd IDE3  然后  elfrun hello.sw
 *
 * DAY47: 程序现在在自己的 64 KB 用户栈上运行，可以 ret 也可以 api->exit() 退出；
 * `-v` 在结论行之后多打印一行运行报告：栈用了多少字节、是 ret 还是 api->exit
 * 退出的。不带 -v 时输出与 Day 46 完全一致。栈溢出（护栏被写）会在结论行之后
 * 报错。原理见 elf.h 的 DAY47 注释。
 *
 * 作用：把加载链从头到尾走一遍并真正执行——
 *     elf_load()           验证文件（Day 44）
 *     elf_load_segments()  把 PT_LOAD 段装进 p_vaddr，清零 .bss（Day 45）
 *     elf_run()            call e_entry，程序 ret 回来（今天）
 * 然后打印程序的退出码（入口函数返回时的 eax）。原理见 elf.h 的 DAY46 注释。
 *
 * 程序在运行期间通过 api->puts 打印的文字直接出现在本终端里，夹在
 * "elfrun: running ..." 与 "elfrun: ... exited with code N" 两行之间。
 *
 * 与 Day 48 的关系：`run` 命令（Day 48）会取代它成为正式入口；elfrun 保留
 * 作为“低层调试命令”，与 elfinfo / elfload / elfmap 并列——每个只做加载链
 * 中的一段。
 *
 * 危险提示：今天程序与内核同在 Ring 0。一个死循环的程序会让整个系统停住
 * （Day 47 的“退出路径”、Day 56 的“用户态异常”才逐步解决），所以只运行
 * 自己信任的 .sw。每行 < 80 列。
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../exec/elf.h"
#include "../../exec/elf_loader.h"
#include "../../exec/elf_run.h"
#include "../../exec/elf_exec.h"
#include "../../lib/string.h"
#include "../shell.h"

static void put_hex8(terminal_t *t, uint32_t v, unsigned char color)
{
    static const char hexd[] = "0123456789abcdef";
    terminal_puts(t, "0x", color);
    for (int i = 7; i >= 0; i--)
        terminal_putchar(t, hexd[(v >> (i * 4)) & 0xF], color);
}

/* 有符号十进制：退出码可以是负数（例如 -1）。 */
static void put_sdec(terminal_t *t, int v, unsigned char color)
{
    char tmp[12];
    int n = 0;
    uint32_t u = (v < 0) ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    if (v < 0) terminal_putchar(t, '-', color);
    while (n) terminal_putchar(t, tmp[--n], color);
}

static void fail_line(terminal_t *t, const char *stage, const char *path, int rc)
{
    terminal_puts(t, "elfrun: ", TERM_COL_ERROR);
    terminal_puts(t, stage, TERM_COL_ERROR);
    terminal_puts(t, " ", TERM_COL_ERROR);
    terminal_puts(t, path, TERM_COL_ERROR);
    terminal_puts(t, ":", TERM_COL_ERROR);
    terminal_newline(t);
    /* 原因另起一行：最长的原因文字约 60 列，与前缀同行会超过 80 列而折行。 */
    terminal_puts(t, "  ", TERM_COL_ERROR);
    terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
    terminal_newline(t);
}

struct hook_ctx { terminal_t *t; const char *path; int verbose; };

static void before_run(void *ctx, const elf_image_t *img, const elf_exec_result_t *res)
{
    struct hook_ctx *h = (struct hook_ctx *)ctx;
    terminal_puts(h->t, "elfrun: running ", TERM_COL_INFO);
    terminal_puts(h->t, h->path, TERM_COL_INFO);
    terminal_puts(h->t, " at entry ", TERM_COL_INFO);
    put_hex8(h->t, img->ehdr.e_entry, TERM_COL_INFO);
    terminal_newline(h->t);
    if (h->verbose) {
        terminal_puts(h->t, "elfrun: address space: ", TERM_COL_INFO);
        put_sdec(h->t, (int)res->as_pages, TERM_COL_INFO);
        terminal_puts(h->t, " pages, ", TERM_COL_INFO);
        put_sdec(h->t, (int)res->as_frames, TERM_COL_INFO);
        terminal_puts(h->t, " frames, pd ", TERM_COL_INFO);
        put_hex8(h->t, res->as_pd, TERM_COL_INFO);
        terminal_newline(h->t);
        terminal_puts(h->t, "elfrun: entry va ", TERM_COL_INFO);
        put_hex8(h->t, img->ehdr.e_entry, TERM_COL_INFO);
        terminal_puts(h->t, " -> pa ", TERM_COL_INFO);
        put_hex8(h->t, res->as_entry_pa, TERM_COL_INFO);
        terminal_newline(h->t);
        terminal_puts(h->t, "elfrun: stack pages pa ", TERM_COL_INFO);
        put_hex8(h->t, res->as_stack_pa, TERM_COL_INFO);
        terminal_puts(h->t, " .. ", TERM_COL_INFO);
        put_hex8(h->t, res->as_stack_top_pa, TERM_COL_INFO);
        terminal_newline(h->t);
    }
}

void cmd_elfrun(terminal_t *t, int argc, char **argv)
{
    int verbose = 0;
    int ai = 1;
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        verbose = 1;
        ai = 2;
    }
    if (argc <= ai) {
        terminal_puts(t, "usage: elfrun [-v] <file>  (e.g. elfrun IDE3/hello.sw)\n",
                      TERM_COL_ERROR);
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(argv[ai], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "elfrun: no mount-slot given and no "
                         "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }

    /* ---- DAY54: 验证 -> 建独立地址空间 -> 装入 -> 运行 -> 销毁，全由 elf_exec 完成。
     * 回调在装入之后、程序第一条指令之前打印 "running ..."（与 Day 46 的输出顺序一致）。 */
    struct hook_ctx hc = { t, path_buf, verbose };
    elf_exec_result_t res;
    /* DAY55: elfrun <file> [args...] 也可以带参数（和 `run` 一样），方便调试要看 argv 的程序。 */
    int pargc = (argc - ai > 1) ? argc - ai : 0;   /* 带参数时 argv[0] = 程序名（和 run 一致）；不带参数时 argc = 0，与 Day 47 完全相同 */
    int rc = elf_exec_args_hook(path_buf, t, pargc, (const char *const *)&argv[ai],
                                &res, before_run, &hc);
    int code = res.code;
    elf_run_info_t info = res.info;
    if (rc != ELF_OK && rc != ELF_ERR_STACK_OVERFLOW) {
        const char *stage = res.stage == ELF_STAGE_LOAD ? "cannot load"
                          : res.stage == ELF_STAGE_MAP  ? "cannot map" : "cannot run";
        fail_line(t, stage, path_buf, rc);
        if (res.stage != ELF_STAGE_RUN)
            terminal_puts(t, "elfrun: nothing was executed\n", TERM_COL_ERROR);
        return;
    }

    /* DAY56: 被内核杀掉（用户态异常 / Ctrl+C）：不是“exited”，报告原因。 */
    if (info.via_exit == ELF_VIA_KILLED) {
        char why[128];
        elf_fault_describe(&info, why, sizeof(why));
        terminal_puts(t, "elfrun: ", TERM_COL_ERROR);
        terminal_puts(t, path_buf, TERM_COL_ERROR);
        terminal_puts(t, " killed:", TERM_COL_ERROR);
        terminal_newline(t);
        terminal_puts(t, "  ", TERM_COL_ERROR);
        terminal_puts(t, why, TERM_COL_ERROR);
        terminal_newline(t);
        if (verbose) {
            terminal_puts(t, "elfrun: exit code ", TERM_COL_INFO);
            put_sdec(t, code, TERM_COL_INFO);
            terminal_puts(t, ", left via killed\n", TERM_COL_INFO);
        }
        return;
    }

    /* 约定：程序的最后一次输出以 '\n' 结尾，结论行才会独占一行。 */
    terminal_puts(t, "elfrun: ", code == 0 ? TERM_COL_SUCCESS : TERM_COL_ERROR);
    terminal_puts(t, path_buf, code == 0 ? TERM_COL_SUCCESS : TERM_COL_ERROR);
    terminal_puts(t, " exited with code ", code == 0 ? TERM_COL_SUCCESS : TERM_COL_ERROR);
    put_sdec(t, code, code == 0 ? TERM_COL_SUCCESS : TERM_COL_ERROR);
    terminal_newline(t);

    if (verbose) {
        terminal_puts(t, "elfrun: stack used ", TERM_COL_INFO);
        put_sdec(t, (int)info.stack_used, TERM_COL_INFO);
        terminal_puts(t, " of ", TERM_COL_INFO);
        put_sdec(t, (int)info.stack_size, TERM_COL_INFO);
        terminal_puts(t, " bytes (top ", TERM_COL_INFO);
        put_hex8(t, info.stack_top, TERM_COL_INFO);
        terminal_puts(t, "), left via ", TERM_COL_INFO);
        terminal_puts(t, info.via_exit == 2 ? "sys_exit\n" : info.via_exit ? "api->exit\n" : "ret\n", TERM_COL_INFO);
    }
    if (rc == ELF_ERR_STACK_OVERFLOW)
        fail_line(t, "after run", path_buf, rc);
}
