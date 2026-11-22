/* kernel/drivers/vga_gfx.c
 * LumaOS 5.0 - Bochs VBE (BGA) Graphics Driver
 *
 * DAY43 PRE-4: TRUE TRANSLUCENCY on an indexed framebuffer ("blend palette").
 *   The whole GUI draws palette INDICES into an 8bpp back buffer, so a
 *   pixel cannot hold "50% of colour A over colour B". vga_blend_color()
 *   fixes that without touching the pipeline: it returns the index of a
 *   palette entry whose RGB IS that mix, allocating one on demand from a
 *   pool (indices 64..255, the GUI itself only uses 0..63). The same
 *   entry is written to the VGA DAC (BGA) or to pal32[] (VMware SVGA), so
 *   it works identically on both backends. A (under, over, alpha) triple
 *   is cached, and when the theme reprograms the base palette every pool
 *   entry is recomputed in place, so no pixel ever shows a stale colour.
 *   vga_blend_rect() uses it to blend a rectangle with what is already
 *   in the back buffer (translucent title bars, soft window shadows).
 *
 * DAY43 PRE-2 (Week 7): VMware SVGA II backend added.
 *   vga_init_graphics() now probes for a VMware SVGA II device first
 *   (kernel/drivers/vmsvga.c; VMware Workstation/Player/Fusion, QEMU
 *   `-vga vmware`). If it is there and accepts 800x450x32 that backend
 *   is used, otherwise the original BGA 800x450x8 path below runs
 *   unchanged (QEMU `-vga std`, Bochs, VirtualBox "VBoxVGA").
 *
 *   Why the GUI code did not have to change: everything above this
 *   layer (gui, shell, write overlay, font code) draws into the 8bpp
 *   back buffer with palette INDICES through vga_put_pixel/draw_rect/
 *   vga_set_palette/vga_flip. The two backends differ only in the last
 *   step, vga_flip():
 *     BGA   : copy the 8bpp back buffer into the 8bpp LFB; the
 *             hardware DAC turns index -> colour.
 *     SVGA  : the framebuffer is 32bpp, there is no DAC, so we keep a
 *             software copy of the palette (pal32[]; vga_set_palette()
 *             fills it), convert only the rows that changed since the
 *             previous frame (compared against a shadow of the last
 *             frame at 0x00400000) and then send ONE SVGA_CMD_UPDATE
 *             for the changed band.  A theme switch reprograms the
 *             palette, which forces the next flip to convert every row.
 *
 *   Memory map (flat, below the 256 MB QEMU gives us):
 *     0x00010000 kernel   0x00200000 stack top   0x00300000 back buffer
 *     0x00400000 shadow of the last frame (SVGA backend only, 360 KB)
 *
 * STAGE1-GFX: Replaced VGA Mode 13h (320x200) with Bochs VBE BGA interface.
 *   Resolution : 800x450, 8bpp (256-color palette), linear framebuffer.
 *   Back buffer : 0x00300000 (3 MB mark, 360 KB required).
 *
 * FIX GFX-1 (CRITICAL — black screen root cause):
 *   The LFB physical address is NOT a fixed constant. The previous code
 *   hard-coded 0xE0000000, which is only correct when QEMU treats the
 *   Bochs display as an ISA-only device. With "-machine type=pc" (the
 *   i440FX/PIIX chipset used by run.sh / run_acpi.sh), the Bochs VGA
 *   device sits on PCI, and SeaBIOS assigns its framebuffer base (BAR0)
 *   dynamically — commonly something like 0xFD000000, NOT 0xE0000000.
 *
 *   Symptom this caused: BGA was correctly configured (800x450 8bpp),
 *   the palette was correctly programmed, vga_flip() ran every frame
 *   with no crash or hang — but every copy went to an address that was
 *   never mapped to real video memory, so nothing ever appeared on
 *   screen. Total black screen with an otherwise fully healthy kernel.
 *
 *   Fix: read the real framebuffer base from PCI config space (BAR0 of
 *   vendor 0x1234 / device 0x1111, the standard Bochs/QEMU VGA ID) at
 *   graphics-init time. Fall back to the legacy 0xE0000000 constant
 *   only if no such PCI device is found (e.g. real hardware or an
 *   ISA-only emulator without a PCI bus).
 *
 * BGA I/O ports:
 *   0x01CE = VBE_DISPI_IOPORT_INDEX
 *   0x01CF = VBE_DISPI_IOPORT_DATA
 *
 * Index registers (write index to 0x01CE, value to 0x01CF):
 *   0x00 = VBE_DISPI_INDEX_ID            (read: 0xB0C0..0xB0C5)
 *   0x01 = VBE_DISPI_INDEX_XRES
 *   0x02 = VBE_DISPI_INDEX_YRES
 *   0x03 = VBE_DISPI_INDEX_BPP
 *   0x04 = VBE_DISPI_INDEX_ENABLE
 *   0x05 = VBE_DISPI_INDEX_BANK
 *   0x06 = VBE_DISPI_INDEX_VIRT_WIDTH
 *   0x07 = VBE_DISPI_INDEX_VIRT_HEIGHT
 *   0x08 = VBE_DISPI_INDEX_X_OFFSET
 *   0x09 = VBE_DISPI_INDEX_Y_OFFSET
 *
 * VBE_DISPI_ENABLED  = 0x01
 * VBE_DISPI_LFB_ENABLED = 0x40
 * VBE_DISPI_NOCLEARMEM  = 0x80
 *
 * IMPORTANT: Build must link with -m elf_i386; back buffer at 3MB is
 * above kernel (~50-80 KB at 0x10000) and above the 2MB kernel stack.
 * The LFB (wherever PCI places it) is in high physical memory; the GDT
 * flat 4 GB data segment (base=0, limit=0xFFFFFFFF) covers it with a
 * plain 32-bit pointer, so no paging/segmentation changes are needed.
 */

