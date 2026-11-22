/*
 * kernel/drivers/vmsvga.c - VMware SVGA II (PCI 15AD:0405) minimal driver
 *
 * DAY43 PRE-2 (Week 7): moved here from vmwarevgadriver/ and wired into
 * the build (build.sh) and the graphics layer (kernel/drivers/vga_gfx.c).
 *
 *   - PCI scan + BAR decode
 *   - version negotiation (SVGA_ID_2)
 *   - set a w x h x 32bpp mode
 *   - bring up the command FIFO, flush with SVGA_CMD_UPDATE
 *
 * How the pieces fit (read this before touching it):
 *
 *   BAR0 (I/O)   index port = base+0, value port = base+SVGA_VALUE_OFF. Every
 *                "register" below (WIDTH, HEIGHT, BPP, ENABLE...) is
 *                reached by writing its number to the index port and
 *                the value to the value port.
 *   BAR1 (MMIO)  the linear framebuffer. Pixels are written here
 *                directly (32bpp 0x00RRGGBB, `pitch` bytes per row).
 *   BAR2 (MMIO)  the command FIFO, a ring buffer of 32-bit words. The
 *                first 4 words are the header {MIN, MAX, NEXT_CMD,
 *                STOP}: we (the producer) advance NEXT_CMD, the host
 *                (the consumer) advances STOP.
 *
 *   Writing pixels is not enough on a virtual display: the host only
 *   re-reads the framebuffer region it has been told about. That is
 *   what SVGA_CMD_UPDATE (x,y,w,h) in the FIFO is for.
 *
 * Precondition: BAR1/BAR2 physical addresses are directly addressable
 * (LumaOS runs flat, no paging yet - same assumption vga_gfx.c makes
 * for the Bochs LFB). Day 53 (paging) must identity-map them.
 *
 * Changes versus the vmwarevgadriver/ sample:
 *   - 64-bit multiply replaced by 32-bit math (no libgcc dependency
 *     in a -ffreestanding -m32 kernel);
 *   - vmsvga_init() validates what the device actually gave back
 *     (bpp == 32, pitch != 0) instead of trusting the request;
 *   - vmsvga_update() is a no-op before init (it used to dereference a
 *     NULL FIFO pointer);
 *   - vmsvga_active() added for the graphics layer.
 */
#include "vmsvga.h"

/* ---------- port I/O (prefixed so they never clash with kernel.h's) ---------- */
static inline void vm_outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint32_t vm_inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

/* ---------- PCI ---------- */
#define PCI_VENDOR_VMWARE 0x15AD
#define PCI_DEVICE_SVGA2  0x0405

static uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off)
{
    vm_outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (off & 0xFC));
    return vm_inl(0xCFC);
}
static void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v)
{
    vm_outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (off & 0xFC));
    vm_outl(0xCFC, v);
}
static int pci_find(uint8_t *B, uint8_t *D, uint8_t *F)
{
    for (uint32_t b = 0; b < 32; b++)
        for (uint32_t d = 0; d < 32; d++)
            for (uint32_t f = 0; f < 8; f++) {
                uint32_t id = pci_read32((uint8_t)b, (uint8_t)d, (uint8_t)f, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; else continue; }
                if ((id & 0xFFFF) == PCI_VENDOR_VMWARE && (id >> 16) == PCI_DEVICE_SVGA2) {
                    *B = (uint8_t)b; *D = (uint8_t)d; *F = (uint8_t)f; return 0;
                }
            }
    return -1;
}

/* ---------- SVGA registers ---------- */
#define SVGA_ID_2          0x90000002u
#define SVGA_CAP_EXT_FIFO  0x00008000u

enum {
    REG_ID = 0, REG_ENABLE = 1, REG_WIDTH = 2, REG_HEIGHT = 3,
    REG_MAX_WIDTH = 4, REG_MAX_HEIGHT = 5, REG_DEPTH = 6, REG_BPP = 7,
    REG_BYTES_PER_LINE = 12, REG_FB_START = 13, REG_FB_OFFSET = 14,
    REG_VRAM_SIZE = 15, REG_FB_SIZE = 16, REG_CAPS = 17,
    REG_MEM_START = 18, REG_MEM_SIZE = 19, REG_CONFIG_DONE = 20,
    REG_SYNC = 21, REG_BUSY = 22, REG_MEM_REGS = 30
};
/* FIFO header: 4 dwords */
enum { FIFO_MIN = 0, FIFO_MAX = 1, FIFO_NEXT_CMD = 2, FIFO_STOP = 3 };
#define SVGA_CMD_UPDATE 1u

/* value port offset from BAR0 (SVGA_VALUE_PORT = 1 in svga_reg.h) */
#define SVGA_VALUE_OFF 1
static uint16_t io_base;
static volatile uint32_t *fifo;
static uint32_t cur_w, cur_h;
static int      g_active;

