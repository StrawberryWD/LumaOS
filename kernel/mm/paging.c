/* kernel/mm/paging.c - LumaOS 5.0
 * DAY53: "开启分页，建立 Kernel Page Table。"  验收："开启分页后系统仍稳定。"
 * 原理、布局、局限见 paging.h。
 */
#include "paging.h"
#include "../core/kernel.h"

#define NPT   (PAGING_SMALL_LIMIT / PAGE_LARGE_SIZE)     /* 64 张页表，各管 4MB */

/* 页目录和页表必须 4KB 对齐。放在 .bss（链接在 1MB 之后，entry.asm 启动时清零），
 * 内核按恒等映射运行，所以数组的地址就是它的物理地址。 */
static uint32_t page_dir[1024]          __attribute__((aligned(4096)));
static uint32_t page_tables[NPT][1024]  __attribute__((aligned(4096)));

static uint32_t g_pf_count;

static inline uint32_t read_cr0(void) { uint32_t v; __asm__ volatile ("mov %%cr0,%0" : "=r"(v)); return v; }
static inline uint32_t read_cr2(void) { uint32_t v; __asm__ volatile ("mov %%cr2,%0" : "=r"(v)); return v; }
static inline uint32_t read_cr3(void) { uint32_t v; __asm__ volatile ("mov %%cr3,%0" : "=r"(v)); return v; }
static inline uint32_t read_cr4(void) { uint32_t v; __asm__ volatile ("mov %%cr4,%0" : "=r"(v)); return v; }

int paging_enabled(void) { return (read_cr0() & 0x80000000u) != 0; }

void paging_init(void)
{
    /* 1. 0 ~ 256MB：64 张页表，4KB 页，恒等；第 0 页不映射（NULL 保护）。 */
    for (uint32_t t = 0; t < NPT; t++) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t phys = t * PAGE_LARGE_SIZE + i * PAGE_SIZE;
            page_tables[t][i] = (phys == 0) ? 0 : (phys | PTE_P | PTE_RW);
        }
        page_dir[t] = (uint32_t)(uintptr_t)&page_tables[t][0] | PTE_P | PTE_RW;
    }
    /* 2. 256MB ~ 4GB：4MB 大页，恒等，不缓存（PCI MMIO / 显存）。 */
    for (uint32_t d = NPT; d < 1024; d++)
        page_dir[d] = (d * PAGE_LARGE_SIZE) | PTE_P | PTE_RW | PDE_PS | PTE_PWT | PTE_PCD;

    /* 3. 开：CR4.PSE（bit4，允许 4MB 大页）-> CR3 -> CR0.PG（bit31）+ CR0.WP（bit16）。
     *    "memory" 屏障：保证上面对页表的写在装 CR3 之前都已完成。 */
    uint32_t cr4 = read_cr4() | 0x10u;
    __asm__ volatile ("mov %0,%%cr4" : : "r"(cr4) : "memory");
    __asm__ volatile ("mov %0,%%cr3" : : "r"((uint32_t)(uintptr_t)page_dir) : "memory");
    uint32_t cr0 = read_cr0() | 0x80000000u | 0x10000u;
    __asm__ volatile ("mov %0,%%cr0" : : "r"(cr0) : "memory");
    /* 下一条指令已经在分页下取指：eip 所在页恒等映射，所以执行流不受影响。 */
}

uint32_t *paging_kernel_pd(void)      { return page_dir; }
uint32_t  paging_kernel_pd_phys(void) { return (uint32_t)(uintptr_t)page_dir; }
void paging_load_cr3(uint32_t pd_phys)
{
    __asm__ volatile ("mov %0,%%cr3" : : "r"(pd_phys) : "memory");   /* 同时冲刷（非全局）TLB */
}
uint32_t paging_current_cr3(void) { return read_cr3(); }

int paging_translate(uint32_t va, uint32_t *pa, uint32_t *flags)
{
    return paging_translate_pd(page_dir, va, pa, flags);
}

int paging_translate_current(uint32_t va, uint32_t *pa, uint32_t *flags)
{
    return paging_translate_pd((const uint32_t *)(uintptr_t)(read_cr3() & 0xFFFFF000u), va, pa, flags);
}

