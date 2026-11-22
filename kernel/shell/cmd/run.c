/* kernel/shell/cmd/run.c
 * LumaOS 5.0 — DAY48 (Week 7, Day 48): "Shell 集成 `run`。"
 *   验收："`run hello.sw` 成功。"
 *
 * 用法:  run <program.sw> [args...]   例如  run IDE3/hello.sw          （LMFS）
 *                                           run IDE0/HELLO.SW          （FAT）
 *                                           cd IDE3  然后  run hello.sw
 *                                           run hello.sw luma          （DAY49：带参数）
 *                                           run args.sw one "two words" ""
 *
 * DAY49 (Week 7, Day 49): "argc/argv。" 验收："`run hello.sw luma` 能读取参数。"
 *   程序名之后的词被切开，作为 argc/argv 交给程序（argv[0] 是用户键入的程序路径，
 *   argc >= 1）。切词规则：空格/Tab 分隔；双引号包住的部分是一个词（引号不进参数）；
 *   `""` 是空参数；无转义字符；引号不成对 -> "unterminated quote"，不运行。
 *   个数/总长上限见 elf.h 的 ELF_ARGS_MAX_*，超出 -> "too many or too long arguments"，不运行。
 *   为什么不用 shell 现成的 argv：那套每词最多 40 字符、最多 16 个、不认引号
 *   （Day 43 PRE-3 修 echo 时已经踩过），run 直接读原始命令行 shell_cmdline_args()。
 *   原理见 elf.h 的 DAY49 注释。
 *
 * 这是 Day 44~47 的“正式出口”：用户敲一个命令，系统按 Unix shell 的习惯运行程序——
 *   - 程序成功（退出码 0）：除了程序自己的输出，什么都不多说；
 *   - 程序以非 0 退出：多一行红色的 "run: <path> exited with code N"；
 *   - 程序最后一次输出没以换行结尾：run 补一个换行，让下一个提示符另起一行；
 *   - 出错：一行说明（原因太长就另起一行），并且明确哪一步出的错——文件有问题
 *     (没动任何东西)、内存有问题 (程序没跑)、栈溢出 (程序跑完了，事后才发现)。
 *
 * 与调试命令 elfrun 的区别：elfrun 是给内核开发者看的（打印入口地址、`-v` 栈报告、
 * 每一步的标题，对任何 ELF 都运行）；run 是给用户用的（安静、只运行 `.sw`）。
 * 运行本身都走同一个 elf_exec()，没有第二套实现。
 *
 * 只运行 .sw：计划.md 规定 LumaOS 的应用执行文件统一用 `.sw`；run 在碰磁盘之前先检查
 * 扩展名（不区分大小写：FAT 的 HELLO.SW 与 LMFS 的 hello.sw 都行），“把一个文本文件
 * 当程序运行”这种明显的误操作在第一步就被挡住，不去读它。注意这只是“防手滑”，不是
 * 安全边界——内容是不是合法 ELF 仍由 elf_load() 验证。
 *
 * 危险提示：今天程序仍与内核同在 Ring 0（Day 54/55 才有硬件隔离），死循环的程序会让
 * 系统停住，所以只运行自己信任的 .sw。每行 < 80 列。
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../exec/elf.h"
#include "../../exec/elf_exec.h"
#include "../../exec/elf_run.h"
#include "../../lib/string.h"
#include "../shell.h"

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

/* name 是否以 ".sw" 结尾（不区分大小写，且 ".sw" 前面至少还有一个字符）。 */
static int has_sw_ext(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) return 0;
    const char *e = name + n - 3;
    return e[0] == '.' && (e[1] == 's' || e[1] == 'S') && (e[2] == 'w' || e[2] == 'W');
}

/* DAY49: 把原始命令行切成词。
 *   line : 形如  hello.sw luma "two words" ""
 *   buf  : 输出，字符串依次存放（每个以 NUL 结尾）；words[i] 指向第 i 个词。
 * 返回词的个数 (>= 0)；-1 = 引号不成对；-2 = 词太多或总长超出 buf。
 * 规则：空格/Tab 分隔；双引号包住的部分是词的一部分（引号本身丢掉），
 * 所以 a"b c"d 是一个词 "ab cd"，"" 是一个空词。 */
static int tokenize(const char *line, char *buf, size_t buf_size,
                    const char **words, int max_words)
{
    int n = 0;
    size_t o = 0;
    const char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (n >= max_words) return -2;
        words[n++] = buf + o;
        int in_q = 0;
        while (*p && (in_q || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') { in_q = !in_q; p++; continue; }
            if (o + 1 >= buf_size) return -2;        /* 要给结尾的 NUL 留位置 */
            buf[o++] = *p++;
        }
        if (in_q) return -1;
        if (o >= buf_size) return -2;
        buf[o++] = '\0';
    }
    return n;
}

