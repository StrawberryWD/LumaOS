/* kernel/exec/elf_run.c
 * LumaOS 5.0 - elf_run(): 跳到已装入内存的 ELF 入口点并执行
 *
 * DAY46 (Week 7, Day 46): "实现 elf_run()。"  验收："hello.sw 可以执行。"
 * DAY47: 用户栈 + 统一退出路径 + 栈事后检查。
 * DAY49: argc/argv —— 把命令行参数复制进用户栈顶，作为入口函数的第 2、3 个参数。
 * 原理见 elf.h 的 DAY46 / DAY47 注释，接口见 elf_run.h，跳板见 elf_enter.asm。
 */
#include "elf_run.h"
#include "../lib/string.h"
#include "urt.h"
extern void proc_on_exit(int code, int via);        /* DAY77 */

/* 汇编跳板：存现场 -> 切到用户栈 -> 跳到 entry(api) -> 统一出口 -> 恢复现场，
 * 返回程序的退出码。elf_api_exit 是给程序的 api->exit；elf_exit_via 记录走了哪条路。 */
extern void     elf_reenter(uint32_t entry, const void *api, uint32_t frame_top, int argc, char **argv);   /* DAY76 */
extern int      elf_enter(uint32_t entry, const void *api, uint32_t frame_top,
                          int argc, char **argv);
extern uint32_t elf_exit_via;      /* 0 = ret, 1 = api->exit, 2 = SYS_EXIT (Day 52) */
extern uint32_t elf_ret_stub_va;
extern void     elf_sys_exit(int code, int via);   /* DAY55: 入口函数 ret 的返回地址（运行时页里的 stub） */

/* ---- 给程序用的“内核服务”（luma_api_t 的实现） ---- */

/* 当前正在运行的程序把输出送到哪个终端。elf_run() 进入前设置、返回后清空。 */
static terminal_t *g_run_out;

/* 一次 puts 最多输出多少字符：程序给的指针指向哪、有没有 NUL 都不可信，
 * 设个上限，免得一个忘了写 '\0' 的字符串把整个内存当文字刷屏到天荒地老。 */
#define API_PUTS_MAX  4096

/* DAY48: 程序一共输出了多少字符、最后一个是什么。`run` 靠它决定要不要补一个换行：
 * 程序最后一次输出不以 '\n' 结尾时，shell 的下一个提示符会接在同一行末尾。 */
static uint32_t g_out_total;
static int      g_out_last;

/* DAY51: the one place program output enters the terminal. Both api->putchar/puts (Day 46) and the
 * SYS_WRITE system call (Day 51) end up here, so the counters `run` uses for its newline fix-up
 * (out_total / out_last) see every byte whichever way the program printed it. */
static void out_char(int c, unsigned char color)
{
    if (g_run_out) {
        terminal_putchar(g_run_out, (char)c, color);
        g_out_total++;
        g_out_last = (unsigned char)c;
    }
}

/* DAY51: SYS_WRITE backend. Returns 0, or -1 when no program is running (nobody to print for). */
int elf_run_write(const char *buf, uint32_t n, int to_stderr)
{
    if (!g_run_out)
        return -1;
    unsigned char col = to_stderr ? TERM_COL_ERROR : TERM_COL_DEFAULT;
    for (uint32_t i = 0; i < n; i++)
        if (buf[i] != '\0')              /* a terminal has no cell for NUL: dropped (still counted as written) */
            out_char((unsigned char)buf[i], col);
    return 0;
}

/* DAY55: api 表不再是内核里的一张函数表——程序在 Ring 3，不能调用内核函数。表和它的函数在每个程序
 * 地址空间的“运行时页”里（urt.h / urt.asm），函数是 Ring 3 stub，靠 int 0x80 的 SYS_WRITE/SYS_EXIT 说话。
 * API_PUTS_MAX（4096）这个上限现在由 stub 里的长度循环实现。 */

