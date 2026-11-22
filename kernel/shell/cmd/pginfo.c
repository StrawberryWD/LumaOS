/* kernel/shell/cmd/pginfo.c
 * LumaOS 5.0 — DAY53 (Week 8, Day 53): "开启分页，建立 Kernel Page Table。"
 *   验收："开启分页后系统仍稳定。"
 *
 * 用法:  pginfo            显示分页状态：CR0.PG/WP、CR4.PSE、CR3，页目录里 4KB 页表/4MB 大页的个数
 *        pginfo <addr>     查表：线性地址 <addr>（十六进制，0x 可省）-> 物理地址与标志，或 "not mapped"
 *        pginfo -t         自检：恒等映射、NULL 页未映射、MMIO 区不缓存、读写经页表仍然正确……
 *        pginfo -a         DAY54 自检：建两个地址空间，同一线性地址指向不同物理页框，互不可见
 *        pginfo -f         故意访问地址 0（NULL 页），演示缺页 -> panic（系统会停住，需重启！）
 *
 * 原理见 kernel/mm/paging.h。每行 < 80 列。
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../mm/paging.h"
#include "../../mm/pmm.h"
#include "../../mm/aspace.h"
#include "../../lib/string.h"
#include "../shell.h"

static void put_hex(terminal_t *t, uint32_t v)
{
    static const char hexd[] = "0123456789abcdef";
    terminal_puts(t, "0x", TERM_COL_DEFAULT);
    for (int i = 7; i >= 0; i--)
        terminal_putchar(t, hexd[(v >> (i * 4)) & 0xF], TERM_COL_DEFAULT);
}

static void put_dec(terminal_t *t, uint32_t u)
{
    char tmp[12];
    int n = 0;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) terminal_putchar(t, tmp[--n], TERM_COL_DEFAULT);
}

static int parse_hex(const char *s, uint32_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return -1;
    uint32_t v = 0;
    for (int n = 0; *s; s++, n++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return -1;
        if (n >= 8) return -1;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return 0;
}

static void put_flags(terminal_t *t, uint32_t f)
{
    terminal_puts(t, (f & PTE_RW) ? "RW" : "R-", TERM_COL_DEFAULT);
    terminal_puts(t, (f & PTE_US) ? " user" : " supervisor", TERM_COL_DEFAULT);
    terminal_puts(t, (f & PDE_PS) ? " 4MB-page" : " 4KB-page", TERM_COL_DEFAULT);
    if (f & PTE_PCD) terminal_puts(t, " uncached", TERM_COL_DEFAULT);
}

static int check(terminal_t *t, const char *what, int ok)
{
    terminal_puts(t, ok ? "  [ok]   " : "  [FAIL] ", ok ? TERM_COL_DEFAULT : TERM_COL_ERROR);
    terminal_puts(t, what, ok ? TERM_COL_DEFAULT : TERM_COL_ERROR);
    terminal_newline(t);
    return ok;
}

/* DAY54: 地址空间自检。两个进程在同一个线性地址 VA 各写一个不同的值，互相看不到；
 * 内核页目录下 VA 处仍是原样；销毁后页框全部归还。 */