#include "kernel.h"
#include "vmsvga.h"
#include <stdint.h>

/* ===== Screen geometry ===== */
/* DAY58b: the resolution is a RUNTIME value now (vga_set_resolution()).
 * The back buffer at 0x300000 has room for 0x100000 bytes (up to
 * VGA_MAX_PIXELS), the SVGA shadow frame at 0x400000 the same. */
static int g_vw = 800, g_vh = 450;
#define VGA_WIDTH    g_vw
#define VGA_HEIGHT   g_vh
#define VGA_MAX_PIXELS 1048576
#define VGA_BPP      8
#define VGA_STRIDE   VGA_WIDTH               /* bytes per row (8bpp) */
#define VGA_SIZE     (VGA_WIDTH * VGA_HEIGHT) /* 360 000 bytes */

/* ===== BGA ports & constants ===== */
#define BGA_IDX   0x01CE
#define BGA_DATA  0x01CF

#define BGA_ID           0x00
#define BGA_XRES         0x01
#define BGA_YRES         0x02
#define BGA_BPP          0x03
#define BGA_ENABLE       0x04
#define BGA_BANK         0x05
#define BGA_VIRT_W       0x06
#define BGA_VIRT_H       0x07
#define BGA_X_OFFSET     0x08
#define BGA_Y_OFFSET     0x09

#define BGA_DISABLED         0x00
#define BGA_ENABLED          0x01
#define BGA_LFB_ENABLED      0x40
#define BGA_NOCLEARMEM       0x80

/* ===== NEW (FIX GFX-1): minimal PCI config-space access =====
 * Just enough to find BAR0 of the Bochs/QEMU standard VGA device —
 * not a general-purpose PCI driver.                                   */
#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

static inline uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  << 8)
                  | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

/* Scan bus 0, all 32 device slots, function 0, for the Bochs/QEMU
 * standard VGA device (vendor 0x1234, device 0x1111) and return its
 * BAR0 (LFB physical base), masked to a clean page-aligned address.
 * Returns 0 if the device is not found on bus 0.                      */
static uint32_t pci_find_bochs_lfb(void)
{
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_cfg_read32(0, dev, 0, 0x00);
        if (id == 0xFFFFFFFFu) continue;              /* no device here */
        uint16_t vendor = (uint16_t)(id & 0xFFFF);
        uint16_t device = (uint16_t)(id >> 16);
        if (vendor == 0x1234 && device == 0x1111) {   /* Bochs/QEMU std VGA */
            uint32_t bar0 = pci_cfg_read32(0, dev, 0, 0x10);
            /* Mask off low info bits (bit0=space, bits1-2=type,
             * bit3=prefetchable) to get the actual base address.      */
            return bar0 & 0xFFFFFFF0u;
        }
    }
    return 0;
}

/* ===== NEW (FIX GFX-1): minimal serial (COM1) debug output =====
 * Used only for pre-GUI diagnostics, since text-mode console output
 * is still available at this point but graphics output is not yet
 * verified. Safe to leave in permanently — costs nothing if nobody
 * is listening on the port.                                           */
#define COM1 0x3F8

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s)
{
    while (*s) serial_putc(*s++);
}

static void serial_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_putc(hex[(v >> i) & 0xF]);
}

/* ===== Linear framebuffer (physical) =====
 * FIX GFX-1: no longer `const` — the real value is discovered at
 * runtime in vga_init_graphics(). Falls back to the legacy ISA
 * address if PCI probing finds nothing.                                */
static unsigned char *vga_mem = (unsigned char *)0xE0000000;