/* ---- DAY47: 用户栈的准备与事后检查 ---- */

/* 把 [BASE, TOP) 整块填成 ELF_STACK_FILL 并逐字节读回。成功返回 1。
 * 目标内存用 volatile 访问（与 Day 45 同理：防止编译器认为“刚写进去的当然读得回来”）。
 * 局限与 Day 45 相同：QEMU 里不存在的地址写入被丢弃、读回 0x00，会被发现。 */
static int stack_prepare(void)
{
    volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)ELF_USER_STACK_BASE;
    for (uint32_t i = 0; i < ELF_USER_STACK_SIZE; i++) {
        p[i] = ELF_STACK_FILL;
        if (p[i] != ELF_STACK_FILL)
            return 0;
    }
    return 1;
}

/* 运行后：从栈底往上找第一个不再是填充值的字节 -> 栈最深用到哪；
 * 最低 ELF_STACK_GUARD 字节（护栏）里只要有被改过的就算溢出。 */
static void stack_inspect(elf_run_info_t *r)
{
    volatile const unsigned char *p = (volatile const unsigned char *)(uintptr_t)ELF_USER_STACK_BASE;
    uint32_t first = ELF_USER_STACK_SIZE;          /* 没找到 = 一个字节都没用 */
    for (uint32_t i = 0; i < ELF_USER_STACK_SIZE; i++) {
        if (p[i] != ELF_STACK_FILL) { first = i; break; }
    }
    r->stack_top  = ELF_USER_STACK_TOP;
    r->stack_size = ELF_USER_STACK_SIZE;
    r->stack_used = ELF_USER_STACK_SIZE - first;
    r->overflow   = (first < ELF_STACK_GUARD) ? 1u : 0u;
}

/* ---- DAY49: 把 argc/argv 复制进用户栈顶 ----
 *
 * 先检查（个数、总字节数、有没有 NULL 指针），全部通过才写内存——不合格就不写、
 * 不跑。布局见 elf.h 的 DAY49 注释。返回 ELF_OK，并给出：
 *   *frame_top : 16 字节对齐的帧顶 F（入口帧画在 F-20 .. F）
 *   *argv_out  : 传给程序的 argv（用户栈里的地址）
 * argc == 0 时不放任何东西：F = TOP，argv 指向 TOP-4 处的 NULL（与 Day 47 的帧位置相同）。
 * 目标内存用 volatile 访问，原因同 stack_prepare()。 */
static int args_install(int argc, const char *const *argv,
                        uint32_t *frame_top, uint32_t *argv_out)
{
    volatile uint32_t *spare = (volatile uint32_t *)(uintptr_t)(ELF_USER_STACK_TOP - 4);

    if (argc < 0 || (uint32_t)argc > ELF_ARGS_MAX_COUNT)
        return ELF_ERR_ARGS;
    if (argc > 0 && argv == (const char *const *)0)
        return ELF_ERR_NULL;

    uint32_t total = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i] == (const char *)0)
            return ELF_ERR_ARGS;
        uint32_t n = (uint32_t)strlen(argv[i]) + 1;      /* 含结尾的 NUL */
        if (n > ELF_ARGS_MAX_BYTES || total > ELF_ARGS_MAX_BYTES - n)
            return ELF_ERR_ARGS;
        total += n;
    }

    *spare = 0;                              /* [TOP-4] = NULL：argc==0 时 argv 指向它 */
    if (argc == 0) {
        *frame_top = ELF_USER_STACK_TOP;
        *argv_out  = ELF_USER_STACK_TOP - 4;
        return ELF_OK;
    }

    /* 字符串从栈顶往下依次放；每放一个记下它在用户栈里的地址。 */
    uint32_t sp = ELF_USER_STACK_TOP;
    uint32_t addr[ELF_ARGS_MAX_COUNT];
    for (int i = argc - 1; i >= 0; i--) {                /* 倒着放：argv[0] 的字符串在最低处 */
        uint32_t n = (uint32_t)strlen(argv[i]) + 1;
        sp -= n;
        volatile char *d = (volatile char *)(uintptr_t)sp;
        for (uint32_t k = 0; k < n; k++)
            d[k] = argv[i][k];                           /* k == n-1 时拷的就是 NUL */
        addr[i] = sp;
    }

    /* 指针数组：argc 个指针 + 结尾的 NULL，4 字节对齐，放在字符串下面。 */
    sp &= ~3u;
    sp -= 4u * ((uint32_t)argc + 1u);
    volatile uint32_t *arr = (volatile uint32_t *)(uintptr_t)sp;
    for (int i = 0; i < argc; i++)
        arr[i] = addr[i];
    arr[argc] = 0;
    *argv_out = sp;

    /* 帧顶：向下取 16 字节对齐。入口帧（20 字节 + 空闲 4 字节）画在它下面。 */
    *frame_top = sp & ~15u;
    ((volatile uint32_t *)(uintptr_t)(*frame_top - 4))[0] = 0;
    return ELF_OK;
}


