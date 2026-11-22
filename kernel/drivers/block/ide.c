/* kernel/drivers/block/ide.c
 * LumaOS 5.0 - IDE (ATA) PIO Driver
 *
 * DAY2 (Week 1, Day 2 of the roadmap):
 *   "创建 kernel/drivers/block/ide.h 和 ide.c。实现 ide_wait()
 *    （轮询 0x1F7 状态寄存器）。编写一个 ide_test() 在 kmain 中调用，
 *    能读到状态寄存器非 0xFF。"
 *
 *   This is intentionally the *minimal* slice: just enough to prove the
 *   kernel can talk to the ATA primary channel at all before Day 3 adds
 *   real PIO sector reads. No sector I/O, no MBR parsing yet — that's
 *   Day 3+.
 *
 * DAY3 (Week 1, Day 3 of the roadmap):
 *   "实现 ide_read_sectors(lba, count, buf)（使用 PIO 模式，insw 指令）。
 *    在 kmain 中读取 LBA 0（MBR），用 console_write_hex() 打印前 64
 *    字节，屏幕上应看到 EB 3C 90（FAT 引导跳转）。"
 *
 *   Adds ide_read_sectors(): standard LBA28 PIO read (command 0x20),
 *   one sector at a time, using insw() to drain each 256-word (512-byte)
 *   data-ready window. kmain() now reads LBA 0 into a stack buffer and
 *   hex-dumps the first 64 bytes so the FAT/MBR boot-jump signature
 *   (EB 3C 90) is directly visible on the text console.
 *
 * DAY4 (Week 1, Day 4 of the roadmap):
 *   "实现 ide_write_sectors(lba, count, buf)（只读模式先通过，写入可先
 *    置空）。能向 LBA 1 写入一个测试字符串，再读回来对比一致。"
 *
 *   Adds ide_write_sectors(): mirrors ide_read_sectors()'s register
 *   setup but issues ATA_CMD_WRITE_PIO (0x30) and drains each sector's
 *   DRQ window with outsw() instead of insw(). Ends with a CACHE_FLUSH
 *   (0xE7) + ide_wait() so the data is guaranteed durable before the
 *   function returns — required for kmain()'s Day 4 round-trip test
 *   (write LBA 1, then immediately read it back and memcmp).
 *
 * DAY7 (Week 1, Day 7 of the roadmap — weekly review):
 *   "整理 IDE 驱动代码，消除硬编码，增加超时保护（防止死循环）。
 *    写一个 ide_dump_info() 打印硬盘型号（IDENTIFY 命令 0xEC）。"
 *
 *   See ide.h's DAY7 header comment for the full rationale. Summary
 *   of what changed in this file:
 *     - io_wait(): new ~1us I/O delay primitive, called once per poll
 *       iteration in ide_wait()/ide_drq_wait() so the timeout tracks
 *       real bus timing instead of raw (host-scheduling-dependent)
 *       spin count.
 *     - IDE_WAIT_TIMEOUT: 100000 -> 1000000, now paced by io_wait()
 *       per iteration rather than a bare register read, giving
 *       genuinely slow first-command latency more real headroom
 *       without letting a truly absent/wedged drive hang boot (the
 *       0xFF floating-bus short-circuit is unchanged).
 *     - ide_setup_lba28(lba, count): the six outb() calls that
 *       programmed sector count + 28-bit LBA + drive/head select were
 *       duplicated verbatim in ide_read_sectors() and
 *       ide_write_sectors(); both now call this one shared helper.
 *     - ide_dump_info(): new IDENTIFY (0xEC) implementation.
 */
#include "ide.h"
#include "../../core/kernel.h"   /* inb/outb, console_write_color, printf */

