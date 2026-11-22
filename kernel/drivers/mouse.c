/* kernel/drivers/mouse.c
 * LumaOS 5.0 - PS/2 Mouse Driver
 *
 * STAGE1-MOUSE changes:
 *   MSE-1: Screen bounds updated to 800x450 (was 320x200).
 *   MSE-2: Added X/Y overflow bit check (byte0 bits 6 & 7).
 *          If overflow is set the packet is corrupted — discard it.
 *   MSE-3: Added simple pointer acceleration.
 *          Small movements (|dx|/|dy| <= 3) are passed through 1:1.
 *          Larger movements are multiplied by ACCEL_FACTOR (1.5x).
 *          This makes the cursor feel natural at 800x450 without
 *          needing to change the QEMU mouse sensitivity.
 *   MSE-4: mouse_cycle sync guard - if byte0 does not have bit 3 set
 *          (always-1 in valid PS/2 packets) and we are at cycle 0,
 *          discard the byte so we re-sync automatically.
 *
 * STAGE3-MOUSE changes (scroll wheel support):
 *   MSE-6: mouse_init() now performs the standard "magic knock" sequence
 *          (set sample rate 200, 100, 80) to request IntelliMouse mode
 *          from the PS/2 controller. If the device ACKs with device ID
 *          0x03 (or 0x04 for 5-button mice), it is a wheel mouse and we
 *          switch mouse_packet_size from 3 to 4 bytes.
 *   MSE-7: When in 4-byte mode, byte[3] is a signed 8-bit wheel delta
 *          (-1 = one notch up/away from user, +1 = one notch down/toward
 *          user on most firmwares -- we expose the raw signed value via
 *          mouse_wheel_delta and let the GUI layer decide direction).
 *          If the device is NOT a wheel mouse we stay in 3-byte mode and
 *          mouse_wheel_delta is always 0, so existing behaviour is
 *          unchanged on hardware/emulators without wheel support.
 *   MSE-8: mouse_wheel_delta is cleared back to 0 by the GUI layer once
 *          consumed (see gui_refresh in window1.c) so a single notch is
 *          never double-applied.
 */
#include "kernel.h"
#include <stdint.h>

#define MOUSE_DATA  0x60
#define MOUSE_STAT  0x64
#define MOUSE_CMD   0x64

/* Screen size (800x450) */
/* DAY58b: the GUI resolution is selectable at run time */
extern int vga_gfx_width(void);
extern int vga_gfx_height(void);
#define SCREEN_W  vga_gfx_width()
#define SCREEN_H  vga_gfx_height()

/* Acceleration threshold and factor (x1.5 = multiply by 3, divide by 2) */
#define ACCEL_THRESHOLD 3

int mouse_x            = 400;
int mouse_y            = 225;
int mouse_left_button  = 0;
int mouse_right_button = 0;
int mouse_middle_button= 0;

/* MSE-7: raw signed wheel delta accumulated since last consumption.
 * Positive = scrolled toward user (typically "down"/away),
 * Negative = scrolled away from user (typically "up"/toward).
 * The GUI layer normalises sign/units into scroll lines. */
int mouse_wheel_delta   = 0;

static unsigned char mouse_byte[4];
static int           mouse_cycle = 0;

/* MSE-6: 3 = classic PS/2, 4 = IntelliMouse (wheel) protocol */
static int mouse_packet_size = 3;
static int mouse_has_wheel   = 0;

/* ===== Low-level helpers ===== */
static void mouse_wait_write(void)
{
    int t = 100000;
    while (t-- > 0 && (inb(MOUSE_STAT) & 0x02)) asm volatile("pause");
}
static void mouse_wait_read(void)
{
    int t = 100000;
    while (t-- > 0 && !(inb(MOUSE_STAT) & 0x01)) asm volatile("pause");
}
static uint8_t mouse_read_byte(void)
{
    mouse_wait_read();
    return inb(MOUSE_DATA);
}
static void mouse_send(uint8_t d)
{
    mouse_wait_write(); outb(MOUSE_CMD,  0xD4);
    mouse_wait_write(); outb(MOUSE_DATA, d);
}

/* MSE-6: Get device ID (0xF2). Used to detect wheel-mouse support. */
static uint8_t mouse_get_id(void)
{
    mouse_send(0xF2);
    mouse_read_byte();          /* ACK */
    return mouse_read_byte();   /* device ID byte */
}

/* MSE-6: "Magic knock" — writing sample rate 200,100,80 in sequence,
 * then reading the device ID, is the documented way to ask a PS/2
 * mouse to enable IntelliMouse (wheel) reporting. A non-wheel mouse
 * simply ignores the sequence and keeps reporting ID 0x00. */
static int mouse_try_enable_wheel(void)
{
    mouse_send(0xF3); mouse_read_byte(); mouse_send(200); mouse_read_byte();
    mouse_send(0xF3); mouse_read_byte(); mouse_send(100); mouse_read_byte();
    mouse_send(0xF3); mouse_read_byte(); mouse_send(80);  mouse_read_byte();
    uint8_t id = mouse_get_id();
    return (id == 0x03 || id == 0x04);
}

/* ===== MSE-3: Acceleration helper ===== */
static int accel(int delta)
{
    if (delta == 0) return 0;
    int sign = (delta > 0) ? 1 : -1;
    int abs  = (delta > 0) ? delta : -delta;
    if (abs > ACCEL_THRESHOLD) {
        /* x 1.5: multiply by 3 then halve (integer arithmetic) */
        abs = (abs * 3) / 2;
    }
    return sign * abs;
}

