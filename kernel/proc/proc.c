/* kernel/proc/proc.c - DAY77: 进程表、fork、wait、exit、协作式切换。设计见 proc.h。 */
#define LUMA_KERNEL_SIDE 1
#include "proc.h"
#include "../arch/isr_context.h"
#include "../core/errno.h"
#include "../exec/elf_run.h"
#include "../lib/string.h"
#include "../mm/aspace.h"
#include "../syscall/fd.h"
#include "../syscall/syscall.h"

typedef struct {
    uint32_t link, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap;
} __attribute__((packed)) tss_view_t;
extern tss_view_t kernel_tss;                 /* kernel/arch/dfault.c */
extern void proc_switch(uint32_t *save_esp, uint32_t new_esp);   /* elf_enter.asm */
extern void proc_child_start(void);
extern const isr_context_t *syscall_current_ctx(void);           /* syscall.c：这次系统调用的中断帧 */

typedef struct {
    int       state;
    int       pid, ppid;
    int       exit_code, via;
    int       reaped_by_root;             /* 预留 */
    aspace_t  as_own;                     /* 子进程的地址空间；根进程不用它 */
    aspace_t *as;                         /* 实际使用的地址空间（根进程 = elf_exec 里那个静态对象） */
    uint32_t  ksp;                        /* 切走时保存的内核 esp */
    uint32_t  ktop;                       /* 内核栈顶（TSS.esp0 的值） */
    uint8_t   kstack[PROC_KSTACK] __attribute__((aligned(16)));
    uint8_t   fds[LUMA_FD_STATE_MAX] __attribute__((aligned(4)));
} proc_t;

static proc_t g_proc[PROC_MAX];
static int    g_n;                        /* 用了几个槽（0 = 没人 fork 过） */
static int    g_cur;                      /* 当前进程的槽号 */
static int    g_next_pid;


int proc_count(void) { return g_n; }

void proc_reset(void)
{
    for (int i = 0; i < PROC_MAX; i++)
        g_proc[i].state = PS_FREE;
    g_n = 0; g_cur = 0; g_next_pid = 2;
}

int proc_getpid(void) { return g_n ? g_proc[g_cur].pid : 1; }

static void slot_init(proc_t *p, int pid, int ppid)
{
    p->state = PS_READY; p->pid = pid; p->ppid = ppid;
    p->exit_code = 0; p->via = 0;
    p->as = &p->as_own;
    p->ksp = 0; p->ktop = 0;
}

/* 第一次 fork 时把“正在运行的程序”登记成 pid 1（它的内核栈就是原来的那个，aspace_t 就是 elf_exec 里的静态对象）。 */
static void adopt_root(void)
{
    proc_t *r = &g_proc[0];
    for (int i = 0; i < PROC_MAX; i++)
        g_proc[i].state = PS_FREE;
    slot_init(r, 1, 0);
    r->state = PS_RUNNING;
    r->as = aspace_current();
    r->ktop = kernel_tss.esp0;
    g_n = 1; g_cur = 0; g_next_pid = 2;
}

static int find_free(void)
{
    for (int i = 1; i < PROC_MAX; i++)
        if (g_proc[i].state == PS_FREE)
            return i;
    return -1;
}

/* 轮转：从当前槽的下一个开始找 READY。找不到返回 -1。 */
static int pick_ready(void)
{
    for (int k = 1; k <= PROC_MAX; k++) {
        int i = (g_cur + k) % PROC_MAX;
        if (g_proc[i].state == PS_READY)
            return i;
    }
    return -1;
}

/* 切到槽 nx：当前进程的状态由调用者事先设好（READY / WAITING / ZOMBIE）。 */
static void switch_to(int nx)
{
    proc_t *old = &g_proc[g_cur], *nw = &g_proc[nx];
    if (nx == g_cur) { old->state = PS_RUNNING; return; }
    fd_state_save(old->fds);
    fd_state_load(nw->fds);
    nw->state = PS_RUNNING;
    g_cur = nx;
    aspace_activate(nw->as);
    kernel_tss.esp0 = nw->ktop;
    proc_switch(&old->ksp, nw->ksp);
    /* 这里是 old 被再次切回来之后继续的地方 */
}

/* 有没有除根以外的“活”进程（没退出的）？ */
static int live_children_of_root(void)
{
    for (int i = 1; i < PROC_MAX; i++)
        if (g_proc[i].state == PS_RUNNING || g_proc[i].state == PS_READY || g_proc[i].state == PS_WAITING)
            return 1;
    return 0;
}

static int has_children(int pid)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc[i].state != PS_FREE && g_proc[i].ppid == pid && &g_proc[i] != &g_proc[g_cur])
            return 1;
    return 0;
}