/* ===== Back buffer (in normal RAM at 3 MB) ===== */
/* 360 000 bytes.  entry.asm sets esp = 0x00200000 (2 MB, top of the
 * kernel stack in BSS). The back buffer lives at 0x00300000 (3 MB) to
 * stay well clear of the stack.                                       */
static unsigned char * const back_buffer = (unsigned char *)0x00300000;

/* ===== DAY43 PRE-2: VMware SVGA backend state ===== */
static int            g_svga        = 0;   /* 1 = VMware SVGA II backend active */
static uint8_t       *svga_fb       = 0;   /* 32bpp framebuffer */
static uint32_t       svga_pitch    = 0;   /* bytes per framebuffer row */
static uint32_t       pal32[256];          /* index -> 0x00RRGGBB (software DAC) */
static int            svga_force_full = 1; /* next flip converts every row */
static unsigned       svga_flips    = 0;
static struct { uint32_t frames, rows_written; } gfx_stats;   /* DAY67: BGA/SVGA 每帧写了多少行 */
/* shadow copy of the last frame sent to the host (8bpp, same layout as
 * back_buffer). Fixed address, NOT .bss: the kernel's BSS is already
 * ~290 KB above a 0x10000 load address and another 360 KB would run into
 * the 0xA0000 VGA/BIOS hole. */
static uint8_t * const prev_frame = (uint8_t *)0x00400000;

/* ===== BGA helper ===== */
static inline void bga_write(uint16_t index, uint16_t value)
{
    outw(BGA_IDX,  index);
    outw(BGA_DATA, value);
}

/* ===== DAY43 PRE-4 / DAY58b: software palette (6-bit) + blend pool =====
 *
 * Indices 0..15 VGA colours, 16..56 theme colours (set by the GUI), and
 * BLEND_FIRST..255 the BLEND POOL: palette entries that hold "colour A seen
 * through colour B at alpha" so translucent drawing stays pixel exact on an
 * 8-bit screen.
 *
 * DAY58b rework (wallpaper turned wrong colours after long use):
 *   1. The pool used to start at 64 and was never emptied: every theme
 *      switch left the old theme's entries behind until the pool was full,
 *      and a full pool answered with `under` or `over` - i.e. a flat,
 *      wrong colour. Now vga_blend_reset() empties it (the GUI calls it
 *      when the theme changes; a complete redraw follows).
 *   2. 57..255 = 199 entries (was 192) - the 7 unused indices joined the pool.
 *   3. DEDUP: a request first computes the wanted RGB; an existing entry
 *      within +-1 DAC step per channel (invisible) is reused. Shadows over 16
 *      neighbouring wallpaper bands now share a handful of entries.
 *   4. FULL POOL: the NEAREST colour of the whole palette (RGB distance) is
 *      used, never an unrelated one.
 *   A "virtual" table (VT_MAX triples -> physical entry) caches every answer
 *   so the drawing loops stay one hash probe per colour change. */
#define BLEND_FIRST 57
#define BLEND_MAX   (256 - BLEND_FIRST)
#define VT_MAX      1024
static uint8_t pal_rgb[256][3];          /* 6-bit R,G,B of every index */
static uint8_t bl_under[BLEND_MAX], bl_over[BLEND_MAX], bl_alpha[BLEND_MAX];   /* owner triple of each physical entry */
static int     bl_n = 0;
typedef struct { uint8_t under, over, alpha, phys; } vt_t;
static vt_t    vt[VT_MAX];
static uint16_t vt_hash[2048];           /* triple hash -> vt index + 1 */
static int     vt_n = 0;

static inline int abs(int v) { return v < 0 ? -v : v; }

static void pal_program(unsigned char index, unsigned char r,
                        unsigned char g, unsigned char b)
{
    uint32_t r8 = ((uint32_t)(r & 63) << 2) | ((r & 63) >> 4);
    uint32_t g8 = ((uint32_t)(g & 63) << 2) | ((g & 63) >> 4);
    uint32_t b8 = ((uint32_t)(b & 63) << 2) | ((b & 63) >> 4);
    pal32[index] = (r8 << 16) | (g8 << 8) | b8;
    if (g_svga) return;                  /* no VGA DAC to program */
    outb(0x3C8, index);
    outb(0x3C9, r);
    outb(0x3C9, g);
    outb(0x3C9, b);
}

static void blend_rgb(int u, int o, int a, unsigned char c[3])
{
    for (int i = 0; i < 3; i++)
        c[i] = (unsigned char)((pal_rgb[u][i] * (255 - a) + pal_rgb[o][i] * a + 127) / 255);
}