/* index port = base+0, value port = base+SVGA_VALUE_OFF */
static void     wreg(uint32_t i, uint32_t v) { vm_outl(io_base, i); vm_outl((uint16_t)(io_base + SVGA_VALUE_OFF), v); }
static uint32_t rreg(uint32_t i)             { vm_outl(io_base, i); return vm_inl((uint16_t)(io_base + SVGA_VALUE_OFF)); }

int vmsvga_detect(void)
{
    uint8_t b, d, f;
    return pci_find(&b, &d, &f);
}

int vmsvga_active(void) { return g_active; }

int vmsvga_init(uint32_t w, uint32_t h, vmsvga_info_t *out)
{
    uint8_t b, d, f;
    if (pci_find(&b, &d, &f)) return -1;

    /* Enable I/O space + memory space + bus mastering. The high 16
     * bits of this dword are the Status register (write-1-to-clear),
     * so they must be masked off. */
    uint32_t cmd = pci_read32(b, d, f, 0x04);
    pci_write32(b, d, f, 0x04, (cmd & 0xFFFF) | 0x7);

    uint32_t bar0 = pci_read32(b, d, f, 0x10);  /* I/O ports   */
    uint32_t bar1 = pci_read32(b, d, f, 0x14);  /* framebuffer */
    uint32_t bar2 = pci_read32(b, d, f, 0x18);  /* FIFO        */
    io_base = (uint16_t)(bar0 & 0xFFFCu);
    uint32_t fb_phys   = bar1 & 0xFFFFFFF0u;
    uint32_t fifo_phys = bar2 & 0xFFFFFFF0u;

    /* Version negotiation: write ID_2, the device echoes it back only
     * if it speaks that protocol. */
    wreg(REG_ID, SVGA_ID_2);
    if (rreg(REG_ID) != SVGA_ID_2) return -2;

    if (w > rreg(REG_MAX_WIDTH) || h > rreg(REG_MAX_HEIGHT)) return -3;
    uint32_t vram = rreg(REG_VRAM_SIZE);
    if (vram && w * h * 4u > vram) return -4;

    /* Mode */
    wreg(REG_WIDTH, w);
    wreg(REG_HEIGHT, h);
    wreg(REG_BPP, 32);

    /* ENABLE first, then FIFO setup, then CONFIG_DONE (same order as
     * Linux's vmwgfx). */
    wreg(REG_ENABLE, 1);

    uint32_t caps = rreg(REG_CAPS);
    uint32_t regs = 4;
    if (caps & SVGA_CAP_EXT_FIFO) {
        regs = rreg(REG_MEM_REGS);
        if (regs < 4) regs = 4;
    }
    uint32_t fifo_size = rreg(REG_MEM_SIZE);
    fifo = (volatile uint32_t *)fifo_phys;
    fifo[FIFO_MIN]      = regs * 4;
    fifo[FIFO_MAX]      = fifo_size;
    fifo[FIFO_NEXT_CMD] = regs * 4;
    fifo[FIFO_STOP]     = regs * 4;
    __asm__ volatile("" ::: "memory");
    wreg(REG_CONFIG_DONE, 1);

    /* Trust what the device reports, not what we asked for. */
    uint32_t got_bpp   = rreg(REG_BPP);
    uint32_t got_pitch = rreg(REG_BYTES_PER_LINE);
    if (got_bpp != 32 || got_pitch == 0) { fifo = (volatile uint32_t *)0; return -5; }

    cur_w = w; cur_h = h;
    g_active = 1;
    if (out) {
        out->fb     = (uint8_t *)(fb_phys + rreg(REG_FB_OFFSET));
        out->width  = rreg(REG_WIDTH);
        out->height = rreg(REG_HEIGHT);
        out->bpp    = got_bpp;
        out->pitch  = got_pitch;
    }
    return 0;
}

void vmsvga_update(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g_active || !fifo) return;

    uint32_t cmd[5] = { SVGA_CMD_UPDATE, x, y, w, h };
    uint32_t min  = fifo[FIFO_MIN];
    uint32_t max  = fifo[FIFO_MAX];
    uint32_t next = fifo[FIFO_NEXT_CMD];

    /* Wait until the ring has room for 20 bytes + one dword of slack. */
    for (int spin = 0; ; spin++) {
        uint32_t stop = fifo[FIFO_STOP];
        uint32_t used = (next >= stop) ? (next - stop) : ((max - min) - (stop - next));
        uint32_t freeb = (max - min) - used;
        if (freeb > sizeof(cmd) + 4) break;
        wreg(REG_SYNC, 1);
        for (int i = 0; i < 1000000 && rreg(REG_BUSY); i++) { }
        if (spin > 100) return;          /* give up rather than hang the GUI */
    }

    for (int i = 0; i < 5; i++) {
        fifo[next >> 2] = cmd[i];
        next += 4;
        if (next >= max) next = min;
    }
    __asm__ volatile("" ::: "memory");
    fifo[FIFO_NEXT_CMD] = next;          /* publish only after the whole command is written */
}

void vmsvga_flush(void)
{
    vmsvga_update(0, 0, cur_w, cur_h);
}
