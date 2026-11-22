/* kernel/drivers/block/ramdisk.c - DAY62: see ramdisk.h */
#include "ramdisk.h"

typedef struct { uint8_t *base; uint32_t sectors; } rd_t;
#define RD_MAX 2
static rd_t rd_state[RD_MAX];
static block_device_t rd_dev[RD_MAX];
static int rd_n = 0;

static void mcopy(void *d, const void *s, uint32_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }

static int rd_read(block_device_t *dev, uint32_t lba, uint8_t count, void *buf)
{
    rd_t *r = dev->dev_private;
    uint32_t n = count ? count : 256;
    if (lba + n > r->sectors) return -1;
    mcopy(buf, r->base + (uint64_t)lba * 512, n * 512);
    return 0;
}
static int rd_write(block_device_t *dev, uint32_t lba, uint8_t count, const void *buf)
{
    rd_t *r = dev->dev_private;
    uint32_t n = count ? count : 256;
    if (lba + n > r->sectors) return -1;
    mcopy(r->base + (uint64_t)lba * 512, buf, n * 512);
    return 0;
}

block_device_t *ramdisk_create(const char *name, void *base, uint32_t bytes)
{
    if (rd_n >= RD_MAX || !base || bytes < 512) return 0;
    rd_state[rd_n].base = base; rd_state[rd_n].sectors = bytes / 512;
    rd_dev[rd_n].name = name; rd_dev[rd_n].sector_size = 512;
    rd_dev[rd_n].read = rd_read; rd_dev[rd_n].write = rd_write; rd_dev[rd_n].dev_private = &rd_state[rd_n];
    return &rd_dev[rd_n++];
}