/* DAY7: raised from 100000 -> 1000000. This bound is now paced by
 * io_wait() once per iteration (see below) rather than a bare
 * register read, so the *real* time it represents is both larger and
 * far more consistent across runs than the old raw spin count was —
 * see ide.h's DAY7 comment for the flakiness this fixes. A genuinely
 * absent drive still returns immediately via the 0xFF floating-bus
 * check in ide_wait()/ide_drq_wait(), so this larger bound only
 * matters for the "drive present but slow to respond" case. */
#define IDE_WAIT_TIMEOUT   1000000

/* io_wait()
 *
 * DAY7: classic OSDev "burn ~1 microsecond" trick — outb() to port
 * 0x80, which the BIOS reserves as a POST diagnostic scratch port
 * that nothing on a PC-compatible system actually reads. Writing to
 * it is a no-op as far as any device is concerned, but it forces one
 * full I/O bus cycle, which takes roughly 1us on real hardware and a
 * comparable, consistent amount of emulated time under QEMU.
 *
 * Called once per polling iteration in ide_wait()/ide_drq_wait() so
 * IDE_WAIT_TIMEOUT's 1,000,000 iterations correspond to a bounded,
 * predictable real-time budget instead of however many raw CPU
 * cycles a bare inb()-in-a-loop happens to take on whatever host is
 * running QEMU that day.
 */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

int ide_wait(void)
{
    uint8_t status;

    for (int i = 0; i < IDE_WAIT_TIMEOUT; i++) {
        status = inb(ATA_REG_STATUS);

        /* Floating bus: no drive present on this channel at all.
         * Bail out immediately rather than spinning to the timeout. */
        if (status == 0xFF)
            return -1;

        /* BSY clear means the drive is done preparing and ready for
         * the next command/data phase. */
        if ((status & ATA_SR_BSY) == 0)
            return 0;

        io_wait(); /* DAY7: pace the poll against real bus timing */
    }

    return -1; /* timed out still busy */
}

void ide_test(void)
{
    console_write_color("[ide]  Probing primary ATA channel (0x1F7)...\n", COLOR_INFO);

    uint8_t status = inb(ATA_REG_STATUS);
    printf("[ide]  status = 0x%02x\n", status);

    if (status == 0xFF) {
        console_write_color("[ide]  FAIL: status stuck at 0xFF (no drive / floating bus)\n",
                             COLOR_ERROR);
        return;
    }

    console_write_color("[ide]  OK: status register is live (drive present)\n",
                         COLOR_SUCCESS);

    /* Also exercise ide_wait() itself, per the Day 2 spec, so a wedged
     * BSY line is caught here too and not just via the raw status read
     * above. */
    if (ide_wait() == 0) {
        console_write_color("[ide]  OK: ide_wait() returned ready (BSY clear)\n",
                             COLOR_SUCCESS);
    } else {
        console_write_color("[ide]  WARN: ide_wait() timed out (BSY never cleared)\n",
                             COLOR_WARNING);
    }
}

/* ide_drq_wait()
 *
 * DAY3 helper: like ide_wait(), but polls until BSY is clear AND DRQ
 * (data request) is set — the point at which the drive has a sector's
 * worth of data staged in its buffer and insw() can safely drain it.
 * Also bails out on ERR/DF (bits checked in the status byte) so a
 * rejected command doesn't spin the full timeout before being reported.
 *
 * Returns 0 when DRQ is ready, -1 on timeout, floating bus, or the
 * drive signaling ERR/DF.
 */
static int ide_drq_wait(void)
{
    uint8_t status;

    for (int i = 0; i < IDE_WAIT_TIMEOUT; i++) {
        status = inb(ATA_REG_STATUS);

        if (status == 0xFF)
            return -1;                      /* no drive / floating bus */

        if (status & (ATA_SR_ERR | ATA_SR_DF))
            return -1;                      /* drive reported an error */

        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ))
            return 0;                       /* data ready to transfer */

        io_wait(); /* DAY7: pace the poll against real bus timing */
    }

    return -1; /* timed out waiting for DRQ */
}

