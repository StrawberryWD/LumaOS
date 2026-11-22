/* kernel/drivers/vga.c
 * LumaOS 5.0 - VGA Text Mode Driver
 * Supports 80x25 text mode with colors
 * Synchronized with console.c for unified text output
 */

#include "kernel.h"
#include <stdint.h>

/* VGA text mode configuration */
#define VGA_TEXT_ADDR   0xB8000
#define VGA_COLS        80
#define VGA_ROWS        25
#define VGA_SIZE        (VGA_COLS * VGA_ROWS)

static volatile uint16_t *vga_text = (volatile uint16_t *)VGA_TEXT_ADDR;

/* VGA palette - standard 16 colors */
static const struct {
    uint8_t r, g, b;
} vga_palette[16] = {
    {0, 0, 0},         /* 0: Black */
    {0, 0, 42},        /* 1: Blue */
    {0, 42, 0},        /* 2: Green */
    {0, 42, 42},       /* 3: Cyan */
    {42, 0, 0},        /* 4: Red */
    {42, 0, 42},       /* 5: Magenta */
    {42, 21, 0},       /* 6: Brown */
    {42, 42, 42},      /* 7: Light Gray */
    {21, 21, 21},      /* 8: Dark Gray */
    {21, 21, 63},      /* 9: Light Blue */
    {21, 63, 21},      /* 10: Light Green */
    {21, 63, 63},      /* 11: Light Cyan */
    {63, 21, 21},      /* 12: Light Red */
    {63, 21, 63},      /* 13: Light Magenta */
    {63, 63, 21},      /* 14: Yellow */
    {63, 63, 63}       /* 15: White */
};

/* Initialize VGA text mode */
void vga_init() {
    /* Clear screen */
    for (int i = 0; i < VGA_SIZE; i++) {
        vga_text[i] = (0x07 << 8) | ' ';  /* Black background, white text */
    }
}

/* Clear entire text screen */
void vga_clear() {
    for (int i = 0; i < VGA_SIZE; i++) {
        vga_text[i] = (0x07 << 8) | ' ';
    }
}

/* Write a character at specific position */
void vga_putchar_at(int x, int y, char c, uint8_t color) {
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
        return;
    }
    vga_text[y * VGA_COLS + x] = (color << 8) | (unsigned char)c;
}

/* Write string at specific position */
void vga_puts_at(int x, int y, const char *str, uint8_t color) {
    int col = x;
    int row = y;
    
    while (*str) {
        if (*str == '\n') {
            row++;
            col = 0;
            if (row >= VGA_ROWS) row = VGA_ROWS - 1;
        } else if (col >= VGA_COLS) {
            row++;
            col = 0;
            if (row >= VGA_ROWS) row = VGA_ROWS - 1;
            vga_putchar_at(col++, row, *str, color);
        } else {
            vga_putchar_at(col++, row, *str, color);
        }
        str++;
    }
}

/* Fill rectangle with character */
void vga_fill_rect(int x, int y, int w, int h, char c, uint8_t color) {
    for (int row = y; row < y + h && row < VGA_ROWS; row++) {
        for (int col = x; col < x + w && col < VGA_COLS; col++) {
            vga_putchar_at(col, row, c, color);
        }
    }
}

/* Scroll text up by one line */
void vga_scroll_up() {
    /* Move all lines up */
    for (int i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++) {
        vga_text[i] = vga_text[i + VGA_COLS];
    }
    
    /* Clear bottom line */
    for (int i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_SIZE; i++) {
        vga_text[i] = (0x07 << 8) | ' ';
    }
}

/* Scroll text down by one line */
void vga_scroll_down() {
    /* Move all lines down */
    for (int i = VGA_SIZE - 1; i >= VGA_COLS; i--) {
        vga_text[i] = vga_text[i - VGA_COLS];
    }
    
    /* Clear top line */
    for (int i = 0; i < VGA_COLS; i++) {
        vga_text[i] = (0x07 << 8) | ' ';
    }
}

/* Get VGA dimensions */
int vga_get_width() {
    return VGA_COLS;
}

int vga_get_height() {
    return VGA_ROWS;
}

/* Draw a box outline */
void vga_draw_box(int x, int y, int w, int h, uint8_t color) {
    /* Top and bottom lines */
    for (int col = x; col < x + w && col < VGA_COLS; col++) {
        vga_putchar_at(col, y, '-', color);
        if (y + h - 1 < VGA_ROWS) {
            vga_putchar_at(col, y + h - 1, '-', color);
        }
    }
    
    /* Left and right lines */
    for (int row = y; row < y + h && row < VGA_ROWS; row++) {
        vga_putchar_at(x, row, '|', color);
        if (x + w - 1 < VGA_COLS) {
            vga_putchar_at(x + w - 1, row, '|', color);
        }
    }
    
    /* Corners */
    vga_putchar_at(x, y, '+', color);
    if (x + w - 1 < VGA_COLS) {
        vga_putchar_at(x + w - 1, y, '+', color);
    }
    if (y + h - 1 < VGA_ROWS) {
        vga_putchar_at(x, y + h - 1, '+', color);
    }
    if (x + w - 1 < VGA_COLS && y + h - 1 < VGA_ROWS) {
        vga_putchar_at(x + w - 1, y + h - 1, '+', color);
    }
}

/* Invert colors in a region */
void vga_invert_region(int x, int y, int w, int h) {
    for (int row = y; row < y + h && row < VGA_ROWS; row++) {
        for (int col = x; col < x + w && col < VGA_COLS; col++) {
            int pos = row * VGA_COLS + col;
            uint16_t val = vga_text[pos];
            uint8_t color = (val >> 8) & 0x0F;
            uint8_t bg = (val >> 12) & 0x0F;
            /* Swap foreground and background */
            vga_text[pos] = (color << 12) | (bg << 8) | (val & 0xFF);
        }
    }
}

/* Update hardware cursor position */
void vga_set_cursor(int x, int y) {
    uint16_t pos = y * VGA_COLS + x;
    
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

/* Enable/disable hardware cursor */
void vga_set_cursor_visible(int visible) {
    uint8_t start, end;
    
    outb(0x3D4, 0x0A);
    start = inb(0x3D5);
    
    outb(0x3D4, 0x0B);
    end = inb(0x3D5);
    
    if (visible) {
        start &= ~0x20;  /* Clear disable bit */
    } else {
        start |= 0x20;   /* Set disable bit */
    }
    
    outb(0x3D4, 0x0A);
    outb(0x3D5, start);
}

/* Get character at position */
char vga_getchar_at(int x, int y) {
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
        return 0;
    }
    return (char)(vga_text[y * VGA_COLS + x] & 0xFF);
}

/* Get color at position */
uint8_t vga_getcolor_at(int x, int y) {
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
        return 0;
    }
    return (uint8_t)((vga_text[y * VGA_COLS + x] >> 8) & 0x0F);
}
