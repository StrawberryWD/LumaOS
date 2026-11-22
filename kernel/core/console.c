/* kernel/core/console.c
 * LumaOS 5.0 - VGA Text Console
 * Handles 80x25 text mode output with colors and cursor
 * Used for early boot messages and fallback text mode
 */

#include "kernel.h"
#include <stdint.h>

/* VGA text mode memory and dimensions */
#define VGA_ADDRESS    0xB8000
#define VGA_COLS       80
#define VGA_ROWS       25
#define VGA_SIZE       (VGA_COLS * VGA_ROWS)

static int cursor_x = 0;
static int cursor_y = 0;
static volatile uint16_t *vga_buffer = (volatile uint16_t *)VGA_ADDRESS;

/* Update hardware cursor position */
static void update_cursor() {
    uint16_t pos = cursor_y * VGA_COLS + cursor_x;
    
    /* Send high byte of cursor position */
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    
    /* Send low byte of cursor position */
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

/* Clear entire screen */
void console_clear() {
    for (int i = 0; i < VGA_SIZE; i++) {
        vga_buffer[i] = (COLOR_DEFAULT << 8) | ' ';
    }
    cursor_x = 0;
    cursor_y = 0;
    update_cursor();
}

/* Initialize console */
void console_init() {
    console_clear();
}

/* Scroll screen up one line */
static void scroll() {
    if (cursor_y < VGA_ROWS) return;
    
    /* Move all lines up */
    for (int i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++) {
        vga_buffer[i] = vga_buffer[i + VGA_COLS];
    }
    
    /* Clear bottom line */
    for (int i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_SIZE; i++) {
        vga_buffer[i] = (COLOR_DEFAULT << 8) | ' ';
    }
    
    cursor_y = VGA_ROWS - 1;
}

/* Put character with color at current cursor position */
void console_putc_color(char c, uint8_t color) {
    if (c == '\r') {
        cursor_x = 0;
        update_cursor();
        return;
    }
    
    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            vga_buffer[cursor_y * VGA_COLS + cursor_x] = (COLOR_DEFAULT << 8) | ' ';
            update_cursor();
        }
        return;
    }
    
    if (c == '\t') {
        int next = (cursor_x + 4) & ~3;
        while (cursor_x < next && cursor_x < VGA_COLS) {
            console_putc_color(' ', color);
        }
        return;
    }
    
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        scroll();
        update_cursor();
        return;
    }
    
    /* Write character to buffer */
    vga_buffer[cursor_y * VGA_COLS + cursor_x] = (color << 8) | (unsigned char)c;
    cursor_x++;
    
    /* Wrap to next line */
    if (cursor_x >= VGA_COLS) {
        cursor_x = 0;
        cursor_y++;
        scroll();
    }
    
    update_cursor();
}

/* Put character with default color */
void console_putc(char c) {
    console_putc_color(c, COLOR_DEFAULT);
}

/* Write string with default color */
void console_write(const char *s) {
    while (*s) {
        console_putc(*s++);
    }
}

/* Write string with specific color */
void console_write_color(const char *s, uint8_t color) {
    while (*s) {
        console_putc_color(*s++, color);
    }
}

/* console_write_hex(data, len)
 * DAY3: hex-dump helper. 16 bytes per line, uppercase "XX " pairs.
 * Used to verify PIO sector reads (e.g. printing LBA 0 / the MBR and
 * checking for the FAT boot-jump signature EB 3C 90 at offset 0). */
static char hex_nibble(uint8_t n) {
    return (n < 10) ? ('0' + n) : ('A' + (n - 10));
}

void console_write_hex(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = p[i];
        console_putc_color(hex_nibble((b >> 4) & 0x0F), COLOR_DEFAULT);
        console_putc_color(hex_nibble(b & 0x0F), COLOR_DEFAULT);
        console_putc_color(' ', COLOR_DEFAULT);

        /* 16 bytes per line for readability */
        if ((i + 1) % 16 == 0) {
            console_putc('\n');
        }
    }

    /* Trailing newline if the last line wasn't already flushed by the
     * 16-byte wrap above (avoids leaving the cursor mid-line). */
    if (len % 16 != 0) {
        console_putc('\n');
    }
}

/* Get cursor X position */
int console_get_x() {
    return cursor_x;
}

/* Get cursor Y position */
int console_get_y() {
    return cursor_y;
}

/* Set cursor position */
void console_set_cursor(int x, int y) {
    if (x >= 0 && x < VGA_COLS) cursor_x = x;
    if (y >= 0 && y < VGA_ROWS) cursor_y = y;
    update_cursor();
}