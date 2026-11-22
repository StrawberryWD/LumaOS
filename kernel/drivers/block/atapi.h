/* kernel/drivers/block/atapi.h - DAY62: ATAPI (CD-ROM) PIO driver.
 *
 * An ATAPI drive sits on the same IDE cable as a hard disk but does not speak ATA commands for data:
 * the host sends a 12-byte SCSI-style PACKET (command 0xA0) and then the drive streams the answer
 * through the data port. READ(10) (opcode 0x28) reads CD sectors of 2048 bytes (NOT 512).
 *
 * Detection: every position (primary/secondary x master/slave) is selected in turn; an ATAPI device
 * answers with the signature 0x14 (LBA mid) / 0xEB (LBA high) in its registers, an ATA disk with 0/0.
 * Interrupts from the channel are disabled (nIEN) - this driver polls, like ide.c. */
#ifndef LUMA_ATAPI_H
#define LUMA_ATAPI_H
#include <stdint.h>
#include "block.h"

#define ATAPI_SECTOR 2048
#define ATAPI_MAX    4          /* 2 channels x 2 drives */

int  atapi_probe(void);                         /* scan all 4 positions; returns number of ATAPI drives */
int  atapi_count(void);
const char *atapi_model(int i);                 /* IDENTIFY PACKET model string of drive i */
const char *atapi_position(int i);              /* "primary master" ... */
/* READ(10): `count` 2048-byte sectors from CD LBA `lba` into buf. 0 = ok, -1 = error. */
int  atapi_read(int i, uint32_t lba, uint32_t count, void *buf);
/* block_device_t view (sector_size 2048, read only) */
block_device_t *atapi_block_device(int i);
#endif
