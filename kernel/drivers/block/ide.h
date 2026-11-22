/* kernel/drivers/block/ide.h
 * LumaOS 5.0 - IDE (ATA) PIO Driver
 *
 * DAY2 (Week 1, Day 2): First cut of the IDE driver. Exposes the
 * primary-channel ATA I/O ports, the status-register bits needed by
 * ide_wait(), and ide_wait()/ide_test().
 *
 * DAY3 (Week 1, Day 3 of the roadmap):
 *   "实现 ide_read_sectors(lba, count, buf)（使用 PIO 模式，insw 指令）。
 *    在 kmain 中读取 LBA 0（MBR），用 console_write_hex() 打印前 64
 *    字节，屏幕上应看到 EB 3C 90（FAT 引导跳转）。"
 *
 *   Adds ide_read_sectors(): issues ATA command 0x20 (READ SECTORS,
 *   LBA28/CHS-disabled PIO mode), waits for DRQ per sector, then pulls
 *   256 words (512 bytes) out of the data port with insw().
 *
 * DAY4 (Week 1, Day 4 of the roadmap):
 *   "实现 ide_write_sectors(lba, count, buf)（只读模式先通过，写入可先
 *    置空）。能向 LBA 1 写入一个测试字符串，再读回来对比一致。"
 *
 *   Adds ide_write_sectors(): issues ATA command 0x30 (WRITE SECTORS,
 *   28-bit LBA PIO mode), waits for DRQ per sector, pushes 256 words
 *   (512 bytes) into the data port with outsw(), then issues CACHE
 *   FLUSH (0xE7) and waits for BSY to clear so the write is durable
 *   before returning — otherwise a QEMU/host cache could still hold
 *   stale data if the "disk" were reread through another path.
 *
 * DAY7 (Week 1, Day 7 of the roadmap — weekly review):
 *   "整理 IDE 驱动代码，消除硬编码，增加超时保护（防止死循环）。
 *    写一个 ide_dump_info() 打印硬盘型号（IDENTIFY 命令 0xEC）。"
 *
 *   Three changes, none of which alter ide_read_sectors()'s or
 *   ide_write_sectors()'s external behavior/signature:
 *
 *   1) 消除硬编码 (de-duplicate the hardcoded register setup): the
 *      six outb() calls that program sector count + 28-bit LBA +
 *      drive/head select were byte-for-byte identical in both
 *      ide_read_sectors() and ide_write_sectors(). Both now call a
 *      shared static ide_setup_lba28(lba, count) helper instead of
 *      repeating the six outb()s inline.
 *
 *   2) 增加超时保护 (timeout hardening): IDE_WAIT_TIMEOUT alone turned
 *      out to be a fragile bound in practice — it's a pure spin count,
 *      not wall-clock time, so how long it actually takes depends on
 *      host/QEMU scheduling jitter (observed: ide_write_sectors()
 *      occasionally failed on a cold first boot, then succeeded on
 *      every subsequent run). Two changes address this:
 *        - Added io_wait(): the classic OSDev "write to an unused
 *          port (0x80) to burn ~1us" trick, called once per poll
 *          iteration in ide_wait()/ide_drq_wait(). This paces the
 *          polling loop against real I/O bus timing instead of raw
 *          CPU cycles, which is what was making the spin count's
 *          real-world duration inconsistent across runs.
 *        - Raised IDE_WAIT_TIMEOUT from 100000 to 1000000 to give
 *          genuinely slower first-command latency (cold QEMU IDE
 *          emulation state, host scheduling hiccups) more headroom,
 *          while a truly wedged/absent drive is still caught (the
 *          0xFF floating-bus check returns immediately either way,
 *          so an absent drive doesn't pay this larger bound).
 *
 *   3) ide_dump_info(): issues ATA_CMD_IDENTIFY (0xEC) and parses the
 *      256-word IDENTIFY DEVICE response, printing the model string
 *      (words 27-46, byte-swapped per the ATA spec's word-endianness
 *      quirk) plus sector count (words 60-61) to the console.
 */
#ifndef LUMA_IDE_H
#define LUMA_IDE_H

#include <stdint.h>

/* ===== ATA Primary Channel I/O Ports (0x1F0-0x1F7) =====
 * Standard ISA/legacy primary ATA channel. Secondary channel
 * (0x170-0x177) is not wired up yet — single-disk assumption for now,
 * matching run.sh's single `-drive ... if=ide`.
 */