/* ---- DAY76: execve 的两半 ----
 * elf_args_validate：只检查不写（execve 必须在“不可回头”之前就知道参数能不能用）。 */
int elf_args_validate(int argc, const char *const *argv)
{
    if (argc < 0 || (uint32_t)argc > ELF_ARGS_MAX_COUNT) return ELF_ERR_ARGS;
    if (argc > 0 && argv == (const char *const *)0) return ELF_ERR_NULL;
    uint32_t total = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i] == (const char *)0) return ELF_ERR_ARGS;
        uint32_t n = (uint32_t)strlen(argv[i]) + 1;
        if (n > ELF_ARGS_MAX_BYTES || total > ELF_ARGS_MAX_BYTES - n) return ELF_ERR_ARGS;
        total += n;
    }
    return ELF_OK;
}

/* elf_reenter_user：当前地址空间已经是新程序的（段已装入）。准备栈和参数，然后直接 iret 进新入口。
 * 不返回；只有栈/参数出问题（此时旧程序已经没了）才杀掉进程：退出码 127，via = 1。 */
void elf_reenter_user(const elf_image_t *img, int argc, const char *const *argv)
{
    uint32_t api_va = urt_api_va();
    uint32_t frame_top = ELF_USER_STACK_TOP, argv_addr = 0;
    if (api_va == 0 || !stack_prepare() || args_install(argc, argv, &frame_top, &argv_addr) != ELF_OK)
        { proc_on_exit(127, 1); elf_sys_exit(127, 1); }
    g_out_total = 0;
    g_out_last = 0;
    elf_ret_stub_va = urt_ret_stub_va();
    elf_reenter(img->ehdr.e_entry, (const void *)(uintptr_t)api_va, frame_top, argc, (char **)(uintptr_t)argv_addr);
    for (;;) ;
}