/* ide_setup_lba28(lba, count)
 *
 * DAY7: factored out of ide_read_sectors()/ide_write_sectors(), which
 * previously repeated these exact six outb() calls verbatim. Programs
 * sector count (0x1F2) and the 28-bit LBA across LBA0/LBA1/LBA2
 * (0x1F3/0x1F4/0x1F5) plus the low nibble + LBA-mode bit of HDDEVSEL
 * (0x1F6). Does NOT send a command byte — the caller does that
 * immediately afterward (ATA_CMD_READ_PIO or ATA_CMD_WRITE_PIO),
 * since that's the one thing that actually differs between the two
 * callers.
 *
 * Caller is responsible for ide_wait()-ing until BSY is clear before
 * calling this, same precondition both ide_read_sectors() and
 * ide_write_sectors() already enforced individually.
 */
static void ide_setup_lba28(uint32_t lba, uint8_t count)
{
    outb(ATA_REG_SECCOUNT0, count);
    outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_REG_HDDEVSEL, (uint8_t)(ATA_HDDEVSEL_LBA | ((lba >> 24) & 0x0F)));
}

int ide_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
    if (count == 0 || buf == 0)
        return -1;

    uint16_t *dst = (uint16_t *)buf;

    /* Drive must be idle (BSY clear) before we start programming
     * registers for a new command. */
    if (ide_wait() != 0)
        return -1;

    /* DAY7: shared helper (was six duplicated outb() calls). */
    ide_setup_lba28(lba, count);

    /* Issue the read command. */
    outb(ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    /* Drain `count` sectors, one at a time: wait for DRQ, then pull
     * 256 words (512 bytes) via insw() from the data port. The drive
     * automatically re-asserts BSY/DRQ for each subsequent sector in
     * a multi-sector PIO read, so we re-poll before every sector. */
    for (uint8_t s = 0; s < count; s++) {
        if (ide_drq_wait() != 0)
            return -1;

        insw(ATA_REG_DATA, dst, 256);
        dst += 256; /* 256 uint16_t = 512 bytes, advance to next sector */
    }

    return 0;
}

int ide_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
    if (count == 0 || buf == 0)
        return -1;

    const uint16_t *src = (const uint16_t *)buf;

    /* Drive must be idle (BSY clear) before we start programming
     * registers for a new command — identical precondition to
     * ide_read_sectors(). */
    if (ide_wait() != 0)
        return -1;

    /* DAY7: shared helper (was six duplicated outb() calls, identical
     * to ide_read_sectors()'s except the command byte below). */
    ide_setup_lba28(lba, count);

    /* Issue the write command. */
    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    /* Push `count` sectors, one at a time: wait for DRQ (drive ready
     * to accept a sector's worth of data into its buffer), then push
     * 256 words (512 bytes) via outsw() to the data port. As with
     * reads, the drive re-asserts BSY/DRQ for each subsequent sector
     * in a multi-sector PIO write, so we re-poll before every sector. */
    for (uint8_t s = 0; s < count; s++) {
        if (ide_drq_wait() != 0)
            return -1;

        outsw(ATA_REG_DATA, src, 256);
        src += 256; /* 256 uint16_t = 512 bytes, advance to next sector */
    }

    /* Force the drive to commit the write (QEMU's IDE emulation does
     * honor CACHE FLUSH synchronously), then wait for BSY to clear
     * again so a caller that immediately issues a read of the same
     * LBA is guaranteed to observe the new data rather than racing
     * an in-flight write. */
    if (ide_wait() != 0)
        return -1;

    outb(ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);

    if (ide_wait() != 0)
        return -1;

    return 0;
}

/* ide_dump_info(void)
 *
 * DAY7: issues ATA_CMD_IDENTIFY (0xEC) and prints the model string +
 * 28-bit sector count from the response. See ide.h's doc comment for
 * the full protocol sequence and the byte-swap quirk in IDENTIFY's
 * string fields.
 */