#define ATA_PRIMARY_IO      0x1F0   /* base of the 8-port command block */
#define ATA_REG_DATA        (ATA_PRIMARY_IO + 0)  /* 0x1F0: 16-bit data port (PIO transfers) */
#define ATA_REG_ERROR       (ATA_PRIMARY_IO + 1)  /* 0x1F1: error (read) */
#define ATA_REG_FEATURES    (ATA_PRIMARY_IO + 1)  /* 0x1F1: features (write) */
#define ATA_REG_SECCOUNT0   (ATA_PRIMARY_IO + 2)  /* 0x1F2: sector count */
#define ATA_REG_LBA0        (ATA_PRIMARY_IO + 3)  /* 0x1F3: LBA bits 0-7 */
#define ATA_REG_LBA1        (ATA_PRIMARY_IO + 4)  /* 0x1F4: LBA bits 8-15 */
#define ATA_REG_LBA2        (ATA_PRIMARY_IO + 5)  /* 0x1F5: LBA bits 16-23 */
#define ATA_REG_HDDEVSEL    (ATA_PRIMARY_IO + 6)  /* 0x1F6: drive/head select */
#define ATA_REG_STATUS      (ATA_PRIMARY_IO + 7)  /* 0x1F7: status (read) */
#define ATA_REG_COMMAND     (ATA_PRIMARY_IO + 7)  /* 0x1F7: command (write) */

/* Control block: alternate status / device control (0x3F6), separate
 * from the command block above. Reading this does NOT clear IRQs the
 * way reading 0x1F7 does, which is handy for polling. */
#define ATA_PRIMARY_CTRL     0x3F6
#define ATA_REG_ALTSTATUS    ATA_PRIMARY_CTRL   /* read */
#define ATA_REG_CONTROL      ATA_PRIMARY_CTRL   /* write */

/* ===== ATA Commands (only what's needed for now) ===== */
#define ATA_CMD_READ_PIO     0x20  /* READ SECTORS (with retry), 28-bit LBA, PIO */
#define ATA_CMD_WRITE_PIO    0x30  /* WRITE SECTORS (with retry), 28-bit LBA, PIO */
#define ATA_CMD_CACHE_FLUSH  0xE7  /* FLUSH CACHE — force write-back before we
                                     * consider ide_write_sectors() complete */
#define ATA_CMD_IDENTIFY     0xEC  /* IDENTIFY DEVICE — used by ide_dump_info() (DAY7) */

/* ===== IDENTIFY DEVICE response layout (DAY7) =====
 * ATA_CMD_IDENTIFY returns one 512-byte (256-word) sector via the
 * same PIO data-port mechanism as a normal sector read. Only the
 * fields ide_dump_info() actually uses are named here — the full
 * IDENTIFY structure has ~100 defined fields, most irrelevant to
 * this roadmap.
 *
 * IMPORTANT: every string field in IDENTIFY (model, serial, firmware)
 * is stored as *byte-swapped* ASCII: within each 16-bit word, the
 * high byte comes before the low byte in reading order. E.g. the
 * two-character sequence "QM" at the start of a real model string is
 * stored as the word 0x4D51 ('M','Q'), not 0x514D. ide_dump_info()
 * has to swap each word's two bytes back before printing.
 */
#define ATA_IDENT_MODEL_WORD_OFF    27  /* words 27-46 (20 words = 40 bytes): model string */
#define ATA_IDENT_MODEL_WORD_COUNT  20
#define ATA_IDENT_SECTORS28_OFF     60  /* words 60-61: 28-bit LBA total sector count */

/* ===== drive/head select (0x1F6) bits for LBA28 mode ===== */
#define ATA_HDDEVSEL_LBA     0xE0  /* bits 7,6,5 fixed 1; bit6=1 selects LBA
                                     * addressing; bit4=DRV (0=master). Bits
                                     * 3-0 carry LBA bits 27-24. Bit4/DRV is
                                     * OR'd in by the caller if slave support
                                     * is ever added — today we only target
                                     * the master (DRV=0), matching run.sh's
                                     * single `-drive ... if=ide`. */

/* ===== Status register bits (0x1F7 / 0x3F6) ===== */
#define ATA_SR_ERR   0x01  /* Error */
#define ATA_SR_IDX   0x02  /* Index (vestigial) */
#define ATA_SR_CORR  0x04  /* Corrected data (vestigial) */
#define ATA_SR_DRQ   0x08  /* Data Request: drive ready to transfer data */
#define ATA_SR_DSC   0x10  /* Drive Seek Complete */
#define ATA_SR_DF    0x20  /* Drive Fault */
#define ATA_SR_DRDY  0x40  /* Drive Ready */
#define ATA_SR_BSY   0x80  /* Busy: drive is preparing to send/receive data */

/* ide_wait()
 *
 * Polls the primary channel's status register (0x1F7) until BSY (bit 7)
 * clears, per the roadmap's Day 2 spec. A drive that is truly absent
 * reads back 0xFF on the status port (floating bus), so ide_wait() also
 * bails out early in that case instead of spinning for the full timeout.
 *
 * Returns 0 if the drive became ready (BSY=0) before the timeout,
 * or -1 on timeout / no-drive-present (status stuck at 0xFF).
 * This return convention is what Day 3's ide_read_sectors() will rely
 * on to detect a wedged drive instead of hanging forever.
 */
int ide_wait(void);