static void blend_compute(int k)
{
    unsigned char c[3];
    blend_rgb(bl_under[k], bl_over[k], bl_alpha[k], c);
    int idx = BLEND_FIRST + k;
    pal_rgb[idx][0] = c[0]; pal_rgb[idx][1] = c[1]; pal_rgb[idx][2] = c[2];
    pal_program((unsigned char)idx, c[0], c[1], c[2]);
}

static void vt_flush(void)
{
    vt_n = 0;
    for (int i = 0; i < 2048; i++) vt_hash[i] = 0;
}

/* Empty the pool (theme change). Pixels already in the back buffer that
 * name pool entries become stale, so the caller redraws the whole frame. */
void vga_blend_reset(void)
{
    bl_n = 0;
    vt_flush();
}

static int rgb_dist(const unsigned char *a, const unsigned char *b)
{
    int d = 0;
    for (int i = 0; i < 3; i++) { int e = (int)a[i] - (int)b[i]; d += e * e; }
    return d;
}

/* vga_blend_color(): palette index showing `over` at weight alpha/255 on
 * top of `under` (see the block comment above for the policy). */
unsigned char vga_blend_color(unsigned char under, unsigned char over, int alpha)
{
    if (alpha <= 0)   return under;
    if (alpha >= 255) return over;
    unsigned slot = ((unsigned)under * 131u + (unsigned)over * 31u + (unsigned)alpha * 7u) & 2047u;
    for (int probe = 0; probe < 4; probe++) {
        int h = vt_hash[(slot + probe) & 2047] - 1;
        if (h < 0) break;
        if (vt[h].under == under && vt[h].over == over && vt[h].alpha == alpha)
            return vt[h].phys;
    }

    unsigned char want[3];
    blend_rgb(under, over, alpha, want);
    int phys = -1;

    /* 1. an existing pool entry that is visually identical (+-1 per channel) */
    for (int k = 0; k < bl_n && phys < 0; k++) {
        const unsigned char *c = pal_rgb[BLEND_FIRST + k];
        if (abs((int)c[0] - want[0]) <= 1 && abs((int)c[1] - want[1]) <= 1 && abs((int)c[2] - want[2]) <= 1)
            phys = BLEND_FIRST + k;
    }
    /* 2. a fixed palette colour that already is that colour */
    for (int i = 0; i < BLEND_FIRST && phys < 0; i++)
        if (pal_rgb[i][0] == want[0] && pal_rgb[i][1] == want[1] && pal_rgb[i][2] == want[2])
            phys = i;
    /* 3. a new entry */
    if (phys < 0 && bl_n < BLEND_MAX) {
        int k = bl_n++;
        bl_under[k] = under; bl_over[k] = over; bl_alpha[k] = (uint8_t)alpha;
        blend_compute(k);
        phys = BLEND_FIRST + k;
    }
    /* 4. pool full: the nearest colour of the whole palette */
    if (phys < 0) {
        int best = 1 << 30;
        for (int i = 0; i < 256; i++) {
            if (i >= BLEND_FIRST + bl_n) break;
            int d = rgb_dist(pal_rgb[i], want);
            if (d < best) { best = d; phys = i; }
        }
    }

    if (vt_n >= VT_MAX) vt_flush();       /* rare: start the cache over, answers are recomputed */
    int v = vt_n++;
    vt[v].under = under; vt[v].over = over; vt[v].alpha = (uint8_t)alpha; vt[v].phys = (uint8_t)phys;
    for (int probe = 0; probe < 4; probe++) {
        unsigned hs = (slot + probe) & 2047;
        if (vt_hash[hs] == 0 || probe == 3) { vt_hash[hs] = (uint16_t)(v + 1); break; }
    }
    return (unsigned char)phys;
}

/* DAY58c: like vga_blend_color() but WITHOUT the +-1 merge. The wallpaper
 * gradient needs every neighbouring 6-bit level as its own palette entry:
 * merging them is what made the visible horizontal bands. Not cached (the
 * caller builds a small table once per frame). */
unsigned char vga_blend_color_exact(unsigned char under, unsigned char over, int alpha)
{
    if (alpha <= 0)   return under;
    if (alpha >= 255) return over;
    unsigned char want[3];
    blend_rgb(under, over, alpha, want);
    for (int k = 0; k < bl_n; k++) {
        const unsigned char *c = pal_rgb[BLEND_FIRST + k];
        if (c[0] == want[0] && c[1] == want[1] && c[2] == want[2]) return (unsigned char)(BLEND_FIRST + k);
    }
    for (int i = 0; i < BLEND_FIRST; i++)
        if (pal_rgb[i][0] == want[0] && pal_rgb[i][1] == want[1] && pal_rgb[i][2] == want[2])
            return (unsigned char)i;
    if (bl_n < BLEND_MAX) {
        int k = bl_n++;
        bl_under[k] = under; bl_over[k] = over; bl_alpha[k] = (uint8_t)alpha;
        blend_compute(k);
        return (unsigned char)(BLEND_FIRST + k);
    }
    int best = 1 << 30, phys = under;
    for (int i = 0; i < BLEND_FIRST + bl_n; i++) {
        int d = rgb_dist(pal_rgb[i], want);
        if (d < best) { best = d; phys = i; }
    }
    return (unsigned char)phys;
}