int paging_translate_pd(const uint32_t *pd, uint32_t va, uint32_t *pa, uint32_t *flags)
{
    uint32_t pde = pd[va >> 22];
    if (!(pde & PTE_P))
        return -1;
    if (pde & PDE_PS) {                         /* 4MB 大页：低 22 位是页内偏移 */
        if (pa) *pa = (pde & 0xFFC00000u) | (va & 0x3FFFFFu);
        if (flags) *flags = (pde & (PTE_RW | PTE_US | PTE_PWT | PTE_PCD)) | PDE_PS | PTE_P;
        return 0;
    }
    const uint32_t *pt = (const uint32_t *)(uintptr_t)(pde & 0xFFFFF000u);
    uint32_t pte = pt[(va >> 12) & 0x3FF];
    if (!(pte & PTE_P))
        return -1;
    if (pa) *pa = (pte & 0xFFFFF000u) | (va & 0xFFFu);
    /* RW/US 要 PDE 与 PTE 两级都允许才有效 */
    if (flags) *flags = (pte & (PTE_PWT | PTE_PCD)) | (pte & pde & (PTE_RW | PTE_US)) | PTE_P;
    return 0;
}

void paging_get_info(paging_info_t *o)
{
    o->cr0 = read_cr0(); o->cr3 = read_cr3(); o->cr4 = read_cr4();
    o->pd_phys = (uint32_t)(uintptr_t)page_dir;
    o->pde_small = o->pde_large = o->pages_mapped = 0;
    for (uint32_t d = 0; d < 1024; d++) {
        if (!(page_dir[d] & PTE_P)) continue;
        if (page_dir[d] & PDE_PS) { o->pde_large++; continue; }
        o->pde_small++;
        const uint32_t *pt = (const uint32_t *)(uintptr_t)(page_dir[d] & 0xFFFFF000u);
        for (uint32_t i = 0; i < 1024; i++)
            if (pt[i] & PTE_P) o->pages_mapped++;
    }
    o->page_faults = g_pf_count;
}

/* ---- 出错画面：panic() 只会显示一个字符串，所以先在缓冲区里拼好 ---- */
static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_hex(char *p, uint32_t v)
{
    static const char h[] = "0123456789abcdef";
    *p++ = '0'; *p++ = 'x';
    for (int i = 7; i >= 0; i--) *p++ = h[(v >> (i * 4)) & 0xF];
    return p;
}

void paging_page_fault(uint32_t err, uint32_t eip, uint32_t cs)
{
    static char msg[300];
    char *p = msg;
    uint32_t cr2 = read_cr2();
    g_pf_count++;
    p = put_str(p, "PAGE FAULT at address ");
    p = put_hex(p, cr2);
    p = put_str(p, "\n  error code ");
    p = put_hex(p, err);
    p = put_str(p, (err & PF_PRESENT) ? " (protection violation, " : " (page not present, ");
    p = put_str(p, (err & PF_FETCH) ? "instruction fetch" : (err & PF_WRITE) ? "write" : "read");
    p = put_str(p, (err & PF_USER) ? ", user mode)" : ", kernel mode)");
    p = put_str(p, "\n  faulting instruction at ");
    p = put_hex(p, cs & 0xFFFF);
    p = put_str(p, ":");
    p = put_hex(p, eip);
    if ((cs & 3) == 3)                                   /* DAY55: 出错的是 Ring 3 的用户程序 */
        p = put_str(p, "\n  the faulting code was a user program (Ring 3)");
    *p = 0;
    panic(msg);
}

static const char *const names[32] = {
        "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range exceeded",
        "invalid opcode", "device not available", "double fault", "coprocessor segment overrun",
        "invalid TSS", "segment not present", "stack-segment fault", "general protection fault",
        "page fault", "reserved", "x87 floating-point error", "alignment check", "machine check",
        "SIMD floating-point error", "virtualization", "control protection", "reserved", "reserved",
        "reserved", "reserved", "reserved", "reserved", "hypervisor injection", "VMM communication",
        "security", "reserved" };

const char *paging_exception_name(uint32_t vec) { return vec < 32 ? names[vec] : "?"; }

void paging_exception_panic(uint32_t vec, uint32_t err, uint32_t eip, uint32_t cs)
{
    static char msg[300];
    char *p = msg;
    p = put_str(p, "CPU EXCEPTION ");
    p = put_hex(p, vec);
    p = put_str(p, ": ");
    p = put_str(p, vec < 32 ? names[vec] : "?");
    p = put_str(p, "\n  error code ");
    p = put_hex(p, err);
    p = put_str(p, "\n  at ");
    p = put_hex(p, cs & 0xFFFF);
    p = put_str(p, ":");
    p = put_hex(p, eip);
    if ((cs & 3) == 3)                                   /* DAY55 */
        p = put_str(p, "\n  the faulting code was a user program (Ring 3)");
    *p = 0;
    panic(msg);
}