/* ide_test()
 *
 * Day 2 acceptance check: read the primary channel's status register
 * and confirm it isn't stuck at the floating-bus value 0xFF (which
 * would mean no drive is wired up at all). Prints the raw status byte
 * and a pass/fail verdict via console_write_color(). Called once from
 * kmain() during boot.
 */
void ide_test(void);

/* ide_read_sectors(lba, count, buf)
 *
 * DAY3: reads `count` 512-byte sectors starting at 28-bit LBA `lba`
 * from the primary channel's master drive into `buf` (which must be
 * at least count*512 bytes), using PIO mode:
 *
 *   1. ide_wait() for BSY to clear before issuing any command.
 *   2. Program sector count (0x1F2), LBA bytes 0-7/8-15/16-23
 *      (0x1F3/0x1F4/0x1F5), and drive/head select with LBA bit 27-24
 *      + LBA-mode bit (0x1F6).
 *   3. Send ATA_CMD_READ_PIO (0x20) to the command port (0x1F7).
 *   4. Per sector: poll status until BSY clear and DRQ set, then
 *      insw() 256 words (512 bytes) from the data port (0x1F0).
 *
 * Returns 0 on success, -1 on timeout/error (drive absent, BSY never
 * clears, or DRQ never sets — checked via ide_wait()-style polling
 * with the same IDE_WAIT_TIMEOUT bound used by ide_wait()).
 *
 * `count` is capped at 256 per the ATA spec's PIO command encoding
 * (a sector count byte of 0x00 means "256 sectors"); this driver only
 * supports the 1-255 explicit range for now since no caller needs
 * the 256-sector edge case yet.
 */
int ide_read_sectors(uint32_t lba, uint8_t count, void *buf);

/* ide_write_sectors(lba, count, buf)
 *
 * DAY4: writes `count` 512-byte sectors starting at 28-bit LBA `lba`
 * to the primary channel's master drive from `buf` (which must hold
 * at least count*512 bytes), using PIO mode:
 *
 *   1. ide_wait() for BSY to clear before issuing any command.
 *   2. Program sector count (0x1F2), LBA bytes 0-7/8-15/16-23
 *      (0x1F3/0x1F4/0x1F5), and drive/head select with LBA bit 27-24
 *      + LBA-mode bit (0x1F6) — identical register setup to
 *      ide_read_sectors().
 *   3. Send ATA_CMD_WRITE_PIO (0x30) to the command port (0x1F7).
 *   4. Per sector: poll status until BSY clear and DRQ set, then
 *      outsw() 256 words (512 bytes) to the data port (0x1F0).
 *   5. After all sectors are pushed, send ATA_CMD_CACHE_FLUSH (0xE7)
 *      and ide_wait() once more so the write is committed before
 *      returning — a caller that immediately reads the same LBA back
 *      (as the Day 4 acceptance test does) must see the new data.
 *
 * Returns 0 on success, -1 on timeout/error (drive absent, BSY never
 * clears, or DRQ never sets), using the same IDE_WAIT_TIMEOUT-bounded
 * polling as ide_read_sectors().
 *
 * Same 1-255 sector-count range restriction as ide_read_sectors()
 * (count=0 means "256 sectors" as an ATA-spec encoding quirk this
 * driver doesn't use yet).
 */
int ide_write_sectors(uint32_t lba, uint8_t count, const void *buf);

/* ide_dump_info(void)
 *
 * DAY7: issues ATA_CMD_IDENTIFY (0xEC) to the primary channel's
 * master drive and prints its model string and total addressable
 * 28-bit LBA sector count to the console.
 *
 * Sequence (per the ATA IDENTIFY DEVICE protocol):
 *   1. ide_wait() for BSY clear, then select the drive via HDDEVSEL
 *      (0x1F6) — no LBA/count fields apply to IDENTIFY itself, unlike
 *      ide_read_sectors()/ide_write_sectors(), so ide_setup_lba28()
 *      is NOT used here.
 *   2. Zero SECCOUNT0/LBA0-2 (0x1F2-0x1F5) per spec before sending
 *      the command — some controllers use non-zero values here to
 *      distinguish IDENTIFY from IDENTIFY PACKET DEVICE.
 *   3. Send ATA_CMD_IDENTIFY (0xEC) to the command port (0x1F7).
 *   4. If the status register reads 0 immediately after, no drive is
 *      attached to this position — bail out.
 *   5. Otherwise poll (ide_drq_wait()-style) until BSY clears and
 *      either DRQ sets (success — response ready) or ERR sets
 *      (failure — likely an ATAPI/non-ATA device that doesn't
 *      support this exact command form).
 *   6. insw() the 256-word (512-byte) response, byte-swap and print
 *      the model string (words 27-46) and the 28-bit sector count
 *      (words 60-61).
 *
 * Prints diagnostics via console_write_color()/printf() either way;
 * does not return a status code since it's purely informational
 * (matches ide_test()'s existing void-returning convention for
 * boot-time diagnostics).
 */
void ide_dump_info(void);

#endif /* LUMA_IDE_H */