static void aspace_selftest(terminal_t *t)
{
    #define VA 0x08048000u
    uint32_t free0 = pmm_free_count(), n = 0, bad = 0;
    static aspace_t a, b;
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)VA;
    terminal_puts(t, "pginfo: address space self-test\n", TERM_COL_DEFAULT);
    uint32_t kcr3 = paging_current_cr3();
    int ok;
    n++; ok = aspace_create(&a) == 0 && aspace_create(&b) == 0;
    bad += !check(t, "two address spaces created", ok);
    n++; ok = aspace_map_range(&a, VA, VA + 8192) == 0 && aspace_map_range(&b, VA, VA + 8192) == 0;
    bad += !check(t, "mapped 2 pages at 0x08048000 in each", ok);
    uint32_t pa_a = 0, pa_b = 0;
    aspace_translate(&a, VA, &pa_a); aspace_translate(&b, VA, &pa_b);
    n++; bad += !check(t, "same VA -> different physical frames",
                       pa_a && pa_b && pa_a != pa_b && pa_a >= PMM_BASE && pa_a < PMM_END);
    aspace_activate(&a); *p = 0xAAAA0001u; uint32_t seen_a = *p;
    aspace_activate(&b); uint32_t b_before = *p; *p = 0xBBBB0002u;
    aspace_activate(&a); uint32_t a_after = *p;
    aspace_deactivate();
    n++; bad += !check(t, "A writes, B still sees 0 (new frames are zeroed)", seen_a == 0xAAAA0001u && b_before == 0);
    n++; bad += !check(t, "B writes, A still sees its own value", a_after == 0xAAAA0001u);
    n++; bad += !check(t, "kernel page directory restored", paging_current_cr3() == kcr3);
    n++; bad += !check(t, "kernel view of VA untouched (still 0)", *p == 0);
    n++; bad += !check(t, "frames hold the data (seen via physical address)",
                       *(volatile uint32_t *)(uintptr_t)pa_a == 0xAAAA0001u &&
                       *(volatile uint32_t *)(uintptr_t)pa_b == 0xBBBB0002u);
    n++; bad += !check(t, "unmapped page in a space reports not mapped",
                       aspace_translate(&a, VA + 0x10000, &pa_a) != 0);
    n++; bad += !check(t, "range outside user area refused",
                       aspace_map_range(&a, 0x00100000u, 0x00101000u) != 0 &&
                       aspace_map_range(&a, 0x0EFFF000u, 0x0F001000u) != 0);
    aspace_destroy(&a); aspace_destroy(&b);
    n++; bad += !check(t, "all frames returned to the pool", pmm_free_count() == free0);
    n++; bad += !check(t, "double free refused", pmm_free(pa_b) != 0);
    terminal_puts(t, bad ? "pginfo: FAIL (" : "pginfo: OK (", bad ? TERM_COL_ERROR : TERM_COL_DEFAULT);
    put_dec(t, n - bad); terminal_puts(t, "/", TERM_COL_DEFAULT); put_dec(t, n);
    terminal_puts(t, " checks)\n", TERM_COL_DEFAULT);
    #undef VA
}

