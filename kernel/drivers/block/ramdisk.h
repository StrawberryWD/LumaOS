/* kernel/drivers/block/ramdisk.h - DAY62: a block device that lives in RAM.
 * The live CD copies its LMFS system image (BOOT/LMFS.IMG) into this and mounts it, so the system
 * resources are a normal, WRITABLE LMFS volume (changes last until reboot - a live CD has no disk). */
#ifndef LUMA_RAMDISK_H
#define LUMA_RAMDISK_H
#include "block.h"

/* Make a 512-byte-sector device over [base, base+bytes). Returns a static device or NULL. */
block_device_t *ramdisk_create(const char *name, void *base, uint32_t bytes);
#endif