void ide_dump_info(void)
{
    console_write_color("[ide]  Sending IDENTIFY (0xEC)...\n", COLOR_INFO);

    if (ide_wait() != 0) {
        console_write_color("[ide]  FAIL: drive busy, IDENTIFY not sent\n",
                             COLOR_ERROR);
        return;
    }

    /* Select the master drive on this channel. No LBA/count fields
     * apply to IDENTIFY itself (unlike a normal read/write), so this
     * intentionally does NOT call ide_setup_lba28(). Per spec, zero
     * out SECCOUNT0/LBA0-2 before sending the command — some
     * controllers use non-zero values in these registers to
     * distinguish IDENTIFY DEVICE from IDENTIFY PACKET DEVICE
     * (ATAPI), and we want the plain-ATA form. */
    outb(ATA_REG_HDDEVSEL, ATA_HDDEVSEL_LBA);
    outb(ATA_REG_SECCOUNT0, 0);
    outb(ATA_REG_LBA0, 0);
    outb(ATA_REG_LBA1, 0);
    outb(ATA_REG_LBA2, 0);

    outb(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_REG_STATUS);
    if (status == 0) {
        console_write_color("[ide]  FAIL: no drive at this position (status=0)\n",
                             COLOR_ERROR);
        return;
    }

    /* Poll BSY clear, then check for ERR (a device that accepted the
     * command byte but doesn't actually support plain IDENTIFY — e.g.
     * an ATAPI drive, which would need IDENTIFY PACKET DEVICE
     * instead — is out of scope for this single-disk roadmap) before
     * looking for DRQ. */
    int timed_out = 1;
    for (int i = 0; i < IDE_WAIT_TIMEOUT; i++) {
        status = inb(ATA_REG_STATUS);

        if (status & ATA_SR_ERR) {
            console_write_color(
                "[ide]  FAIL: drive reported ERR on IDENTIFY (not a plain ATA disk?)\n",
                COLOR_ERROR);
            return;
        }

        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ)) {
            timed_out = 0;
            break;
        }

        io_wait();
    }

    if (timed_out) {
        console_write_color("[ide]  FAIL: IDENTIFY timed out waiting for DRQ\n",
                             COLOR_ERROR);
        return;
    }

    static uint16_t ident[256];
    insw(ATA_REG_DATA, ident, 256);

    /* Words 27-46: model string, 20 words = 40 ASCII bytes, each word
     * byte-swapped (high byte printed before low byte). Trailing
     * spaces are trimmed for a cleaner printout. */
    static char model[ATA_IDENT_MODEL_WORD_COUNT * 2 + 1];
    for (int i = 0; i < ATA_IDENT_MODEL_WORD_COUNT; i++) {
        uint16_t w = ident[ATA_IDENT_MODEL_WORD_OFF + i];
        model[i * 2]     = (char)(w >> 8);
        model[i * 2 + 1] = (char)(w & 0xFF);
    }
    model[ATA_IDENT_MODEL_WORD_COUNT * 2] = '\0';

    int end = ATA_IDENT_MODEL_WORD_COUNT * 2 - 1;
    while (end >= 0 && model[end] == ' ') {
        model[end] = '\0';
        end--;
    }

    /* Words 60-61: 28-bit LBA total sector count, low word first (NOT
     * byte-swapped like the string fields — this is a plain 32-bit
     * little-endian value split across two words). */
    uint32_t total_sectors = (uint32_t)ident[ATA_IDENT_SECTORS28_OFF] |
                              ((uint32_t)ident[ATA_IDENT_SECTORS28_OFF + 1] << 16);

    console_write_color("[ide]  OK: IDENTIFY response received\n", COLOR_SUCCESS);
    printf("[ide]  Model: %s\n", model);
    printf("[ide]  Total sectors (28-bit LBA): %u (%u MB)\n",
           total_sectors, total_sectors / 2048);
}