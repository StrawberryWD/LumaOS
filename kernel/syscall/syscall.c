/* kernel/syscall/syscall.c - LumaOS 5.0
 * DAY50: "定义 syscall ABI。"  验收："`int 0x80` 能进入 Kernel。"
 * ABI 与原理见 syscall.h。
 *
 * 流程：用户 `int 0x80` -> idt[0x80] -> isr128 -> isr_common（存现场）-> isr_handler()
 *        -> syscall_dispatch(ctx)：读 ctx->eax（调用号）、ebx/ecx/edx/esi/edi/ebp（参数），
 *        查表调用，把返回值写进 ctx->eax -> isr_common 弹回寄存器 -> iret。
 */
#define LUMA_KERNEL_SIDE 1
#define LUMA_KERNEL_SIDE 1
#include "syscall.h"
#include "../mm/aspace.h"
#include "../arch/isr_context.h"
#include "../mm/paging.h"

_Static_assert(LUMA_USER_LO == 0x01000000u && LUMA_USER_HI == 0x0F000000u,
               "keep in sync with ELF_LOAD_MIN_ADDR / ELF_LOAD_MAX_ADDR in kernel/exec/elf.h");

/* ---- 各个系统调用的实现。参数一律当成不可信的整数。 ---- */

/* SYS_NOP：什么也不做。 */
static int32_t sys_nop(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return 0;
}

/* SYS_GETABI：ABI 版本号。 */
static int32_t sys_getabi(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return LUMA_SYSCALL_ABI_VERSION;
}

/* ---- DAY51: 用户指针检查 ---- */

int luma_user_range_ok(uint32_t addr, uint32_t len)
{
    if (len == 0)
        return 1;
    if (addr < LUMA_USER_LO || addr >= LUMA_USER_HI)
        return 0;
    /* 不写 addr + len <= HI：addr 接近 4 GB 时会 32 位回绕成小数而通过检查。 */
    if (len > LUMA_USER_HI - addr)
        return 0;
    /* DAY54: 范围合法还不够——每一页在“当前页目录”（CR3）里都必须真的映射了。
     * 否则内核自己读这块内存时会缺页 panic（程序一个坏指针就能把内核打死）。
     * 在内核页目录下（内核自己 `int 0x80` 时）整个用户区是恒等映射的，行为和 Day 51 一样。 */
    uint32_t last = addr + len - 1;               /* 上面已保证不回绕 */
    for (uint32_t pg = addr & ~0xFFFu; ; pg += 4096u) {
        uint32_t fl = 0;
        if (paging_translate_current(pg, (uint32_t *)0, &fl) != 0)
            return 0;
        /* DAY55: 在程序的页目录下，用户页必须带 US 位（内核恒等映射的页没有）。内核自己 int 0x80
         * 做自检时用的是内核页目录，那里的用户区没有 US，不查。 */
        if (paging_current_cr3() != paging_kernel_pd_phys() && !(fl & PTE_US))
            return 0;
        if (pg >= (last & ~0xFFFu))
            break;
    }
    return 1;
}

int luma_copy_from_user(void *dst, uint32_t src, uint32_t n)
{
    if (!luma_user_range_ok(src, n))
        return -LUMA_EFAULT;
    const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
    return 0;
}

/* SYS_WRITE(fd, buf, len)：见 syscall.h 的 DAY51 一节。检查顺序：fd -> len==0 -> 指针 -> 有无程序。 */
extern int elf_run_write(const char *buf, uint32_t n, int to_stderr);   /* kernel/exec/elf_run.c */

static int32_t sys_write(uint32_t fd, uint32_t buf, uint32_t len, uint32_t a4, uint32_t a5, uint32_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (fd != LUMA_STDOUT && fd != LUMA_STDERR)
        return -LUMA_EBADF;
    if (len == 0)
        return 0;
    if (len > LUMA_WRITE_MAX)
        len = LUMA_WRITE_MAX;                       /* 短写：只写前 4096 字节 */
    if (!luma_user_range_ok(buf, len))
        return -LUMA_EFAULT;                        /* 整块检查在先：失败时一个字节都不输出 */

    char chunk[128];
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (luma_copy_from_user(chunk, buf + done, n) != 0)
            return -LUMA_EFAULT;
        if (elf_run_write(chunk, n, fd == LUMA_STDERR) != 0)
            return -LUMA_EBADF;                     /* 没有程序在运行：没有终端可写 */
        done += n;
    }
    return (int32_t)done;
}

/* SYS_EXIT(code)：见 syscall.h 的 DAY52 一节。成功时不返回。 */
static syscall_stats_t g_stats;          /* 下面再次出现是同一个对象（C 的暂定定义）；sys_exit 要读 last_eip */
extern void elf_sys_exit(int code, int via);  /* kernel/exec/elf_enter.asm: 切回内核栈的统一出口 */
extern int  urt_exit_via(uint32_t eip);      /* kernel/exec/urt.c: DAY55 按调用者 eip 判断退出方式 */
extern uint32_t elf_running;              /* 同上：1 = 有程序在跑 */

