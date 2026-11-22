/* kernel/fs/fat/fat_cache.c - DAY65: FAT 表扇区缓存，说明见 fat_cache.h */
#include "fat_cache.h"
#include "../../lib/string.h"

typedef struct {
    block_device_t *dev;      /* NULL = 空槽 */
    uint32_t        lba;
    uint8_t         data[BLOCK_SECTOR_SIZE];
} fatc_slot_t;

static fatc_slot_t g_slots[FATC_SLOTS];

fatc_stats_t g_fatc_stats;
int          g_fatc_enabled = 1;
uint32_t     g_fat_gen = 0;

/* 直接映射：lba 低位选槽。设备指针也混进去，免得两个卷的同号扇区总是互相踢。
 * 顺序读 FAT 时相邻的 FAT 扇区 lba 连续，落在相邻槽，不会互相冲突。 */
static inline uint32_t slot_of(const block_device_t *dev, uint32_t lba)
{
    return (lba + (uint32_t)(uintptr_t)dev / 16u) % FATC_SLOTS;
}

void fatc_stats_reset(void)
{
    memset(&g_fatc_stats, 0, sizeof(g_fatc_stats));
}

void fatc_invalidate_all(void)
{
    for (int i = 0; i < FATC_SLOTS; i++)
        g_slots[i].dev = (block_device_t *)0;
    g_fat_gen++;
}

int fatc_read(block_device_t *dev, uint32_t lba, uint8_t count, void *buf)
{
    if (!dev || !dev->read || !buf || count == 0)
        return -1;

    if (!g_fatc_enabled) {
        g_fatc_stats.lookups += count;
        g_fatc_stats.misses  += count;
        g_fatc_stats.dev_reads++;
        return dev->read(dev, lba, count, buf);
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t hit_mask = 0, nhit = 0;
    for (uint32_t i = 0; i < count; i++) {
        fatc_slot_t *s = &g_slots[slot_of(dev, lba + i)];
        g_fatc_stats.lookups++;
        if (s->dev == dev && s->lba == lba + i) {
            memcpy(out + i * BLOCK_SECTOR_SIZE, s->data, BLOCK_SECTOR_SIZE);
            hit_mask |= 1u << i;
            nhit++;
            g_fatc_stats.hits++;
        }
    }
    if (nhit == count)
        return 0;

    if (nhit == 0) {
        /* 全部未命中：一条命令读完整段，再分别放进各自的槽 */
        g_fatc_stats.misses += count;
        g_fatc_stats.dev_reads++;
        if (dev->read(dev, lba, count, buf) != 0)
            return -1;
        for (uint32_t i = 0; i < count; i++) {
            fatc_slot_t *s = &g_slots[slot_of(dev, lba + i)];
            memcpy(s->data, out + i * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE);
            s->dev = dev;
            s->lba = lba + i;
        }
        return 0;
    }

    /* 部分命中：只补读没命中的扇区（一个一个读，这种情况极少见——FAT12 的 2 扇区读才会遇到） */
    for (uint32_t i = 0; i < count; i++) {
        if (hit_mask & (1u << i))
            continue;
        g_fatc_stats.misses++;
        g_fatc_stats.dev_reads++;
        if (dev->read(dev, lba + i, 1, out + i * BLOCK_SECTOR_SIZE) != 0)
            return -1;
        fatc_slot_t *s = &g_slots[slot_of(dev, lba + i)];
        memcpy(s->data, out + i * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE);
        s->dev = dev;
        s->lba = lba + i;
    }
    return 0;
}

void fatc_note_write(uint32_t lba, uint32_t count)
{
    g_fat_gen++;
    for (int i = 0; i < FATC_SLOTS; i++) {
        fatc_slot_t *s = &g_slots[i];
        if (s->dev && s->lba >= lba && s->lba < lba + count) {
            s->dev = (block_device_t *)0;
            g_fatc_stats.invalidations++;
        }
    }
}

int fatc_write(block_device_t *dev, uint32_t lba, uint8_t count, const void *buf)
{
    if (!dev || !dev->write || !buf || count == 0)
        return -1;
    g_fatc_stats.dev_writes++;
    /* dev->write() 内部（block.c 的 ide0_write）会调用 fatc_note_write() 作废重叠槽，
     * 所以先写设备，成功后再把新内容放进缓存。 */
    if (dev->write(dev, lba, count, buf) != 0)
        return -1;
    if (!g_fatc_enabled)
        return 0;
    const uint8_t *in = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        fatc_slot_t *s = &g_slots[slot_of(dev, lba + i)];
        memcpy(s->data, in + i * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE);
        s->dev = dev;
        s->lba = lba + i;
    }
    return 0;
}