void cmd_pginfo(terminal_t *t, int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        terminal_puts(t, "pginfo: reading address 0 (the unmapped NULL page)...\n", TERM_COL_DEFAULT);
        volatile uint32_t *np = (volatile uint32_t *)0;
        uint32_t v = *np;                         /* -> #PF -> panic */
        terminal_puts(t, "pginfo: no fault?! read ", TERM_COL_ERROR);
        put_hex(t, v);
        terminal_newline(t);
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-t") == 0) {
        int ok = 1;
        paging_info_t i;
        paging_get_info(&i);
        terminal_puts(t, "pginfo: kernel page table self-test\n", TERM_COL_DEFAULT);
        ok &= check(t, "paging is enabled (CR0.PG=1) and write-protect on (CR0.WP=1)",
                    paging_enabled() && (i.cr0 & 0x10000u));
        ok &= check(t, "CR4.PSE=1 (4MB pages allowed)", (i.cr4 & 0x10u) != 0);
        ok &= check(t, "CR3 holds the page directory (4KB aligned)", (i.cr3 & 0xFFFFF000u) == i.pd_phys && (i.pd_phys & 0xFFFu) == 0);
        ok &= check(t, "64 page tables (0-256MB, 4KB pages) + 960 large pages", i.pde_small == 64 && i.pde_large == 960);
        ok &= check(t, "4KB pages mapped: 64*1024 - 1 (page 0 is the NULL guard)", i.pages_mapped == 64u * 1024u - 1u);
        uint32_t pa = 0xFFFFFFFFu, fl = 0;
        ok &= check(t, "address 0 is NOT mapped", paging_translate(0, &pa, &fl) == -1);
        ok &= check(t, "address 0xFFF (end of the NULL page) is NOT mapped", paging_translate(0xFFF, &pa, &fl) == -1);
        ok &= check(t, "address 0x1000 maps to itself", paging_translate(0x1000, &pa, &fl) == 0 && pa == 0x1000);
        ok &= check(t, "kernel text (this function) is identity mapped",
                    paging_translate((uint32_t)(uintptr_t)cmd_pginfo, &pa, &fl) == 0 && pa == (uint32_t)(uintptr_t)cmd_pginfo);
        ok &= check(t, "ELF load area 0x08048000 is identity mapped, RW supervisor 4KB-page",
                    paging_translate(0x08048123, &pa, &fl) == 0 && pa == 0x08048123 &&
                    (fl & PTE_RW) && !(fl & PTE_US) && !(fl & PDE_PS));
        ok &= check(t, "user stack 0x09FFFFFF (last byte) is mapped",
                    paging_translate(0x09FFFFFF, &pa, &fl) == 0 && pa == 0x09FFFFFF);
        ok &= check(t, "VGA text/graphics windows 0xB8000 / 0xA0000 are mapped",
                    paging_translate(0xB8000, &pa, 0) == 0 && paging_translate(0xA0000, &pa, 0) == 0);
        ok &= check(t, "last 4KB page below 256MB (0x0FFFF000) is mapped",
                    paging_translate(0x0FFFF000, &pa, &fl) == 0 && pa == 0x0FFFF000);
        ok &= check(t, "MMIO area 0xFD000000 is a 4MB uncached identity page",
                    paging_translate(0xFD123456, &pa, &fl) == 0 && pa == 0xFD123456 && (fl & PDE_PS) && (fl & PTE_PCD));
        ok &= check(t, "top of the address space 0xFFFFFFFF is mapped",
                    paging_translate(0xFFFFFFFFu, &pa, &fl) == 0 && pa == 0xFFFFFFFFu);
        /* read/write through the mapping really works (here: a static array in the kernel's .bss) */
        static volatile uint32_t probe[4];
        for (int k = 0; k < 4; k++) probe[k] = 0xA5A50000u + (uint32_t)k;
        int same = 1;
        for (int k = 0; k < 4; k++) if (probe[k] != 0xA5A50000u + (uint32_t)k) same = 0;
        ok &= check(t, "write/read through the page table gives back the same data", same);
        ok &= check(t, "no page fault has been handled so far", i.page_faults == 0);
        terminal_puts(t, ok ? "pginfo: OK\n" : "pginfo: FAIL\n", ok ? TERM_COL_DEFAULT : TERM_COL_ERROR);
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        aspace_selftest(t);
        return;
    }

    if (argc > 1) {
        uint32_t va;
        if (parse_hex(argv[1], &va) != 0) {
            terminal_puts(t, "usage: pginfo [-t | -a | -f | <hex-address>]\n", TERM_COL_ERROR);
            return;
        }
        uint32_t pa, fl;
        put_hex(t, va);
        if (paging_translate(va, &pa, &fl) != 0) {
            terminal_puts(t, " -> not mapped\n", TERM_COL_DEFAULT);
            return;
        }
        terminal_puts(t, " -> ", TERM_COL_DEFAULT);
        put_hex(t, pa);
        terminal_puts(t, "  ", TERM_COL_DEFAULT);
        put_flags(t, fl);
        terminal_newline(t);
        return;
    }

    paging_info_t i;
    paging_get_info(&i);
    terminal_puts(t, "paging: ", TERM_COL_DEFAULT);
    terminal_puts(t, paging_enabled() ? "ENABLED" : "disabled", TERM_COL_DEFAULT);
    terminal_puts(t, "   CR0 ", TERM_COL_DEFAULT); put_hex(t, i.cr0);
    terminal_puts(t, "  CR3 ", TERM_COL_DEFAULT); put_hex(t, i.cr3);
    terminal_puts(t, "  CR4 ", TERM_COL_DEFAULT); put_hex(t, i.cr4);
    terminal_newline(t);
    terminal_puts(t, "page directory at ", TERM_COL_DEFAULT); put_hex(t, i.pd_phys);
    terminal_puts(t, ": ", TERM_COL_DEFAULT); put_dec(t, i.pde_small);
    terminal_puts(t, " page tables (4KB pages), ", TERM_COL_DEFAULT); put_dec(t, i.pde_large);
    terminal_puts(t, " large pages (4MB)\n", TERM_COL_DEFAULT);
    terminal_puts(t, "4KB pages mapped: ", TERM_COL_DEFAULT); put_dec(t, i.pages_mapped);
    terminal_puts(t, " (", TERM_COL_DEFAULT); put_dec(t, i.pages_mapped * 4 / 1024);
    terminal_puts(t, " MB), page faults handled: ", TERM_COL_DEFAULT); put_dec(t, i.page_faults);
    terminal_newline(t);
    terminal_puts(t, "frame pool: ", TERM_COL_DEFAULT); put_dec(t, pmm_free_count());
    terminal_puts(t, " of ", TERM_COL_DEFAULT); put_dec(t, pmm_total());
    terminal_puts(t, " frames free (8-16 MB)\n", TERM_COL_DEFAULT);
}
