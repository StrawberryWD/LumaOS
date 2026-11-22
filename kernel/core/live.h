/* kernel/core/live.h - DAY62/63: live-CD services (what a CD-booted system has instead of a hard disk).
 *   Day 62  find the boot CD with the ATAPI driver, copy BOOT/LMFS.IMG into RAM, mount it as "RAM0"
 *   Day 63  mount the ISO9660 volume itself read-only as "CD0" (fonts, config ...) */
#ifndef LUMA_LIVE_H
#define LUMA_LIVE_H
#include <stdint.h>

int  live_init(void);            /* 0 = everything mounted, <0 = a step failed (see console lines) */
int  live_cd_index(void);
int  live_cd_mounted(void);       /* ISO9660 volume mounted as CD0 */
void live_apply_config(void);    /* GUI up -> apply theme / resolution / desktop_icons from g_cfg */        /* ATAPI drive index of the boot CD, -1 if none */
int  live_ram_mounted(void);
uint32_t live_ram_bytes(void);
#endif