/* One scan line made of a repeating 4-pixel pattern (ordered dithering). */
void vga_hline_pat4(int y, const unsigned char *pat)
{
    if (y < 0 || y >= VGA_HEIGHT) return;
    unsigned char *p = back_buffer + y * VGA_STRIDE;
    for (int x = 0; x < VGA_WIDTH; x++) p[x] = pat[x & 3];
}

int vga_gfx_blend_used(void) { return bl_n; }
int vga_gfx_blend_max(void)  { return BLEND_MAX; }

/* ===== vga_set_palette ===== */
/* VGA DAC (palette) registers are still present on the Bochs VGA device
 * even in LFB mode.  8bpp indexed color uses the same DAC as Mode 13h. */
void vga_set_palette(unsigned char index, unsigned char r,
                     unsigned char g, unsigned char b)
{
    /* DAY43 PRE-2: always keep the software copy (6-bit VGA DAC value
     * 0..63 -> 8-bit: v<<2 | v>>4, so 63 -> 255). The SVGA backend has
     * no DAC and converts through this table in vga_flip(). */
    pal_rgb[index][0] = r & 63; pal_rgb[index][1] = g & 63; pal_rgb[index][2] = b & 63;
    pal_program(index, r, g, b);
    svga_force_full = 1;                 /* colours changed: repaint everything */

    /* DAY43 PRE-4: a base colour changed -> every blend entry that mixes it
     * must follow, otherwise translucent pixels keep the old theme's tint. */
    if (index < BLEND_FIRST) {
        for (int k = 0; k < bl_n; k++) blend_compute(k);
        vt_flush();                      /* aliases were chosen with the old colours */
    }
}

/* vga_blend_rect(): blend `over` at alpha/255 onto whatever the back
 * buffer already holds in the rectangle. Pixel-exact translucency. */
void vga_blend_rect(int x, int y, int w, int h, unsigned char over, int alpha)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VGA_WIDTH)  w = VGA_WIDTH  - x;
    if (y + h > VGA_HEIGHT) h = VGA_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        unsigned char *p = back_buffer + (y + row) * VGA_STRIDE + x;
        unsigned char last_u = p[0], last_r = vga_blend_color(p[0], over, alpha);
        for (int col = 0; col < w; col++) {
            unsigned char u = p[col];
            if (u != last_u) { last_u = u; last_r = vga_blend_color(u, over, alpha); }
            p[col] = last_r;
        }
    }
}

