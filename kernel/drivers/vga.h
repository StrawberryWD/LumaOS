/* kernel/drivers/vga.h
 * LumaOS 5.0 - VGA driver interface (text + BGA graphics)
 *
 * STAGE1-GFX: Updated VGA_GFX_W/H to 800x450 for Bochs VBE BGA driver.
 */
#ifndef LUMA_VGA_H
#define LUMA_VGA_H

#include <stdint.h>

/* ===== Text mode colors ===== */
#define VGA_BLACK        0x00
#define VGA_BLUE         0x01
#define VGA_GREEN        0x02
#define VGA_CYAN         0x03
#define VGA_RED          0x04
#define VGA_MAGENTA      0x05
#define VGA_BROWN        0x06
#define VGA_LGRAY        0x07
#define VGA_DGRAY        0x08
#define VGA_LBLUE        0x09
#define VGA_LGREEN       0x0A
#define VGA_LCYAN        0x0B
#define VGA_LRED         0x0C
#define VGA_LMAGENTA     0x0D
#define VGA_YELLOW       0x0E
#define VGA_WHITE        0x0F

#define VGA_COLOR(fg, bg) ((uint8_t)(((bg) << 4) | ((fg) & 0x0F)))

/* ===== Text mode (80x25) ===== */
#define VGA_COLS 80
#define VGA_ROWS 25

void    vga_init(void);
void    vga_clear(void);
void    vga_putchar_at(int x, int y, char c, uint8_t color);
void    vga_puts_at(int x, int y, const char *str, uint8_t color);
void    vga_fill_rect(int x, int y, int w, int h, char c, uint8_t color);
void    vga_draw_box(int x, int y, int w, int h, uint8_t color);
void    vga_scroll_up(void);
void    vga_scroll_down(void);
void    vga_set_cursor(int x, int y);
void    vga_set_cursor_visible(int visible);
void    vga_invert_region(int x, int y, int w, int h);
char    vga_getchar_at(int x, int y);
uint8_t vga_getcolor_at(int x, int y);

/* ===== Graphics mode: Bochs BGA 800x450 8bpp ===== */
#define VGA_GFX_W  800
#define VGA_GFX_H  450

void          vga_init_graphics(void);
void          vga_set_palette(unsigned char idx, unsigned char r,
                              unsigned char g, unsigned char b);
void          vga_clear_screen(unsigned char color);
void          vga_flip(void);
void          vga_put_pixel(int x, int y, unsigned char color);
unsigned char vga_get_pixel(int x, int y);
void          vga_draw_rect(int x, int y, int w, int h, unsigned char color);
void          vga_draw_hline(int x, int y, int len, unsigned char color);
void          vga_draw_vline(int x, int y, int len, unsigned char color);
void          vga_draw_circle(int cx, int cy, int r, unsigned char color);
/* DAY43 PRE-4: true translucency on the indexed framebuffer (see vga_gfx.c) */
unsigned char vga_blend_color(unsigned char under, unsigned char over, int alpha);
void          vga_blend_rect(int x, int y, int w, int h, unsigned char over, int alpha);
void          vga_draw_char(int x, int y, char c, unsigned char color);
void          vga_draw_string(int x, int y, char *str, unsigned char color);
int           vga_gfx_width(void);
int           vga_gfx_height(void);

#endif /* LUMA_VGA_H */