/* ===== IRQ12 handler ===== */
void mouse_irq_handler(void)
{
    unsigned char status = inb(MOUSE_STAT);
    if (!(status & 0x01)) return;           /* no data */
    if (!(status & 0x20)) {
        /* Data is from keyboard, not mouse — drain and ignore */
        inb(MOUSE_DATA);
        return;
    }

    unsigned char data = inb(MOUSE_DATA);

    switch (mouse_cycle) {

    case 0:
        /* MSE-4: Byte 0 must have bit 3 set (always-1 in PS/2 packets).
         * If not, we are out of sync — discard and stay at cycle 0.    */
        if (!(data & 0x08)) {
            mouse_cycle = 0;
            return;
        }
        mouse_byte[0] = data;
        mouse_cycle   = 1;
        break;

    case 1:
        mouse_byte[1] = data;
        mouse_cycle   = 2;
        break;

    case 2:
        mouse_byte[2] = data;
        /* MSE-6/7: if wheel mode is active, a 4th byte follows.
         * Otherwise this completes the packet like before. */
        if (mouse_has_wheel && mouse_packet_size == 4) {
            mouse_cycle = 3;
            break;
        }
        mouse_cycle = 0;
        goto commit;

    case 3:
        mouse_byte[3] = data;
        mouse_cycle   = 0;
        goto commit;
    }
    return;

commit:
    /* MSE-2: Check overflow bits (byte0 bits 6 & 7).
     * If either is set the packet is garbage — skip it.            */
    if (mouse_byte[0] & 0xC0) return;

    /* Button state */
    mouse_left_button   = (mouse_byte[0] & 0x01);
    mouse_right_button  = (mouse_byte[0] & 0x02) >> 1;
    mouse_middle_button = (mouse_byte[0] & 0x04) >> 2;

    /* Raw signed deltas */
    int dx = (int)(int8_t)mouse_byte[1];
    int dy = (int)(int8_t)mouse_byte[2];

    /* MSE-3: Apply acceleration */
    dx = accel(dx);
    dy = accel(dy);

    /* Update position — Y is inverted (mouse-up = screen-up) */
    mouse_x += dx;
    mouse_y -= dy;

    /* MSE-1: Clamp to 800x450 */
    if (mouse_x < 0)        mouse_x = 0;
    if (mouse_y < 0)        mouse_y = 0;
    if (mouse_x >= SCREEN_W) mouse_x = SCREEN_W - 1;
    if (mouse_y >= SCREEN_H) mouse_y = SCREEN_H - 1;

    /* MSE-7: Wheel delta is a signed 8-bit value in the low nibble of
     * byte[3] (bits 4-7 are used by 5-button mice for extra buttons,
     * which we ignore here). Sign-extend from 4 bits. */
    if (mouse_has_wheel && mouse_packet_size == 4) {
        int8_t raw = (int8_t)(mouse_byte[3] & 0x0F);
        if (raw & 0x08) raw |= 0xF0;   /* sign-extend 4-bit value */
        if (raw != 0) mouse_wheel_delta += raw;
    }
}

/* ===== Initialisation ===== */
void mouse_init(void)
{
    unsigned char status;

    /* Enable auxiliary (mouse) port */
    mouse_wait_write(); outb(MOUSE_CMD, 0xA8);

    /* Read Controller Command Byte and enable IRQ12 */
    mouse_wait_write(); outb(MOUSE_CMD, 0x20);
    status = mouse_read_byte();
    status |=  0x02;   /* enable IRQ12 */
    status &= ~0x20;   /* clear "disable mouse clock" */
    mouse_wait_write(); outb(MOUSE_CMD,  0x60);
    mouse_wait_write(); outb(MOUSE_DATA, status);

    /* Reset mouse to default state */
    mouse_send(0xFF);
    mouse_read_byte();  /* ACK  */
    mouse_read_byte();  /* 0xAA (self-test pass) */
    mouse_read_byte();  /* 0x00 (device ID) */

    /* Set defaults */
    mouse_send(0xF6);
    mouse_read_byte();  /* ACK */

    /* MSE-6: Attempt to switch the device into IntelliMouse (wheel)
     * mode. Safe no-op on mice that don't support it. */
    mouse_has_wheel   = mouse_try_enable_wheel();
    mouse_packet_size = mouse_has_wheel ? 4 : 3;

    /* Set sample rate to 80 samples/sec for smoother movement
     * (the knock sequence above already left it at 80, but set it
     * again explicitly in case wheel detection failed partway). */
    mouse_send(0xF3); mouse_read_byte();
    mouse_send(80);   mouse_read_byte();

    /* Set resolution: 2 counts/mm (index 1) */
    mouse_send(0xE8); mouse_read_byte();
    mouse_send(0x01); mouse_read_byte();

    /* Enable data reporting */
    mouse_send(0xF4);
    mouse_read_byte();  /* ACK */

    pic_enable_irq(12);
    mouse_cycle = 0;
    mouse_wheel_delta = 0;

    /* Start cursor in screen centre */
    mouse_x = SCREEN_W / 2;
    mouse_y = SCREEN_H / 2;
}

/* ===== Accessors ===== */
void mouse_get_position(int *x, int *y)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}
void mouse_set_position(int x, int y)
{
    if (x >= 0 && x < SCREEN_W) mouse_x = x;
    if (y >= 0 && y < SCREEN_H) mouse_y = y;
}

/* MSE-8: Consume and clear the accumulated wheel delta.
 * Returns the net notches scrolled since the last call. */
int mouse_consume_wheel(void)
{
    int d = mouse_wheel_delta;
    mouse_wheel_delta = 0;
    return d;
}

int mouse_has_wheel_support(void)
{
    return mouse_has_wheel;
}