/* ===== vga_init_graphics ===== */
void vga_init_graphics(void)
{
    /* FIX GFX-1: probe PCI for the real framebuffer address before
     * touching anything else. Serial output lets you confirm what
     * address was actually used even though nothing is on screen yet. */
    serial_init();
    serial_puts("[vga_gfx] probing display hardware (VMware SVGA II, then Bochs BGA)...\r\n");

    /* DAY43 PRE-2: VMware SVGA II first. Probe/initialise it; on any
     * failure fall through to the Bochs VBE path below. */
    g_svga = 0;
    if (vmsvga_detect() == 0) {
        vmsvga_info_t si;
        int rc = vmsvga_init(VGA_WIDTH, VGA_HEIGHT, &si);
        if (rc == 0 && si.width == VGA_WIDTH && si.height == VGA_HEIGHT) {
            g_svga     = 1;
            svga_fb    = si.fb;
            svga_pitch = si.pitch;
            svga_force_full = 1;
            serial_puts("[vga_gfx] backend = VMware SVGA II 800x450x32, fb=");
            serial_hex32((uint32_t)si.fb);
            serial_puts(" pitch=");
            serial_hex32(si.pitch);
            serial_puts("\r\n");
        } else {
            serial_puts("[vga_gfx] VMware SVGA II found but init failed (rc=");
            serial_hex32((uint32_t)rc);
            serial_puts("), falling back to BGA\r\n");
        }
    }

    uint32_t lfb = g_svga ? 0 : pci_find_bochs_lfb();
    if (!g_svga) {
    if (lfb != 0) {
        vga_mem = (unsigned char *)lfb;
        serial_puts("[vga_gfx] found BAR0 = ");
        serial_hex32(lfb);
        serial_puts("  -> using as LFB\r\n");
    } else {
        serial_puts("[vga_gfx] PCI probe found no Bochs VGA device, "
                     "falling back to 0xE0000000\r\n");
    }

    /* 1. Disable VBE extension while we configure */
    bga_write(BGA_ENABLE, BGA_DISABLED);

    /* 2. Set resolution and colour depth */
    bga_write(BGA_XRES, (uint16_t)VGA_WIDTH);
    bga_write(BGA_YRES, (uint16_t)VGA_HEIGHT);
    bga_write(BGA_BPP,  (uint16_t)VGA_BPP);

    /* 3. Virtual size == physical size (no double-height trick needed) */
    bga_write(BGA_VIRT_W, (uint16_t)VGA_WIDTH);
    bga_write(BGA_VIRT_H, (uint16_t)VGA_HEIGHT);
    bga_write(BGA_X_OFFSET, 0);
    bga_write(BGA_Y_OFFSET, 0);

    /* 4. Enable LFB mode (do NOT clear mem so we can prefill palette) */
    bga_write(BGA_ENABLE, BGA_ENABLED | BGA_LFB_ENABLED);
    }   /* !g_svga */

    /* 5. Set up the 256-colour palette (same values as old Mode 13h) */
    /* Standard 16 CGA colours (indices 0-15) */
    vga_set_palette( 0,  0,  0,  0);
    vga_set_palette( 1,  0,  0, 42);
    vga_set_palette( 2,  0, 42,  0);
    vga_set_palette( 3,  0, 42, 42);
    vga_set_palette( 4, 42,  0,  0);
    vga_set_palette( 5, 42,  0, 42);
    vga_set_palette( 6, 42, 21,  0);
    vga_set_palette( 7, 42, 42, 42);
    vga_set_palette( 8, 21, 21, 21);
    vga_set_palette( 9, 21, 21, 63);
    vga_set_palette(10, 21, 63, 21);
    vga_set_palette(11, 21, 63, 63);
    vga_set_palette(12, 63, 21, 21);
    vga_set_palette(13, 63, 21, 63);
    vga_set_palette(14, 63, 63, 21);
    vga_set_palette(15, 63, 63, 63);

    /* Extended GUI palette (indices 16-35) — filled by gui_set_palette().
     * We zero them here so the screen is black on first flip.          */
    for (int i = 16; i < 256; i++)
        vga_set_palette((unsigned char)i, 0, 0, 0);

    /* 6. Clear back buffer to colour 0 (black) */
    for (int i = 0; i < VGA_SIZE; i++)
        back_buffer[i] = 0;

    serial_puts("[vga_gfx] init complete\r\n");
}

/* ===== svga_flip (DAY43 PRE-2) =====
 * 8bpp back buffer -> 32bpp VMware framebuffer, only for rows that
 * differ from the previous frame (or all rows after a palette change),
 * then a single UPDATE command covering the changed band of rows. */
static void svga_flip(void)
{
    const uint8_t *src  = back_buffer;
    uint8_t       *prev = prev_frame;
    int y0 = -1, y1 = -1;

    for (int y = 0; y < VGA_HEIGHT; y++) {
        const uint8_t  *srow = src  + y * VGA_STRIDE;
        uint8_t        *prow = prev + y * VGA_STRIDE;
        if (!svga_force_full) {
            const uint32_t *s32 = (const uint32_t *)srow;
            const uint32_t *p32 = (const uint32_t *)prow;
            int same = 1;
            for (int i = 0; i < VGA_STRIDE / 4; i++)
                if (s32[i] != p32[i]) { same = 0; break; }
            if (same) continue;
        }
        uint32_t *dst = (uint32_t *)(svga_fb + (uint32_t)y * svga_pitch);
        for (int x = 0; x < VGA_WIDTH; x++) {
            unsigned char c = srow[x];
            dst[x]  = pal32[c];
            prow[x] = c;
        }
        if (y0 < 0) y0 = y;
        y1 = y;
        gfx_stats.rows_written++;
    }
    svga_force_full = 0;
    svga_flips++;
    gfx_stats.frames++;

    if (y0 >= 0)
        vmsvga_update(0, (uint32_t)y0, VGA_WIDTH, (uint32_t)(y1 - y0 + 1));
    else if ((svga_flips & 63u) == 0)
        vmsvga_flush();      /* nothing changed: still re-announce the whole
                              * screen now and then in case the host dropped one */
}

/* ===== vga_flip ===== */
/* BGA: copy back buffer -> LFB (800*450 = 360 000 bytes, 32 bits at a
 * time). SVGA: see svga_flip() above.                                  */
