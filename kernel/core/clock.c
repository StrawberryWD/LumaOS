/* kernel/core/clock.c - DAY72+: 见 clock.h */
#define LUMA_KERNEL_SIDE 1
#include "kernel.h"
#include "clock.h"

static uint32_t g_khz;               /* TSC 周期 / 毫秒 */
static uint64_t g_last;              /* 上次调用时的 TSC（已扣除不足 1ms 的余数）*/
static uint32_t g_ms;

static inline uint64_t rdtsc64(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* PIT 通道 2 单次计数 ~20 ms（23864 个 1193182Hz 的节拍），返回期间 TSC 增量 */
static uint32_t calibrate_khz(void)
{
    uint32_t best = 0;
    for (int rep = 0; rep < 3; rep++) {
        uint8_t p61 = inb(0x61);
        outb(0x61, (uint8_t)(p61 & 0xFC));            /* 门关闭、扬声器关 */
        outb(0x43, 0xB0);                             /* 通道 2，先低后高字节，模式 0（计数到 0 时 OUT 变高）*/
        outb(0x42, 23864 & 0xFF);
        outb(0x42, 23864 >> 8);
        uint64_t t0 = rdtsc64();
        outb(0x61, (uint8_t)((p61 & 0xFC) | 1));      /* 门打开：开始计数 */
        int guard = 50000000;
        while (!(inb(0x61) & 0x20) && --guard) { }    /* bit5 = OUT2 */
        uint64_t t1 = rdtsc64();
        outb(0x61, p61);
        if (!guard) continue;
        uint32_t khz = (uint32_t)(t1 - t0) / 20;       /* 20 ms 内的增量 < 2^32（TSC < 214 GHz），32 位除法 */
        if (khz > best) best = khz;                   /* 取最大：被中断/VM 抢占只会让测量偏小 */
    }
    return best;
}

uint32_t clock_tsc_khz(void) { return g_khz; }

uint32_t clock_ms(void)
{
    if (!g_khz) {
        g_khz = calibrate_khz();
        if (g_khz < 100000) g_khz = 1000000;          /* 校准失败：假设 1 GHz（至少单调、不会除零）*/
        g_last = rdtsc64();
        g_ms = 0;
        return 0;
    }
    uint64_t now = rdtsc64();
    uint64_t d = now - g_last;
    uint32_t chunk = g_khz * 100u;                    /* 100 ms 的周期数（TSC < 42 GHz 不溢出 32 位）*/
    while (d >= chunk) { d -= chunk; g_ms += 100; g_last += chunk; }
    uint32_t dl = (uint32_t)d;
    uint32_t ms = dl / g_khz;
    g_ms += ms;
    g_last += (uint64_t)ms * g_khz;
    return g_ms;
}