/* "run: <path>: <reason>"；总长超过 78 列就把原因另起一行。 */
static void err_line(terminal_t *t, const char *path, const char *reason)
{
    terminal_puts(t, "run: ", TERM_COL_ERROR);
    terminal_puts(t, path, TERM_COL_ERROR);
    if (strlen(path) + strlen(reason) + 8 > 78) {
        terminal_puts(t, ":", TERM_COL_ERROR);
        terminal_newline(t);
        terminal_puts(t, "  ", TERM_COL_ERROR);
    } else {
        terminal_puts(t, ": ", TERM_COL_ERROR);
    }
    terminal_puts(t, reason, TERM_COL_ERROR);
    terminal_newline(t);
}

void cmd_run(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: run <program.sw> [args...]  (e.g. run hello.sw luma)\n",
                      TERM_COL_ERROR);
        return;
    }

    /* DAY49: 切词。存放区比 elf.h 的总上限略大：切词只管“切得开”，
     * 个数/长度是否合格由交付者 elf_run_args() 判定。 */
    static char tok_buf[ELF_ARGS_MAX_BYTES + 64];
    static const char *words[ELF_ARGS_MAX_COUNT + 1];
    int nw = tokenize(shell_cmdline_args(), tok_buf, sizeof(tok_buf), words,
                      (int)ELF_ARGS_MAX_COUNT + 1);
    if (nw == -1) {
        terminal_puts(t, "run: unterminated quote\n", TERM_COL_ERROR);
        return;
    }
    if (nw < 0 || nw > (int)ELF_ARGS_MAX_COUNT) {
        terminal_puts(t, "run: too many or too long arguments\n", TERM_COL_ERROR);
        return;
    }
    if (nw == 0 || words[0][0] == '\0') {   /* 没有程序名（`run ""` 的空词也算没有） */
        terminal_puts(t, "usage: run <program.sw> [args...]\n", TERM_COL_ERROR);
        return;
    }
    const char *prog = words[0];

    /* 先看扩展名，再碰磁盘。取最后一个 '/' 之后的文件名。 */
    const char *base = strrchr(prog, '/');
    base = base ? base + 1 : prog;
    if (!has_sw_ext(base)) {
        err_line(t, prog, "not a .sw program");
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(prog, path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "run: no mount-slot given and no "
                         "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }

    static elf_exec_result_t res;
    elf_exec_args(path_buf, t, nw, words, &res);

    /* 程序真的跑过：先收拾输出（补换行），再报告退出码 / 栈溢出。 */
    if (res.ran) {
        if (res.info.out_total > 0 && res.info.out_last != '\n')
            terminal_newline(t);
        if (res.info.via_exit == ELF_VIA_KILLED) {          /* DAY56: 用户态异常 / Ctrl+C：内核杀掉了程序 */
            char why[128];
            elf_fault_describe(&res.info, why, sizeof(why));
            terminal_puts(t, "run: ", TERM_COL_ERROR);
            terminal_puts(t, path_buf, TERM_COL_ERROR);
            terminal_puts(t, " killed:", TERM_COL_ERROR);
            terminal_newline(t);
            terminal_puts(t, "  ", TERM_COL_ERROR);        /* 原因另起一行：最长约 70 列，与前缀同行会折行 */
            terminal_puts(t, why, TERM_COL_ERROR);
            terminal_newline(t);
            return;
        }
        if (res.code != 0) {
            terminal_puts(t, "run: ", TERM_COL_ERROR);
            terminal_puts(t, path_buf, TERM_COL_ERROR);
            terminal_puts(t, " exited with code ", TERM_COL_ERROR);
            put_sdec(t, res.code, TERM_COL_ERROR);
            terminal_newline(t);
        }
        if (res.rc == ELF_ERR_STACK_OVERFLOW)
            err_line(t, path_buf, elf_strerror(res.rc));
        return;
    }

    if (res.rc == ELF_OK)
        return;                      /* 不会发生：成功必然 ran == 1，保险起见 */

    /* 没有跑：按“死在哪一步”给出准确的话。 */
    err_line(t, path_buf, elf_strerror(res.rc));
    if (res.stage == ELF_STAGE_LOAD)
        return;                      /* 文件问题：说明原因就够了 */
    terminal_puts(t, "run: ", TERM_COL_ERROR);
    terminal_puts(t, "the program was not executed\n", TERM_COL_ERROR);
}
