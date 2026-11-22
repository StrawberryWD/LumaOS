/* kernel/core/kernel.h
 * LumaOS 5.0 - Main Kernel Header
 *
 * STAGE1: Version bump to 4.1; no functional changes to this file beyond
 *         adding gui_clip() prototype (used by Stage 3 renderer).
 */
#ifndef LUMA_KERNEL_H
#define LUMA_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ===== I/O ===== */
static inline uint8_t inb(uint16_t p)
{
    uint8_t v;
    asm volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void outb(uint16_t p, uint8_t v)
{
    asm volatile("outb %0,%1" : : "a"(v), "Nd"(p));
}
static inline uint16_t inw(uint16_t p)
{
    uint16_t v;
    asm volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void outw(uint16_t p, uint16_t v)
{
    asm volatile("outw %0,%1" : : "a"(v), "Nd"(p));
}
static inline uint32_t inl(uint16_t p)
{
    uint32_t v;
    asm volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void outl(uint16_t p, uint32_t v)
{
    asm volatile("outl %0,%1" : : "a"(v), "Nd"(p));
}
/* insw(port, buf, count)
 *
 * DAY3: reads `count` 16-bit words from I/O port `port` into `buf`,
 * using the x86 string-input instruction (`rep insw`). This is the
 * PIO transfer primitive ide_read_sectors() uses to pull each 512-byte
 * sector (256 words) out of the ATA data port (0x1F0) once DRQ is set.
 * `buf` need not be word-aligned; the "D" constraint fixes the port in
 * DX as REP INSW requires. Direction flag is assumed clear (kernel
 * convention — nothing in this codebase sets STD). */
static inline void insw(uint16_t port, void *buf, uint32_t count)
{
    asm volatile("rep insw"
                 : "+D"(buf), "+c"(count)
                 : "d"(port)
                 : "memory");
}

/* outsw(port, buf, count)
 *
 * DAY4: writes `count` 16-bit words from `buf` out to I/O port `port`,
 * using the x86 string-output instruction (`rep outsw`). This is the
 * PIO transfer primitive ide_write_sectors() uses to push each 512-byte
 * sector (256 words) into the ATA data port (0x1F0) once DRQ is set.
 * `buf` need not be word-aligned; the "S" constraint fixes the port in
 * DX as REP OUTSW requires. Direction flag is assumed clear (kernel
 * convention — nothing in this codebase sets STD). */
static inline void outsw(uint16_t port, const void *buf, uint32_t count)
{
    asm volatile("rep outsw"
                 : "+S"(buf), "+c"(count)
                 : "d"(port)
                 : "memory");
}

/* ===== Console colors ===== */
#define COLOR_DEFAULT  0x07
#define COLOR_ERROR    0x0C
#define COLOR_INFO     0x0A
#define COLOR_PROMPT   0x0B
#define COLOR_SUCCESS  0x02
#define COLOR_WARNING  0x0E

/* ===== Console (VGA text mode – used only before graphics init) ===== */
void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_putc_color(char c, uint8_t color);
void console_write(const char *s);
void console_write_color(const char *s, uint8_t color);
/* console_write_hex(data, len)
 *
 * DAY3: hex-dumps `len` bytes starting at `data`, 16 bytes per line,
 * as space-separated uppercase pairs (e.g. "EB 3C 90 ..."). Used by
 * kmain() to print the first 64 bytes read back from LBA 0 so the
 * FAT boot-sector jump signature (EB 3C 90) can be eyeballed on
 * screen per the Day 3 acceptance criteria.
 */
void console_write_hex(const void *data, size_t len);
/* console_get_x/get_y/set_cursor — DAY14: needed by kmain.c's
 * text-mode line editor (text_shell_run()) to redraw the in-progress
 * input line and reposition the hardware cursor for LEFT/RIGHT
 * arrow-key movement. These were already implemented in console.c
 * (used internally by update_cursor()'s callers) but never had
 * prototypes in this header before nothing outside console.c needed
 * them. */
int  console_get_x(void);
int  console_get_y(void);
void console_set_cursor(int x, int y);

/* ===== VGA graphics (BGA 800x450 8bpp) ===== */
void vga_init_graphics(void);
void vga_set_palette(unsigned char index, unsigned char r,
                     unsigned char g, unsigned char b);
void vga_clear_screen(unsigned char color);
void vga_flip(void);
void vga_put_pixel(int x, int y, unsigned char color);
void vga_draw_rect(int x, int y, int w, int h, unsigned char color);
void vga_draw_hline(int x, int y, int len, unsigned char color);
void vga_draw_vline(int x, int y, int len, unsigned char color);
void vga_draw_char(int x, int y, char c, unsigned char color);
void vga_draw_string(int x, int y, char *str, unsigned char color);
/* === MODIFIED SECTION START ===
 * STAGE4: width-aware variant of vga_draw_string() — wraps/clips to
 * max_w pixels instead of the full screen width. Use this from any
 * window/widget draw code (titlebars, dock labels, settings rows,
 * popups, etc.) so long text can't bleed past that widget's edge. */
void vga_draw_string_clipped(int x, int y, char *str, unsigned char color, int max_w);
/* === MODIFIED SECTION END === */
unsigned char vga_get_pixel(int x, int y);
void vga_draw_circle(int cx, int cy, int radius, unsigned char color);
/* DAY43 PRE-4: true translucency on the indexed framebuffer (blend palette, vga_gfx.c) */
unsigned char vga_blend_color(unsigned char under, unsigned char over, int alpha);
unsigned char vga_blend_color_exact(unsigned char under, unsigned char over, int alpha);
int  font_load(const unsigned char *d, unsigned len, const char *source);   /* DAY63 */
const char *font_source(void);
unsigned font_checksum(void);
void vga_hline_pat4(int y, const unsigned char *pat);
void          vga_blend_rect(int x, int y, int w, int h, unsigned char over, int alpha);
int           vga_gfx_blend_used(void);   /* blend-pool entries in use */
int           vga_gfx_blend_max(void);    /* pool capacity (199) */
void          vga_blend_reset(void);      /* DAY58b: empty the pool (theme switch) */
int           vga_mode_count(void);       /* DAY58b: selectable resolutions */
int           vga_mode_get(int i, int *w, int *h);
int           vga_mode_find(int w, int h);
int           vga_set_resolution(int w, int h);
int  vga_gfx_width(void);
int  vga_gfx_height(void);
/* DAY43 PRE-2: which graphics backend vga_init_graphics() picked */
int         vga_gfx_is_svga(void);        /* 1 = VMware SVGA II, 0 = Bochs BGA */
const char *vga_gfx_backend_name(void);
uint32_t    vga_gfx_fb_addr(void);        /* framebuffer physical address */

/* ===== Keyboard ===== */
void          keyboard_init(void);
char          keyboard_getchar(void);
unsigned char keyboard_read(void);
int           keyboard_available(void);
void          keyboard_irq_handler(void);
void          keyboard_clear_buffer(void);

/* ===== Mouse ===== */
extern int mouse_x;
extern int mouse_y;
extern int mouse_left_button;
extern int mouse_right_button;
extern int mouse_middle_button;
void mouse_init(void);
void mouse_irq_handler(void);

/* ===== GUI ===== */
void gui_init(void);
void gui_refresh(void);
void gui_update_mouse(int mx, int my);

/* ===== Shell ===== */
void shell_init(void);

/* ===== System ===== */
void sys_shutdown(void);
void sys_reboot(void);
void panic(const char *msg);
void dfault_init(void);     /* DAY54: kernel/arch/dfault.c */
void paging_init(void);      /* DAY53: kernel/mm/paging.c */

/* ===== Architecture ===== */
void gdt_init(void);
void idt_init(void);
void pic_init(void);
void pic_eoi(uint8_t irq);
void pic_disable_irq(uint8_t irq);
void pic_enable_irq(uint8_t irq);
void enable_interrupts(void);
void disable_interrupts(void);

/* ===== String ===== */
size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
int    atoi(const char *s);

/* ===== Printf ===== */
int printf(const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t sz, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);

#endif /* LUMA_KERNEL_H */