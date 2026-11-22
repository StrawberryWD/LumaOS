/* kernel/exec/elf_exec.c
 * LumaOS 5.0 - elf_exec(): 按路径运行一个 .sw 程序（load -> map -> run）
 *
 * DAY48 (Week 7, Day 48): "Shell 集成 `run`。"  原理见 elf.h 的 DAY48 注释，接口见 elf_exec.h。
 *
 * 这个函数本身几乎没有逻辑——它的价值是“把三步放在一个地方、顺序不可能写错”：
 *   - 必须先 load（验证）再 map（装入），最后 run；
 *   - run 只认 loaded==1，所以 map 失败时即使调用者忘了检查 rc，elf_run 也会拒绝；
 *   - 失败时告诉调用者“死在哪一步”，调用者据此给出准确的话：load 失败 = 文件有问题，
 *     什么都没动；map 失败 = 内存有问题，程序没跑；run 失败 = 栈有问题（程序没跑）或
 *     栈溢出（程序已经跑完，事后才发现）。
 */
#include "elf_exec.h"
#include "elf_loader.h"
#include "../lib/string.h"
#include "../mm/aspace.h"
#include "../syscall/syscall.h"
#include "../syscall/fd.h"
#include "urt.h"

/* DAY54: 程序在自己的地址空间里运行——
 *     load(验证) -> 建地址空间 -> 映射段+栈 -> 切 CR3 -> 装入段 -> 运行 -> 切回内核 CR3 -> 销毁
 * 每一条失败路径都先切回内核页目录再销毁，页框一个都不会泄漏。原理见 aspace.h。 */
int elf_exec_args_hook(const char *path, terminal_t *out, int argc, const char *const *argv,
                       elf_exec_result_t *res, elf_exec_hook_t hook, void *ctx)
{
    if (res == (elf_exec_result_t *)0)
        return ELF_ERR_NULL;
    memset(res, 0, sizeof(*res));
    if (path == (const char *)0 || out == (terminal_t *)0) {
        res->stage = ELF_STAGE_LOAD;
        res->rc = ELF_ERR_NULL;
        return res->rc;
    }

    /* 静态：elf_image_t 约 450 字节，不放内核栈。 */
    static elf_image_t img;

    int rc = elf_load(path, &img);                       /* Day 44 */
    if (rc != ELF_OK) {
        res->stage = ELF_STAGE_LOAD;
        res->rc = rc;
        return rc;
    }
    res->entry = img.ehdr.e_entry;

    /* ---- Day 54: 建地址空间，映射每个 PT_LOAD 段和用户栈 ---- */
    static aspace_t as;
    if (aspace_create(&as) != 0) {
        res->stage = ELF_STAGE_MAP;
        res->rc = ELF_ERR_NO_MEMORY;
        return res->rc;
    }
    int ok = 1;
    uint32_t seg_end = 0;                                /* DAY73: 最高段末尾 -> 堆起点 */
    for (uint32_t i = 0; ok && i < img.ehdr.e_phnum; i++) {
        const elf32_phdr_t *p = &img.phdr[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0)
            continue;
        ok = aspace_map_range(&as, p->p_vaddr, p->p_vaddr + p->p_memsz) == 0;
        if (p->p_vaddr + p->p_memsz > seg_end)
            seg_end = p->p_vaddr + p->p_memsz;
    }
    {   /* DAY73: 堆区 [lo, hi)：lo = 段末尾上取整；hi = lo + 8MB，但不越过栈护栏页（程序放在栈下方时）和用户区上界 */
        uint32_t lo = (seg_end + 0xFFFu) & ~0xFFFu, hi = lo + LUMA_HEAP_MAX;
        uint32_t guard = ELF_USER_STACK_BASE - 0x1000u;
        if (lo <= guard && hi > guard) hi = guard;
        if (hi > ASPACE_USER_HI || hi < lo) hi = ASPACE_USER_HI;
        if (lo >= ASPACE_USER_HI || lo < ASPACE_USER_LO) lo = hi = 0;
        else if (lo > guard && lo < ELF_USER_STACK_TOP + 0x1000u) lo = hi = 0;     /* 段占据栈区：没有堆 */
        aspace_set_heap(&as, lo, hi);
    }
    if (ok)
        ok = aspace_map_range(&as, ELF_USER_STACK_BASE, ELF_USER_STACK_TOP) == 0;
    if (ok)
        ok = urt_install(&as) == 0;                     /* DAY55: 栈上方的运行时页（api 表 + stub） */
    if (!ok) {
        aspace_destroy(&as);
        res->stage = ELF_STAGE_MAP;
        res->rc = ELF_ERR_NO_MEMORY;
        return res->rc;
    }
    res->as_pd = as.pd_phys;
    res->as_frames = as.frames;
    res->as_pages = as.data_pages;
    if (aspace_translate(&as, img.ehdr.e_entry, &res->as_entry_pa) != 0)
        res->as_entry_pa = 0;
    if (aspace_translate(&as, ELF_USER_STACK_BASE, &res->as_stack_pa) != 0)
        res->as_stack_pa = 0;
    if (aspace_translate(&as, ELF_USER_STACK_TOP - 4096, &res->as_stack_top_pa) != 0)
        res->as_stack_top_pa = 0;

    aspace_activate(&as);                                /* 从这里起，线性地址按进程页表翻译 */

    rc = elf_load_segments(path, &img, (elf_load_stats_t *)0);   /* Day 45 */
    if (rc != ELF_OK) {
        aspace_deactivate();
        aspace_destroy(&as);
        res->stage = ELF_STAGE_MAP;
        res->rc = rc;
        return rc;
    }

    if (hook)
        hook(ctx, &img, res);

    fd_reset();                                          /* DAY74: 每个程序一张干净的 fd 表 */
    rc = elf_run_args(&img, out, argc, argv, &res->code, &res->info);   /* Day 46/47/49 */
    fd_reset();                                          /* 无论怎么结束，忘记关的文件在这里关掉 */
    aspace_deactivate();                                 /* 无论程序怎么结束，先回内核页目录 */
    aspace_destroy(&as);
    res->rc = rc;
    if (rc == ELF_OK || rc == ELF_ERR_STACK_OVERFLOW)
        res->ran = 1;                                    /* 栈溢出：程序已跑完，事后才发现 */
    res->stage = (rc == ELF_OK) ? ELF_STAGE_DONE : ELF_STAGE_RUN;
    return rc;
}

int elf_exec_args(const char *path, terminal_t *out, int argc, const char *const *argv,
                  elf_exec_result_t *res)
{
    return elf_exec_args_hook(path, out, argc, argv, res, (elf_exec_hook_t)0, (void *)0);
}

/* 不带参数的版本（argc = 0）。 */
int elf_exec(const char *path, terminal_t *out, elf_exec_result_t *res)
{
    return elf_exec_args(path, out, 0, (const char *const *)0, res);
}
