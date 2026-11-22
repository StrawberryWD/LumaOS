/* kernel/mm/pmm.c - LumaOS 5.0 - DAY54 物理页框分配器（位图，next-fit）。说明见 pmm.h。 */
#include "pmm.h"

static uint8_t  bitmap[PMM_FRAMES / 8];        /* .bss：启动时清零 = 全部空闲 */
static uint32_t used;
static uint32_t next_hint;

static int idx_of(uint32_t pa)
{
    if (pa < PMM_BASE || pa >= PMM_END || (pa & 0xFFFu))
        return -1;
    return (int)((pa - PMM_BASE) >> 12);
}

uint32_t pmm_alloc(void)
{
    for (uint32_t n = 0; n < PMM_FRAMES; n++) {
        uint32_t i = (next_hint + n) % PMM_FRAMES;
        if (!(bitmap[i >> 3] & (1u << (i & 7)))) {
            bitmap[i >> 3] |= (uint8_t)(1u << (i & 7));
            used++;
            next_hint = i + 1;
            uint32_t pa = PMM_BASE + i * 4096u;
            volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)pa;   /* 内核恒等映射可直接写 */
            for (uint32_t k = 0; k < 1024; k++)
                p[k] = 0;
            return pa;
        }
    }
    return 0;
}

int pmm_free(uint32_t pa)
{
    int i = idx_of(pa);
    if (i < 0 || !(bitmap[i >> 3] & (1u << (i & 7))))
        return -1;
    bitmap[i >> 3] &= (uint8_t)~(1u << (i & 7));
    used--;
    if ((uint32_t)i < next_hint)
        next_hint = (uint32_t)i;                  /* 让低地址的空闲页框先被复用，行为可预测 */
    return 0;
}

int pmm_is_allocated(uint32_t pa)
{
    int i = idx_of(pa);
    return i >= 0 && (bitmap[i >> 3] & (1u << (i & 7)));
}

uint32_t pmm_free_count(void) { return PMM_FRAMES - used; }
uint32_t pmm_total(void)      { return PMM_FRAMES; }
