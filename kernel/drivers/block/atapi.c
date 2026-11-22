/* kernel/drivers/block/atapi.c - DAY62: see atapi.h */
#include "atapi.h"
#include "../../core/kernel.h"

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_BSY 0x80

typedef struct { uint16_t io, ctl; uint8_t drv; char model[41]; const char *pos; block_device_t bd; } atapi_t;
static atapi_t g_at[ATAPI_MAX];
static int     g_at_n = 0;

static inline void delay400(uint16_t ctl) { for (int i = 0; i < 4; i++) (void)inb(ctl); }

/* wait until BSY clears; returns the final status or -1 on timeout */
static int wait_not_busy(uint16_t io)
{
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(io + 7);
        if (!(s & ST_BSY)) return s;
        outb(0x80, 0);
    }
    return -1;
}

static void select_drive(const atapi_t *a)
{
    outb(a->io + 6, (uint8_t)(0xA0 | (a->drv << 4)));
    delay400(a->ctl);
}

/* send a 12-byte packet; byte-count limit `lim`; returns 0 when the drive asked for the packet */
static int send_packet(const atapi_t *a, const uint8_t *pkt, uint16_t lim)
{
    select_drive(a);
    if (wait_not_busy(a->io) < 0) return -1;
    outb(a->io + 1, 0);                    /* PIO, no DMA */
    outb(a->io + 4, (uint8_t)(lim & 0xFF));
    outb(a->io + 5, (uint8_t)(lim >> 8));
    outb(a->io + 7, 0xA0);                 /* PACKET */
    delay400(a->ctl);
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(a->io + 7);
        if (s & ST_ERR) return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ)) {
            outsw(a->io, pkt, 6);
            return 0;
        }
        outb(0x80, 0);
    }
    return -1;
}

static int identify(atapi_t *a)
{
    select_drive(a);
    if (wait_not_busy(a->io) < 0) return -1;
    outb(a->io + 7, 0xA1);                 /* IDENTIFY PACKET DEVICE */
    delay400(a->ctl);
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(a->io + 7);
        if (s & ST_ERR) return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ)) {
            uint16_t w[256];
            insw(a->io, w, 256);
            for (int k = 0; k < 20; k++) {       /* words 27..46: model, byte-swapped pairs */
                a->model[k * 2]     = (char)(w[27 + k] >> 8);
                a->model[k * 2 + 1] = (char)(w[27 + k] & 0xFF);
            }
            a->model[40] = 0;
            for (int k = 39; k >= 0 && (a->model[k] == ' ' || a->model[k] == 0); k--) a->model[k] = 0;
            return 0;
        }
        outb(0x80, 0);
    }
    return -1;
}

static int bd_read(block_device_t *dev, uint32_t lba, uint8_t count, void *buf)
{
    return atapi_read((int)(uintptr_t)dev->dev_private, lba, count ? count : 256, buf);
}

int atapi_probe(void)
{
    static const uint16_t IO[2] = {0x1F0, 0x170}, CTL[2] = {0x3F6, 0x376};
    static const char *POS[4] = {"primary master", "primary slave", "secondary master", "secondary slave"};
    g_at_n = 0;
    for (int ch = 0; ch < 2; ch++) {
        outb(CTL[ch], 0x02);                              /* nIEN: we poll, no IRQ 14/15 */
        for (int dr = 0; dr < 2; dr++) {
            atapi_t t; for (unsigned k = 0; k < sizeof t; k++) ((char *)&t)[k] = 0;
            t.io = IO[ch]; t.ctl = CTL[ch]; t.drv = (uint8_t)dr; t.pos = POS[ch * 2 + dr];
            select_drive(&t);
            uint8_t st = inb(t.io + 7);
            if (st == 0xFF || st == 0x00) continue;       /* floating bus / nothing there */
            if (wait_not_busy(t.io) < 0) continue;
            /* NOT the 0x14/0xEB signature: it is only valid right after a reset - the BIOS has just used
             * this drive to boot from it, so those registers hold the last transfer's byte count.
             * IDENTIFY PACKET DEVICE (0xA1) is the reliable test: an ATAPI drive answers with 256 words,
             * an ATA disk aborts the command (ERR). */
            if (identify(&t) != 0) continue;
            g_at[g_at_n] = t;
            g_at[g_at_n].bd.name = "cd";
            g_at[g_at_n].bd.sector_size = ATAPI_SECTOR;
            g_at[g_at_n].bd.read = bd_read;
            g_at[g_at_n].bd.write = 0;                    /* read only */
            g_at[g_at_n].bd.dev_private = (void *)(uintptr_t)g_at_n;
            g_at_n++;
        }
    }
    return g_at_n;
}

int atapi_count(void) { return g_at_n; }
const char *atapi_model(int i) { return (i >= 0 && i < g_at_n) ? g_at[i].model : ""; }
const char *atapi_position(int i) { return (i >= 0 && i < g_at_n) ? g_at[i].pos : ""; }
block_device_t *atapi_block_device(int i) { return (i >= 0 && i < g_at_n) ? &g_at[i].bd : 0; }

/* one READ(10) of up to 16 sectors; true on success */
static int read_chunk(const atapi_t *a, uint32_t lba, uint32_t n, uint8_t *dst)
{
    uint8_t pkt[12] = {0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                       0, (uint8_t)(n >> 8), (uint8_t)n, 0, 0, 0};
    if (send_packet(a, pkt, ATAPI_SECTOR) != 0) return 0;
    for (uint32_t s = 0; s < n; s++) {
        int st = -1;
        for (int i = 0; i < 4000000; i++) {              /* a cold drive may spin up for a second or two */
            uint8_t v = inb(a->io + 7);
            if (!(v & ST_BSY)) { st = v; break; }
            outb(0x80, 0);
        }
        if (st < 0 || (st & ST_ERR) || !(st & ST_DRQ)) return 0;
        uint32_t bytes = (uint32_t)inb(a->io + 4) | ((uint32_t)inb(a->io + 5) << 8);
        if (bytes != ATAPI_SECTOR) return 0;
        insw(a->io, dst + s * ATAPI_SECTOR, ATAPI_SECTOR / 2);
    }
    int st = wait_not_busy(a->io);                        /* command complete: BSY=0, DRQ=0, no ERR */
    return st >= 0 && !(st & (ST_ERR | ST_DRQ));
}

int atapi_read(int i, uint32_t lba, uint32_t count, void *buf)
{
    if (i < 0 || i >= g_at_n || !buf) return -1;
    const atapi_t *a = &g_at[i];
    uint8_t *dst = (uint8_t *)buf;
    while (count) {
        uint32_t n = count > 16 ? 16 : count;
        int ok = 0;
        for (int retry = 0; retry < 4 && !ok; retry++)     /* first command after power-up: UNIT ATTENTION */
            ok = read_chunk(a, lba, n, dst);
        if (!ok) return -1;
        lba += n; count -= n; dst += n * ATAPI_SECTOR;
    }
    return 0;
}