int elf_run_args(elf_image_t *img, terminal_t *out, int argc, const char *const *argv,
                 int *exit_code, elf_run_info_t *info)
{
    if (exit_code)
        *exit_code = 0;
    if (info)
        memset(info, 0, sizeof(*info));
    if (img == (elf_image_t *)0 || out == (terminal_t *)0)
        return ELF_ERR_NULL;

    /* 1. 只认“已装入”的映像：loaded 只有 elf_load_segments() 全部成功才会置 1。 */
    if (img->loaded != 1)
        return ELF_ERR_NOT_LOADED;

    /* 2. 不信任 img：重新校验。静态变量——elf_image_t 约 450 字节，不放内核栈。
     *    elf32_check_phdrs() 会整体重写它的 *img 参数，所以用一份临时拷贝。
     *    （DAY47：这次校验同时拒绝与用户栈重叠的段 -> ELF_ERR_SEG_STACK。） */
    static elf_image_t chk;
    int rc = elf32_check_header(&img->ehdr, img->file_size);
    if (rc != ELF_OK)
        return rc;
    if (img->ehdr.e_phnum > ELF_MAX_PHDRS)
        return ELF_ERR_TOO_MANY_PHDRS;
    {
        elf32_ehdr_t eh = img->ehdr;
        rc = elf32_check_phdrs(&eh, img->phdr, img->ehdr.e_phnum,
                               img->file_size, &chk);
        if (rc != ELF_OK)
            return rc;
    }

    /* 入口点：校验函数已保证它落在某个带 PF_X 的 PT_LOAD 的文件部分里，
     * 这里再用“校验结果”（chk，而不是调用者传来的 img->entry_seg）确认一次。
     * 因为 img->entry_seg 也是调用者可以改的字段。 */
    const elf32_phdr_t *es = &chk.phdr[chk.entry_seg];
    uint32_t entry = chk.ehdr.e_entry;
    if (es->p_type != PT_LOAD || !(es->p_flags & PF_X) ||
        entry < es->p_vaddr || entry - es->p_vaddr >= es->p_filesz)
        return ELF_ERR_ENTRY_NOT_EXEC;

    /* 2b. DAY55: 必须在程序的用户地址空间里（运行时页已映射且带 US）：Ring 3 的程序只能碰带 US 位的页。 */
    uint32_t api_va = urt_api_va();
    if (api_va == 0)
        return ELF_ERR_NO_ASPACE;

    /* 3. DAY47: 准备用户栈（失败 = 该处没有 RAM：此时还没执行任何一条程序指令）。 */
    if (!stack_prepare())
        return ELF_ERR_NO_STACK;

    /* 3b. DAY49: 参数先检查再复制进栈顶（必须在 stack_prepare 之后：它会把整块栈填成 0xA5）。
     *     不合格 -> ELF_ERR_ARGS，程序没跑。 */
    uint32_t frame_top = ELF_USER_STACK_TOP, argv_addr = 0;
    rc = args_install(argc, argv, &frame_top, &argv_addr);
    if (rc != ELF_OK)
        return rc;

    /* 4. 跳进去（在用户栈上）。g_run_out 在进入前设好，返回后清掉
     *    （程序只在运行期间能打印）。无论 ret 还是 api->exit 都从同一个出口回来。 */
    g_run_out = out;
    g_out_total = 0;
    g_out_last = 0;
    elf_ret_stub_va = urt_ret_stub_va();
    int code = elf_enter(entry, (const void *)(uintptr_t)api_va, frame_top, argc, (char **)(uintptr_t)argv_addr);
    g_run_out = (terminal_t *)0;

    /* 5. 程序已经改写过自己的 .data/.bss：这份内存映像不再“干净”。 */
    img->loaded = 0;

    if (exit_code)
        *exit_code = code;

    /* 6. DAY47: 事后检查栈。 */
    elf_run_info_t r;
    stack_inspect(&r);
    r.via_exit = elf_exit_via;
    r.fault_vector = r.fault_err = r.fault_addr = r.fault_eip = 0;
    if (elf_exit_via == ELF_VIA_KILLED) {                /* DAY56 */
        r.fault_vector = elf_fault_last.vector;
        r.fault_err    = elf_fault_last.err;
        r.fault_addr   = elf_fault_last.addr;
        r.fault_eip    = elf_fault_last.eip;
    }
    r.out_total = g_out_total;           /* DAY48 */
    r.out_last  = (uint32_t)g_out_last;
    if (info)
        *info = r;
    if (r.overflow)
        return ELF_ERR_STACK_OVERFLOW;
    return ELF_OK;
}

/* 不带参数的 elf_run()：argc = 0。elfrun 调试命令与 Day 46~47 的测试用它，
 * 栈帧位置与 Day 47 完全相同。 */
int elf_run(elf_image_t *img, terminal_t *out, int *exit_code, elf_run_info_t *info)
{
    return elf_run_args(img, out, 0, (const char *const *)0, exit_code, info);
}
