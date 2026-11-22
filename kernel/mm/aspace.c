/* kernel/mm/aspace.c - LumaOS 5.0 - DAY54 每个程序一个地址空间。原理见 aspace.h。 */
#include "aspace.h"
#include "paging.h"
#include "pmm.h"

static uint32_t *pd_ptr(const aspace_t *as) { return (uint32_t *)(uintptr_t)as->pd_phys; }

int aspace_create(aspace_t *as)
{
    as->pd_phys = 0; as->frames = 0; as->data_pages = 0;
    uint32_t pd = pmm_alloc();
    if (!pd)
        return -1;
    as->pd_phys = pd;
    as->frames = 1;
    const uint32_t *kpd = paging_kernel_pd();
    uint32_t *p = pd_ptr(as);
    for (uint32_t i = 0; i < 1024; i++)
        p[i] = (i >= ASPACE_PDE_LO && i < ASPACE_PDE_HI) ? 0 : kpd[i];   /* 共享内核部分 */
    return 0;
}

static int map_page(aspace_t *as, uint32_t va)
{
    uint32_t *pd = pd_ptr(as);
    uint32_t di = va >> 22;
    if (!(pd[di] & PTE_P)) {
        uint32_t pt = pmm_alloc();
        if (!pt)
            return -1;
        as->frames++;
        pd[di] = pt | PTE_P | PTE_RW | PTE_US;   /* DAY55: Ring 3 可访问；是否真能访问还看 PTE 的 US */
    }
    uint32_t *pt = (uint32_t *)(uintptr_t)(pd[di] & 0xFFFFF000u);
    uint32_t ti = (va >> 12) & 0x3FF;
    if (pt[ti] & PTE_P)
        return 0;                                  /* 已映射（两个段共用一页）*/
    uint32_t fr = pmm_alloc();
    if (!fr)
        return -1;
    as->frames++;
    as->data_pages++;
    pt[ti] = fr | PTE_P | PTE_RW | PTE_US;
    return 0;
}

int aspace_map_range(aspace_t *as, uint32_t lo, uint32_t hi)
{
    if (!as->pd_phys || lo >= hi || lo < ASPACE_USER_LO || hi > ASPACE_USER_HI)
        return -1;
    for (uint32_t va = lo & ~0xFFFu; va < hi; va += 4096u)
        if (map_page(as, va) != 0)
            return -1;
    return 0;
}

int aspace_protect_ro(aspace_t *as, uint32_t va)
{
    if (!as->pd_phys || va < ASPACE_USER_LO || va >= ASPACE_USER_HI)
        return -1;
    uint32_t *pd = pd_ptr(as);
    if (!(pd[va >> 22] & PTE_P))
        return -1;
    uint32_t *pt = (uint32_t *)(uintptr_t)(pd[va >> 22] & 0xFFFFF000u);
    uint32_t i = (va >> 12) & 0x3FF;
    if (!(pt[i] & PTE_P))
        return -1;
    pt[i] &= ~PTE_RW;
    return 0;
}

int aspace_translate(const aspace_t *as, uint32_t va, uint32_t *pa)
{
    if (!as->pd_phys)
        return -1;
    return paging_translate_pd(pd_ptr(as), va, pa, (uint32_t *)0);
}

void aspace_activate(const aspace_t *as)   { paging_load_cr3(as->pd_phys); }
void aspace_deactivate(void)               { paging_load_cr3(paging_kernel_pd_phys()); }

void aspace_destroy(aspace_t *as)
{
    if (!as->pd_phys)
        return;
    uint32_t *pd = pd_ptr(as);
    for (uint32_t d = ASPACE_PDE_LO; d < ASPACE_PDE_HI; d++) {
        if (!(pd[d] & PTE_P))
            continue;
        uint32_t *pt = (uint32_t *)(uintptr_t)(pd[d] & 0xFFFFF000u);
        for (uint32_t i = 0; i < 1024; i++)
            if (pt[i] & PTE_P)
                pmm_free(pt[i] & 0xFFFFF000u);
        pmm_free(pd[d] & 0xFFFFF000u);
    }
    pmm_free(as->pd_phys);
    as->pd_phys = 0; as->frames = 0; as->data_pages = 0;
}