void vga_flip(void)
{
    if (g_svga) { svga_flip(); return; }

    /* DAY67: 只把变化的行写进 LFB。
     * 以前（DAY57）每帧都用一条 rep movsl 把 360 000 字节整屏写进 LFB。LFB 是显存（PCI 设备内存，不走
     * 缓存），写它比读普通 RAM 慢一个数量级；而桌面大多数帧只有光标/一个窗口的几行在变。
     * 做法：back_buffer 与上一帧的副本 prev_frame（和 SVGA 后端共用）逐行比较（读的是普通 RAM，很快），
     * 只有不同的行才 rep movsl 到 LFB 并更新副本。调色板改变/重新设置模式时 svga_force_full = 1，
     * 下一帧整屏重写（BGA 的调色板在 DAC 里，其实不需要，但模式切换会清显存，必须整屏重画）。
     * 效果由 `gfxstat` 命令显示（每帧平均写了多少行）。 */
    const int row_dw = VGA_STRIDE / 4;
    gfx_stats.frames++;
    for (int y = 0; y < VGA_HEIGHT; y++) {
        const uint32_t *s32 = (const uint32_t *)(back_buffer + y * VGA_STRIDE);
        uint32_t       *p32 = (uint32_t *)(prev_frame + y * VGA_STRIDE);
        if (!svga_force_full) {
            int same = 1;
            for (int i = 0; i < row_dw; i++)
                if (s32[i] != p32[i]) { same = 0; break; }
            if (same) continue;
        }
        uint32_t       *dst = (uint32_t *)(vga_mem + y * VGA_STRIDE);
        const uint32_t *src = s32;
        int n = row_dw;
        asm volatile("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
        for (int i = 0; i < row_dw; i++) p32[i] = s32[i];
        gfx_stats.rows_written++;
    }
    svga_force_full = 0;
}

void vga_gfx_stats(uint32_t *frames, uint32_t *rows_written, uint32_t *rows_total)
{
    if (frames) *frames = gfx_stats.frames;
    if (rows_written) *rows_written = gfx_stats.rows_written;
    if (rows_total) *rows_total = gfx_stats.frames * (uint32_t)VGA_HEIGHT;
}
void vga_gfx_stats_reset(void) { gfx_stats.frames = gfx_stats.rows_written = 0; }

/* ===== vga_clear_screen ===== */
void vga_clear_screen(unsigned char color)
{
    /* Fill all 4 bytes at once */
    uint32_t val = (uint32_t)color
                 | ((uint32_t)color << 8)
                 | ((uint32_t)color << 16)
                 | ((uint32_t)color << 24);
    uint32_t *buf = (uint32_t *)back_buffer;
    for (int i = 0; i < (VGA_SIZE / 4); i++)
        buf[i] = val;
}

/* ===== vga_put_pixel ===== */
void vga_put_pixel(int x, int y, unsigned char color)
{
    if ((unsigned)x >= VGA_WIDTH || (unsigned)y >= VGA_HEIGHT) return;
    back_buffer[y * VGA_STRIDE + x] = color;
}

/* ===== vga_get_pixel ===== */
unsigned char vga_get_pixel(int x, int y)
{
    if ((unsigned)x >= VGA_WIDTH || (unsigned)y >= VGA_HEIGHT) return 0;
    return back_buffer[y * VGA_STRIDE + x];
}

/* ===== vga_draw_rect ===== */
void vga_draw_rect(int x, int y, int w, int h, unsigned char color)
{
    /* Clip */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VGA_WIDTH)  w = VGA_WIDTH  - x;
    if (y + h > VGA_HEIGHT) h = VGA_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        unsigned char *p = back_buffer + (y + row) * VGA_STRIDE + x;
        int n = w;
        /* DAY57: rep stosb runs at memory speed on every CPU QEMU models */
        asm volatile("cld; rep stosb" : "+D"(p), "+c"(n) : "a"(color) : "memory");
    }
}

/* ===== vga_draw_hline ===== */
void vga_draw_hline(int x, int y, int len, unsigned char color)
{
    if ((unsigned)y >= VGA_HEIGHT) return;
    if (x < 0) { len += x; x = 0; }
    if (x + len > VGA_WIDTH) len = VGA_WIDTH - x;
    if (len <= 0) return;
    unsigned char *p = back_buffer + y * VGA_STRIDE + x;
    for (int i = 0; i < len; i++) p[i] = color;
}

/* ===== vga_draw_vline ===== */
void vga_draw_vline(int x, int y, int len, unsigned char color)
{
    if ((unsigned)x >= VGA_WIDTH) return;
    if (y < 0) { len += y; y = 0; }
    if (y + len > VGA_HEIGHT) len = VGA_HEIGHT - y;
    if (len <= 0) return;
    for (int i = 0; i < len; i++)
        back_buffer[(y + i) * VGA_STRIDE + x] = color;
}

