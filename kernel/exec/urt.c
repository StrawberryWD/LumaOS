/* kernel/exec/urt.c - LumaOS 5.0 - DAY55 用户态运行时页。说明见 urt.h / urt.asm。 */
#include "urt.h"
#include "elf_run.h"
#include "../mm/paging.h"

extern char urt_start[], urt_end[], urt_api[], urt_ret_stub[], urt_ret_stub_end[],
            urt_api_exit[], urt_api_exit_end[], urt_puts[], urt_putchar[];

static uint32_t va_of(const char *sym) { return LUMA_URT_BASE + (uint32_t)(sym - urt_start); }

int urt_install(aspace_t *as)
{
    if ((uint32_t)(urt_end - urt_start) > LUMA_URT_SIZE)
        return -1;
    if (aspace_map_range(as, LUMA_URT_BASE, LUMA_URT_BASE + LUMA_URT_SIZE) != 0)
        return -1;
    uint32_t pa;
    if (aspace_translate(as, LUMA_URT_BASE, &pa) != 0)
        return -1;
    volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)pa;      /* 内核恒等映射可直接写页框 */
    for (uint32_t i = 0; i < (uint32_t)(urt_end - urt_start); i++)
        d[i] = (uint8_t)urt_start[i];
    volatile uint32_t *t = (volatile uint32_t *)(uintptr_t)(pa + (uint32_t)(urt_api - urt_start));
    t[0] = LUMA_API_MAGIC;
    t[1] = LUMA_API_VERSION;
    t[2] = va_of(urt_puts);
    t[3] = va_of(urt_putchar);
    t[4] = va_of(urt_api_exit);
    aspace_protect_ro(as, LUMA_URT_BASE);                          /* 之后用户（和 WP=1 的内核）都写不了 */
    return 0;
}

uint32_t urt_api_va(void)
{
    uint32_t fl;
    if (paging_translate_current(LUMA_URT_BASE, (uint32_t *)0, &fl) != 0 || !(fl & PTE_US))
        return 0;
    return va_of(urt_api);
}

uint32_t urt_ret_stub_va(void)
{
    return urt_api_va() ? va_of(urt_ret_stub) : 0;
}

int urt_exit_via(uint32_t eip)
{
    if (eip == va_of(urt_ret_stub_end))
        return 0;
    if (eip == va_of(urt_api_exit_end))
        return 1;
    return 2;
}
