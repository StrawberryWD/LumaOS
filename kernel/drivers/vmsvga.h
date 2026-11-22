/* kernel/drivers/vmsvga.h
 * LumaOS 5.0 - VMware SVGA II driver (PCI 15AD:0405)
 *
 * DAY43 PRE-2: this driver used to live in vmwarevgadriver/ as a
 * stand-alone sample. It is now part of the kernel tree
 * (kernel/drivers/) and is driven by kernel/drivers/vga_gfx.c, which
 * picks it ahead of the Bochs VBE (BGA) path whenever the device is
 * present (VMware Workstation/Player/Fusion, QEMU `-vga vmware`).
 *
 * Contract (unchanged from the original sample, plus two helpers):
 *   vmsvga_detect()  0 = a VMware SVGA II device is on the PCI bus
 *   vmsvga_init()    set w x h x 32bpp, fill *out; 0 = ok, <0 = failure
 *   vmsvga_update()  tell the host a rectangle of the framebuffer changed
 *   vmsvga_flush()   update the whole screen
 *   vmsvga_active()  1 once vmsvga_init() has succeeded
 */
#ifndef VMSVGA_H
#define VMSVGA_H

#include <stdint.h>

typedef struct {
    uint8_t *fb;        /* framebuffer start (FB_OFFSET already added) */
    uint32_t width;
    uint32_t height;
    uint32_t bpp;       /* 32 */
    uint32_t pitch;     /* bytes per row - always use it, never assume width*4 */
} vmsvga_info_t;

/* 0 = VMware SVGA II (PCI 15AD:0405) found */
int  vmsvga_detect(void);

/* Set w x h x 32bpp. 0 on success, negative on failure
 * (-1 no device, -2 ID_2 negotiation refused, -3 mode too big,
 *  -4 not enough VRAM, -5 device reports a non-32bpp / zero pitch mode). */
int  vmsvga_init(uint32_t w, uint32_t h, vmsvga_info_t *out);

/* Tell the host this rectangle of the framebuffer changed (REQUIRED -
 * without it the host may never repaint). Safe to call before init
 * (ignored). */
void vmsvga_update(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Update the whole screen */
void vmsvga_flush(void);

/* 1 once vmsvga_init() succeeded */
int  vmsvga_active(void);

#endif