int proc_fork(void)
{
    const isr_context_t *ctx = syscall_current_ctx();
    if (ctx == (const isr_context_t *)0)
        return -LUMA_EINVAL;
    if (g_n == 0) {
        if (aspace_current() == (aspace_t *)0)
            return -LUMA_ESRCH;
        adopt_root();
    }
    int c = find_free();
    if (c < 0)
        return -LUMA_EAGAIN;
    proc_t *p = &g_proc[c], *me = &g_proc[g_cur];
    slot_init(p, g_next_pid, me->pid);
    if (aspace_clone(me->as, &p->as_own) != 0) {
        p->state = PS_FREE;
        return -LUMA_ENOMEM;
    }
    fd_state_save(me->fds);                          /* 先把父进程当前的表存回它的槽，再整份复制给子进程 */
    memcpy(p->fds, me->fds, LUMA_FD_STATE_MAX);
    fd_state_strip_writers(p->fds);                  /* DAY77b：写缓冲不跨进程共享 */

    /* 子进程内核栈：栈顶放中断帧拷贝（isr_context_t + CPU 在特权级切换时压的 useresp/ss 共 7 个 dword 之后还有 2 个），
     * 下面放 proc_switch 的假帧：4 个寄存器 + 返回地址 proc_child_start。 */
    uint32_t top = (uint32_t)(uintptr_t)(p->kstack + PROC_KSTACK);
    uint32_t frame_bytes = (uint32_t)sizeof(isr_context_t) + 8;       /* + useresp, ss */
    uint32_t fbase = top - frame_bytes;
    memcpy((void *)(uintptr_t)fbase, ctx, frame_bytes);
    ((isr_context_t *)(uintptr_t)fbase)->eax = 0;                     /* 子进程里 fork() 返回 0 */
    uint32_t *sp = (uint32_t *)(uintptr_t)fbase;
    *--sp = (uint32_t)(uintptr_t)proc_child_start;                    /* ret 的目标 */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;                       /* ebp ebx esi edi */
    p->ksp = (uint32_t)(uintptr_t)sp;
    p->ktop = top;
    p->state = PS_READY;
    g_n++;
    return g_next_pid++;
}

void proc_yield(void)
{
    if (!g_n) return;
    int nx = pick_ready();
    if (nx < 0) return;
    g_proc[g_cur].state = PS_READY;
    switch_to(nx);
}

/* 状态字：和 Unix 一样把退出码放在第 8~15 位；被异常/Ctrl+C 杀死的把信号编号（异常向量/2 之类不重要，用 128+n 的 n）放低 7 位。 */
static int make_status(const proc_t *p)
{
    if (p->via == (int)ELF_VIA_KILLED)
        return (p->exit_code - 128) & 0x7F ? ((p->exit_code - 128) & 0x7F) : 0x7F;
    return (p->exit_code & 0xFF) << 8;
}

int proc_wait(uint32_t ustatus)
{
    if (!g_n)
        return -LUMA_ECHILD;
    if (ustatus && !luma_user_range_ok(ustatus, 4))
        return -LUMA_EFAULT;
    int mypid = g_proc[g_cur].pid;
    for (;;) {
        for (int i = 0; i < PROC_MAX; i++) {
            proc_t *p = &g_proc[i];
            if (p->state == PS_ZOMBIE && p->ppid == mypid) {
                int pid = p->pid;
                if (ustatus)
                    *(volatile int *)(uintptr_t)ustatus = make_status(p);
                p->state = PS_FREE;
                g_n--;
                return pid;
            }
        }
        if (!has_children(mypid))
            return -LUMA_ECHILD;
        int nx = pick_ready();
        if (nx < 0)
            return -LUMA_ECHILD;                      /* 子进程全在等别人：死锁，不让它挂死 */
        g_proc[g_cur].state = PS_WAITING;
        switch_to(nx);
    }
}

void proc_on_exit(int code, int via)
{
    if (!g_n)
        return;
    proc_t *me = &g_proc[g_cur];
    me->exit_code = code; me->via = via;
    if (g_cur == 0) {
        /* 根进程：先让所有还活着的进程跑完（它们的 wait 者可能就是根），最后一个退出的会把我切回来 */
        while (live_children_of_root()) {
            int nx = pick_ready();
            if (nx < 0)
                break;                                 /* 剩下的都在互相等：放弃它们（地址空间随进程表清空而泄漏——死锁的罕见情况） */
            me->state = PS_WAITING;
            switch_to(nx);
        }
        for (int i = 1; i < PROC_MAX; i++)             /* 收掉没人 wait 的僵尸 */
            g_proc[i].state = PS_FREE;
        g_n = 0;
        fd_reset();
        return;                                        /* 调用者接着走 elf_sys_exit，回 shell */
    }
    /* 子进程：孤儿归根，关 fd，销毁地址空间，变僵尸，叫醒等它的人，切走 */
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc[i].state != PS_FREE && g_proc[i].ppid == me->pid)
            g_proc[i].ppid = 1;
    fd_reset();
    aspace_deactivate();
    aspace_destroy(me->as);
    me->state = PS_ZOMBIE;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc[i].state == PS_WAITING && g_proc[i].pid == me->ppid)
            g_proc[i].state = PS_READY;
    if (g_proc[0].state == PS_WAITING && !live_children_of_root())
        g_proc[0].state = PS_READY;                    /* 根在等最后一个孩子结束 */
    int nx = pick_ready();
    if (nx < 0) {
        /* 不应发生：根进程要么 READY 要么 WAITING-被叫醒。保险：回根进程。 */
        g_proc[0].state = PS_READY;
        nx = 0;
    }
    g_cur = g_cur;                                     /* switch_to 用 g_cur 记录“旧进程” */
    switch_to(nx);
    for (;;) ;                                         /* 僵尸永远不会被再次调度 */
}