static int32_t sys_exit(uint32_t code, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!elf_running)
        return -LUMA_ESRCH;               /* 没有程序可结束：不跳 */
    elf_sys_exit((int)code, urt_exit_via(g_stats.last_eip));   /* 不返回 */
    return 0;
}

/* DAY70: SYS_STRERROR(errnum, buf, len) —— 见 syscall.h。把 luma_strerror() 的文字交给用户程序。
 * 写用户内存前先检查整块 [buf, buf+len) 都在用户范围内（和 sys_write 读用户内存同一套规则）。 */
static int32_t sys_strerror(uint32_t errnum, uint32_t buf, uint32_t len, uint32_t a4, uint32_t a5, uint32_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (len == 0)
        return -LUMA_EINVAL;
    if (!luma_user_range_ok(buf, len))
        return -LUMA_EFAULT;
    const char *msg = luma_strerror((int)errnum);
    uint32_t n = 0;
    while (msg[n]) n++;
    uint32_t copy = n < len - 1 ? n : len - 1;
    volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)buf;
    for (uint32_t i = 0; i < copy; i++)
        d[i] = (uint8_t)msg[i];
    d[copy] = 0;
    return copy < n ? -LUMA_ERANGE : (int32_t)n;
}

/* DAY73: SYS_BRK(addr) —— 见 syscall.h */
static int32_t sys_brk(uint32_t addr, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    aspace_t *as = aspace_current();
    if (!elf_running || !as)
        return -LUMA_ESRCH;
    if (!as->heap_hi)
        return -LUMA_ENOMEM;
    if (addr == 0)
        return (int32_t)as->heap_brk;
    if (addr < as->heap_lo)
        return -LUMA_EINVAL;
    if (aspace_brk(as, addr) != 0)
        return -LUMA_ENOMEM;
    return (int32_t)addr;
}

/* 调用表：下标 = 调用号；NULL = 没有这个调用（返回 -ENOSYS）。
 * SYS_EXIT / SYS_WRITE 的号码今天就占住了（ABI 已定），实现留给 Day 52 / Day 51。 */
static const syscall_fn_t syscall_table[LUMA_SYSCALL_MAX] = {
    [SYS_NOP]    = sys_nop,
    [SYS_EXIT]   = sys_exit,        /* DAY52 */
    [SYS_WRITE]  = sys_write,       /* DAY51 */
    [SYS_GETABI] = sys_getabi,
    [SYS_STRERROR] = sys_strerror,  /* DAY70 */
    [SYS_BRK]    = sys_brk,         /* DAY73 */
};

static syscall_stats_t g_stats;

const syscall_stats_t *syscall_get_stats(void) { return &g_stats; }
void syscall_reset_stats(void)
{
    syscall_stats_t z = {0};
    g_stats = z;
}

void syscall_dispatch(struct isr_context *c)
{
    isr_context_t *ctx = (isr_context_t *)c;
    uint32_t nr = ctx->eax;
    int32_t ret;

    /* DAY52: 记录先于调用——SYS_EXIT 不会返回到这里。last_ret 在返回后再补（SYS_EXIT 记退出码）。 */
    g_stats.calls++;
    g_stats.last_nr = nr;
    g_stats.last_ret = (nr == SYS_EXIT) ? (int32_t)ctx->ebx : 0;
    g_stats.last_args[0] = ctx->ebx; g_stats.last_args[1] = ctx->ecx; g_stats.last_args[2] = ctx->edx;
    g_stats.last_args[3] = ctx->esi; g_stats.last_args[4] = ctx->edi; g_stats.last_args[5] = ctx->ebp;
    g_stats.last_eip = ctx->eip;
    g_stats.last_cs = ctx->cs;

    if (nr < LUMA_SYSCALL_MAX && syscall_table[nr]) {
        ret = syscall_table[nr](ctx->ebx, ctx->ecx, ctx->edx, ctx->esi, ctx->edi, ctx->ebp);
    } else {
        ret = -LUMA_ENOSYS;
        g_stats.bad_calls++;
    }

    g_stats.last_ret = ret;

    ctx->eax = (uint32_t)ret;            /* 返回值：isr_common 出口会 pop 进 eax */
}

void syscall_read_gate(syscall_gate_t *g)
{
    struct { uint16_t limit; uint32_t base; } __attribute__((packed)) idtr;
    __asm__ volatile ("sidt %0" : "=m"(idtr));
    const uint8_t *e = (const uint8_t *)(uintptr_t)(idtr.base + 8u * LUMA_SYSCALL_VECTOR);
    g->handler  = (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24;
    g->selector = (uint16_t)(e[2] | e[3] << 8);
    g->type_attr = e[5];
}