/* ===== vga_draw_circle (Bresenham) ===== */
void vga_draw_circle(int cx, int cy, int radius, unsigned char color)
{
    int x = radius, y = 0, d = 3 - 2 * radius;
    while (x >= y) {
        vga_put_pixel(cx+x, cy+y, color); vga_put_pixel(cx-x, cy+y, color);
        vga_put_pixel(cx+x, cy-y, color); vga_put_pixel(cx-x, cy-y, color);
        vga_put_pixel(cx+y, cy+x, color); vga_put_pixel(cx-y, cy+x, color);
        vga_put_pixel(cx+y, cy-x, color); vga_put_pixel(cx-y, cy-x, color);
        if (d < 0) { d += 4*y+6; }
        else       { d += 4*(y-x)+10; x--; }
        y++;
    }
}

/* ===== Geometry accessors ===== */
/* ===== DAY58b: selectable resolutions =====
 * All are 8bpp indexed. The list is what the Settings window cycles through;
 * vga_set_resolution() accepts any listed mode. Limits: the back buffer
 * (0x300000..0x3FFFFF) and the SVGA shadow frame (0x400000..0x4FFFFF) each
 * hold VGA_MAX_PIXELS bytes. */
static const struct { int w, h; } vga_modes[] = {
    { 640, 360 }, { 800, 450 }, { 800, 600 }, { 1024, 576 }, { 1024, 768 }, { 1280, 720 },
};
#define VGA_MODE_COUNT ((int)(sizeof(vga_modes) / sizeof(vga_modes[0])))
int vga_mode_count(void) { return VGA_MODE_COUNT; }
int vga_mode_get(int i, int *w, int *h)
{
    if (i < 0 || i >= VGA_MODE_COUNT) return -1;
    *w = vga_modes[i].w; *h = vga_modes[i].h;
    return 0;
}
int vga_mode_find(int w, int h)
{
    for (int i = 0; i < VGA_MODE_COUNT; i++)
        if (vga_modes[i].w == w && vga_modes[i].h == h) return i;
    return -1;
}

/* Switch the mode at run time. Returns 0, or <0 if the mode is unknown or the
 * hardware refused (the previous mode stays active in that case). */
int vga_set_resolution(int w, int h)
{
    if (vga_mode_find(w, h) < 0 || w * h > VGA_MAX_PIXELS) return -1;
    if (w == g_vw && h == g_vh) return 0;
    int ow = g_vw, oh = g_vh;
    if (g_svga) {
        vmsvga_info_t si;
        int rc = vmsvga_init((uint32_t)w, (uint32_t)h, &si);
        if (rc != 0 || (int)si.width != w || (int)si.height != h) {
            vmsvga_init((uint32_t)ow, (uint32_t)oh, &si);
            svga_fb = si.fb; svga_pitch = si.pitch;
            svga_force_full = 1;
            return -2;
        }
        svga_fb = si.fb; svga_pitch = si.pitch;
    } else {
        bga_write(BGA_ENABLE, BGA_DISABLED);
        bga_write(BGA_XRES, (uint16_t)w);
        bga_write(BGA_YRES, (uint16_t)h);
        bga_write(BGA_BPP,  (uint16_t)VGA_BPP);
        bga_write(BGA_VIRT_W, (uint16_t)w);
        bga_write(BGA_VIRT_H, (uint16_t)h);
        bga_write(BGA_X_OFFSET, 0);
        bga_write(BGA_Y_OFFSET, 0);
        bga_write(BGA_ENABLE, BGA_ENABLED | BGA_LFB_ENABLED);
    }
    g_vw = w; g_vh = h;
    svga_force_full = 1;
    /* the DAC may have been reset by the mode change: program every entry again */
    for (int i = 0; i < 256; i++)
        pal_program((unsigned char)i, pal_rgb[i][0], pal_rgb[i][1], pal_rgb[i][2]);
    uint32_t *buf = (uint32_t *)back_buffer;
    for (int i = 0; i < (VGA_SIZE / 4); i++) buf[i] = 0;
    return 0;
}

int vga_gfx_width(void)  { return VGA_WIDTH;  }
int vga_gfx_height(void) { return VGA_HEIGHT; }

/* ===== DAY43 PRE-2: backend introspection (Info window, serial log) ===== */
int vga_gfx_is_svga(void) { return g_svga; }

const char *vga_gfx_backend_name(void)
{
    return g_svga ? "VMware SVGA II 800x450 32bpp"
                  : "BGA 800x450 256-color";
}

/* physical address of the framebuffer actually being written */
uint32_t vga_gfx_fb_addr(void)
{
    return g_svga ? (uint32_t)svga_fb : (uint32_t)vga_mem;
}
