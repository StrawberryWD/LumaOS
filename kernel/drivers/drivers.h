/* kernel/drivers/drivers.h
 * LumaOS 5.0 - Unified driver interface header
 * Merged from 3.5 drivers.h and 2.2 individual headers
 */
#ifndef LUMA_DRIVERS_H
#define LUMA_DRIVERS_H

#include <stdint.h>

/* ===== Keyboard ===== */
void keyboard_init(void);
char keyboard_getchar(void);
unsigned char keyboard_read(void);
int  keyboard_available(void);
void keyboard_irq_handler(void);
void keyboard_clear_buffer(void);

/* ===== Mouse ===== */
extern int mouse_x;
extern int mouse_y;
extern int mouse_left_button;
extern int mouse_right_button;

void mouse_init(void);
void mouse_irq_handler(void);
void mouse_get_position(int *x, int *y);
void mouse_set_position(int x, int y);

/* ===== VGA Text (80x25) ===== */
void vga_init(void);
void vga_clear(void);
void vga_putchar_at(int x, int y, char c, uint8_t color);
void vga_puts_at(int x, int y, const char *str, uint8_t color);
void vga_set_cursor(int x, int y);
void vga_set_cursor_visible(int visible);
void vga_scroll_up(void);

/* ===== VGA Graphics (320x200 mode 13h) ===== */
void vga_init_graphics(void);
void vga_set_palette(unsigned char index, unsigned char r,
                     unsigned char g, unsigned char b);
void vga_clear_screen(unsigned char color);
void vga_flip(void);
/* DAY67: 每帧写进显存的行数统计（`gfxstat` 命令）。rows_total = frames * 屏幕高度。 */
void vga_gfx_stats(uint32_t *frames, uint32_t *rows_written, uint32_t *rows_total);
void vga_gfx_stats_reset(void);
void vga_put_pixel(int x, int y, unsigned char color);
unsigned char vga_get_pixel(int x, int y);
void vga_draw_rect(int x, int y, int w, int h, unsigned char color);
void vga_draw_hline(int x, int y, int len, unsigned char color);
void vga_draw_vline(int x, int y, int len, unsigned char color);
void vga_draw_char(int x, int y, char c, unsigned char color);
void vga_draw_string(int x, int y, char *str, unsigned char color);
/* === MODIFIED SECTION START ===
 * STAGE4: width-aware variant — see kernel/core/kernel.h for details. */
void vga_draw_string_clipped(int x, int y, char *str, unsigned char color, int max_w);
/* === MODIFIED SECTION END === */
void vga_draw_circle(int cx, int cy, int radius, unsigned char color);
/* DAY43 PRE-4: true translucency (blend palette, see vga_gfx.c) */
unsigned char vga_blend_color(unsigned char under, unsigned char over, int alpha);
void vga_blend_rect(int x, int y, int w, int h, unsigned char over, int alpha);

#endif /* LUMA_DRIVERS_H */