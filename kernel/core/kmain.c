/* kernel/core/kmain.c
 * LumaOS 5.0 - Kernel Main Entry Point
 *
 * DAY2 (Week 1, Day 2 of the 5.0 filesystem roadmap):
 *   - #include the new IDE driver header and call ide_test() once,
 *     right after interrupts are enabled, so the Day-2 acceptance
 *     check ("status register reads back non-0xFF") runs on every
 *     boot.
 *
 * DAY3 (Week 1, Day 3 of the 5.0 filesystem roadmap):
 *   - After ide_test(), call ide_read_sectors(0, 1, mbr_buf) to read
 *     LBA 0 into a stack buffer, then console_write_hex() the first
 *     64 bytes. This is the roadmap's *proof of correct PIO reads*:
 *     the driver is reading real disk data, not garbage/all-zero/
 *     all-0xFF.
 *
 *     NOTE ON THE "EB 3C 90" CHECK: the roadmap's acceptance text
 *     assumes LBA 0 holds a standard FAT boot sector, which begins
 *     with the 3-byte jump "EB 3C 90". LumaOS's LBA 0 is instead
 *     boot/mbr.asm's own custom (non-FAT) bootloader — its real
 *     first bytes assemble to "FA 31 C0 8E D8 ..." (cli; xor ax,ax;
 *     ...), confirmed by hand-assembling mbr.asm. That's expected and
 *     correct for this project: it proves the read pulled real,
 *     deterministic bytes off LBA 0 rather than confirming a FAT
 *     signature that doesn't apply until a FAT-formatted partition/
 *     image is read (Day 5+ MBR-partition work, and the "mkfs.fat"
 *     test images built starting Day 16). Don't "fix" this to force
 *     EB 3C 90 to appear — that would mean writing a fake boot sector
 *     over the real one.
 *
 * DAY4 (Week 1, Day 4 of the 5.0 filesystem roadmap):
 *   - After the Day 3 LBA 0 read/hex-dump, add the Day 4 acceptance
 *     test: build a test string in a stack buffer, zero-pad it to a
 *     full 512-byte sector, ide_write_sectors() it to LBA 2000, then
 *     ide_read_sectors() it back into a *separate* buffer and
 *     memcmp() the two.
 *
 * DAY5 (Week 1, Day 5 of the 5.0 filesystem roadmap):
 *   - "解析 MBR（LBA 0），扫描分区表（偏移 0x1BE）。识别第一个活动分区
 *      （Type 0x0C = FAT32 LBA，0x06 = FAT16，0x01 = FAT12）。在终端
 *      打印出分区起始 LBA 和大小。"
 *   - Reuses the same mbr_buf already read for the Day 3 hex-dump
 *     (LBA 0 is only read from disk once per boot) and hands it to
 *     the new kernel/drivers/block/mbr.{h,c} parser: mbr_parse()
 *     copies out the 4 raw partition-table entries at offset 0x1BE
 *     and checks the 0x55AA signature, mbr_dump() prints every
 *     non-empty entry's start LBA and sector count to the console
 *     (the Day 5 acceptance criteria), and
 *     mbr_find_first_active_fat() reports which entry (if any) is
 *     both active (status 0x80) and one of the three recognized FAT
 *     types.
 *   - NOTE: build.sh's disk image (see its DAY1/DAY2 comments) does
 *     not itself carve out a partitioned FAT volume — LBA 0 holds
 *     LumaOS's own custom mbr.asm bootloader, not a partitioned
 *     disk, so an all-empty/all-inactive partition table here is the
 *     *expected* result on the stock image. This step proves the
 *     scanner itself is correct (offsets, signature check, type
 *     matching); Day 6+ block-device work and the Day 16+ FAT test
 *     images are what will actually populate a matching partition
 *     table to exercise the "found a partition" path.
 *
 * DAY6 (Week 1, Day 6 of the 5.0 filesystem roadmap):
 *   "封装块设备层：struct block_device 包含 read(lba, count, buf) 函数
 *    指针。上层代码不再直接调用 ide 函数，统一走设备结构体。"
 *
 *   Every ide_read_sectors()/ide_write_sectors() call this file made
 *   directly on Day 3/4 is replaced with a call through a
 *   `block_device_t *` obtained from ide0_block_device()
 *   (kernel/drivers/block/block.{h,c}): dev->read(dev, lba, count,
 *   buf) and dev->write(dev, lba, count, buf) instead of
 *   ide_read_sectors(lba, count, buf) / ide_write_sectors(lba, count,
 *   buf). Functionally identical output (same acceptance checks, same
 *   hex dump, same LBA 2000 write/readback test, same MBR parse) —
 *   this step is purely about routing through the new abstraction so
 *   Week 2's VFS/FAT code has a device-independent interface to build
 *   on instead of naming the IDE driver directly. ide_test() itself
 *   is left as a direct call: it's an IDE-specific diagnostic (probes
 *   the raw status register), not a sector I/O operation, so it has
 *   no block_device_t equivalent and isn't in scope for this
 *   abstraction.
 *
 *     LBA 2000 (NOT LBA 1!) is used deliberately. build.sh lays out
 *     the image as: mbr.bin @ LBA 0, loader.bin @ LBA 1-5, kernel.bin
 *     @ LBA 6 through (6 + MAX_LOAD/512 - 1) = LBA 261 (MAX_LOAD is
 *     131072 bytes = 256 sectors). LBA 1 is loader.bin's *on-disk*
 *     location — writing there doesn't corrupt the current boot
 *     (loader.bin is already loaded into RAM and finished running by
 *     the time kmain() executes), but it permanently corrupts
 *     loader.bin inside lumaos-5.0.img itself, so the *next* boot
 *     from that same image file loads garbage in place of the real
 *     second-stage loader and fails at the BIOS INT13h/AH=42h read
 *     with "DISK ERROR" (confirmed via GDB: EIP lands in loader.asm's
 *     disk_err handler with AH=0x04). LBA 2000 sits far past LBA 261
 *     (still well inside the 2880-sector/1.44MB image build.sh
 *     creates) so it can never overlap mbr/loader/kernel on-disk,
 *     making the write-then-reread test idempotent across reboots.
 *
 * DAY7 (Week 1, Day 7 of the roadmap — weekly review):
 *   "整理 IDE 驱动代码，消除硬编码，增加超时保护（防止死循环）。
 *    写一个 ide_dump_info() 打印硬盘型号（IDENTIFY 命令 0xEC）。"
 *
 *   This file's only change: one new call, ide_dump_info(), right
 *   after ide_test() and before the Day 6 block_device_t section
 *   (grouped with ide_test() since both are IDE-specific diagnostics
 *   with no block_device_t equivalent — see the Day 6 note above on
 *   why ide_test() itself isn't routed through the abstraction
 *   either). The IDE driver's own cleanup (io_wait()-paced polling,
 *   the raised IDE_WAIT_TIMEOUT, and the new shared
 *   ide_setup_lba28() helper replacing duplicated register-setup
 *   code) lives entirely in kernel/drivers/block/ide.c and doesn't
 *   change this file's behavior — same hex dump, same LBA 2000
 *   write/readback result, same MBR parse as Day 6, just with more
 *   resilient timeout handling underneath.
 *
 * DAY9 (Week 2, Day 9 of the 5.0 filesystem roadmap):
 *   "创建 kernel/fs/fat/fat12.h。定义 BPB 结构体（bpb_12_t）和目录项
 *    结构（fat_dirent_t）。写一个 fat12_parse_bpb(buf)，能算出
 *    RootDir 扇区数、Data 起始扇区。"
 *
 *   Added a new section right after the Day 5 MBR-parse block: reads
 *   LBA 0 through the same disk->read(...) block_device_t call
 *   already used for the Day 3/5 mbr_buf (no second disk read —
 *   mbr_buf is reused again, same as Day 5 reused Day 3's read), and
 *   hands it to fat12_parse_bpb() (kernel/fs/fat/fat12.{h,c}).
 *
 *   NOTE ON WHY THIS "PARSES" LumaOS's OWN mbr.asm SECTOR: exactly
 *   like Day 3/5's note about LBA 0 not being a real FAT boot sector,
 *   fat12_parse_bpb()'s signature+field sanity check is *expected* to
 *   report failure (return -1) on the stock image, since LBA 0 here
 *   is LumaOS's own custom bootloader, not a FAT12-formatted volume.
 *   That's the correct, expected result — it proves the parser's
 *   sanity check and geometry arithmetic run without crashing on
 *   arbitrary sector data; a real "RootDirSectors/first data sector"
 *   result that matches a known-good FAT12 image is exercised once
 *   Day 10's fat12_mount() is pointed at an actual `mkfs.fat -F 12`
 *   test image (Day 16+ introduces the FAT16/32 test-image tooling
 *   this roadmap step is a precursor to). Printing the geometry
 *   fields regardless of the sanity-check result (same "print
 *   anyway, treat with suspicion" contract mbr_parse()/mbr_dump()
 *   already use) is what satisfies Day 9's acceptance criteria today
 *   without a FAT-formatted image yet on hand.
 *
 * DAY9-FOLLOWUP (verifying fat12_parse_bpb() against a REAL FAT12
 * volume, not just LBA 0's expected-invalid result):
 *   Added a second read/parse pass right after the LBA 0 one, this
 *   time against LBA 2500, which build.sh now bakes a genuine
 *   `mkfs.fat -F 12` boot sector onto (tests/fat12-bpb.bin — see
 *   build.sh's DAY9-FOLLOWUP comment for the exact layout/rationale).
 *   This is the positive-case counterpart to the LBA 0 negative case
 *   above: same fat12_parse_bpb() call, but against a sector that
 *   really is a valid FAT12 BPB, so bpb2_ok is expected to be true
 *   and the printed RootDirSectors/FirstDataSector/etc. are expected
 *   to be real, correct geometry (14/19/33/1/2880 for the specific
 *   1.44MB image tests/fat12-bpb.bin's boot sector came from) rather
 *   than the LBA 0 pass's garbage-but-harmless numbers.
 *
 * DAY10 (Week 2, Day 10 of the 5.0 filesystem roadmap):
 *   "实现 fat12_mount(block_dev)：读取引导扇区，检查 BS_FileSysType
 *    是否为 FAT12。挂载成功返回 0，失败返回 -1，在终端打印
 *    'FAT12 mounted'。"
 *
 *   Added two fat12_mount() calls right after the Day 9/9-followup
 *   parsing section: one against LBA 2500 (the same real FAT12
 *   fixture from the Day 9-followup section — expected to succeed,
 *   prints "FAT12 mounted"), and one against LBA 0 (LumaOS's own
 *   non-FAT mbr.asm sector — expected to fail with -1, demonstrating
 *   the acceptance criteria's other required path). Unlike the Day 9
 *   tests, which read the sector here in kmain.c and passed the
 *   buffer to fat12_parse_bpb(), fat12_mount() does its own
 *   dev->read(...) internally — kmain.c just passes it the
 *   block_device_t and an LBA. See kernel/fs/fat/fat12.{h,c}'s DAY10
 *   comments for fat12_mount()'s full contract.
 *
 * DAY11 (Week 2, Day 11 of the 5.0 filesystem roadmap):
 *   "实现 FAT12 簇链遍历函数 fat12_next_cluster(cluster)（处理 1.5
 *    字节对齐，注意奇偶簇）。给定根目录的起始簇（通常为 0），能遍历
 *    打印出前 10 个簇号。"
 *
 *   NOTE ON "根目录的起始簇（通常为 0）": FAT12's root directory is a
 *   fixed-size region (fat12_geometry_t.first_root_dir_sec), not a
 *   cluster chain — there's no root-directory "starting cluster" to
 *   hand fat12_next_cluster() on FAT12/16 (see fat12.h's DAY11
 *   comment). This section instead: (a) walks from cluster 2 — the
 *   first legal data cluster on any FAT12 volume — which is exactly
 *   BIGFILE.BIN's actual start cluster in the fixture below, so the
 *   10 printed values are a real, verifiable file's cluster chain,
 *   not arbitrary numbers; and (b) separately queries clusters 0 and
 *   1 (the two truly reserved FAT entries) to demonstrate the walker
 *   recognizes them as reserved/end-of-chain-range values rather than
 *   crashing or looping — the closer reading of what "簇 0 通常" is
 *   actually pointing at.
 *
 *   FIXTURE: tests/fat12-bpb.bin (Day 9/10, LBA 2500) is only a
 *   512-byte boot sector — everything past it on the disk image is
 *   whatever was already zeroed there, so its FAT table has no real
 *   entries and can't exercise the odd/even 1.5-byte packing with an
 *   actual multi-cluster chain. build.sh now also embeds a SECOND,
 *   larger fixture, tests/fat12-with-file.img, at LBA 5000: a
 *   complete 1.44MB `mkfs.fat -F 12` image (512 bytes/sector, 1
 *   sector/cluster) containing two real files copied in with mtools
 *   (mcopy) — BIGFILE.BIN (10240 bytes = exactly 20 clusters, so
 *   "print 10 cluster numbers" has 10 genuine, non-repeating chain
 *   links to show) and SMALL.BIN (1536 bytes = 3 clusters, included
 *   per the roadmap's "至少1-2个" but not otherwise used by this
 *   test). tests/fat12-bpb.bin/LBA 2500 is untouched — this is a
 *   second, independent mount, not a replacement.
 *
 *   CROSS-VALIDATION (done on the host, outside the kernel build, by
 *   parsing tests/fat12-with-file.img's BPB/FAT/root-directory bytes
 *   directly in a Python script — same BPB geometry formulas as
 *   fat12_parse_bpb(), same odd/even FAT12 unpacking as
 *   fat12_next_cluster(), reimplemented independently rather than by
 *   calling this project's own code, and cross-checked against
 *   `fsck.fat -v`'s "23/2847 clusters" used-cluster count and an
 *   `mcopy`-extracted-and-`cmp`'d-byte-identical copy of BIGFILE.BIN):
 *     - BIGFILE.BIN's directory entry: DIR_FstClusLO = 2, file size
 *       10240 bytes (= 20 * 512-byte clusters).
 *     - Expected first 10 fat12_next_cluster() outputs, starting from
 *       cluster 2 and feeding each result back in as the next call's
 *       argument:
 *           2 -> 3 -> 4 -> 5 -> 6 -> 7 -> 8 -> 9 -> 10 -> 11 -> 12
 *         i.e. the sequence of RETURNED values (not counting the
 *         initial 2, which is the starting point, not a return value)
 *         is: 3, 4, 5, 6, 7, 8, 9, 10, 11, 12.
 *       (The full chain continues contiguously through cluster 22,
 *       then FAT[22] = 0xFFF marks end-of-chain — this project's
 *       fixture happens to be built on a freshly-formatted, otherwise
 *       empty volume, so mtools allocated BIGFILE.BIN's 20 clusters
 *       fully contiguously; that's a property of how the fixture was
 *       built; fat12_next_cluster() itself doesn't assume or require
 *       contiguity.)
 *     - FAT[0] = 0xFF0 (media-descriptor-derived) and FAT[1] = 0xFFF
 *       (reads as an ordinary end-of-chain value) — both are RESERVED
 *       BY DEFINITION (the spec never lets a real file claim clusters
 *       0/1), regardless of whether the stored value happens to fall
 *       in the FAT12_CLUSTER_EOC_MIN..MAX range or not (0xFF0 doesn't;
 *       0xFFF does) — confirming the "walking cluster 0 or 1 doesn't
 *       crash or loop" expectation from part (b) above.
 *   The section below prints fat12_next_cluster()'s ACTUAL output for
 *   this exact scenario; comparing the on-screen QEMU output against
 *   the expected values documented here is the acceptance check for
 *   this Day.
 *
 * ============================================================================
 * !! 非常重要 / IMPORTANT — 月末（Day 29 开始）前必须恢复的临时改动 !!
 * ============================================================================
 * 依据路线图第一个月建议（"第 1 个月千万别碰 GUI：文件系统调试期间，只使用
 * 文本终端"），本文件从 DAY2 起临时禁用了两处 GUI 相关调用，让 QEMU 全程
 * 停留在纯文本控制台模式，方便驱动/文件系统的快速调试迭代：
 *
 *   1) vga_init_graphics() 调用本身 —— 见下方 [6/7] 步骤处的注释块。
 *      这一步会把显示器从文本模式切到 BGA 800x450 图形模式；一旦切换，
 *      文本控制台（console_write 系列，写 0xB8000）的输出就再也不会显示
 *      在屏幕上了（黑屏），即使程序运行完全正常。DAY2 之前的版本只注释
 *      掉了下面第 2 条却漏了这一条，导致"程序其实没崩溃，但屏幕全黑"。
 *
 *   2) gui_init() 及其后的 gui_refresh() 主循环 —— 见 kmain() 末尾的
 *      注释块。这是 GUI 桌面/窗口系统的入口，正常情况下它会接管屏幕并
 *      持续绘制界面。
 *
 * 这两处必须【同时】保持注释状态或【同时】恢复，不能只改其中一个：
 *   - 只注释 (2) 不注释 (1) → 会黑屏（已实测复现的问题）。
 *   - 只注释 (1) 不注释 (2) → gui_init() 会在文本模式下强行接管显示，
 *     行为未定义，不要这样做。
 *
 * 本月（Day 1~28，文件系统与磁盘驱动开发期）请保持两处均为注释状态。
 * 第 29 天开始 GUI/桌面相关工作前，请【一次性】取消两处的注释，恢复成
 * 正常的图形模式 + gui_init() 主循环。
 *
 * DAY43 更新：两处已【同时】恢复，但不再用"注释/取消注释"手工切换，
 * 而是编译开关 LUMA_TEXT_MODE（`LUMAOS_TEXT=1 ./build.sh`）：定义它 →
 * 旧的纯文本调试模式（text_shell_run）；不定义（默认）→ GUI 模式
 * （vga_init_graphics + gui_init + gui_refresh 主循环）。见 kmain() 末尾。
 * ============================================================================
 */
#include "kernel.h"
#include "../drivers/block/ide.h"
#include "../drivers/block/block.h"
#include "../drivers/block/mbr.h"
#include "../fs/fat/fat12.h"
#include "../fs/fat/fat_common.h"  /* DAY15: fat_mount(), fat_type_t, FAT_TYPE_*,
                                       fat_bpb_common_t, fat_geometry_t — pulled in
                                       transitively via fat12.h too, but included
                                       explicitly since this file's new DAY15 test
                                       section below uses these names directly */
#include "../fs/fat/fat16.h"      /* DAY16: fat16_fs_t, fat16_mount(), fat16_readdir() */
#include "../fs/fat/fat32.h"      /* DAY17: fat32_fs_t, fat32_mount(), fat32_read_fsinfo(),
                                       fat32_readdir() */
#include "../fs/fat/fat_vfs.h"    /* DAY16/17: fat_vfs_mount() auto-detect dispatcher */
#include "../fs/lmfs/lmfs.h"      /* DAY32: lmfs_fs_t, lmfs_mount(), LMFS_MAGIC */
#include "../fs/lmfs/lmfs_vfs.h"  /* DAY35: lmfs_vfs_mount() */
#include "../fs/vfs/vfs.h"       /* DAY13: vfs_root, vfs_mount() */
#include "../lib/string.h"       /* DAY35: strcmp() for the IDE0..IDE3 check */
#include "../shell/shell.h"      /* DAY14: shell_init(), shell_exec(),
                                     terminal_t (pulled in via shell.h's
                                     own #include "../gui/gui.h") */
#include "version.h"
#include "bootinfo.h"          /* DAY61: boot_info_init(), boot_is_cd() */
#include "live.h"              /* DAY62: live_init() */
#include "../drivers/keyboard.h"  /* DAY14 bugfix: KEY_UP/DOWN/LEFT/RIGHT/
                                      HOME/END constants for the text-mode
                                      line editor's arrow-key handling */

static const char banner[] =
    LUMAOS_FULLNAME " Kernel\n"
    "==================\n";

/* ============================================================================
 * DAY14 (Week 2, Day 14 of the 5.0 filesystem roadmap):
 *   "整合 Shell：新增命令 cmd_ls（调用 vfs_readdir 列出文件）。在终端输入
 *    ls，能看到 FAT 镜像里的所有文件。"
 *   {大注意：此时需要在非GUI模式中启用shell再操作！}
 *
 * WHY A NEW DRIVER FUNCTION, NOT JUST "turn the GUI back on":
 *   shell_exec()/cmd_table (kernel/shell/shell.c) and every cmd_*.c
 *   file, including this Day's new cmd_ls, are written against
 *   `terminal_t *t` — kernel/gui/gui.h's 400-row/120-col in-memory
 *   scrollback buffer, normally driven by keyboard events routed
 *   through the GUI window system (window1.c's terminal_handle_key(),
 *   fed by gui_refresh()'s main loop) and displayed by
 *   terminal_render() drawing glyphs into the BGA graphics
 *   framebuffer. None of that is available this month: this file's
 *   own "非常重要" header block disables BOTH vga_init_graphics() and
 *   gui_init() through Day 28, staying in plain 80x25 VGA TEXT mode
 *   (0xB8000, console_* functions) for fast driver/filesystem
 *   iteration. Re-enabling the GUI just to reach `ls` would violate
 *   that standing rule and re-introduce the exact "screen goes black"
 *   risk documented at the top of this file.
 *
 *   The fix is NOT a new shell — shell_init()/shell_exec()/cmd_table
 *   and every existing command (including today's cmd_ls) are reused
 *   completely unmodified. What's missing is only the wiring around
 *   them: something to read keystrokes without the GUI's key-routing,
 *   and something to get terminal_t's buffered output onto the real
 *   text-mode screen without terminal_render()'s graphics-only
 *   drawing code. text_shell_run() below is exactly that adapter —
 *   the text-mode equivalent of what terminal_handle_key() +
 *   terminal_render() do for the GUI, nothing more:
 *
 *     1. INPUT: keyboard_getchar() (kernel/drivers/keyboard.c) is a
 *        blocking, interrupt-driven read that already existed before
 *        Day 14 (Day 4/testing infrastructure) and needs no GUI to
 *        work — it just pops bytes off the IRQ1 ring buffer. Each
 *        typed line is assembled into a small local buffer and echoed
 *        straight to the VGA console via console_putc_color() (Enter/
 *        Backspace map onto console_putc_color()'s existing '\n'/'\b'
 *        handling — see console.c — no new console code needed).
 *     2. DISPATCH: once Enter is pressed, the completed line is
 *        handed to shell_exec(&g_text_shell, line) exactly as
 *        terminal_handle_key() already does for the GUI path
 *        (window1.c) — shell_exec() itself has no idea whether its
 *        terminal_t is destined for a graphics window or a text-mode
 *        adapter, which is the whole point of keeping cmd_*.c
 *        GUI-agnostic.
 *     3. OUTPUT: shell_exec() -> the invoked cmd_*() function calls
 *        terminal_puts()/terminal_putchar()/terminal_newline()
 *        (window1.c), which — see that file's own functions — only
 *        ever write into g_text_shell's in-memory `lines[]` array;
 *        they never touch graphics hardware (only terminal_render()
 *        does that, and this file never calls it). So after each
 *        shell_exec() call, text_shell_flush_new_lines() below simply
 *        walks whichever of g_text_shell.lines[] are new since the
 *        last flush and prints THEIR TEXT (not colors — term_line_t
 *        stores a color per cell, but the simplest correct adapter
 *        for Day 14's acceptance bar re-prints each captured line in
 *        one console_write_color() call; a per-cell color remap is a
 *        nice-to-have, not required by "能看到 FAT 镜像里的所有文件")
 *        onto the real 0xB8000 console via console_write_color().
 *
 *   This keeps every later shell command (Day 15 on) working for free
 *   in text mode with zero changes to shell.c/cmd_*.c, and once Day 29
 *   restores the GUI, this whole text_shell_run() function simply
 *   stops being called (kmain() goes back to gui_init()+gui_refresh())
 *   — it does not need to be deleted, since it costs nothing sitting
 *   unused, but it also isn't in the GUI's call path in any way.
 * ============================================================================
 */

/* g_text_shell: the SAME terminal_t shape the GUI terminal window
 * uses (kernel/gui/gui.h), reused here purely as an in-memory command
 * buffer/dispatch target — never rendered to the graphics framebuffer.
 * Static (not a kmain() local) for the same reason kmain.c's other
 * large fixture buffers are static: this struct is a few dozen KB
 * (TERM_BUF_ROWS * TERM_BUF_COLS cells) and this project has no
 * kernel heap yet (Day 73). */
static terminal_t g_text_shell;

/* ---------------------------------------------------------------------
 * BUGFIX (post-DAY14, found from an actual boot screenshot): the first
 * version of this file tracked an ABSOLUTE line-number watermark
 * (g_text_shell_flushed_lines) and only printed g_text_shell lines at
 * index >= that watermark. That silently breaks the moment
 * g_text_shell.total_lines stops growing: window1.c's terminal_newline()
 * caps total_lines at TERM_BUF_ROWS (400) and, once the buffer is
 * full, starts SHIFTING every line down by one slot per newline
 * instead of incrementing total_lines any further (a ring-buffer-style
 * scroll — see terminal_newline()'s own body). After that point
 * total_lines never changes again, so a watermark that was already
 * sitting at 400 makes the very next flush's `for (i = 400; i <
 * total_lines; i++)` loop never execute — new command output is
 * computed correctly by cmd_ls/etc, but silently never reaches the
 * real screen. This is exactly what the reported screenshot showed:
 * the SECOND `ls` printed only SMALL.BIN, not BIGFILE.BIN, because by
 * then g_text_shell had scrolled past the 400-line mark and the
 * watermark-based flush stopped emitting anything for one line's
 * worth of output.
 *
 * FIX: don't track a cross-command watermark at all. terminal_clear()
 * g_text_shell before every shell_exec() call, so each command starts
 * from an empty buffer (total_lines reset to 1, see terminal_clear()),
 * and text_shell_flush_new_lines() below always prints from index 0.
 * No single shell command in this project prints anywhere close to
 * 400 lines, so the "does total_lines ever hit the ring-buffer cap"
 * failure mode this bug came from is now structurally unreachable,
 * not just patched around.
 * --------------------------------------------------------------------- */

/* text_shell_flush_new_lines()
 *
 * Prints every line currently in g_text_shell (index 0 through
 * total_lines-1) to the real text-mode console. Called once per
 * shell_exec(), right after it, with g_text_shell freshly cleared
 * beforehand by the caller — see the BUGFIX note above for why this
 * always starts at 0 rather than tracking a watermark. Reads
 * term_line_t.text[0..len) directly (NUL-terminating into a small
 * stack copy first, since term_line_t.text is not itself
 * NUL-terminated — it's a fixed TERM_BUF_COLS array padded with
 * spaces, see window1.c's terminal_init()/terminal_clear()).
 *
 * Per-cell color (term_line_t.color[]) is intentionally not consulted
 * here — see this file's DAY14 header comment above for why a single
 * COLOR_DEFAULT per line is the correct scope for Day 14 rather than
 * a full per-cell color remap. */
/* term_color_to_vga(gui_color)
 *
 * BUGFIX (found from a review of actual boot output): the first
 * version of text_shell_flush_new_lines() printed every line in one
 * flat COLOR_DEFAULT, discarding term_line_t.color[]'s per-character
 * colors entirely — which is why cmd_shell.c commands that rely on
 * color to communicate meaning (cmd_help.c's TERM_COL_CMD command
 * names, shell.c's own TERM_COL_ERROR "Unknown: foo" message,
 * cmd_color.c's whole reason for existing) all rendered as one flat
 * color in the text-mode shell even though they call terminal_puts()
 * with a real, distinct color argument every time.
 *
 * The reason this couldn't just be "pass ln->color[j] straight to
 * console_putc_color()" is that gui.h's TERM_COL_* constants are NOT
 * VGA text-mode attribute values — most of them (TERM_COL_DEFAULT =
 * COL_TERM_FG = 27, TERM_COL_CMD = COL_TEXT_PRIMARY = 40) are BGA
 * graphics-mode PALETTE INDICES from gui.h's extended UI theme
 * palette (COL_DESKTOP..COL_BTN_CLOSE and beyond, see gui.h), which
 * go well past VGA text mode's legal 0-15 foreground range. Writing
 * one of those raw into a VGA text attribute byte doesn't just pick
 * the "wrong" color — many values corrupt the BACKGROUND nibble too
 * (a VGA text attribute byte packs background in bits 4-7), which is
 * worse than the flat-gray fallback this replaces.
 *
 * So every TERM_COL_* value cmd_*.c code in this project actually
 * uses is mapped by hand below to its nearest VGA 0-15 foreground
 * color. This is a lookup table, not a formula, because the two
 * palettes don't share any numeric relationship (COL_TERM_FG=27 has
 * nothing to do with a VGA "27") — it only needs to cover the colors
 * this project's shell code actually emits (cmd_color.c's COLOR
 * command sets raw 0-15 values directly, which pass through
 * unchanged; everything else funnels through the small set of
 * TERM_COL_* names declared in gui.h). Anything unrecognized falls
 * back to COL_LGRAY (7), the standard VGA "default text" color,
 * rather than crashing or silently going black-on-black. */
static uint8_t term_color_to_vga(uint8_t gui_color)
{
    /* Raw VGA 0-15 values (as cmd_color.c's `color <n>` command uses
     * directly) pass through unchanged — this is what lets `color 1`,
     * `color 3`, etc. keep working exactly as before. */
    if (gui_color <= COL_WHITE)
        return gui_color;

    switch (gui_color) {
    case TERM_COL_DEFAULT: return COL_LGRAY;    /* COL_TERM_FG   -> light gray */
    case TERM_COL_CMD:     return COL_WHITE;    /* COL_TEXT_PRIMARY -> white   */
    /* TERM_COL_PROMPT/SUCCESS (COL_LGREEN), TERM_COL_ERROR (COL_LRED),
     * TERM_COL_INFO (COL_LCYAN) are all <= COL_WHITE already and
     * handled by the pass-through check above — listed here only for
     * documentation, these cases are unreachable. */
    default:
        return COL_LGRAY;
    }
}

/* text_shell_flush_new_lines()
 *
 * Prints every line currently in g_text_shell (index 0 through
 * total_lines-1) to the real text-mode console, ONE CHARACTER AT A
 * TIME through console_putc_color(), using each cell's own
 * ln->color[j] (mapped through term_color_to_vga() — see that
 * function's comment for why a straight pass-through doesn't work)
 * instead of one flat color per line. This is what restores the
 * "Unknown: foo" error appearing in red, `color N` actually changing
 * output color, cmd_help.c's command names appearing in a different
 * color from their descriptions, etc. — all of which cmd_*.c code
 * already requests via terminal_puts()'s color argument; only the
 * flush step was throwing that information away.
 *
 * Called once per shell_exec(), right after it, with g_text_shell
 * freshly cleared beforehand by the caller — see the earlier BUGFIX
 * note (above g_text_shell_flushed_lines' old declaration site) for
 * why this always starts at line index 0 rather than tracking a
 * watermark. */
static void text_shell_flush_new_lines(void)
{
    for (int i = 0; i < g_text_shell.total_lines; i++) {
        term_line_t *ln = &g_text_shell.lines[i];
        int len = ln->len;
        if (len > TERM_BUF_COLS) len = TERM_BUF_COLS;
        for (int j = 0; j < len; j++) {
            console_putc_color(ln->text[j], term_color_to_vga(ln->color[j]));
        }
        console_putc('\n');
    }
}

/* text_shell_prompt()
 *
 * Prints the shell prompt, reflecting the actually-mounted device
 * name plus a trailing "/" before the ">".
 *
 * DAY18-FOLLOWUP: the name shown is now `vfs_mount_name_of(vfs_root->
 * fs)` (vfs.c) — the volume's TRUE mount-slot name from the numbered
 * mount table ("IDE0"/"IDE1"/"IDE2", in mount order) — rather than
 * `vfs_root->name` directly. Reading `vfs_root->name` used to be
 * correct back when "IDE0" was the only device string that ever
 * existed (Day 13/14), but stopped being reliable once multiple
 * volumes could be mounted (DAY18-FOLLOWUP's mount table): fat12_vfs_
 * mount()/fat16_vfs_mount()/fat32_vfs_mount() each hardcode their own
 * root node's `.name` to the literal "IDE0" by default and can reset
 * it back to that at any later re-mount of the same fixture (this
 * project's own DAY17-FOLLOWUP "multi-LBA mount summary" section does
 * exactly that, for demo purposes, after the real DAY16/17 mounts
 * already registered their true slot names) — see vfs.c's doc comment
 * on vfs_mount_name_of() for the full story. Falls back to the node's
 * own `.name` if the lookup finds nothing (e.g. `vfs_root` came from
 * somewhere that never called vfs_register_mount() — shouldn't happen
 * in this project today, but keeps this function from ever printing
 * an empty prompt over a NULL/missing name).
 *
 * FORMAT NOTE (per 计划.md's "LumaOS 9.0 目录架构规范"): the final
 * architecture mounts IDE's root at paths like IDE/Boot/, IDE/System/
 * Kernel/, IDE/User/User_1/Documents/ — i.e. the shell's current
 * location is always a PATH, not a bare device name. The "/" here is
 * that path's root separator, shown now even though Day 14's shell
 * has no `cd`/subdirectory support yet (cmd_ls.c only lists
 * vfs_root's immediate children) specifically so the prompt format
 * doesn't need a breaking change later: once a Day well past 14 adds
 * a "current directory" concept, this becomes
 * "IDE/System/Kernel> " — filling in the same slot this "/" already
 * occupies — instead of every earlier "IDE0> "-style prompt needing a
 * retrofit. g_text_shell_cwd below is that future extension point,
 * deliberately kept as just "" for now (see its own comment).
 *
 * Falls back to "LumaOS/ " — the OS's own name, keeping the same
 * trailing-"/" path convention — only if vfs_mount("IDE0") failed and
 * vfs_root is still NULL, so the prompt never dereferences a NULL
 * vfs_root->name (same defensive NULL check cmd_ls.c's own body
 * already applies for the same underlying reason). */

/* DAY42: the "current directory" slot the DAY14 comments above kept
 * reserved ("g_text_shell_cwd ... wiring `cd` up later is a one-line
 * change") is now real. The cwd string lives in kernel/shell/shell.c
 * (shell_get_cwd(), set by cmd_cd once LMFS subdirectories exist —
 * lmfs_mkdir(), Day 42) as a path relative to the current volume's
 * root, no leading/trailing '/', "" = the volume root. The old static
 * g_text_shell_cwd[64] placeholder variable is gone; this prompt asks
 * shell.c instead, so the GUI shell (when it returns) and this text
 * shell can never disagree about where "here" is.
 *
 * Prompt shape (the "/ >" tail is unchanged — tests/lmfs_test_helpers.py
 * uses it as the "boot finished" marker):
 *     volume root        "IDE3/ > "
 *     subdirectory       "IDE3/docs/notes/ > "
 */
static void text_shell_prompt(void)
{
    if (vfs_root && vfs_root->name[0]) {
        const char *mount_name = vfs_mount_name_of(vfs_root->fs);
        console_write_color(mount_name ? mount_name : vfs_root->name,
                             COLOR_PROMPT);
    } else {
        console_write_color("LumaOS", COLOR_PROMPT);
    }
    console_write_color("/", COLOR_PROMPT);

    const char *cwd = shell_get_cwd();
    if (cwd[0]) {
        console_write_color(cwd, COLOR_PROMPT);
        console_write_color("/", COLOR_PROMPT);
    }
    console_write_color(" > ", COLOR_PROMPT);
}

/* ---------------------------------------------------------------------
 * DAY14 line editor (BUGFIX round 2 — history/cursor movement)
 *
 * The first version of text_shell_run() read keyboard_getchar() into
 * a flat local `char line[128]` buffer with backspace-only editing,
 * explicitly filtering out anything >= 0x80 — which silently threw
 * away KEY_UP/KEY_DOWN/KEY_LEFT/KEY_RIGHT (keyboard.h defines these
 * as 0x80-0x83) instead of acting on them. That matched the original
 * DAY14 scope note ("no arrow-key history recall in this text-mode
 * path"), but the reviewer correctly pointed out this isn't
 * acceptable — the GUI terminal (terminal_handle_key(), window1.c)
 * already has working history/cursor movement, and text_shell_run()
 * should not be a strictly worse line editor.
 *
 * Rather than inventing a parallel history/cursor implementation,
 * this reuses the EXACT SAME STATE FIELDS terminal_handle_key()
 * already maintains on a terminal_t — t->input[]/input_len/
 * input_cursor for the in-progress line, t->history[]/history_count/
 * history_pos for recall (all declared once in gui.h, shared by both
 * the GUI and text-mode code paths on the SAME g_text_shell struct).
 * What differs is only how a change gets drawn: terminal_handle_key()
 * sets t->redraw_needed and lets terminal_render() (graphics-only,
 * still never called from this file) repaint the whole GUI window;
 * text_shell_redraw_input_line() below instead repositions the real
 * VGA text-mode cursor and rewrites just the input line in place —
 * the text-console equivalent of the same redraw-on-change idea.
 * --------------------------------------------------------------------- */

/* text_shell_redraw_input_line(&prompt_row, prompt_col)
 *
 * Rewrites the current input line (g_text_shell.input, up to
 * input_len bytes) starting at VGA text position (prompt_col,
 * prompt_row) — i.e. right after the prompt, which the caller must
 * have already printed. Pads over any leftover characters from a
 * previous, longer draw of this same line (e.g. after Backspace/Del
 * shortens it, or after loading a shorter history entry) with spaces,
 * then places the hardware cursor at prompt_col + input_cursor, so
 * LEFT/RIGHT visibly move the blinking cursor within the line instead
 * of only at its end. */
/* DAY43 PRE-3 (③ echo / long input): the old version assumed the whole
 * input line fits on the prompt's row. It does not once prompt + input
 * exceeds 80 columns (e.g. a long `echo` line):
 *   - console_set_cursor() silently IGNORES an x >= 80, so the cursor
 *     (and the redraw start) stayed on the wrong column;
 *   - when the line ran past the bottom row the screen scrolled, but
 *     prompt_row still named the OLD row, so every later keystroke
 *     redrew the line one row too low — the stacked
 *     "teen fouecho one two ..." garbage seen in QEMU.
 * Now: text_shell_place_cursor() converts a flat index into (row, col)
 * with wrapping, and the redraw function detects the scroll (the row the
 * text WOULD end on is below row 24) and moves *prompt_row up by exactly
 * that many rows, so (prompt_row, prompt_col) always names where the
 * prompt's first input cell really is. */
static void text_shell_place_cursor(int prompt_row, int prompt_col, int idx)
{
    int flat = prompt_col + idx;
    console_set_cursor(flat % 80, prompt_row + flat / 80);
}

/* prev_drawn_len: how many cells the previous draw of THIS prompt used
 * (so a shorter line is padded with blanks). Reset at every new prompt. */
static int g_prev_drawn_len = 0;

static void text_shell_redraw_input_line(int *prompt_row, int prompt_col)
{
    text_shell_place_cursor(*prompt_row, prompt_col, 0);
    for (int i = 0; i < g_text_shell.input_len; i++) {
        console_putc_color(g_text_shell.input[i], COLOR_DEFAULT);
    }
    int drawn = g_text_shell.input_len;
    for (int i = g_text_shell.input_len; i < g_prev_drawn_len; i++) {
        console_putc_color(' ', COLOR_DEFAULT);
        drawn++;
    }
    g_prev_drawn_len = g_text_shell.input_len;

    /* Scroll detection: after `drawn` cells starting at prompt_col the
     * cursor sits on row prompt_row + (prompt_col+drawn)/80. Rows past 24
     * do not exist — the console scrolled by exactly the overshoot. */
    int end_row = *prompt_row + (prompt_col + drawn) / 80;
    if (end_row > 24) *prompt_row -= end_row - 24;
    if (*prompt_row < 0) *prompt_row = 0;

    text_shell_place_cursor(*prompt_row, prompt_col, g_text_shell.input_cursor);
}

/* text_shell_history_push/load — thin wrappers around g_text_shell's
 * own history[]/history_count/history_pos fields, mirroring
 * window1.c's static history_push()/history_load() exactly (same
 * field names, same modulo-TERM_HISTORY ring behavior) since those
 * two functions are `static` to window1.c and not exported — rather
 * than exporting them, this project's convention (see fat12_vfs.c's
 * own "separate adapter file" split) is to keep the duplication small
 * and local instead of widening window1.c's API for one caller. */
static void text_shell_history_push(const char *cmd)
{
    if (!cmd[0]) return;
    int slot = g_text_shell.history_count % TERM_HISTORY;
    int ci = 0;
    while (cmd[ci] && ci < TERM_HIST_LEN - 1) {
        g_text_shell.history[slot][ci] = cmd[ci];
        ci++;
    }
    g_text_shell.history[slot][ci] = 0;
    g_text_shell.history_count++;
}

static void text_shell_history_load(void)
{
    if (g_text_shell.history_pos < 0) {
        g_text_shell.input[0] = 0;
        g_text_shell.input_len = 0;
        g_text_shell.input_cursor = 0;
        return;
    }
    int idx = (g_text_shell.history_count - 1 - g_text_shell.history_pos) % TERM_HISTORY;
    if (idx < 0) idx += TERM_HISTORY;
    int ci = 0;
    while (g_text_shell.history[idx][ci] && ci < TERM_INPUT_MAX) {
        g_text_shell.input[ci] = g_text_shell.history[idx][ci];
        ci++;
    }
    g_text_shell.input[ci] = 0;
    g_text_shell.input_len = ci;
    g_text_shell.input_cursor = ci;
}

static void text_shell_run(void)
{
    shell_init();
    terminal_init(&g_text_shell);

    console_write_color(
        "\n[shell] Text-mode shell ready. Type 'help' for commands.\n",
        COLOR_INFO);

    /* Requested: an explicit "starting the prompt on IDE0" status line,
     * separate from the one-time vfs_mount() success message printed
     * earlier by the DAY13 section above — that one reports the MOUNT
     * result; this one reports that the SHELL is now about to start
     * accepting input against that mount.
     *
     * DAY18-FOLLOWUP: uses vfs_mount_name_of(vfs_root->fs) rather than
     * vfs_root->name for the same reason text_shell_prompt() above
     * does — see that function's comment and vfs.c's doc comment on
     * vfs_mount_name_of() for the full story of why the node's own
     * `.name` field can't be trusted for this anymore now that more
     * than one volume can be mounted. Only claims a name when vfs_root
     * is actually non-NULL and a slot name was found — if the mount
     * failed, this says so plainly instead of lying about which device
     * the prompt is backed by. */
    {
        const char *mount_name =
            vfs_root ? vfs_mount_name_of(vfs_root->fs) : (const char *)0;
        if (!mount_name && vfs_root && vfs_root->name[0])
            mount_name = vfs_root->name;   /* defensive fallback only */

        if (mount_name) {
            printf("[shell] Start prompt on %s\n", mount_name);
        } else {
            console_write_color(
                "[shell] Start prompt (no filesystem mounted)\n",
                COLOR_WARNING);
        }
    }

    for (;;) {
        /* Each command's output is rendered into a freshly-cleared
         * g_text_shell (see text_shell_flush_new_lines()'s header
         * comment for why total_lines always starts back at 1 here
         * rather than tracking a cross-command watermark), so this
         * can never hit window1.c's TERM_BUF_ROWS ring-buffer-scroll
         * path for a single command's output. terminal_clear() does
         * NOT touch input_len/input_cursor/history[]/history_count/
         * history_pos (see its own body in window1.c) — those persist
         * across commands exactly as the GUI terminal's do, which is
         * what makes history recall span multiple commands. */
        terminal_clear(&g_text_shell);

        text_shell_prompt();
        int prompt_row = console_get_y();
        int prompt_col = console_get_x();
        g_prev_drawn_len = 0;          /* PRE-3: new prompt, nothing drawn yet */

        g_text_shell.input[0] = 0;
        g_text_shell.input_len = 0;
        g_text_shell.input_cursor = 0;
        g_text_shell.history_pos = -1;

        for (;;) {
            char c = keyboard_getchar();
            unsigned char key = (unsigned char)c;

            if (key == KEY_UP) {
                if (g_text_shell.history_count > 0 &&
                    g_text_shell.history_pos < g_text_shell.history_count - 1) {
                    g_text_shell.history_pos++;
                    text_shell_history_load();
                }
                text_shell_redraw_input_line(&prompt_row, prompt_col);
                continue;
            }
            if (key == KEY_DOWN) {
                if (g_text_shell.history_pos > 0) {
                    g_text_shell.history_pos--;
                    text_shell_history_load();
                } else if (g_text_shell.history_pos == 0) {
                    g_text_shell.history_pos = -1;
                    text_shell_history_load();
                }
                text_shell_redraw_input_line(&prompt_row, prompt_col);
                continue;
            }
            if (key == KEY_LEFT) {
                if (g_text_shell.input_cursor > 0) g_text_shell.input_cursor--;
                text_shell_place_cursor(prompt_row, prompt_col, g_text_shell.input_cursor);
                continue;
            }
            if (key == KEY_RIGHT) {
                if (g_text_shell.input_cursor < g_text_shell.input_len)
                    g_text_shell.input_cursor++;
                text_shell_place_cursor(prompt_row, prompt_col, g_text_shell.input_cursor);
                continue;
            }
            if (key == KEY_HOME) {
                g_text_shell.input_cursor = 0;
                text_shell_place_cursor(prompt_row, prompt_col, 0);
                continue;
            }
            if (key == KEY_END) {
                g_text_shell.input_cursor = g_text_shell.input_len;
                text_shell_place_cursor(prompt_row, prompt_col, g_text_shell.input_cursor);
                continue;
            }
            /* DAY28 ("并提供delete键支持" — provide Delete-key support):
             * forward-delete — removes the character AT the cursor
             * (shifting everything after it left by one), the mirror
             * image of Backspace's "remove the character BEFORE the
             * cursor". keyboard.c already decodes the real Delete key
             * (E0 53 / numpad-Del-without-Num-Lock) into KEY_DEL — see
             * keyboard.h's own comment — this single-line editor simply
             * never had a case for it before today, unlike the GUI's
             * own terminal_handle_key() (kernel/gui/window1.c), which
             * has handled KEY_DEL this exact way since before this
             * project's Day 1. This is a straight port of that
             * already-correct logic into the text-mode adapter — the
             * same "reuse the GUI's own state-machine shape" principle
             * this whole function's header comment already documents.
             * A no-op at end-of-line (input_cursor == input_len — there
             * is nothing to the right of the cursor to pull left). */
            if (key == KEY_DEL) {
                if (g_text_shell.input_cursor < g_text_shell.input_len) {
                    for (int i = g_text_shell.input_cursor;
                         i < g_text_shell.input_len - 1; i++)
                        g_text_shell.input[i] = g_text_shell.input[i + 1];
                    g_text_shell.input_len--;
                    g_text_shell.input[g_text_shell.input_len] = 0;
                    text_shell_redraw_input_line(&prompt_row, prompt_col);
                }
                continue;
            }

            if (c == '\r' || c == '\n') {
                text_shell_place_cursor(prompt_row, prompt_col, g_text_shell.input_len);
                console_putc('\n');
                break;
            }
            if (c == '\b') {
                /* Backspace: delete the char BEFORE the cursor
                 * (matches shell.c's simple model — keyboard.c's own
                 * keymap already gives us '\b' for the Backspace key,
                 * same as before this bugfix). */
                if (g_text_shell.input_cursor > 0) {
                    for (int i = g_text_shell.input_cursor - 1;
                         i < g_text_shell.input_len - 1; i++)
                        g_text_shell.input[i] = g_text_shell.input[i + 1];
                    g_text_shell.input_len--;
                    g_text_shell.input_cursor--;
                    g_text_shell.input[g_text_shell.input_len] = 0;
                    text_shell_redraw_input_line(&prompt_row, prompt_col);
                }
                continue;
            }
            /* Any other non-printable/control byte (function keys,
             * PGUP/PGDN, Ctrl combos, KEY_INS, ...) is simply not
             * handled by this simple line editor — matches shell.c's
             * own "space-delimited argv" scope, same as before this
             * bugfix. DAY28: KEY_DEL is no longer in that list — see
             * the dedicated branch above. */
            if (key < 0x20 || key > 0x7E)
                continue;

            if (g_text_shell.input_len < TERM_INPUT_MAX) {
                /* Insert at cursor (not always append) — shift
                 * everything from input_cursor onward right by one. */
                for (int i = g_text_shell.input_len; i > g_text_shell.input_cursor; i--)
                    g_text_shell.input[i] = g_text_shell.input[i - 1];
                g_text_shell.input[g_text_shell.input_cursor] = c;
                g_text_shell.input_len++;
                g_text_shell.input_cursor++;
                g_text_shell.input[g_text_shell.input_len] = 0;
                text_shell_redraw_input_line(&prompt_row, prompt_col);
            }
        }

        text_shell_history_push(g_text_shell.input);
        shell_exec(&g_text_shell, g_text_shell.input);

        /* BUGFIX (reported: "clear 不好使"): cmd_clear.c's body is
         * just `terminal_clear(t)` — correct and sufficient for the
         * GUI path, where terminal_render() repaints the whole window
         * from g_terminal's (now-empty) buffer every frame, so an
         * empty buffer IS an empty screen. In this text-mode path
         * there is no equivalent repaint: text_shell_flush_new_lines()
         * only ever APPENDS g_text_shell's lines to whatever is
         * already on the real VGA screen, it never erases anything
         * already there. So after `clear` ran, g_text_shell was
         * correctly empty (flush prints nothing extra, matching this
         * function's own now-stale NOTE below) but the actual screen
         * — the thing the person is looking at — never got touched,
         * making the command look like a no-op.
         *
         * Fix: when the command that just ran was `clear` (bare, or
         * with any argv shell.c's parser would still route to
         * cmd_clear — this project's parser only ever looks at
         * argv[0], see shell.c's shell_exec()/shell_parse()), also
         * clear the REAL screen via console_clear() (console.c —
         * already used by console_init() at boot) and let the next
         * loop iteration's text_shell_prompt() print fresh at row 0
         * instead of wherever output had scrolled to. Detected by
         * comparing g_text_shell.input's first whitespace-delimited
         * token to "clear" here, rather than special-casing inside
         * cmd_clear.c itself — cmd_clear.c stays GUI/text-mode
         * agnostic like every other cmd_*.c file (see cmd_ls.c's own
         * header comment for why that separation matters). */
        {
            const char *p = g_text_shell.input;
            while (*p == ' ') p++;
            int i = 0;
            while (p[i] && p[i] != ' ' && i < 15) i++;
            if (i == 5 && p[0]=='c' && p[1]=='l' && p[2]=='e' && p[3]=='a' && p[4]=='r')
                console_clear();
        }

        text_shell_flush_new_lines();
    }
}
/* NOTE: g_text_shell is cleared at the TOP of each loop iteration
 * (right before text_shell_prompt()), not here at the bottom — this
 * matters for `clear`: cmd_clear.c's own body is `terminal_clear(t)`,
 * so if shell_exec() just ran `clear`, g_text_shell is already empty
 * and text_shell_flush_new_lines() correctly prints nothing extra;
 * the NEXT loop iteration's terminal_clear() at the top is then a
 * harmless no-op on an already-empty buffer. Separately, the REAL
 * screen is now also cleared right after shell_exec() above — see
 * that block's own comment for why that extra step was missing
 * before and made `clear` look broken in text mode specifically. */

/* day42_mkdir_idem(fs, parent, name, out)
 *
 * DAY42 boot self-test helper: make sure directory `parent`/`name`
 * exists and return its inode in *out. "Already exists as a directory"
 * counts as success — the image may be left over from an earlier boot
 * (same idempotent-setup trick DAY38~41 use for their files), and the
 * Day 42 section below must give the same answer on boot 1 and boot 2.
 * Returns 1 on success, 0 on failure. */
static int day42_mkdir_idem(lmfs_fs_t *fs, uint32_t parent, const char *name,
                            uint32_t *out)
{
    uint32_t existing;
    if (lmfs_lookup(fs, parent, name, &existing) == 0) {
        lmfs_inode_t ino;
        if (lmfs_read_inode(fs, existing, &ino) != 0 ||
            ino.type != LMFS_TYPE_DIR)
            return 0;
        *out = existing;
        return 1;
    }
    if (lmfs_mkdir(fs, parent, name, out) != 0)
        return 0;
    return 1;
}

void kmain(void)
{
    boot_info_init();        /* DAY61: copy the loader's block at 0x600 before anything can reuse it */
    console_init();
    console_write_color(banner, COLOR_INFO);

    console_write_color("[1/7] GDT...\n", COLOR_INFO);
    gdt_init();
    console_write_color("      OK\n", COLOR_SUCCESS);

    console_write_color("[2/7] IDT...\n", COLOR_INFO);
    idt_init();
    console_write_color("      OK\n", COLOR_SUCCESS);

    /* DAY53: 开启分页（恒等映射的内核页表，见 kernel/mm/paging.h）。必须在 idt_init() 之后：
     * 缺页由 IDT 里的向量 14 处理。之后的一切（驱动、GUI、ELF 装载）都在分页下运行。 */
    console_write_color("[2b/7] Paging...\n", COLOR_INFO);
    paging_init();
    dfault_init();               /* DAY54: #DF 任务门（栈溢出进护栏页时给出 panic 而不是三重故障复位）*/
    console_write_color("      OK\n", COLOR_SUCCESS);

    console_write_color("[3/7] PIC...\n", COLOR_INFO);
    pic_init();
    console_write_color("      OK\n", COLOR_SUCCESS);

    console_write_color("[4/7] Keyboard...\n", COLOR_INFO);
    keyboard_init();
    console_write_color("      OK\n", COLOR_SUCCESS);

    console_write_color("[5/7] Mouse...\n", COLOR_INFO);
    mouse_init();
    console_write_color("      OK\n", COLOR_SUCCESS);

    /* ------------------------------------------------------------------
     * !! DAY2 临时改动 (禁用位置 1/2) — 月末 (Day 29) 前必须保持注释 !!
     * 详细原因见文件顶部的说明块。简述：调用 vga_init_graphics() 会把
     * 显示器切换到 BGA 800x450 图形模式，切换之后文本控制台的输出
     * (console_write 系列) 就不会再显示在屏幕上了 —— 这正是"程序其实
     * 跑得好好的，但屏幕全黑"这个现象的根因。第一个月保持文本模式调试，
     * 所以这里不调用它。要恢复图形模式时，取消下面两行注释，并同步
     * 取消文件末尾 gui_init() 那个块的注释（禁用位置 2/2）。
     *
     * console_write_color("[6/7] VGA BGA 800x450...\n", COLOR_INFO);
     * vga_init_graphics();
     * console_write_color("      OK\n", COLOR_SUCCESS);
     * ------------------------------------------------------------------ */
    /* DAY43: the call itself now happens at the very end of kmain(),
     * right before gui_init() (see the DAY43 comment there for why). */
    console_write_color("[6/7] VGA BGA 800x450... (deferred until the boot self-tests finish)\n",
                        COLOR_WARNING);

    console_write_color("[7/7] Interrupts...\n", COLOR_INFO);
    enable_interrupts();
    console_write_color("      OK\n", COLOR_SUCCESS);

    /* DAY61: LIVE CD MODE. Everything between here and `live_boot` below (about 4000 lines) is the
     * DAY2..DAY42 self-test suite: it probes a HARD DISK full of fixtures at fixed LBAs (FAT12/16/32
     * volumes at 2500/5000/10000/30000, LMFS at 100000). A CD-only machine has none of that - and must
     * not write to anything - so it skips straight to the desktop/shell. (Day 62/63 add the CD's own
     * resources instead.) */
    if (boot_is_cd()) {
        printf("[boot] Booted from CD-ROM (BIOS drive 0x%02X, %u sectors, kernel @LBA %u, LMFS @LBA %u)\n",
               (unsigned)g_boot.drive, (unsigned)g_boot.iso_sectors,
               (unsigned)g_boot.kernel_lba, (unsigned)g_boot.lmfs_lba);
        console_write_color("[boot] Live CD mode: hard-disk self-tests skipped\n", COLOR_WARNING);
        (void)live_init();          /* DAY62: ATAPI -> find the boot CD -> LMFS.IMG into RAM -> mount RAM0 */
        goto live_boot;
    }

    console_write_color("[ide]  IDE driver test...\n", COLOR_INFO);
    ide_test();

    /* ------------------------------------------------------------------
     * DAY7: IDENTIFY (0xEC) — print the attached drive's model string
     * and total sector count. Like ide_test() above, this is an
     * IDE-specific diagnostic (no block_device_t equivalent), so it's
     * called directly rather than through the Day 6 abstraction below.
     * ------------------------------------------------------------------ */
    ide_dump_info();

    /* ------------------------------------------------------------------
     * DAY6: obtain the generic block_device_t for the primary IDE
     * disk. From here on, kmain() reads/writes sectors exclusively
     * through disk->read(...)/disk->write(...) — it never calls
     * ide_read_sectors()/ide_write_sectors() by name again below this
     * point. ide_test() above is exempt: it's a raw status-register
     * probe, not sector I/O, so it has no block_device_t equivalent.
     * ------------------------------------------------------------------ */
    block_device_t *disk = ide0_block_device();
    printf("[block] Using block device \"%s\" (sector_size=%u)\n",
           disk->name, disk->sector_size);

    /* ------------------------------------------------------------------
     * DAY3: read LBA 0 (the MBR) via PIO and hex-dump the first 64
     * bytes. Acceptance check per the roadmap: the first three bytes
     * should read "EB 3C 90" (a short jump + NOP), the standard FAT
     * boot-sector signature that boot/mbr.bin begins with.
     * ------------------------------------------------------------------ */
    console_write_color("[ide]  Reading LBA 0 (MBR) via PIO...\n", COLOR_INFO);
    /* DAY5: hoisted out of the old nested block (was `static uint8_t
     * mbr_buf[512];` scoped to just the hex-dump below) so the Day 5
     * partition-table scan further down can reuse the same 512-byte
     * read of LBA 0 instead of issuing a second read call for
     * identical data.
     * DAY6: now goes through disk->read(...) instead of calling
     * ide_read_sectors() directly. */
    static uint8_t mbr_buf[512];
    int mbr_read_ok = (disk->read(disk, 0, 1, mbr_buf) == 0);

    if (mbr_read_ok) {
        console_write_color("[ide]  OK: LBA 0 read (512 bytes)\n", COLOR_SUCCESS);
        console_write_color("[ide]  First 64 bytes:\n", COLOR_INFO);
        console_write_hex(mbr_buf, 64);
    } else {
        console_write_color("[ide]  FAIL: disk->read(0, 1, ...) failed\n",
                             COLOR_ERROR);
    }

    /* ------------------------------------------------------------------
     * DAY4: write a test string to LBA 2000, read it back, and compare.
     * LBA 2000 is used (NOT LBA 1) so this test can never overlap
     * mbr.bin (LBA 0), loader.bin (LBA 1-5), or kernel.bin (LBA
     * 6-261) on disk — see the file-header comment for why that
     * matters (overwriting loader.bin here would corrupt the image
     * for the *next* boot even though the *current* boot is fine).
     * DAY6: now goes through disk->write(...)/disk->read(...) instead
     * of calling ide_write_sectors()/ide_read_sectors() directly.
     * ------------------------------------------------------------------ */
    console_write_color("[ide]  Write/readback test on LBA 2000...\n", COLOR_INFO);
    {
        static const char test_msg[] = "LumaOS Day4 IDE write/read test - hello disk!";
        static uint8_t write_buf[512];
        static uint8_t read_buf[512];
        const uint32_t test_lba = 2000;

        memset(write_buf, 0, sizeof(write_buf));
        strncpy((char *)write_buf, test_msg, sizeof(write_buf));

        if (disk->write(disk, test_lba, 1, write_buf) != 0) {
            console_write_color("[ide]  FAIL: disk->write(2000, 1, ...) failed\n",
                                 COLOR_ERROR);
        } else {
            console_write_color("[ide]  OK: wrote 512 bytes to LBA 2000\n", COLOR_SUCCESS);

            memset(read_buf, 0, sizeof(read_buf));

            if (disk->read(disk, test_lba, 1, read_buf) != 0) {
                console_write_color("[ide]  FAIL: disk->read(2000, 1, ...) failed\n",
                                     COLOR_ERROR);
            } else if (memcmp(write_buf, read_buf, sizeof(write_buf)) == 0) {
                console_write_color("[ide]  OK: readback matches what was written\n",
                                     COLOR_SUCCESS);
                console_write_color("[ide]  Readback string: ", COLOR_INFO);
                console_write_color((const char *)read_buf, COLOR_DEFAULT);
                console_write_color("\n", COLOR_DEFAULT);
            } else {
                console_write_color("[ide]  FAIL: readback does NOT match written data\n",
                                     COLOR_ERROR);
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY5: parse the MBR partition table (offset 0x1BE of LBA 0) and
     * identify the first *active* FAT partition (type 0x0C/0x06/0x01).
     * Reuses mbr_buf from the Day 3 read above instead of reading
     * LBA 0 a second time. mbr_dump() prints every non-empty entry's
     * start LBA and sector count, satisfying the Day 5 acceptance
     * criteria; mbr_find_first_active_fat() then reports which entry
     * (if any) qualifies as "the first active partition".
     * ------------------------------------------------------------------ */
    console_write_color("[mbr]  Parsing partition table (LBA 0, offset 0x1BE)...\n",
                         COLOR_INFO);
    if (!mbr_read_ok) {
        console_write_color("[mbr]  SKIP: LBA 0 was not read successfully above\n",
                             COLOR_WARNING);
    } else {
        static mbr_partition_entry_t parts[MBR_PARTITION_COUNT];
        int sig_ok = (mbr_parse(mbr_buf, parts) == 0);

        if (sig_ok) {
            console_write_color("[mbr]  OK: boot signature 0x55AA present\n",
                                 COLOR_SUCCESS);
        } else {
            console_write_color(
                "[mbr]  NOTE: boot signature 0x55AA missing at offset 0x1FE "
                "(expected on LumaOS's own mbr.asm image — see kmain.c DAY5 "
                "header comment); parsing entries anyway.\n",
                COLOR_WARNING);
        }

        mbr_dump(parts);

        int active_idx = -1;
        const mbr_partition_entry_t *active =
            mbr_find_first_active_fat(parts, &active_idx);

        if (active) {
            printf("[mbr]  First active FAT partition: entry %d, type=%s, "
                   "start_lba=%u, size=%u sectors\n",
                   active_idx, mbr_type_name(active->type),
                   active->lba_start, active->sector_count);
        } else {
            console_write_color(
                "[mbr]  No active FAT partition found (table empty/inactive "
                "on this image — expected until a real FAT-partitioned "
                "image is used, Day 16+)\n",
                COLOR_WARNING);
        }
    }

    /* ------------------------------------------------------------------
     * DAY6 acceptance check: every sector I/O call above this point
     * (LBA 0 read, LBA 2000 write/readback) went through
     * disk->read(...)/disk->write(...) rather than calling
     * ide_read_sectors()/ide_write_sectors() by name — confirming the
     * block_device_t abstraction is what upper-layer code now uses.
     * ------------------------------------------------------------------ */
    printf("[block] OK: all sector I/O above went through block_device \"%s\"\n",
           disk->name);

    /* ------------------------------------------------------------------
     * DAY9: parse LBA 0 as a FAT12 BPB (fat12_parse_bpb()) and print
     * the two derived geometry values the roadmap's acceptance
     * criteria asks for: RootDirSectors and the first Data Region
     * sector. Reuses mbr_buf from the Day 3 read (already sitting in
     * memory) instead of a third read of LBA 0 — same reuse pattern
     * Day 5's MBR parse already established.
     * ------------------------------------------------------------------ */
    console_write_color("[fat12] Parsing LBA 0 as a FAT12 BPB...\n", COLOR_INFO);
    if (!mbr_read_ok) {
        console_write_color("[fat12] SKIP: LBA 0 was not read successfully above\n",
                             COLOR_WARNING);
    } else {
        static bpb_12_t bpb;
        static fat12_geometry_t geom;
        int bpb_ok = (fat12_parse_bpb(mbr_buf, &bpb, &geom) == 0);

        if (bpb_ok) {
            console_write_color("[fat12] OK: signature 0x55AA + BPB fields look valid\n",
                                 COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat12] NOTE: LBA 0 does not look like a valid FAT12 BPB "
                "(expected on LumaOS's own mbr.asm image — see kmain.c DAY9 "
                "header comment); printing derived geometry anyway.\n",
                COLOR_WARNING);
        }

        printf("[fat12] BytesPerSec=%u SecPerClus=%u NumFATs=%u "
               "RootEntCnt=%u FATSz16=%u\n",
               bpb.BPB_BytesPerSec, bpb.BPB_SecPerClus, bpb.BPB_NumFATs,
               bpb.BPB_RootEntCnt, bpb.BPB_FATSz16);
        printf("[fat12] RootDirSectors=%u  FirstRootDirSector=%u  "
               "FirstDataSector=%u  FirstFATSector=%u  TotalSectors=%u\n",
               geom.root_dir_sectors, geom.first_root_dir_sec,
               geom.first_data_sector, geom.first_fat_sector,
               geom.total_sectors);
    }

    /* ------------------------------------------------------------------
     * DAY9-FOLLOWUP: the LBA 0 pass above only proves fat12_parse_bpb()
     * correctly *rejects* a non-FAT sector without crashing — it can't
     * by itself prove the RootDirSectors/FirstDataSector arithmetic is
     * right, since LBA 0 never was a FAT12 volume. build.sh now bakes
     * a genuine `mkfs.fat -F 12` boot sector (tests/fat12-bpb.bin) onto
     * this image at LBA 2500 (see build.sh's DAY9-FOLLOWUP comment for
     * why that LBA is safe/unused), so this second pass reads *that*
     * sector and runs it through the same fat12_parse_bpb() call,
     * giving a visible, on-screen positive-case result to go with the
     * negative-case one above.
     * ------------------------------------------------------------------ */
    console_write_color("[fat12] Reading LBA 2500 (embedded real FAT12 BPB fixture)...\n",
                         COLOR_INFO);
    {
        static uint8_t fat12_fixture_buf[512];
        int fixture_read_ok = (disk->read(disk, 2500, 1, fat12_fixture_buf) == 0);

        if (!fixture_read_ok) {
            console_write_color("[fat12] SKIP: disk->read(2500, 1, ...) failed\n",
                                 COLOR_ERROR);
        } else {
            static bpb_12_t bpb2;
            static fat12_geometry_t geom2;
            int bpb2_ok = (fat12_parse_bpb(fat12_fixture_buf, &bpb2, &geom2) == 0);

            if (bpb2_ok) {
                console_write_color(
                    "[fat12] OK: LBA 2500 is a valid FAT12 BPB (real mkfs.fat image)\n",
                    COLOR_SUCCESS);
            } else {
                console_write_color(
                    "[fat12] FAIL: LBA 2500 did not parse as a valid FAT12 BPB "
                    "(tests/fat12-bpb.bin missing from image? see build.sh)\n",
                    COLOR_ERROR);
            }

            printf("[fat12] BytesPerSec=%u SecPerClus=%u NumFATs=%u "
                   "RootEntCnt=%u FATSz16=%u\n",
                   bpb2.BPB_BytesPerSec, bpb2.BPB_SecPerClus, bpb2.BPB_NumFATs,
                   bpb2.BPB_RootEntCnt, bpb2.BPB_FATSz16);
            printf("[fat12] RootDirSectors=%u  FirstRootDirSector=%u  "
                   "FirstDataSector=%u  FirstFATSector=%u  TotalSectors=%u\n",
                   geom2.root_dir_sectors, geom2.first_root_dir_sec,
                   geom2.first_data_sector, geom2.first_fat_sector,
                   geom2.total_sectors);
        }
    }

    /* ------------------------------------------------------------------
     * DAY10: "实现 fat12_mount(block_dev)：读取引导扇区，检查
     *  BS_FileSysType 是否为 FAT12。挂载成功返回 0，失败返回 -1，在
     *  终端打印 'FAT12 mounted'。"
     *
     *   fat12_mount() (kernel/fs/fat/fat12.{h,c}) does its own
     *   dev->read(...) internally (unlike the Day 9 tests above, which
     *   read the sector here in kmain.c and handed the buffer to
     *   fat12_parse_bpb() directly) — so this call passes `disk` and
     *   an LBA, not a buffer. Mounted against LBA 2500, the same real
     *   FAT12 fixture (tests/fat12-bpb.bin) the Day 9-followup section
     *   above already parsed successfully, so this is expected to
     *   return 0 and print "FAT12 mounted". A second call against
     *   LBA 0 (LumaOS's own non-FAT mbr.asm sector) demonstrates the
     *   -1/failure path the acceptance criteria also asks for.
     * ------------------------------------------------------------------ */
    console_write_color("[fat12] Mounting FAT12 volume at LBA 2500...\n", COLOR_INFO);
    {
        static fat12_fs_t fs;
        if (fat12_mount(disk, 2500, &fs) == 0) {
            console_write_color("FAT12 mounted\n", COLOR_SUCCESS);
            printf("[fat12] mounted: %u total sectors, %u bytes/cluster, "
                   "data region starts at sector %u\n",
                   fs.geom.total_sectors, fs.geom.bytes_per_cluster,
                   fs.geom.first_data_sector);
        } else {
            console_write_color("[fat12] FAIL: fat12_mount(disk, 2500, ...) "
                                 "returned -1 (unexpected — LBA 2500 should "
                                 "hold a valid FAT12 BPB)\n", COLOR_ERROR);
        }
    }

    console_write_color("[fat12] Mounting FAT12 volume at LBA 0 (expected to fail)...\n",
                         COLOR_INFO);
    {
        static fat12_fs_t fs0;
        if (fat12_mount(disk, 0, &fs0) == 0) {
            console_write_color("FAT12 mounted\n", COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat12] NOTE: mount failed as expected (LBA 0 is LumaOS's "
                "own mbr.asm sector, not a FAT12 volume — see kmain.c DAY9 "
                "comment)\n", COLOR_WARNING);
        }
    }

    /* ------------------------------------------------------------------
     * DAY11: "实现 FAT12 簇链遍历函数 fat12_next_cluster(cluster)（处理
     *  1.5 字节对齐，注意奇偶簇）。给定根目录的起始簇（通常为 0），能
     *  遍历打印出前 10 个簇号。"
     *
     *   Mounts the DAY11 fixture (tests/fat12-with-file.img, embedded
     *   by build.sh at LBA 5000 — see this file's DAY11 header comment
     *   for how it was built and independently cross-validated), then
     *   exercises fat12_next_cluster() two ways: first on the two
     *   truly-reserved cluster numbers (0 and 1), to show the walker
     *   handles them without crashing/looping; then on the real
     *   BIGFILE.BIN chain starting at cluster 2, printing 10
     *   consecutive fat12_next_cluster() return values. Compare the
     *   printed values against the "CROSS-VALIDATION" list in this
     *   file's DAY11 header comment.
     * ------------------------------------------------------------------ */
    console_write_color("[fat12] Mounting DAY11 fixture at LBA 5000 "
                         "(FAT12 volume w/ real files)...\n", COLOR_INFO);
    {
        static fat12_fs_t fs11;
        if (fat12_mount(disk, 5000, &fs11) != 0) {
            console_write_color(
                "[fat12] FAIL: fat12_mount(disk, 5000, ...) returned -1 "
                "(tests/fat12-with-file.img missing from image? see "
                "build.sh)\n", COLOR_ERROR);
        } else {
            console_write_color("FAT12 mounted\n", COLOR_SUCCESS);

            /* ---- (b) sanity pass: clusters 0 and 1 are reserved,
             * not real chain links. Confirm fat12_next_cluster()
             * reports them (as raw FAT[0]/FAT[1] values) rather than
             * crashing — see this file's DAY11 header comment for why
             * this stands in for the roadmap's literal "簇号 0".
             *
             * NOTE: clusters 0/1 are reserved BY DEFINITION (the spec
             * never lets any file claim them), not because their
             * stored value happens to fall in the FAT12_CLUSTER_EOC_
             * MIN..MAX range — cluster 0's entry conventionally holds
             * the media descriptor byte reflected into its low 8 bits
             * (0xFF0 for this fixture's 0xF0 media byte), which is
             * BELOW 0xFF8 despite being reserved. So this check must
             * NOT test "is the value >= EOC_MIN" (an earlier version
             * of this test did, and wrongly printed FAT[0]'s
             * legitimate 0xFF0 as "UNEXPECTED"); it just reports the
             * raw values for a human to read against the fixture's
             * known-good numbers documented above. ---- */
            console_write_color(
                "[fat12] Reserved-cluster sanity check (clusters 0 and 1):\n",
                COLOR_INFO);
            for (uint16_t c = 0; c <= 1; c++) {
                int32_t v = fat12_next_cluster(&fs11, c);
                if (v < 0) {
                    printf("[fat12]   FAT[%u] -> read error (%d)\n", c, v);
                } else {
                    printf("[fat12]   FAT[%u] = 0x%03X (reserved cluster -- "
                           "never a real chain link, walker did not crash)\n",
                           c, (unsigned)v);
                }
            }

            /* ---- (a) real chain walk: BIGFILE.BIN starts at cluster
             * 2 (fixture's own on-disk directory entry — see this
             * file's DAY11 header comment's CROSS-VALIDATION section
             * for the independently-computed expected sequence).
             * Print up to 10 fat12_next_cluster() outputs, stopping
             * early on error or end-of-chain/bad-cluster, per the
             * roadmap's "遍历打印出前 10 个簇号" (with 20 total
             * clusters in the fixture's chain, all 10 are expected to
             * print without hitting end-of-chain). ---- */
            console_write_color(
                "[fat12] Walking BIGFILE.BIN's cluster chain from cluster 2 "
                "(expect 3,4,5,6,7,8,9,10,11,12):\n", COLOR_INFO);
            uint16_t cluster = 2;
            for (int i = 0; i < 10; i++) {
                int32_t next = fat12_next_cluster(&fs11, cluster);

                if (next < 0) {
                    printf("[fat12]   [%d] fat12_next_cluster(%u) -> "
                           "read error (%d), stopping early\n",
                           i, cluster, next);
                    break;
                }
                if (next == FAT12_CLUSTER_FREE) {
                    printf("[fat12]   [%d] fat12_next_cluster(%u) -> "
                           "0x000 (free cluster?!), stopping early\n",
                           i, cluster);
                    break;
                }
                if (next == FAT12_CLUSTER_BAD) {
                    printf("[fat12]   [%d] fat12_next_cluster(%u) -> "
                           "0xFF7 (bad cluster), stopping early\n",
                           i, cluster);
                    break;
                }
                if (next >= FAT12_CLUSTER_EOC_MIN) {
                    printf("[fat12]   [%d] fat12_next_cluster(%u) -> "
                           "0x%03X (end of chain), stopping early\n",
                           i, cluster, (unsigned)next);
                    break;
                }

                printf("[fat12]   [%d] fat12_next_cluster(%u) -> %u\n",
                       i, cluster, (unsigned)next);
                cluster = (uint16_t)next;
            }
        }

        /* --------------------------------------------------------------
         * DAY12: "实现 fat12_readdir(root_node)：读取根目录区（Root
         *  Directory Region），遍历每 32 字节条目。提取文件名（去掉
         *  空格）、属性、起始簇、文件大小。在终端列出根目录下所有文件
         *  名（至少能看到 KERNEL BIN 和 LOADER BIN）。"
         *
         *   Lists the root directory of the SAME fs11 handle Day 11's
         *   cluster-chain walk above just mounted (tests/fat12-with-
         *   file.img at LBA 5000, still in scope here — no re-mount
         *   needed). See fat12.h's DAY12 header comment for why this
         *   fixture's BIGFILE.BIN/SMALL.BIN stand in for the roadmap
         *   text's literal "KERNEL BIN"/"LOADER BIN" example names:
         *   nothing in this project ever stores kernel.bin/loader.bin
         *   as named entries inside a FAT12 volume (build.sh dd's them
         *   onto raw disk-absolute sectors instead — see kmain.c's
         *   own DAY1/DAY6 header comments), so listing the real,
         *   already-cross-validated fixture files here is the closer
         *   reading of the acceptance criteria's intent ("show the
         *   readdir walk actually finds real files"), not a literal
         *   filename match.
         * -------------------------------------------------------------- */
        console_write_color(
            "[fat12] Listing root directory (fat12_readdir)...\n", COLOR_INFO);
        {
            static fat12_dirlist_entry_t dirlist[FAT12_DIRLIST_MAX_ENTRIES];
            uint32_t dirlist_count = 0;
            int rd_ret = fat12_readdir(&fs11, dirlist,
                                        FAT12_DIRLIST_MAX_ENTRIES, &dirlist_count);

            if (rd_ret != 0) {
                printf("[fat12] FAIL: fat12_readdir() returned %d after "
                       "decoding %u entries\n", rd_ret, dirlist_count);
            } else {
                printf("[fat12] OK: %u entries in root directory "
                       "(expect BIGFILE.BIN and SMALL.BIN):\n",
                       dirlist_count);

                int saw_bigfile = 0, saw_small = 0;
                for (uint32_t e = 0; e < dirlist_count; e++) {
                    fat12_dirlist_entry_t *ent = &dirlist[e];
                    const char *kind =
                        (ent->attr & FAT_ATTR_DIRECTORY) ? "<DIR>" : "     ";

                    /* NOTE: this project's printf() (kernel/lib/printf.c)
                     * only supports right-justified %s width padding
                     * (pad-then-string), not a '-' left-justify flag —
                     * so name is printed unpadded here rather than via
                     * a "%-13s"-style spec that this printf would
                     * misparse (it has no left-justify support at
                     * all; see printf.c's vprintf_internal()). */
                    printf("[fat12]   %s %s attr=0x%02X cluster=%u size=%u\n",
                           ent->name, kind, ent->attr,
                           (unsigned)ent->start_cluster, ent->size);

                    if (strcmp(ent->name, "BIGFILE.BIN") == 0)
                        saw_bigfile = 1;
                    if (strcmp(ent->name, "SMALL.BIN") == 0)
                        saw_small = 1;
                }

                if (saw_bigfile && saw_small) {
                    console_write_color(
                        "[fat12] OK: found both BIGFILE.BIN and SMALL.BIN\n",
                        COLOR_SUCCESS);
                } else {
                    console_write_color(
                        "[fat12] NOTE: did not find both expected fixture "
                        "files (tests/fat12-with-file.img contents changed?)\n",
                        COLOR_WARNING);
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY15: "重构 FAT 通用层：分离 FAT12/16/32 的共性（BPB 解析、目录
     *  项结构），创建 fat_common.c。一个 fat_mount() 根据 BPB 的
     *  BytesPerSector 和 TotalSectors 自动判断类型。"
     *
     *   Exercises the NEW kernel/fs/fat/fat_common.c entry point,
     *   fat_mount(), against the exact three LBAs this file's Day 9-12
     *   sections above already read directly — not because fat_mount()
     *   needs a new fixture, but so this test can prove the Day 15
     *   refactor changed NOTHING observable about those known-good
     *   results while adding the new "which FAT variant is this"
     *   classification on top:
     *
     *     - LBA 0:    LumaOS's own boot/mbr.asm — NOT a FAT BPB at all
     *                 (see this file's DAY9 header comment). Expected:
     *                 FAT_TYPE_UNKNOWN, the negative case.
     *     - LBA 2500: tests/fat12-bpb.bin, a real `mkfs.fat -F 12` boot
     *                 sector (DAY9-FOLLOWUP fixture). Expected: FAT12.
     *     - LBA 5000: tests/fat12-with-file.img, the DAY11 fixture
     *                 (also a real `mkfs.fat -F 12` volume, just with
     *                 files on it). Expected: FAT12.
     *
     *   Both positive cases are genuine FAT12 volumes (this project has
     *   no FAT16/32 fixture yet — that's Day 16/17's job), so this test
     *   cannot show fat_classify_type() picking FAT16 or FAT32 on real
     *   media; what it CAN and does show is that the shared geometry
     *   math (now living in fat_common.c instead of duplicated inline
     *   in fat12.c) still produces a plausible, non-crashing
     *   TotalClusters figure and correctly classifies it under 4085 ->
     *   FAT12 for both real fixtures, while still correctly rejecting
     *   LBA 0 the same way fat12_parse_bpb()'s own sanity check always
     *   has. fat_classify_type()'s FAT16/32 boundaries themselves are
     *   pure arithmetic or synthetic BPBs, independently exercised by
     *   this project's host-side test harness (tests/
     *   day15_fat_mount_host_test.c) — see that file's own header
     *   comment for why a boot-time QEMU test can't manufacture a
     *   65525+-cluster volume just to prove a comparison operator.
     * ------------------------------------------------------------------ */
    console_write_color(
        "[fat] fat_mount() auto-detection (DAY15 fat_common.c)...\n",
        COLOR_INFO);
    {
        struct { uint32_t lba; fat_type_t expect; const char *label; } cases[] = {
            { 0,    FAT_TYPE_UNKNOWN, "LBA 0 (LumaOS mbr.asm, not FAT)" },
            { 2500, FAT_TYPE_FAT12,   "LBA 2500 (fat12-bpb.bin fixture)" },
            { 5000, FAT_TYPE_FAT12,   "LBA 5000 (fat12-with-file.img fixture)" },
        };
        int all_ok = 1;

        for (int i = 0; i < 3; i++) {
            static fat_bpb_common_t bpb15;
            static fat_geometry_t   geom15;
            fat_type_t got = fat_mount(disk, cases[i].lba, &bpb15, &geom15);

            const char *got_name =
                (got == FAT_TYPE_FAT12) ? "FAT12" :
                (got == FAT_TYPE_FAT16) ? "FAT16" :
                (got == FAT_TYPE_FAT32) ? "FAT32" : "UNKNOWN";

            printf("[fat]   %s -> %s (BytesPerSec=%u SecPerClus=%u "
                   "TotalSectors=%u TotalClusters=%u)\n",
                   cases[i].label, got_name,
                   bpb15.BPB_BytesPerSec, bpb15.BPB_SecPerClus,
                   geom15.total_sectors, geom15.total_clusters);

            if (got != cases[i].expect) {
                printf("[fat]   FAIL: expected type %d, got %d\n",
                       (int)cases[i].expect, (int)got);
                all_ok = 0;
            }
        }

        if (all_ok) {
            console_write_color(
                "[fat] OK: fat_mount() classified all three LBAs correctly\n",
                COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat] FAIL: fat_mount() misclassified at least one LBA "
                "above\n", COLOR_ERROR);
        }
    }

    /* ------------------------------------------------------------------
     * DAY13: "将 FAT12 挂接到 VFS：注册 fat12_filesystem_ops。实现
     *  vfs_mount("IDE0")。调用 vfs_mount 后，vfs_root 指针指向 FAT
     *  根目录。"
     *
     *   Unlike Day 9-12 above (which call fat12_* functions directly
     *   and hold their own `fat12_fs_t` locals), this section goes
     *   through the NEW generic layer exclusively: vfs_mount()
     *   (kernel/fs/vfs/vfs.c) internally mounts FAT12 via
     *   fat12_vfs_mount() (kernel/fs/fat/fat12_vfs.c) and points the
     *   global vfs_root at the result — nothing here calls
     *   fat12_mount()/fat12_readdir() by name, proving the VFS
     *   indirection actually works end-to-end, the same way the
     *   DAY6 block_device_t check above proves I/O went through
     *   `disk->read()` instead of ide_read_sectors(). The device
     *   string is "IDE0", not plain "IDE" — numbered to match
     *   block.c's ide0_block_device() accessor, since a bare "IDE"
     *   couldn't distinguish a future second physical drive (see
     *   vfs.c's header comment for the full rationale). See vfs.c's
     *   header comment for exactly which physical LBA "IDE0"
     *   resolves to on this project's stock image today.
     * ------------------------------------------------------------------ */
    console_write_color("[vfs] vfs_mount(\"IDE0\")...\n", COLOR_INFO);
    if (vfs_mount("IDE0") != 0) {
        console_write_color(
            "[vfs] FAIL: vfs_mount(\"IDE0\") returned -1\n", COLOR_ERROR);
    } else if (!vfs_root) {
        console_write_color(
            "[vfs] FAIL: vfs_mount() returned 0 but vfs_root is still NULL\n",
            COLOR_ERROR);
    } else {
        console_write_color(
            "[vfs] OK: vfs_root now points at the mounted FAT12 root\n",
            COLOR_SUCCESS);
        /* NOTE: vfs_root->name itself is stored WITHOUT a trailing
         * "/" ("IDE0", not "IDE0/") — appending "/" here is purely a
         * DISPLAY convention matching how 计划.md's namespace diagram
         * writes every directory (IDE/, System/, Kernel/, ...), not
         * something baked into the stored name. Keeping the stored
         * name slash-free avoids double-slash bugs once Day 18 starts
         * concatenating path segments (e.g. "IDE0" + "/" + "System"),
         * so the "/" suffix is computed fresh here from `type` instead. */
        printf("[vfs] vfs_root: name=\"%s%s\" type=%s fs=\"%s\"\n",
               vfs_root->name,
               (vfs_root->type == VFS_TYPE_DIR) ? "/" : "",
               (vfs_root->type == VFS_TYPE_DIR) ? "DIR" : "FILE",
               vfs_root->fs->name);

        /* Walk vfs_root through fs->readdir()/fat12_filesystem_ops —
         * NOT fat12_readdir() directly — to prove the readdir call is
         * actually going through the vfs_node_t/filesystem_t function
         * pointers this file wires up, the same generic path Day 14's
         * cmd_ls is meant to use next. */
        console_write_color(
            "[vfs] Listing vfs_root via vfs_root->fs->readdir()...\n",
            COLOR_INFO);
        {
            vfs_node_t child;
            uint32_t idx = 0;
            int any = 0;
            while (vfs_root->fs->readdir(vfs_root, idx, &child) == 0) {
                /* Same display-only "/" suffix rule as vfs_root above —
                 * none of today's entries are directories (the DAY11
                 * fixture volume has no subdirectories), but this keeps
                 * the rule uniform for whenever one does show up. */
                const char *slash = (child.type == VFS_TYPE_DIR) ? "/" : "";
                printf("[vfs]   %s%s size=%u\n", child.name, slash, child.size);
                any = 1;
                idx++;
            }
            if (!any) {
                console_write_color(
                    "[vfs] NOTE: readdir returned no entries (unexpected — "
                    "see build.sh's tests/fat12-with-file.img embedding)\n",
                    COLOR_WARNING);
            } else {
                console_write_color(
                    "[vfs] OK: readdir via vfs_root->fs->readdir() worked\n",
                    COLOR_SUCCESS);
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
     *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载
     *    一个 FAT16 镜像（可以用 mkfs.fat -F 16 制作），ls 能正确列出
     *    文件。"
     *
     *   Mounts tests/fat16-with-file.img (build.sh embeds it at LBA
     *   10000 — see that file's own DAY16 comment) through the SAME
     *   fat_vfs_mount() auto-detect dispatcher (kernel/fs/fat/fat_vfs.c)
     *   the DAY13 section above's vfs_mount("IDE0") now uses
     *   internally, but called here directly against a known FAT16
     *   LBA rather than through vfs_mount()'s "IDE0"-string/MBR-scan
     *   resolution — proving fat_vfs_mount() itself correctly detects
     *   FAT16 (not just FAT12) and routes to fat16_vfs_mount().
     *
     *   After a successful mount, vfs_root is repointed at this FAT16
     *   volume's root — on top of, not instead of, the DAY13 section's
     *   own FAT12 mount+readdir above (which is left completely
     *   untouched as a regression check that FAT12 still works). This
     *   means that once the shell prompt appears below, typing `ls`
     *   lists THIS volume's files (BIGFILE.BIN/SMALL.BIN, same names
     *   as the FAT12 fixture but a completely separate volume) — which
     *   is exactly this Day's own acceptance text: "挂载一个 FAT16
     *   镜像，ls 能正确列出文件。"
     * ------------------------------------------------------------------ */
#define FAT16_FIXTURE_LBA  10000  /* build.sh: tests/fat16-with-file.img */
    console_write_color(
        "[fat16] fat_vfs_mount() against the DAY16 FAT16 fixture...\n",
        COLOR_INFO);
    {
        filesystem_t *fs16 = fat_vfs_mount(disk, FAT16_FIXTURE_LBA);
        if (!fs16) {
            console_write_color(
                "[fat16] FAIL: fat_vfs_mount() returned NULL (not a FAT16 "
                "volume at LBA 10000?)\n", COLOR_ERROR);
        } else {
            printf("[fat16] OK: mounted, fs->name=\"%s\"\n", fs16->name);

            console_write_color(
                "[fat16] Listing root via fs->readdir()...\n", COLOR_INFO);
            {
                vfs_node_t child;
                uint32_t idx = 0;
                int any = 0;
                while (fs16->readdir(fs16->root, idx, &child) == 0) {
                    const char *slash = (child.type == VFS_TYPE_DIR) ? "/" : "";
                    printf("[fat16]   %s%s size=%u\n", child.name, slash, child.size);
                    any = 1;
                    idx++;
                }
                if (!any) {
                    console_write_color(
                        "[fat16] NOTE: readdir returned no entries (unexpected "
                        "— see build.sh's tests/fat16-with-file.img embedding)\n",
                        COLOR_WARNING);
                } else {
                    console_write_color(
                        "[fat16] OK: readdir listed the FAT16 volume's files\n",
                        COLOR_SUCCESS);
                }
            }

            /* Make this the live volume for the shell's `ls` below —
             * see this section's header comment above for why.
             *
             * DAY18-FOLLOWUP: also register it into the numbered mount
             * table (vfs.h) so it stays independently reachable by
             * name afterward even once FAT32 mounts below and becomes
             * the new vfs_root — this is expected to become slot
             * "IDE1" (FAT12's vfs_mount("IDE0") call above already
             * claimed "IDE0"). vfs_register_mount() sets vfs_root
             * itself, so the old direct assignment is replaced rather
             * than duplicated. */
            char slot16[8];
            if (vfs_register_mount(fs16, slot16) == 0) {
                printf("[fat16] Registered as mount slot \"%s\" — try "
                       "`ls` at the shell prompt below\n", slot16);
            } else {
                console_write_color(
                    "[fat16] WARN: vfs_register_mount() failed (mount "
                    "table full?) — falling back to a bare vfs_root "
                    "assignment\n", COLOR_WARNING);
                vfs_root = fs16->root;
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY17 (Week 3, Day 17 of the 5.0 filesystem roadmap):
     *   "实现 FAT32 解析（注意 BPB_RootClus，根目录是一个普通簇链，且存在
     *    FSInfo 结构）。挂载 FAT32 镜像（mkfs.fat -F 32），ls 能工作。"
     *
     *   Mounts tests/fat32-with-file.img (build.sh embeds it at LBA
     *   30000 — see that file's own DAY17 comment) through
     *   fat_vfs_mount(), then reads and validates the FAT32-specific
     *   FSInfo structure via fat32_read_fsinfo() before listing the
     *   root directory — proving the whole FAT32 chain end to end:
     *   FATSz32-based geometry, FSInfo signatures, BPB_RootClus
     *   cluster-chain root directory walk, all the way to readdir().
     *
     *   As with the DAY16 section above, this is left ON TOP of (not
     *   instead of) both the DAY13 FAT12 test and the DAY16 FAT16
     *   test — after this section runs, vfs_root is repointed once
     *   more, now at the FAT32 volume, so `ls` at the shell prompt
     *   below lists FAT32's files.
     * ------------------------------------------------------------------ */
#define FAT32_FIXTURE_LBA  30000  /* build.sh: tests/fat32-with-file.img */
    console_write_color(
        "[fat32] fat_vfs_mount() against the DAY17 FAT32 fixture...\n",
        COLOR_INFO);
    {
        filesystem_t *fs32 = fat_vfs_mount(disk, FAT32_FIXTURE_LBA);
        if (!fs32) {
            console_write_color(
                "[fat32] FAIL: fat_vfs_mount() returned NULL (not a FAT32 "
                "volume at LBA 30000?)\n", COLOR_ERROR);
        } else {
            printf("[fat32] OK: mounted, fs->name=\"%s\"\n", fs32->name);

            /* "且存在 FSInfo 结构" — the acceptance text explicitly
             * calls this out, so it gets its own explicit check here
             * rather than only being exercised silently inside
             * fat32_mount(). */
            {
                fat32_fs_t *raw_fs = (fat32_fs_t *)fs32->fs_private;
                fat32_fsinfo_t fsinfo;
                if (fat32_read_fsinfo(raw_fs, &fsinfo) == 0) {
                    printf("[fat32] OK: FSInfo signatures valid "
                           "(FSI_Free_Count=%u, FSI_Nxt_Free=%u)\n",
                           fsinfo.FSI_Free_Count, fsinfo.FSI_Nxt_Free);
                } else {
                    console_write_color(
                        "[fat32] NOTE: FSInfo signature check failed "
                        "(BPB_FSInfo didn't point at a valid FSInfo sector)\n",
                        COLOR_WARNING);
                }
            }

            console_write_color(
                "[fat32] Listing root via fs->readdir() (BPB_RootClus "
                "cluster-chain walk)...\n", COLOR_INFO);
            {
                vfs_node_t child;
                uint32_t idx = 0;
                int any = 0;
                while (fs32->readdir(fs32->root, idx, &child) == 0) {
                    const char *slash = (child.type == VFS_TYPE_DIR) ? "/" : "";
                    printf("[fat32]   %s%s size=%u\n", child.name, slash, child.size);
                    any = 1;
                    idx++;
                }
                if (!any) {
                    console_write_color(
                        "[fat32] NOTE: readdir returned no entries (unexpected "
                        "— see build.sh's tests/fat32-with-file.img embedding)\n",
                        COLOR_WARNING);
                } else {
                    console_write_color(
                        "[fat32] OK: readdir listed the FAT32 volume's files\n",
                        COLOR_SUCCESS);
                }
            }

            /* DAY18-FOLLOWUP: register into the numbered mount table
             * (vfs.h) instead of a bare vfs_root assignment — expected
             * to become slot "IDE2" (FAT12 claimed "IDE0" above, FAT16
             * claimed "IDE1"). See the FAT16 section above for the
             * identical pattern and rationale. */
            char slot32[8];
            if (vfs_register_mount(fs32, slot32) == 0) {
                printf("[fat32] Registered as mount slot \"%s\" — try "
                       "`ls` at the shell prompt below\n", slot32);
            } else {
                console_write_color(
                    "[fat32] WARN: vfs_register_mount() failed (mount "
                    "table full?) — falling back to a bare vfs_root "
                    "assignment\n", COLOR_WARNING);
                vfs_root = fs32->root;
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY17-FOLLOWUP: multi-LBA mount summary.
     *
     *   Answers a question this project's own author asked after Day
     *   16: "can it mount several LBAs, and can the boot log show
     *   which type each one turned out to be — e.g. LBA 5000 reads as
     *   FAT12, LBA 10000 reads as FAT16?" It can: fat_vfs_mount() has
     *   no per-LBA special-casing anywhere in it (see fat_vfs.c — it's
     *   a plain switch on fat_mount()'s classification result), so
     *   calling it against several different LBAs in a row, each
     *   producing its own independent filesystem_t, already works —
     *   this loop is a summary/demonstration, not new mounting logic.
     *
     *   NOT (yet) supported: several of these mounted volumes being
     *   simultaneously live/browsable at once (e.g. `cd`-ing between
     *   two different drives without re-mounting). That needs a real
     *   multi-mount VFS (a path-keyed table of mounted filesystem_t
     *   instances instead of a single global vfs_root) — this
     *   project's own roadmap allocates that to Day 35 ("LMFS 注册进
     *   VFS。FAT 与 LMFS 可以同时存在。"), once a second filesystem
     *   family (LMFS) exists to actually need it, so it isn't built
     *   ahead of that need today. Until then, each fat_vfs_mount()
     *   call below is independent and self-contained (its own static
     *   g_fat12_mount/g_fat16_mount/g_fat32_mount instance, per
     *   fat12_vfs.c/fat16_vfs.c/fat32_vfs.c's own "single mount
     *   instance" design note) — only the LAST vfs_root assignment
     *   among this file's several test sections is what the shell
     *   actually sees below.
     * ------------------------------------------------------------------ */
    console_write_color(
        "[fat] Multi-LBA mount summary:\n", COLOR_INFO);
    {
        struct { uint32_t lba; const char *note; } lbas[] = {
            { 5000,  "tests/fat12-with-file.img (Day 11)" },
            { 10000, "tests/fat16-with-file.img (Day 16)" },
            { 30000, "tests/fat32-with-file.img (Day 17)" },
        };
        uint32_t i;
        for (i = 0; i < sizeof(lbas) / sizeof(lbas[0]); i++) {
            filesystem_t *fs = fat_vfs_mount(disk, lbas[i].lba);
            if (fs) {
                printf("[fat]   LBA %u -> %s (%s)\n",
                       lbas[i].lba, fs->name, lbas[i].note);
            } else {
                printf("[fat]   LBA %u -> (not a recognized FAT volume) (%s)\n",
                       lbas[i].lba, lbas[i].note);
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY18 (Week 3, Day 18 of the 5.0 filesystem roadmap):
     *   "实现 fat_open(path)：根据路径（如 /KERNEL.BIN）遍历目录项，返回
     *    文件的起始簇号。对根目录文件能正确返回簇号。"
     *
     * DAY18-FOLLOWUP (user-requested): fat_open() now takes ONE combined
     * "<mount-slot>/<filename>" path and resolves the mount-slot segment
     * through the vfs.h mount table (vfs_lookup_mount()) instead of
     * taking an explicit `filesystem_t *`. Nothing needs to be
     * re-mounted here anymore — by this point in kmain.c, the DAY13
     * section's `vfs_mount("IDE0")` call has already registered the
     * FAT12 fixture as slot "IDE0", the DAY16 section's
     * `vfs_register_mount(fs16, ...)` call registered FAT16 as "IDE1",
     * and the DAY17 section's `vfs_register_mount(fs32, ...)` call
     * registered FAT32 as "IDE2" — in exactly that order, matching
     * "先 FAT12 然后 FAT16 然后 FAT32" mounting order this test section
     * exercises fat_open() against. This also proves fat_open() needs
     * no FAT12/16/32-specific branch at all, exactly as fat_vfs.h's
     * DAY18 doc comment argues: the SAME fat_open(path, &node) call
     * resolves a path on whichever variant that path's mount-slot
     * segment names, because it only ever goes through
     * vfs_lookup_mount() + fs->readdir(), already identical across all
     * three since Day 13/16/17.
     *
     * Six positive cases (one per fixture file, across all three
     * mounted volumes) plus three negative cases (unknown filename on
     * a real mount; a subdirectory-shaped path, still out of Day 18's
     * scope; an unregistered mount-slot name) are checked. FAT12/16
     * share the fixture names BIGFILE.BIN/SMALL.BIN (see build.sh's
     * gen_fat12_fixtures()/gen_fat16_fixtures()); FAT32's own fixture
     * uses different names, FAT32BIG.BIN/SMALLFIL.BIN (build.sh's
     * gen_fat32_fixtures()) — both are exercised as-is rather than
     * renamed to match, so this section also incidentally proves
     * fat_open() doesn't assume any particular fixed filename.
     * ------------------------------------------------------------------ */
    console_write_color(
        "[fat] fat_open() path resolution (DAY18)...\n", COLOR_INFO);
    {
        struct { const char *tag; const char *p1; const char *p2; }
        cases18[] = {
            { "IDE0 (fat12)", "IDE0/BIGFILE.BIN",  "IDE0/SMALL.BIN"    },
            { "IDE1 (fat16)", "IDE1/BIGFILE.BIN",  "IDE1/SMALL.BIN"    },
            { "IDE2 (fat32)", "IDE2/FAT32BIG.BIN", "IDE2/SMALLFIL.BIN" },
        };
        uint32_t i;
        int all_ok = 1;

        for (i = 0; i < sizeof(cases18) / sizeof(cases18[0]); i++) {
            const char *paths[2] = { cases18[i].p1, cases18[i].p2 };
            uint32_t j;
            for (j = 0; j < 2; j++) {
                vfs_node_t node;
                if (fat_open(paths[j], &node) == 0) {
                    uint32_t cluster = (uint32_t)(uintptr_t)node.fs_private;
                    printf("[fat18] OK: %s fat_open(\"%s\") -> "
                           "cluster=%u size=%u\n",
                           cases18[i].tag, paths[j], cluster, node.size);
                    if (cluster == 0) {
                        printf("[fat18] FAIL: %s %s resolved but cluster==0 "
                               "(expected a real data cluster)\n",
                               cases18[i].tag, paths[j]);
                        all_ok = 0;
                    }
                } else {
                    printf("[fat18] FAIL: %s fat_open(\"%s\") returned -1 "
                           "(expected success)\n", cases18[i].tag, paths[j]);
                    all_ok = 0;
                }
            }
        }

        /* Negative case: a filename that doesn't exist on a real mount
         * must fail cleanly (-1), not crash or fabricate a node. */
        {
            vfs_node_t node;
            if (fat_open("IDE0/NOPE.BIN", &node) != 0) {
                console_write_color(
                    "[fat18] OK: fat_open(\"IDE0/NOPE.BIN\") correctly "
                    "returned -1 (not found)\n", COLOR_SUCCESS);
            } else {
                console_write_color(
                    "[fat18] FAIL: fat_open(\"IDE0/NOPE.BIN\") "
                    "unexpectedly succeeded\n", COLOR_ERROR);
                all_ok = 0;
            }
        }

        /* Negative case: a subdirectory-shaped path is out of scope
         * for Day 18 (see fat_vfs.h's doc comment) and must return -1,
         * not silently mis-resolve. */
        {
            vfs_node_t node;
            if (fat_open("IDE0/SYS/FOO.BIN", &node) != 0) {
                console_write_color(
                    "[fat18] OK: subdirectory-shaped path correctly "
                    "rejected (-1, unsupported until Day 42)\n",
                    COLOR_SUCCESS);
            } else {
                console_write_color(
                    "[fat18] FAIL: subdirectory-shaped path was not "
                    "rejected as expected\n", COLOR_ERROR);
                all_ok = 0;
            }
        }

        /* Negative case: a mount-slot name that was never registered
         * (DAY18-FOLLOWUP's new failure mode — see fat_vfs.h's doc
         * comment) must also fail cleanly rather than crash on a NULL
         * filesystem_t. */
        {
            vfs_node_t node;
            if (fat_open("IDE9/BIGFILE.BIN", &node) != 0) {
                console_write_color(
                    "[fat18] OK: unregistered mount slot \"IDE9\" "
                    "correctly rejected (-1)\n", COLOR_SUCCESS);
            } else {
                console_write_color(
                    "[fat18] FAIL: unregistered mount slot \"IDE9\" "
                    "was not rejected as expected\n", COLOR_ERROR);
                all_ok = 0;
            }
        }

        if (all_ok) {
            console_write_color(
                "[fat18] OK: fat_open() resolved every case correctly "
                "across mount slots IDE0/IDE1/IDE2 (FAT12/16/32)\n",
                COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat18] FAIL: at least one fat_open() case above did "
                "not behave as expected\n", COLOR_ERROR);
        }
    }

    /* ------------------------------------------------------------------
     * DAY19 (Week 3, Day 19 of the 5.0 filesystem roadmap):
     *   "实现 fat_read(file, buf, size)：根据起始簇，依次读取簇链，将
     *    数据拷贝到 buf。实现 cmd_cat /KERNEL.BIN，能在终端打印出内核
     *    二进制的开头部分（乱码也行，证明读到了数据）。"
     *
     *   cmd_cat itself (kernel/shell/cmd/cat.c) is exercised
     *   interactively from the text-mode shell started just below
     *   (`cat IDE0/BIGFILE.BIN`, etc. — see that file's header comment
     *   for why "<mount-slot>/<filename>", not a literal "/KERNEL.BIN",
     *   is this project's real argument shape). This section is the
     *   automated, boot-time counterpart: it calls fat_open() + the
     *   SAME fs->open()/fs->read() pair cmd_cat uses (never
     *   fat12_next_cluster()/fat_read_chain() or any other backend-
     *   specific/static function by name) against all three mounted
     *   volumes (IDE0=FAT12, IDE1=FAT16, IDE2=FAT32 — registered by
     *   the DAY13/16/17 sections above, in that order), and checks the
     *   bytes read back EXACTLY match the fixture's known-deterministic
     *   content instead of merely "some bytes came back, non-zero
     *   length" — a stronger check than the roadmap's own "乱码也行"
     *   (garbled is fine) bar, in the same spirit as DAY11's cluster-
     *   chain test cross-validating exact cluster numbers rather than
     *   just "didn't crash".
     *
     *   Fixture content (build.sh's gen_fat12_fixtures()/
     *   gen_fat16_fixtures(), byte-for-byte identical generation for
     *   both — gen_fat32_fixtures() reuses the same Python one-liners
     *   under different output filenames):
     *     BIGFILE.BIN: byte i = i % 256        (10240 bytes, 20 clusters)
     *     SMALL.BIN  : byte i = (i*7+3) % 256   (1536 bytes,  3 clusters)
     *   FAT32's own fixture uses FAT32BIG.BIN/SMALLFIL.BIN for the same
     *   two patterns (see the DAY18 section above's cases18[] table for
     *   why the names differ there too).
     *
     *   Three checks per volume: (1) fat_open()+open()+read() succeeds
     *   and returns the full requested length; (2) the first 16 bytes
     *   match the known pattern exactly; (3) a SECOND read of the same
     *   already-open file_t (Day 20 preview — file->pos already
     *   advanced by the first read, see fat_vfs.h's fat_read() doc
     *   comment) returns the NEXT 16 bytes of the pattern, not a repeat
     *   of the first 16 — proving file->pos is genuinely threaded
     *   through fs->read() today even though no shell command exercises
     *   that yet (Day 20's own scope).
     * ------------------------------------------------------------------ */
    console_write_color(
        "[fat] fat_read() cluster-chain read (DAY19)...\n", COLOR_INFO);
    {
        struct { const char *tag; const char *path; int mul7; }
        cases19[] = {
            { "IDE0 (fat12) BIGFILE.BIN",  "IDE0/BIGFILE.BIN",  0 },
            { "IDE0 (fat12) SMALL.BIN",    "IDE0/SMALL.BIN",    1 },
            { "IDE1 (fat16) BIGFILE.BIN",  "IDE1/BIGFILE.BIN",  0 },
            { "IDE1 (fat16) SMALL.BIN",    "IDE1/SMALL.BIN",    1 },
            { "IDE2 (fat32) FAT32BIG.BIN", "IDE2/FAT32BIG.BIN", 0 },
            { "IDE2 (fat32) SMALLFIL.BIN", "IDE2/SMALLFIL.BIN", 1 },
        };
        uint32_t i;
        int all_ok = 1;

        for (i = 0; i < sizeof(cases19) / sizeof(cases19[0]); i++) {
            vfs_node_t node;
            if (fat_open(cases19[i].path, &node) != 0) {
                printf("[fat19] FAIL: %s fat_open(\"%s\") returned -1\n",
                       cases19[i].tag, cases19[i].path);
                all_ok = 0;
                continue;
            }
            if (!node.fs || !node.fs->open || !node.fs->read) {
                printf("[fat19] FAIL: %s missing open/read ops\n",
                       cases19[i].tag);
                all_ok = 0;
                continue;
            }

            file_t file;
            if (node.fs->open(&node, &file) != 0) {
                printf("[fat19] FAIL: %s open() returned -1\n",
                       cases19[i].tag);
                all_ok = 0;
                continue;
            }

            uint8_t chunk1[16];
            int n1 = node.fs->read(&file, chunk1, sizeof(chunk1));
            if (n1 != (int)sizeof(chunk1)) {
                printf("[fat19] FAIL: %s first read() returned %d "
                       "(expected %u)\n", cases19[i].tag, n1,
                       (uint32_t)sizeof(chunk1));
                all_ok = 0;
                continue;
            }

            int first_ok = 1;
            for (uint32_t b = 0; b < sizeof(chunk1); b++) {
                uint8_t expected = cases19[i].mul7
                                        ? (uint8_t)((b * 7 + 3) % 256)
                                        : (uint8_t)(b % 256);
                if (chunk1[b] != expected) {
                    first_ok = 0;
                    break;
                }
            }

            /* Day 20 preview: file.pos was advanced by the first
             * read() (see fat_vfs.h's fat_read() doc comment) — a
             * second read() on the SAME file_t must continue from
             * byte 16, not restart at byte 0. */
            uint8_t chunk2[16];
            int n2 = node.fs->read(&file, chunk2, sizeof(chunk2));
            int second_ok = (n2 == (int)sizeof(chunk2));
            if (second_ok) {
                for (uint32_t b = 0; b < sizeof(chunk2); b++) {
                    uint32_t idx = sizeof(chunk1) + b;
                    uint8_t expected = cases19[i].mul7
                                            ? (uint8_t)((idx * 7 + 3) % 256)
                                            : (uint8_t)(idx % 256);
                    if (chunk2[b] != expected) {
                        second_ok = 0;
                        break;
                    }
                }
            }

            if (first_ok && second_ok) {
                printf("[fat19] OK: %s read %d+%d bytes, content matches "
                       "(size=%u)\n", cases19[i].tag, n1, n2, node.size);
            } else {
                printf("[fat19] FAIL: %s content mismatch "
                       "(first_ok=%d second_ok=%d)\n",
                       cases19[i].tag, first_ok, second_ok);
                all_ok = 0;
            }
        }

        if (all_ok) {
            console_write_color(
                "[fat19] OK: fat_read() correctly walked the cluster "
                "chain and returned exact fixture content across "
                "IDE0/IDE1/IDE2 (FAT12/16/32)\n", COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat19] FAIL: at least one fat_read() case above did "
                "not behave as expected\n", COLOR_ERROR);
        }
    }

    /* ------------------------------------------------------------------
     * DAY20 (Week 3, Day 20 of the 5.0 filesystem roadmap):
     *   "增加文件偏移指针 file->pos，支持多次读取。连续调用两次 read，
     *    第二次能接着上次位置读。"
     *
     *   Day 19's own boot-time section above already snuck in a "Day 20
     *   preview": a second 16-byte read on the same file_t, proving
     *   file->pos carries forward at all. But that preview only ever
     *   advances file->pos from 0 to 32 — nowhere near this project's
     *   512-byte cluster size (every fixture here uses BPB_SecPerClus=1
     *   — see fat_vfs.c's DAY19 comment), so it never once exercises
     *   fat_read_chain()'s "skip whole clusters until file_pos's
     *   cluster" loop (kernel/fs/fat/fat_vfs.c) — that loop's body had
     *   literally never executed anywhere in this codebase before this
     *   section. THIS is the actual gap Day 20 exists to close: prove
     *   file->pos-driven multi-read keeps working once it has to walk
     *   PAST one or more whole clusters to resume, not just within the
     *   first one.
     *
     *   Two checks, both against the same known-deterministic fixture
     *   content DAY19 used (byte i = i%256 for BIGFILE.BIN/FAT32BIG.BIN,
     *   byte i = (i*7+3)%256 for SMALL.BIN/SMALLFIL.BIN):
     *
     *   (a) Chunked whole-file re-read: open each fixture fresh and
     *       call fs->read() repeatedly in FAT20_CHUNK_BYTES (300-byte)
     *       chunks until EOF (0), checking every byte against the
     *       pattern at its true file offset each time. 300 does NOT
     *       divide 512 evenly on purpose (unlike a 256- or 512-byte
     *       chunk) — so across BIGFILE.BIN's 10240 bytes, most chunk
     *       boundaries land MID-cluster, forcing fat_read_chain() to
     *       resume with a nonzero `offset_in_cluster` almost every
     *       call, not just a clean "next whole cluster" boundary. A
     *       trailing zero-length read after EOF must then return 0
     *       (not -1) per vfs_read_fn's contract.
     *   (b) A single, hand-picked boundary-crossing read: read 500
     *       bytes (entirely inside cluster 0, since 500 < 512), then
     *       ONE more 40-byte read starting at file->pos=500 that spans
     *       bytes 500..539 — crossing the cluster-0/cluster-1 boundary
     *       at byte 512 within a SINGLE fs->read() call (not across two
     *       separate calls, which is what (a) already covers) and
     *       checking the returned bytes are correct on BOTH sides of
     *       that boundary. This exercises fat_read_chain()'s inner
     *       while-loop's cluster-to-cluster carry (the `next_cluster()`
     *       call plus `offset_in_cluster = 0` reset around line ~347),
     *       which chunked reads with 300-byte-or-larger chunks landing
     *       on multiples of 40 past 500 might not reliably hit.
     * ------------------------------------------------------------------ */
    console_write_color(
        "[fat] file->pos multi-read across cluster boundaries (DAY20)...\n",
        COLOR_INFO);
    {
#define FAT20_CHUNK_BYTES  300
        struct { const char *tag; const char *path; int mul7; uint32_t size; }
        cases20[] = {
            { "IDE0 (fat12) BIGFILE.BIN",  "IDE0/BIGFILE.BIN",  0, 10240 },
            { "IDE0 (fat12) SMALL.BIN",    "IDE0/SMALL.BIN",    1,  1536 },
            { "IDE1 (fat16) BIGFILE.BIN",  "IDE1/BIGFILE.BIN",  0, 10240 },
            { "IDE1 (fat16) SMALL.BIN",    "IDE1/SMALL.BIN",    1,  1536 },
            { "IDE2 (fat32) FAT32BIG.BIN", "IDE2/FAT32BIG.BIN", 0, 10240 },
            { "IDE2 (fat32) SMALLFIL.BIN", "IDE2/SMALLFIL.BIN", 1,  1536 },
        };
        uint32_t i;
        int all_ok = 1;

        /* ---- Check (a): chunked whole-file re-read, every case ---- */
        for (i = 0; i < sizeof(cases20) / sizeof(cases20[0]); i++) {
            vfs_node_t node;
            if (fat_open(cases20[i].path, &node) != 0 ||
                !node.fs || !node.fs->open || !node.fs->read) {
                printf("[fat20] FAIL: %s open path failed\n", cases20[i].tag);
                all_ok = 0;
                continue;
            }

            file_t file;
            if (node.fs->open(&node, &file) != 0) {
                printf("[fat20] FAIL: %s open() returned -1\n",
                       cases20[i].tag);
                all_ok = 0;
                continue;
            }

            uint32_t pos = 0;
            uint32_t calls = 0;
            int ok = 1;

            while (pos < cases20[i].size) {
                uint8_t chunk[FAT20_CHUNK_BYTES];
                uint32_t want = cases20[i].size - pos;
                if (want > FAT20_CHUNK_BYTES)
                    want = FAT20_CHUNK_BYTES;

                int n = node.fs->read(&file, chunk, want);
                if (n != (int)want) {
                    printf("[fat20] FAIL: %s read() at pos=%u returned %d "
                           "(expected %u)\n", cases20[i].tag, pos, n, want);
                    ok = 0;
                    break;
                }

                uint32_t b;
                for (b = 0; b < (uint32_t)n; b++) {
                    uint32_t idx = pos + b;
                    uint8_t expected = cases20[i].mul7
                                            ? (uint8_t)((idx * 7 + 3) % 256)
                                            : (uint8_t)(idx % 256);
                    if (chunk[b] != expected) {
                        printf("[fat20] FAIL: %s byte at offset %u "
                               "mismatch\n", cases20[i].tag, idx);
                        ok = 0;
                        break;
                    }
                }
                if (!ok)
                    break;

                pos += (uint32_t)n;
                calls++;
            }

            /* One more read past EOF must return 0, not an error —
             * proves file->pos correctly settled at file->node->size
             * rather than overshooting or wrapping. */
            if (ok) {
                uint8_t extra[4];
                int n_eof = node.fs->read(&file, extra, sizeof(extra));
                if (n_eof != 0) {
                    printf("[fat20] FAIL: %s post-EOF read() returned %d "
                           "(expected 0)\n", cases20[i].tag, n_eof);
                    ok = 0;
                }
            }

            if (ok) {
                printf("[fat20] OK: %s re-read via %u chunked calls, "
                       "content matches (size=%u)\n",
                       cases20[i].tag, calls, cases20[i].size);
            } else {
                all_ok = 0;
            }
        }

        /* ---- Check (b): one read() call whose bytes straddle a real
         * cluster boundary (BIGFILE.BIN on IDE0, bytes 500..539). ---- */
        {
            vfs_node_t node;
            file_t file;
            uint8_t head[500];
            uint8_t straddle[40];
            int b_ok = 1;

            if (fat_open("IDE0/BIGFILE.BIN", &node) != 0 ||
                !node.fs || !node.fs->open || !node.fs->read) {
                printf("[fat20b] FAIL: IDE0/BIGFILE.BIN open path failed\n");
                b_ok = 0;
            } else if (node.fs->open(&node, &file) != 0) {
                printf("[fat20b] FAIL: IDE0/BIGFILE.BIN open() returned -1\n");
                b_ok = 0;
            } else {
                int n1 = node.fs->read(&file, head, sizeof(head));
                if (n1 != (int)sizeof(head)) {
                    printf("[fat20b] FAIL: first read() returned %d "
                           "(expected %u)\n", n1, (uint32_t)sizeof(head));
                    b_ok = 0;
                }

                int n2 = 0;
                if (b_ok) {
                    n2 = node.fs->read(&file, straddle, sizeof(straddle));
                    if (n2 != (int)sizeof(straddle)) {
                        printf("[fat20b] FAIL: straddling read() returned "
                               "%d (expected %u)\n", n2,
                               (uint32_t)sizeof(straddle));
                        b_ok = 0;
                    }
                }

                if (b_ok) {
                    uint32_t b;
                    for (b = 0; b < sizeof(straddle); b++) {
                        uint32_t idx = sizeof(head) + b;   /* 500..539 */
                        uint8_t expected = (uint8_t)(idx % 256);
                        if (straddle[b] != expected) {
                            printf("[fat20b] FAIL: byte at offset %u "
                                   "(cluster %s side) mismatch\n", idx,
                                   idx < 512 ? "0" : "1");
                            b_ok = 0;
                        }
                    }
                }
            }

            if (b_ok) {
                console_write_color(
                    "[fat20b] OK: single read() correctly straddled the "
                    "cluster-0/cluster-1 boundary (bytes 500..539)\n",
                    COLOR_SUCCESS);
            } else {
                all_ok = 0;
            }
        }

        if (all_ok) {
            console_write_color(
                "[fat20] OK: file->pos correctly resumes multi-read "
                "across cluster boundaries on IDE0/IDE1/IDE2 "
                "(FAT12/16/32)\n", COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat20] FAIL: at least one DAY20 case above did not "
                "behave as expected\n", COLOR_ERROR);
        }
#undef FAT20_CHUNK_BYTES
    }

    /* ------------------------------------------------------------------
     * DAY21 (Week 3, Day 21 of the 5.0 filesystem roadmap — "周复盘"):
     *   "添加错误处理（文件不存在、簇链断裂）。关闭文件功能（fat_close）。"
     *
     *   Three checks, each isolating exactly one DAY21 addition:
     *
     *   (a) "文件不存在": fat_open() on a filename that genuinely does
     *       not exist on a valid, registered mount must return
     *       FAT_ERR_NOT_FOUND specifically — not merely "some negative
     *       number", which Day 18's own [fat18] section already
     *       checked (it only asserted `!= 0`, see its "unregistered
     *       mount slot" case above). This is the DAY21 upgrade: the
     *       SAME kind of failure now carries a diagnosable reason.
     *
     *   (b) "簇链断裂": rather than building a whole new corrupted disk
     *       image just to exercise this path, this reuses IDE0/
     *       SMALL.BIN (a real, valid, 3-cluster/1536-byte file whose
     *       fat_open() this project already trusts, see [fat19]/
     *       [fat20] above) and lies to fat_read() about its size —
     *       inflating the LOCAL vfs_node_t copy's `.size` field (safe
     *       to do: fat_open() fills a caller-owned struct, see its own
     *       doc comment) far past what the file's REAL 3-cluster chain
     *       on disk actually contains. Reading from an offset that
     *       lands past cluster 3 then makes fat_read_chain()'s
     *       cluster-skip loop walk the chain's real, legitimate
     *       FAT_CHAIN_EOC before reaching that offset — exactly the
     *       "corrupt/short chain" situation FAT_ERR_CHAIN_BROKEN
     *       exists to name. The disk itself is never actually
     *       corrupted; only this test's own claimed `file_size` is a
     *       lie, which is what makes the chain look short by
     *       comparison — a clean, fixture-free way to exercise a
     *       fault path that would otherwise need a deliberately
     *       hand-corrupted FAT table image.
     *
     *   (c) fat_close(): opens IDE0/SMALL.BIN, closes it via
     *       fs->close(), then asserts a FOLLOW-UP fs->read() on that
     *       same (now-closed) file_t fails — proving close() actually
     *       invalidated the handle (file->node cleared to NULL, see
     *       fat_close()'s own doc comment) rather than being a no-op
     *       stub. A second fs->close() call on the already-closed
     *       file_t is also checked to correctly report
     *       FAT_ERR_INVALID_ARGS (double-close), not silently "succeed"
     *       again.
     * ------------------------------------------------------------------ */
    console_write_color(
        "[fat] error handling + fat_close() (DAY21)...\n", COLOR_INFO);
    {
        int all_ok = 1;

        /* ---- (a) "文件不存在" -> FAT_ERR_NOT_FOUND, not just "!= 0" ---- */
        {
            vfs_node_t node;
            int err = fat_open("IDE0/NOSUCHFILE.TXT", &node);
            if (err == FAT_ERR_NOT_FOUND) {
                printf("[fat21a] OK: fat_open() on a missing file "
                       "returned FAT_ERR_NOT_FOUND (\"%s\")\n",
                       fat_strerror(err));
            } else {
                printf("[fat21a] FAIL: fat_open() on a missing file "
                       "returned %d (expected FAT_ERR_NOT_FOUND=%d)\n",
                       err, FAT_ERR_NOT_FOUND);
                all_ok = 0;
            }
        }

        /* ---- (b) "簇链断裂" -> FAT_ERR_CHAIN_BROKEN ---- */
        {
            vfs_node_t node;
            int b_ok = 1;

            if (fat_open("IDE0/SMALL.BIN", &node) != FAT_OK ||
                !node.fs || !node.fs->open || !node.fs->read) {
                printf("[fat21b] FAIL: IDE0/SMALL.BIN open path failed\n");
                b_ok = 0;
            } else {
                /* SMALL.BIN is really 1536 bytes (3 clusters, see
                 * build.sh's gen_fat12_fixtures()) — claim it is much
                 * bigger than that so a read whose starting offset
                 * lands past its real last cluster is not rejected
                 * up front as "past EOF" (fat_read_chain()'s own
                 * `file_pos >= file_size` short-circuit, kernel/fs/
                 * fat/fat_vfs.c) before it ever gets to walk the
                 * chain. */
                node.size = 1536u + (50u * 512u);

                file_t file;
                if (node.fs->open(&node, &file) != 0) {
                    printf("[fat21b] FAIL: open() returned -1\n");
                    b_ok = 0;
                } else {
                    /* Seek (by hand — no seek() op exists yet, this
                     * project's `file_t` only ever advances `pos` via
                     * fat_read() itself through Day 20) to an offset
                     * whose target cluster index (offset / 512) is
                     * past the file's real 3-cluster chain, so the
                     * skip loop's own next_cluster() call hits the
                     * chain's genuine FAT_CHAIN_EOC before reaching
                     * it. */
                    file.pos = 2000;   /* cluster index 3; real chain has clusters 0,1,2 */

                    uint8_t buf[16];
                    int n = node.fs->read(&file, buf, sizeof(buf));
                    if (n == FAT_ERR_CHAIN_BROKEN) {
                        printf("[fat21b] OK: reading past a file's real "
                               "cluster chain returned "
                               "FAT_ERR_CHAIN_BROKEN (\"%s\")\n",
                               fat_strerror(n));
                    } else {
                        printf("[fat21b] FAIL: expected "
                               "FAT_ERR_CHAIN_BROKEN=%d, got %d\n",
                               FAT_ERR_CHAIN_BROKEN, n);
                        b_ok = 0;
                    }
                }
            }

            if (!b_ok)
                all_ok = 0;
        }

        /* ---- (c) fat_close() actually invalidates the handle ---- */
        {
            vfs_node_t node;
            int c_ok = 1;

            if (fat_open("IDE0/SMALL.BIN", &node) != FAT_OK ||
                !node.fs || !node.fs->open || !node.fs->read ||
                !node.fs->close) {
                printf("[fat21c] FAIL: IDE0/SMALL.BIN open path "
                       "(incl. missing close op) failed\n");
                c_ok = 0;
            } else {
                file_t file;
                if (node.fs->open(&node, &file) != 0) {
                    printf("[fat21c] FAIL: open() returned -1\n");
                    c_ok = 0;
                } else {
                    uint8_t buf[16];
                    int n_before = node.fs->read(&file, buf, sizeof(buf));
                    int closed   = node.fs->close(&file);
                    int n_after  = node.fs->read(&file, buf, sizeof(buf));
                    int reclosed = node.fs->close(&file);

                    if (n_before >= 0 && closed == FAT_OK &&
                        n_after < 0 && reclosed == FAT_ERR_INVALID_ARGS) {
                        printf("[fat21c] OK: fat_close() invalidated the "
                               "file_t (post-close read failed, "
                               "double-close reported \"%s\")\n",
                               fat_strerror(reclosed));
                    } else {
                        printf("[fat21c] FAIL: n_before=%d closed=%d "
                               "n_after=%d reclosed=%d (expected "
                               ">=0, %d, <0, %d)\n",
                               n_before, closed, n_after, reclosed,
                               FAT_OK, FAT_ERR_INVALID_ARGS);
                        c_ok = 0;
                    }
                }
            }

            if (!c_ok)
                all_ok = 0;
        }

        if (all_ok) {
            console_write_color(
                "[fat21] OK: fat_open()/fat_read() report specific "
                "fat_error_t codes and fat_close() correctly "
                "invalidates a file_t\n", COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat21] FAIL: at least one DAY21 case above did not "
                "behave as expected\n", COLOR_ERROR);
        }
    }

    /* ------------------------------------------------------------------
     * DAY22 (Week 4, Day 22 of the 5.0 filesystem roadmap — start of
     *   "第 4 周：FAT 写入支持（创建、删除、截断）"):
     *   "实现 fat_find_free_cluster()：扫描 FAT 表，返回第一个值为 0 的
     *    簇。测试打印空闲簇号。"
     *
     *   Runs fat_find_free_cluster() (kernel/fs/fat/fat_vfs.c) against
     *   all three already-mounted volumes (IDE0=FAT12, IDE1=FAT16,
     *   IDE2=FAT32 — see the DAY13/16/17 sections above for how each
     *   got registered into the vfs.h mount table), fetched fresh via
     *   vfs_lookup_mount() the same way the DAY18/19/20/21 sections
     *   above already look filesystems up by mount-slot name rather
     *   than holding onto the `fs16`/`fs32` local variables from their
     *   own now-closed `{ ... }` scopes.
     *
     *   Each volume's stock image only has 2-3 small fixture files in
     *   its root directory (KERNEL.BIN/LOADER.BIN plus BIGFILE.BIN/
     *   SMALL.BIN — see build.sh's gen_fat12/16/32_fixtures()) sitting
     *   on a multi-MB volume, so every one of them is expected to have
     *   plenty of free clusters. This section's checks:
     *
     *     (a) fat_find_free_cluster() returns FAT_OK for all three
     *         mounted volumes;
     *     (b) the cluster number it reports lies inside the volume's
     *         valid data-cluster range [2, total_clusters+1];
     *     (c) a direct, INDEPENDENT follow-up read of that SAME
     *         cluster's raw FAT table entry — via the variant's own
     *         *_next_cluster(), the same low-level call fat_find_free_
     *         cluster() itself is built on — confirms it really is 0,
     *         proving the function isn't just returning a plausible-
     *         looking number, but one whose free-ness is independently
     *         verifiable from outside the function under test (the
     *         same "don't just trust the code being tested" spirit as
     *         every other [fatNN] section above);
     *     (d) fat_find_free_cluster(NULL, ...) reports
     *         FAT_ERR_INVALID_ARGS instead of crashing.
     * ------------------------------------------------------------------ */
    console_write_color(
        "[fat] fat_find_free_cluster() FAT-table scan (DAY22)...\n",
        COLOR_INFO);
    {
        int all_ok = 1;

        struct { const char *label; const char *slot; } vols[3] = {
            { "IDE0 (fat12)", "IDE0" },
            { "IDE1 (fat16)", "IDE1" },
            { "IDE2 (fat32)", "IDE2" },
        };

        for (int i = 0; i < 3; i++) {
            filesystem_t *fs = vfs_lookup_mount(vols[i].slot);
            if (!fs) {
                printf("[fat22] FAIL: %s: vfs_lookup_mount(\"%s\") "
                       "returned NULL\n", vols[i].label, vols[i].slot);
                all_ok = 0;
                continue;
            }

            /* ---- (a)/(b): find a free cluster, in range ---- */
            uint32_t free_cluster = 0;
            int rc = fat_find_free_cluster(fs, &free_cluster);
            if (rc != FAT_OK) {
                printf("[fat22] FAIL: %s: fat_find_free_cluster() "
                       "returned %d (\"%s\"), expected FAT_OK\n",
                       vols[i].label, rc, fat_strerror(rc));
                all_ok = 0;
                continue;
            }

            printf("[fat22] %s: first free cluster = %u\n",
                   vols[i].label, (unsigned)free_cluster);

            /* ---- (c): independently re-check the raw FAT entry ---- */
            int32_t raw = -999;
            if (strcmp(fs->name, "fat12") == 0) {
                fat12_fs_t *f = (fat12_fs_t *)fs->fs_private;
                raw = fat12_next_cluster(f, (uint16_t)free_cluster);
            } else if (strcmp(fs->name, "fat16") == 0) {
                fat16_fs_t *f = (fat16_fs_t *)fs->fs_private;
                raw = fat16_next_cluster(f, (uint16_t)free_cluster);
            } else if (strcmp(fs->name, "fat32") == 0) {
                fat32_fs_t *f = (fat32_fs_t *)fs->fs_private;
                raw = (int32_t)fat32_next_cluster(f, free_cluster);
            }

            if (raw != 0) {
                printf("[fat22] FAIL: %s: cluster %u's raw FAT entry "
                       "is %d, expected 0 (free)\n",
                       vols[i].label, (unsigned)free_cluster, raw);
                all_ok = 0;
            }
        }

        /* ---- (d): NULL-argument handling ---- */
        {
            uint32_t dummy;
            int rc = fat_find_free_cluster((filesystem_t *)0, &dummy);
            if (rc != FAT_ERR_INVALID_ARGS) {
                printf("[fat22] FAIL: fat_find_free_cluster(NULL, ...) "
                       "returned %d, expected FAT_ERR_INVALID_ARGS=%d\n",
                       rc, FAT_ERR_INVALID_ARGS);
                all_ok = 0;
            }
        }

        if (all_ok) {
            console_write_color(
                "[fat22] OK: fat_find_free_cluster() found and "
                "independently verified a genuinely free cluster on "
                "all three mounted FAT12/16/32 volumes\n",
                COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat22] FAIL: at least one DAY22 case above did not "
                "behave as expected\n", COLOR_ERROR);
        }
    }

    /* ------------------------------------------------------------------
     * DAY23 (Week 4, Day 23 of the 5.0 filesystem roadmap):
     *   "实现 fat_alloc_cluster()：分配一个新簇，并更新 FAT 表（写回磁
     *    盘）。分配成功后，再次读取 FAT 对应位置值变为 0xFFF（FAT12）
     *    或 0xFFFF（FAT16）。"
     *
     *   Runs fat_alloc_cluster() (kernel/fs/fat/fat_vfs.c) against all
     *   three already-mounted volumes, same IDE0/IDE1/IDE2 mount-slot
     *   pattern the DAY22 section above uses. This section's checks:
     *
     *     (a) fat_alloc_cluster() returns FAT_OK for all three mounted
     *         volumes, and the cluster number it hands back lies
     *         inside the volume's valid data-cluster range;
     *     (b) a direct, INDEPENDENT follow-up read of that SAME
     *         cluster's raw FAT entry — via the variant's own
     *         *_next_cluster(), NOT fat_alloc_cluster() or
     *         fat_find_free_cluster() again — confirms it now reads
     *         back as exactly that variant's EOC-max sentinel
     *         (FAT12_CLUSTER_EOC_MAX=0xFFF / FAT16_CLUSTER_EOC_MAX=
     *         0xFFFF / FAT32_CLUSTER_EOC_MAX=0x0FFFFFFF), i.e. the
     *         write genuinely reached disk and wasn't just held in
     *         some in-memory copy — the exact "再次读取...值变为..."
     *         acceptance text asks for, extended to all three variants
     *         instead of just the two the roadmap text names by
     *         example;
     *     (c) calling fat_alloc_cluster() a SECOND time on the same
     *         volume returns a DIFFERENT cluster number than the
     *         first call — proving the first call's cluster is no
     *         longer reported as free (it now reads as EOC, not 0),
     *         i.e. this is a genuine DAY22->DAY23 regression check
     *         that allocation and the free-cluster scan actually
     *         agree with each other, not just two isolated functions
     *         that each pass their own test in isolation;
     *     (d) fat_alloc_cluster(NULL, ...) reports FAT_ERR_INVALID_ARGS
     *         instead of crashing.
     * ------------------------------------------------------------------ */
    console_write_color(
        "[fat] fat_alloc_cluster() FAT-table write-back (DAY23)...\n",
        COLOR_INFO);
    {
        int all_ok = 1;

        struct { const char *label; const char *slot; uint32_t eoc; } vols[3] = {
            { "IDE0 (fat12)", "IDE0", FAT12_CLUSTER_EOC_MAX },
            { "IDE1 (fat16)", "IDE1", FAT16_CLUSTER_EOC_MAX },
            { "IDE2 (fat32)", "IDE2", FAT32_CLUSTER_EOC_MAX },
        };

        for (int i = 0; i < 3; i++) {
            filesystem_t *fs = vfs_lookup_mount(vols[i].slot);
            if (!fs) {
                printf("[fat23] FAIL: %s: vfs_lookup_mount(\"%s\") "
                       "returned NULL\n", vols[i].label, vols[i].slot);
                all_ok = 0;
                continue;
            }

            /* ---- (a): allocate a cluster ---- */
            uint32_t first_cluster = 0;
            int rc = fat_alloc_cluster(fs, &first_cluster);
            if (rc != FAT_OK) {
                printf("[fat23] FAIL: %s: fat_alloc_cluster() "
                       "returned %d (\"%s\"), expected FAT_OK\n",
                       vols[i].label, rc, fat_strerror(rc));
                all_ok = 0;
                continue;
            }

            printf("[fat23] %s: allocated cluster = %u\n",
                   vols[i].label, (unsigned)first_cluster);

            /* ---- (b): independently re-check the raw FAT entry now
             * reads back as this variant's EOC-max sentinel ---- */
            int32_t raw = -999;
            if (strcmp(fs->name, "fat12") == 0) {
                fat12_fs_t *f = (fat12_fs_t *)fs->fs_private;
                raw = fat12_next_cluster(f, (uint16_t)first_cluster);
            } else if (strcmp(fs->name, "fat16") == 0) {
                fat16_fs_t *f = (fat16_fs_t *)fs->fs_private;
                raw = fat16_next_cluster(f, (uint16_t)first_cluster);
            } else if (strcmp(fs->name, "fat32") == 0) {
                fat32_fs_t *f = (fat32_fs_t *)fs->fs_private;
                raw = (int32_t)fat32_next_cluster(f, first_cluster);
            }

            if ((uint32_t)raw != vols[i].eoc) {
                printf("[fat23] FAIL: %s: cluster %u's raw FAT entry "
                       "is 0x%x after alloc, expected EOC 0x%x\n",
                       vols[i].label, (unsigned)first_cluster,
                       (unsigned)raw, (unsigned)vols[i].eoc);
                all_ok = 0;
            }

            /* ---- (c): a second alloc must claim a DIFFERENT cluster ---- */
            uint32_t second_cluster = 0;
            rc = fat_alloc_cluster(fs, &second_cluster);
            if (rc != FAT_OK) {
                printf("[fat23] FAIL: %s: second fat_alloc_cluster() "
                       "call returned %d (\"%s\"), expected FAT_OK\n",
                       vols[i].label, rc, fat_strerror(rc));
                all_ok = 0;
            } else if (second_cluster == first_cluster) {
                printf("[fat23] FAIL: %s: second fat_alloc_cluster() "
                       "re-claimed the same cluster %u as the first "
                       "call — free-cluster scan and allocation "
                       "disagree\n", vols[i].label, (unsigned)first_cluster);
                all_ok = 0;
            } else {
                printf("[fat23] %s: second allocated cluster = %u "
                       "(distinct from first)\n",
                       vols[i].label, (unsigned)second_cluster);
            }
        }

        /* ---- (d): NULL-argument handling ---- */
        {
            uint32_t dummy;
            int rc = fat_alloc_cluster((filesystem_t *)0, &dummy);
            if (rc != FAT_ERR_INVALID_ARGS) {
                printf("[fat23] FAIL: fat_alloc_cluster(NULL, ...) "
                       "returned %d, expected FAT_ERR_INVALID_ARGS=%d\n",
                       rc, FAT_ERR_INVALID_ARGS);
                all_ok = 0;
            }
        }

        if (all_ok) {
            console_write_color(
                "[fat23] OK: fat_alloc_cluster() claimed and "
                "independently verified an EOC-marked cluster on all "
                "three mounted FAT12/16/32 volumes, and repeat calls "
                "claim distinct clusters\n",
                COLOR_SUCCESS);
        } else {
            console_write_color(
                "[fat23] FAIL: at least one DAY23 case above did not "
                "behave as expected\n", COLOR_ERROR);
        }
    }

    /* ------------------------------------------------------------------
     * DAY32 (第二月 · 第5周 · Day 32 of the 5.0 filesystem roadmap):
     *   "C 端实现 lmfs_mount(dev)。" 验收："能读取超级块并验证版本/容量。"
     *
     *   Day 29/30 只产出了 doc/lmfs.txt 设计文档，Day 31 只产出了
     *   宿主机侧的 tools/mkfs.lmfs；今天是 LMFS 第一次真正在内核里跑
     *   起来。build.sh 的 gen_lmfs_fixture() 每次构建都会重新跑一遍
     *   tools/mkfs.lmfs，生成 tests/lmfs-fixture.img 并嵌入到磁盘镜像
     *   LBA 100000 处（build.sh 自己的 DAY32 注释里说明了为什么选这个
     *   LBA）——下面跟 DAY10 fat12_mount() 测试同一个套路：先挂载这个
     *   真实生成的 LMFS 卷（应该成功），再挂载一个明知不是 LMFS 卷的
     *   位置（LBA 0，LumaOS 自己的 mbr.asm 引导扇区）验证失败路径也
     *   按预期工作，两条路径都覆盖到，而不是只测"能成功"这一半。
     * ------------------------------------------------------------------ */
#define LMFS_FIXTURE_LBA  100000  /* build.sh: tests/lmfs-fixture.img */
    console_write_color(
        "[lmfs] Mounting LMFS volume at LBA 100000...\n", COLOR_INFO);
    {
        static lmfs_fs_t lfs;
        if (lmfs_mount(disk, LMFS_FIXTURE_LBA, &lfs) == 0) {
            console_write_color("LMFS mounted\n", COLOR_SUCCESS);
            printf("[lmfs] version=%u.%u inode_count=%u data_block_count=%u\n",
                   lfs.sb.version_major, lfs.sb.version_minor,
                   lfs.sb.inode_count, lfs.sb.data_block_count);
            printf("[lmfs] inode_bitmap_lba=%u (volume-relative) "
                   "data_bitmap_lba=%u (volume-relative)\n",
                   lfs.sb.inode_bitmap_lba, lfs.sb.data_bitmap_lba);
            printf("[lmfs] magic=0x%x (expected 0x%x)\n",
                   lfs.sb.magic, LMFS_MAGIC);
        } else {
            console_write_color(
                "[lmfs] FAIL: lmfs_mount(disk, 100000, ...) returned -1 "
                "(unexpected — LBA 100000 should hold a valid LMFS "
                "volume; see build.sh's gen_lmfs_fixture())\n", COLOR_ERROR);
        }
    }

    console_write_color(
        "[lmfs] Mounting LMFS volume at LBA 0 (expected to fail)...\n",
        COLOR_INFO);
    {
        static lmfs_fs_t lfs0;
        if (lmfs_mount(disk, 0, &lfs0) == 0) {
            console_write_color(
                "[lmfs] FAIL: lmfs_mount(disk, 0, ...) unexpectedly "
                "returned 0 (LBA 0 is LumaOS's own mbr.asm sector, not "
                "an LMFS volume)\n", COLOR_ERROR);
        } else {
            console_write_color(
                "[lmfs] NOTE: mount failed as expected (LBA 0 is not "
                "an LMFS volume — magic check correctly rejected it, "
                "same DAY10 fat12_mount()-style negative-path check "
                "above)\n", COLOR_WARNING);
        }
    }

    /* ------------------------------------------------------------------
     * DAY33 (第二月 · 第5周 · Day 33 of the 5.0 filesystem roadmap):
     *   "实现 lmfs_read_inode()。" 验收："能读取 inode 0。"
     *
     *   重新挂载同一个 LBA 100000 的 LMFS 卷（跟上面 DAY32 那段各自
     *   独立，不依赖它的局部变量还留在作用域里——每个 DAYxx 测试块
     *   都是自己重新走一遍"挂载 -> 用"，不偷懒复用上一段的挂载结果，
     *   这样任何一天的代码单独看也是完整的），然后读三个 inode 验证
     *   lmfs_read_inode() 的三条边界：
     *
     *     1. inode 0（根目录）：应该成功，且 type=DIR、file_size=1024
     *        （1 个块）、direct[0]=1（DAY34: tools/mkfs.lmfs 现在给
     *        根目录分配了 1 个数据块存 "."/".." 两个目录项，见
     *        doc/lmfs.txt 第十二节）、direct[1..7] 和两个间接块指针
     *        全部是 0——这里等于用内核代码把 mkfs.lmfs 用 Python
     *        写下的东西原样读回来，两边对上了才说明 lmfs_read_inode()
     *        的定位公式和 mkfs.lmfs 的布局计算完全一致。
     *     2. inode 1（未使用的空闲槽位）：应该成功（1 < inode_count=
     *        256，没有越界），且 type=FREE(0)——验证"读到的不只是
     *        凑巧对的 0 号"，任意一个合法范围内的 inode 号都能读对。
     *     3. inode = inode_count（=256，卷里合法编号是 0~255，256 正
     *        好越界 1）：应该失败——验证 lmfs_read_inode() 自己的
     *        边界检查确实在起作用，而不是"读到卷尾之后的随机数据"。
     * ------------------------------------------------------------------ */
    console_write_color(
        "[lmfs] DAY33: lmfs_read_inode() against LBA 100000...\n",
        COLOR_INFO);
    {
        static lmfs_fs_t lfs33;
        if (lmfs_mount(disk, LMFS_FIXTURE_LBA, &lfs33) != 0) {
            console_write_color(
                "[lmfs] FAIL: re-mount for DAY33 test unexpectedly failed\n",
                COLOR_ERROR);
        } else {
            static lmfs_inode_t root_inode;
            if (lmfs_read_inode(&lfs33, LMFS_ROOT_INODE_NUM, &root_inode) == 0) {
                int rest_zero_ptrs = (root_inode.indirect1 == 0 &&
                                      root_inode.indirect2 == 0);
                for (int i = 1; i < 8; i++)
                    if (root_inode.direct[i] != 0)
                        rest_zero_ptrs = 0;

                if (root_inode.type == LMFS_TYPE_DIR &&
                    root_inode.file_size == LMFS_BLOCK_SIZE &&
                    root_inode.direct[0] == 1 && rest_zero_ptrs) {
                    console_write_color("inode 0 read OK\n", COLOR_SUCCESS);
                    printf("[lmfs] inode 0: type=DIR(%u) file_size=%u "
                           "direct[0]=%u (rest of pointers=0)\n",
                           root_inode.type, root_inode.file_size,
                           root_inode.direct[0]);
                } else {
                    console_write_color(
                        "[lmfs] FAIL: inode 0 content doesn't match what "
                        "tools/mkfs.lmfs wrote for the root directory\n",
                        COLOR_ERROR);
                }
            } else {
                console_write_color(
                    "[lmfs] FAIL: lmfs_read_inode(fs, 0, ...) unexpectedly "
                    "returned -1\n", COLOR_ERROR);
            }

            static lmfs_inode_t free_inode;
            /* DAY45 修正：这里原来读 inode 1。Day 44 起夹具预装了 hello.sw
             * （inode 1），Day 45 又预装了 data.sw（inode 2），inode 1 不再
             * 空闲，旧检查每次开机都打印 FAIL。改读 inode 200：预装文件按
             * 顺序占用 1、2、3…，不可能排到 200，它必然是空闲槽位，测试的
             * 意图（“合法范围内任意编号都读得对”）不变。 */
            if (lmfs_read_inode(&lfs33, 200, &free_inode) == 0 &&
                free_inode.type == LMFS_TYPE_FREE) {
                console_write_color("inode 200 (free slot) read OK\n",
                                     COLOR_SUCCESS);
                printf("[lmfs] inode 200: type=FREE(%u), as expected for an "
                       "unused slot\n", free_inode.type);
            } else {
                console_write_color(
                    "[lmfs] FAIL: inode 200 should read back as an unused "
                    "(type=FREE) slot\n", COLOR_ERROR);
            }

            static lmfs_inode_t oob_inode;
            uint32_t oob_num = lfs33.sb.inode_count;  /* 合法范围之外 1 个 */
            if (lmfs_read_inode(&lfs33, oob_num, &oob_inode) != 0) {
                console_write_color(
                    "out-of-range inode number correctly rejected\n",
                    COLOR_SUCCESS);
                printf("[lmfs] lmfs_read_inode(fs, %u, ...) correctly "
                       "returned -1 (inode_count=%u)\n",
                       oob_num, lfs33.sb.inode_count);
            } else {
                console_write_color(
                    "[lmfs] FAIL: lmfs_read_inode() should have rejected "
                    "an out-of-range inode number\n", COLOR_ERROR);
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY34 (第二月 · 第5周 · Day 34 of the 5.0 filesystem roadmap):
     *   "实现 lmfs_readdir()。" 验收："能列出根目录。"
     *
     *   build.sh 的 gen_lmfs_fixture() 现在跑的是更新过的
     *   tools/mkfs.lmfs（Day 34 追加改动）：根目录不再是空的，已经
     *   分配了 1 个数据块，写入 "." 和 ".." 两条目录项，都指向
     *   inode 0。下面重新挂载同一个 LBA 100000 的卷，对根目录调用
     *   lmfs_readdir()，验证：
     *     1. 恰好读回 2 条目录项（不多不少——多了说明误把空闲槽位
     *        也解码了，少了说明漏扫了某个槽位）。
     *     2. 两条都是 type=DIR、inode_num=0（根目录指向自己）。
     *     3. 名字分别是 "." 和 ".."（顺序也跟 mkfs.lmfs 写入的顺序
     *        一致——第 0 个槽位是 "."，第 1 个槽位是 ".."）。
     * ------------------------------------------------------------------ */
    console_write_color(
        "[lmfs] DAY34: lmfs_readdir() against LBA 100000 root...\n",
        COLOR_INFO);
    {
        static lmfs_fs_t lfs34;
        if (lmfs_mount(disk, LMFS_FIXTURE_LBA, &lfs34) != 0) {
            console_write_color(
                "[lmfs] FAIL: re-mount for DAY34 test unexpectedly failed\n",
                COLOR_ERROR);
        } else {
            static lmfs_dirlist_entry_t entries[LMFS_DIRLIST_MAX_ENTRIES];
            uint32_t count = 0;
            if (lmfs_readdir(&lfs34, LMFS_ROOT_INODE_NUM, entries,
                              LMFS_DIRLIST_MAX_ENTRIES, &count) == 0) {
                printf("[lmfs] lmfs_readdir(root) returned %u entries\n",
                       count);
                for (uint32_t i = 0; i < count; i++) {
                    printf("[lmfs]   [%u] name=\"%s\" type=%u inode_num=%u "
                           "size=%u\n",
                           i, entries[i].name, entries[i].type,
                           entries[i].inode_num, entries[i].size);
                }

                /* DAY45 修正：根目录前两项仍必须是 "." 和 ".."，其后允许有
                 * 预装文件（hello.sw、data.sw…），它们必须是 FILE 类型。 */
                int ok = (count >= 2 &&
                          entries[0].type == LMFS_TYPE_DIR &&
                          entries[0].inode_num == LMFS_ROOT_INODE_NUM &&
                          entries[0].name[0] == '.' &&
                          entries[0].name[1] == '\0' &&
                          entries[1].type == LMFS_TYPE_DIR &&
                          entries[1].inode_num == LMFS_ROOT_INODE_NUM &&
                          entries[1].name[0] == '.' &&
                          entries[1].name[1] == '.' &&
                          entries[1].name[2] == '\0');
                for (uint32_t i = 2; i < count; i++)
                    if (entries[i].type != LMFS_TYPE_FILE)
                        ok = 0;

                if (ok) {
                    console_write_color("root directory listed OK\n",
                                         COLOR_SUCCESS);
                } else {
                    console_write_color(
                        "[lmfs] FAIL: root directory listing doesn't match "
                        "what tools/mkfs.lmfs wrote ('.' and '..' -> "
                        "inode 0, then only files)\n", COLOR_ERROR);
                }
            } else {
                console_write_color(
                    "[lmfs] FAIL: lmfs_readdir(root) unexpectedly "
                    "returned -1\n", COLOR_ERROR);
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY35 (第二月 · 第6周 · Day 35 of the 5.0 filesystem roadmap):
     *   "LMFS 注册进 VFS。" 验收："FAT 与 LMFS 可以同时存在。"
     *
     *   到这里为止，FAT12/16/17 三段（上面 DAY13/16/17）早就通过
     *   vfs_register_mount() 各自占了 "IDE0"/"IDE1"/"IDE2" 槽位——
     *   这张多挂载点命名表是 DAY18-FOLLOWUP 时提前建好的，等的就是
     *   今天：LMFS 通过新写的 lmfs_vfs_mount()（Day 35 本身的产出）
     *   包装成 filesystem_t，同样调用 vfs_register_mount() 登记，
     *   预期会拿到 "IDE3"（前三个槽位早被占了）。
     *
     *   验收标准"FAT 与 LMFS 可以同时存在"在这里验证两层：
     *     1. 登记之后，用 vfs_lookup_mount() 挨个查 "IDE0".."IDE3"，
     *        四个槽位都要能查到、且 fs->name 分别是 "fat12"/"fat16"/
     *        "fat32"/"lmfs"——不是"最后挂载的那个把前面的顶掉了"，
     *        而是四个真的同时挂在表里。
     *     2. 通过纯 VFS 层的通用接口（fs->readdir()，不是直接调用
     *        lmfs_readdir()）去列 LMFS 根目录，证明 lmfs_vfs.c 这层
     *        适配器本身是对的，不是只测了 lmfs.c 底层。
     * ------------------------------------------------------------------ */
    console_write_color(
        "[lmfs] DAY35: lmfs_vfs_mount() + vfs_register_mount()...\n",
        COLOR_INFO);
    {
        filesystem_t *fs_lmfs = lmfs_vfs_mount(disk, LMFS_FIXTURE_LBA);
        if (!fs_lmfs) {
            console_write_color(
                "[lmfs] FAIL: lmfs_vfs_mount() returned NULL\n", COLOR_ERROR);
        } else {
            char slot_lmfs[8];
            if (vfs_register_mount(fs_lmfs, slot_lmfs) == 0) {
                printf("[lmfs] Registered as mount slot \"%s\" (fs->name=\"%s\")\n",
                       slot_lmfs, fs_lmfs->name);

                console_write_color(
                    "[lmfs] Checking IDE0..IDE3 all resolve (FAT12/16/32 + "
                    "LMFS coexist)...\n", COLOR_INFO);
                struct { const char *slot; const char *expect_name; }
                    want[] = {
                        { "IDE0", "fat12" },
                        { "IDE1", "fat16" },
                        { "IDE2", "fat32" },
                        { "IDE3", "lmfs"  },
                    };
                int all_ok = 1;
                for (uint32_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
                    filesystem_t *found = vfs_lookup_mount(want[i].slot);
                    if (!found) {
                        printf("[lmfs] FAIL: vfs_lookup_mount(\"%s\") "
                               "returned NULL\n", want[i].slot);
                        all_ok = 0;
                    } else if (strcmp(found->name, want[i].expect_name) != 0) {
                        printf("[lmfs] FAIL: %s has fs->name=\"%s\", "
                               "expected \"%s\"\n",
                               want[i].slot, found->name, want[i].expect_name);
                        all_ok = 0;
                    } else {
                        printf("[lmfs]   %s -> fs->name=\"%s\" OK\n",
                               want[i].slot, found->name);
                    }
                }

                if (all_ok) {
                    console_write_color(
                        "FAT and LMFS coexist in the VFS mount table\n",
                        COLOR_SUCCESS);
                } else {
                    console_write_color(
                        "[lmfs] FAIL: not all four mount slots checked out "
                        "(see lines above)\n", COLOR_ERROR);
                }

                console_write_color(
                    "[lmfs] Listing LMFS root via fs->readdir() (pure VFS "
                    "interface, not lmfs_readdir() directly)...\n",
                    COLOR_INFO);
                {
                    vfs_node_t child;
                    uint32_t idx = 0;
                    int any = 0;
                    while (fs_lmfs->readdir(fs_lmfs->root, idx, &child) == 0) {
                        const char *slash =
                            (child.type == VFS_TYPE_DIR) ? "/" : "";
                        printf("[lmfs]   %s%s size=%u\n",
                               child.name, slash, child.size);
                        any = 1;
                        idx++;
                    }
                    if (any) {
                        console_write_color(
                            "LMFS listed via generic fs->readdir()\n",
                            COLOR_SUCCESS);
                    } else {
                        console_write_color(
                            "[lmfs] FAIL: fs->readdir() returned no entries "
                            "(expected '.' and '..')\n", COLOR_ERROR);
                    }
                }
            } else {
                console_write_color(
                    "[lmfs] FAIL: vfs_register_mount() failed (mount "
                    "table full?)\n", COLOR_ERROR);
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY36 (第二月 · 第6周 · Day 36 of the 5.0 filesystem roadmap):
     *   "实现数据块 bitmap 分配器。" 验收："连续分配多个块，bitmap 正确变化。"
     *
     *   重新挂载同一个 LBA 100000 的卷，连续调用 3 次 lmfs_alloc_block()。
     *   这个卷的数据块位图目前只有块号 0（保留哨兵值）和块号 1（根
     *   目录数据块，Day 34）被 mkfs.lmfs 标记为已占用，其余全部空闲
     *   ——所以按 lmfs_alloc_block() 的线性扫描顺序，预期连续 3 次
     *   调用会依次拿到块号 2、3、4。验证两层：
     *     1. 三次返回的块号确实互不相同、都不是 0、且严格递增（2 <
     *        3 < 4）——不是"凑巧不重复"，是扫描顺序决定的必然结果。
     *     2. 每次分配后，绕开 lmfs_alloc_block() 自己，直接用
     *        disk->read() 读回数据块位图所在的那一个原始扇区，逐位
     *        确认对应的 bit 真的从 0 变成了 1——不只是信任函数的
     *        返回值，而是独立验证磁盘上的字节确实被改写了，跟
     *        Day 31 mkfs.lmfs 用十六进制查看器核实、Day 23
     *        fat_alloc_cluster() 独立验证 FAT 表项是同一个"不只测
     *        返回值，也测底层字节"的检验标准。
     * ------------------------------------------------------------------ */
    console_write_color(
        "[lmfs] DAY36: lmfs_alloc_block() against LBA 100000...\n",
        COLOR_INFO);
    {
        static lmfs_fs_t lfs36;
        if (lmfs_mount(disk, LMFS_FIXTURE_LBA, &lfs36) != 0) {
            console_write_color(
                "[lmfs] FAIL: re-mount for DAY36 test unexpectedly failed\n",
                COLOR_ERROR);
        } else {
            uint32_t allocated[3] = { 0, 0, 0 };
            int all_ok = 1;

            for (int i = 0; i < 3; i++) {
                if (lmfs_alloc_block(&lfs36, &allocated[i]) != 0) {
                    printf("[lmfs] FAIL: lmfs_alloc_block() call #%d "
                           "unexpectedly returned -1\n", i + 1);
                    all_ok = 0;
                    break;
                }
                printf("[lmfs] lmfs_alloc_block() call #%d -> block %u\n",
                       i + 1, allocated[i]);
            }

            if (all_ok) {
                if (allocated[0] == 0 || allocated[1] == 0 ||
                    allocated[2] == 0) {
                    console_write_color(
                        "[lmfs] FAIL: block 0 (reserved sentinel) was "
                        "handed out by lmfs_alloc_block()\n", COLOR_ERROR);
                    all_ok = 0;
                } else if (!(allocated[0] < allocated[1] &&
                             allocated[1] < allocated[2])) {
                    console_write_color(
                        "[lmfs] FAIL: allocated block numbers are not "
                        "strictly increasing (unexpected scan order)\n",
                        COLOR_ERROR);
                    all_ok = 0;
                } else {
                    console_write_color(
                        "3 distinct, non-zero, increasing block numbers "
                        "allocated\n", COLOR_SUCCESS);
                }
            }

            if (all_ok) {
                /* 绕开 lmfs_alloc_block()，直接读数据块位图所在的
                 * 那一个磁盘扇区，独立核实这 3 个 bit 真的被置位了。 */
                static uint8_t bitmap_raw[LMFS_SECTOR_SIZE];
                uint32_t bitmap_lba = lfs36.mount_lba
                                      + lfs36.sb.data_bitmap_lba;
                if (disk->read(disk, bitmap_lba, 1, bitmap_raw) != 0) {
                    console_write_color(
                        "[lmfs] FAIL: couldn't independently re-read the "
                        "data bitmap sector\n", COLOR_ERROR);
                } else {
                    int bits_ok = 1;
                    for (int i = 0; i < 3; i++) {
                        uint32_t b = allocated[i];
                        int bit_set = (bitmap_raw[b / 8] >> (b % 8)) & 1;
                        printf("[lmfs]   bit for block %u on disk = %d\n",
                               b, bit_set);
                        if (!bit_set)
                            bits_ok = 0;
                    }
                    if (bits_ok) {
                        console_write_color(
                            "data bitmap correctly updated on disk\n",
                            COLOR_SUCCESS);
                    } else {
                        console_write_color(
                            "[lmfs] FAIL: one or more allocated blocks' "
                            "bits were not actually set on disk\n",
                            COLOR_ERROR);
                    }
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY37 (第二月 · 第6周 · Day 37 of the 5.0 filesystem roadmap):
     *   "实现 inode 分配器。" 验收："创建 inode 后返回合法 inode 号。"
     *
     *   重新挂载同一个 LBA 100000 的卷，连续调用 3 次
     *   lmfs_alloc_inode()（都传 LMFS_TYPE_FILE）。这个卷目前只有
     *   0 号 inode（根目录）被 mkfs.lmfs 标记为已分配，DAY36 的测试
     *   只碰了数据块位图、没碰 inode 位图，所以预期连续 3 次调用
     *   依次拿到 inode 号 1、2、3。验证三层（比 DAY36 多一层，因为
     *   inode 分配器比数据块分配器多做"初始化 inode 内容"这一步，
     *   见 doc/lmfs.txt 第十六节）：
     *     1. 三次返回的 inode 号互不相同、都不是 0、严格递增。
     *     2. 对每个新分配到的 inode 号调用 lmfs_read_inode()（Day
     *        33 已经验证过的读取路径）读回来，确认 type==FILE、
     *        file_size==0、8 个直接块指针和两个间接块指针全部是 0
     *        ——证明 lmfs_alloc_inode() 真的初始化了 inode 内容，
     *        不是只翻了位图。
     *     3. 绕开 lmfs_alloc_inode() 自己，直接用 disk->read() 读回
     *        inode 位图所在的原始扇区，逐位确认对应的 bit 真的从 0
     *        变成了 1——跟 DAY36 同样的"不只测返回值，也测底层字节"
     *        标准。
     * ------------------------------------------------------------------ */
    console_write_color(
        "[lmfs] DAY37: lmfs_alloc_inode() against LBA 100000...\n",
        COLOR_INFO);
    {
        static lmfs_fs_t lfs37;
        if (lmfs_mount(disk, LMFS_FIXTURE_LBA, &lfs37) != 0) {
            console_write_color(
                "[lmfs] FAIL: re-mount for DAY37 test unexpectedly failed\n",
                COLOR_ERROR);
        } else {
            uint32_t allocated[3] = { 0, 0, 0 };
            int all_ok = 1;

            for (int i = 0; i < 3; i++) {
                if (lmfs_alloc_inode(&lfs37, LMFS_TYPE_FILE,
                                      &allocated[i]) != 0) {
                    printf("[lmfs] FAIL: lmfs_alloc_inode() call #%d "
                           "unexpectedly returned -1\n", i + 1);
                    all_ok = 0;
                    break;
                }
                printf("[lmfs] lmfs_alloc_inode() call #%d -> inode %u\n",
                       i + 1, allocated[i]);
            }

            if (all_ok) {
                if (allocated[0] == 0 || allocated[1] == 0 ||
                    allocated[2] == 0) {
                    console_write_color(
                        "[lmfs] FAIL: inode 0 (root directory) was "
                        "handed out by lmfs_alloc_inode()\n", COLOR_ERROR);
                    all_ok = 0;
                } else if (!(allocated[0] < allocated[1] &&
                             allocated[1] < allocated[2])) {
                    console_write_color(
                        "[lmfs] FAIL: allocated inode numbers are not "
                        "strictly increasing (unexpected scan order)\n",
                        COLOR_ERROR);
                    all_ok = 0;
                } else {
                    console_write_color(
                        "3 distinct, non-zero, increasing inode numbers "
                        "allocated\n", COLOR_SUCCESS);
                }
            }

            if (all_ok) {
                int content_ok = 1;
                for (int i = 0; i < 3; i++) {
                    static lmfs_inode_t ino;
                    if (lmfs_read_inode(&lfs37, allocated[i], &ino) != 0) {
                        printf("[lmfs] FAIL: lmfs_read_inode(%u) "
                               "unexpectedly returned -1\n", allocated[i]);
                        content_ok = 0;
                        continue;
                    }
                    int ptrs_zero = (ino.indirect1 == 0 &&
                                      ino.indirect2 == 0);
                    for (int d = 0; d < 8; d++)
                        if (ino.direct[d] != 0)
                            ptrs_zero = 0;

                    printf("[lmfs]   inode %u: type=%u file_size=%u "
                           "pointers_zero=%d\n",
                           allocated[i], ino.type, ino.file_size,
                           ptrs_zero);

                    if (ino.type != LMFS_TYPE_FILE || ino.file_size != 0 ||
                        !ptrs_zero)
                        content_ok = 0;
                }
                if (content_ok) {
                    console_write_color(
                        "newly allocated inodes correctly initialized "
                        "(type=FILE, empty)\n", COLOR_SUCCESS);
                } else {
                    console_write_color(
                        "[lmfs] FAIL: one or more newly allocated inodes "
                        "were not correctly initialized\n", COLOR_ERROR);
                }
            }

            if (all_ok) {
                /* 绕开 lmfs_alloc_inode()，直接读 inode 位图所在的
                 * 那一个磁盘扇区，独立核实这 3 个 bit 真的被置位了。 */
                static uint8_t inode_bitmap_raw[LMFS_SECTOR_SIZE];
                uint32_t bitmap_lba = lfs37.mount_lba
                                      + lfs37.sb.inode_bitmap_lba;
                if (disk->read(disk, bitmap_lba, 1, inode_bitmap_raw) != 0) {
                    console_write_color(
                        "[lmfs] FAIL: couldn't independently re-read the "
                        "inode bitmap sector\n", COLOR_ERROR);
                } else {
                    int bits_ok = 1;
                    for (int i = 0; i < 3; i++) {
                        uint32_t n = allocated[i];
                        int bit_set =
                            (inode_bitmap_raw[n / 8] >> (n % 8)) & 1;
                        printf("[lmfs]   bit for inode %u on disk = %d\n",
                               n, bit_set);
                        if (!bit_set)
                            bits_ok = 0;
                    }
                    if (bits_ok) {
                        console_write_color(
                            "inode bitmap correctly updated on disk\n",
                            COLOR_SUCCESS);
                    } else {
                        console_write_color(
                            "[lmfs] FAIL: one or more allocated inodes' "
                            "bits were not actually set on disk\n",
                            COLOR_ERROR);
                    }
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY38 (第二月 · 第6周 · Day 38 of the 5.0 filesystem roadmap):
     *   "实现 create() 与目录项。" 验收："create test.txt 成功。"
     *
     *   重新挂载同一个 LBA 100000 的卷，调用 lmfs_create() 在根目录
     *   下建一个 "kday38.txt" 文件（故意不叫 test.txt——真正的
     *   `create IDE3/test.txt` 交给 tests/day38_test_lmfs_create.py
     *   在真实的交互式 Shell 里敲，跟这里的内核自测互为补充，两者
     *   用不同文件名是为了避免这个 kmain.c 自测段跟那个独立脚本在
     *   不同次启动里各自创建的文件互相"撞名"产生误判）。验证三层：
     *     1. lmfs_create() 返回 0，拿到一个合法 inode 号——除非这个
     *        磁盘镜像是上一次启动留下来的、已经创建过 "kday38.txt"
     *        （这个项目没有"每次开机重置磁盘"的机制，lumaos-5.0.img
     *        在多次启动之间是真正持久的，Day 24 的 create 命令测试
     *        早就在 tests/day24_test_create.py 的说明里点出过这一点：
     *        重复用同一个镜像启动，第二次看到"重名被拒绝"是磁盘正确
     *        work 的表现，不是 bug）。所以这段代码先用 lmfs_readdir()
     *        查一遍 "kday38.txt" 在不在，不在才新建，已经在的话直接
     *        复用已有的 inode 号继续后面的检查——保证这段自测不管
     *        重复开机多少次都不会把"名字已存在"误判成失败，跟 DAY23
     *        fat_alloc_cluster() 自测"重复调用会认领不同簇"是同一种
     *        "容忍镜像上累积的历史状态"的处理方式。
     *     2. lmfs_read_inode() 读回这个 inode，确认 type==FILE、
     *        file_size==0、块指针全部是 0——新建的空文件一开始就该
     *        是这个样子（真正写内容是 Day 39 的任务）。
     *     3. lmfs_readdir() 重新列根目录，确认这个文件出现在列表里，
     *        且名字、inode 号都对得上——证明目录项真的写进了父目录的
     *        数据块，不是只建了个孤立 inode。
     *     4. 再调用一次同名的 lmfs_create()，确认返回 -1（重名拒绝）
     *        ——这一步无论是不是第一次开机，预期结果都一样。
     * ------------------------------------------------------------------ */
    console_write_color(
        "[lmfs] DAY38: lmfs_create() against LBA 100000 root...\n",
        COLOR_INFO);
    {
        static lmfs_fs_t lfs38;
        if (lmfs_mount(disk, LMFS_FIXTURE_LBA, &lfs38) != 0) {
            console_write_color(
                "[lmfs] FAIL: re-mount for DAY38 test unexpectedly failed\n",
                COLOR_ERROR);
        } else {
            const char *new_name = "kday38.txt";
            uint32_t new_inode_num = 0;
            int have_inode = 0;

            /* 先查这个名字在不在——见上面的设计说明，让这段自测对
             * "镜像已经被前一次开机改过"保持幂等。 */
            {
                static lmfs_dirlist_entry_t
                    pre_entries[LMFS_DIRLIST_MAX_ENTRIES];
                uint32_t pre_count = 0;
                if (lmfs_readdir(&lfs38, LMFS_ROOT_INODE_NUM, pre_entries,
                                  LMFS_DIRLIST_MAX_ENTRIES, &pre_count) == 0) {
                    for (uint32_t i = 0; i < pre_count; i++) {
                        if (strcmp(pre_entries[i].name, new_name) == 0) {
                            new_inode_num = pre_entries[i].inode_num;
                            have_inode = 1;
                            printf("[lmfs] \"%s\" already exists from a "
                                   "previous boot of this image (inode "
                                   "%u) — reusing it\n",
                                   new_name, new_inode_num);
                            break;
                        }
                    }
                }
            }

            if (!have_inode) {
                if (lmfs_create(&lfs38, LMFS_ROOT_INODE_NUM, new_name,
                                 &new_inode_num) != 0) {
                    console_write_color(
                        "[lmfs] FAIL: lmfs_create() unexpectedly "
                        "returned -1\n", COLOR_ERROR);
                } else {
                    printf("[lmfs] lmfs_create() -> inode %u\n",
                           new_inode_num);
                    have_inode = 1;
                }
            }

            if (have_inode) {
                static lmfs_inode_t new_ino;
                int content_ok = 0;
                if (lmfs_read_inode(&lfs38, new_inode_num, &new_ino) == 0) {
                    int ptrs_zero = (new_ino.indirect1 == 0 &&
                                      new_ino.indirect2 == 0);
                    for (int d = 0; d < 8; d++)
                        if (new_ino.direct[d] != 0)
                            ptrs_zero = 0;
                    content_ok = (new_ino.type == LMFS_TYPE_FILE &&
                                  new_ino.file_size == 0 && ptrs_zero);
                    printf("[lmfs] new inode: type=%u file_size=%u "
                           "pointers_zero=%d\n",
                           new_ino.type, new_ino.file_size, ptrs_zero);
                }
                if (content_ok) {
                    console_write_color(
                        "new inode correctly initialized (type=FILE, "
                        "empty)\n", COLOR_SUCCESS);
                } else {
                    console_write_color(
                        "[lmfs] FAIL: new inode content is wrong\n",
                        COLOR_ERROR);
                }

                static lmfs_dirlist_entry_t entries[LMFS_DIRLIST_MAX_ENTRIES];
                uint32_t count = 0;
                int found = 0;
                if (lmfs_readdir(&lfs38, LMFS_ROOT_INODE_NUM, entries,
                                  LMFS_DIRLIST_MAX_ENTRIES, &count) == 0) {
                    for (uint32_t i = 0; i < count; i++) {
                        if (strcmp(entries[i].name, new_name) == 0 &&
                            entries[i].inode_num == new_inode_num) {
                            found = 1;
                            break;
                        }
                    }
                }
                printf("[lmfs] root now has %u entries after create()\n",
                       count);
                if (found) {
                    console_write_color(
                        "new file visible in root directory listing\n",
                        COLOR_SUCCESS);
                } else {
                    console_write_color(
                        "[lmfs] FAIL: new file not found in "
                        "lmfs_readdir(root) output\n", COLOR_ERROR);
                }

                uint32_t dup_inode_num = 0;
                if (lmfs_create(&lfs38, LMFS_ROOT_INODE_NUM, new_name,
                                 &dup_inode_num) != 0) {
                    console_write_color(
                        "duplicate create correctly rejected\n",
                        COLOR_SUCCESS);
                } else {
                    console_write_color(
                        "[lmfs] FAIL: creating the same name twice was "
                        "not rejected\n", COLOR_ERROR);
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY39 (第二月 · 第6周 · Day 39 of the 5.0 filesystem roadmap):
     *   "实现 write()，支持直接块与间接块。" 验收："写入 10KB 文件并验证。"
     *
     *   10240 字节（10KB）故意超过 8 个直接块能装的 8192 字节，逼着
     *   lmfs_write() 真的走一级间接块这条路径，不能只靠直接块蒙混
     *   过关——见 doc/lmfs.txt 第二十节"为什么验收标准特意选 10KB"。
     *
     *   先用 lmfs_create() 幂等地拿到一个叫 "kday39.txt" 的文件（跟
     *   DAY38 同样的"镜像可能是上次开机留下的，已存在就复用"处理，
     *   见那段注释），再调用 lmfs_write() 写入一段内容可核对的
     *   10240 字节缓冲区（字节值按 i mod 256 循环，方便肉眼核对、
     *   也方便逐字节比较）。验证四层：
     *     1. lmfs_write() 返回 0。
     *     2. 读回的 inode：file_size==10240、8 个直接块指针全部
     *        非零、indirect1 也非零（间接块路径真的被用上了）。
     *     3. 绕开还不存在的 lmfs_read()（Day 40 才会有），测试代码
     *        自己顺着 direct[]/indirect1 的索引表把全部 10240 字节
     *        读回来，逐字节核对内容完全一致——这是"验证"二字真正
     *        落地的地方，不是只看 lmfs_write() 的返回值。
     *     4. 超过 LMFS_MAX_FILE_SIZE 的写入请求被正确拒绝（边界
     *        检查，不只测"刚好够用"这一种情况）。
     * ------------------------------------------------------------------ */
    console_write_color(
        "[lmfs] DAY39: lmfs_write() against LBA 100000 (10KB, direct+"
        "indirect1)...\n", COLOR_INFO);
    {
        static lmfs_fs_t lfs39;
        if (lmfs_mount(disk, LMFS_FIXTURE_LBA, &lfs39) != 0) {
            console_write_color(
                "[lmfs] FAIL: re-mount for DAY39 test unexpectedly failed\n",
                COLOR_ERROR);
        } else {
            const char *fname = "kday39.txt";
            uint32_t target_inode = 0;
            int have_inode = 0;

            static lmfs_dirlist_entry_t
                pre_entries[LMFS_DIRLIST_MAX_ENTRIES];
            uint32_t pre_count = 0;
            if (lmfs_readdir(&lfs39, LMFS_ROOT_INODE_NUM, pre_entries,
                              LMFS_DIRLIST_MAX_ENTRIES, &pre_count) == 0) {
                for (uint32_t i = 0; i < pre_count; i++) {
                    if (strcmp(pre_entries[i].name, fname) == 0) {
                        target_inode = pre_entries[i].inode_num;
                        have_inode = 1;
                        break;
                    }
                }
            }
            if (!have_inode) {
                if (lmfs_create(&lfs39, LMFS_ROOT_INODE_NUM, fname,
                                 &target_inode) == 0) {
                    have_inode = 1;
                }
            }

            if (!have_inode) {
                console_write_color(
                    "[lmfs] FAIL: couldn't get/create kday39.txt\n",
                    COLOR_ERROR);
            } else {
#define LMFS_TEST39_SIZE 10240u
                static uint8_t pattern[LMFS_TEST39_SIZE];
                for (uint32_t i = 0; i < LMFS_TEST39_SIZE; i++)
                    pattern[i] = (uint8_t)(i & 0xFF);

                if (lmfs_write(&lfs39, target_inode, pattern,
                                LMFS_TEST39_SIZE) != 0) {
                    console_write_color(
                        "[lmfs] FAIL: lmfs_write() unexpectedly "
                        "returned -1\n", COLOR_ERROR);
                } else {
                    console_write_color("lmfs_write() returned 0\n",
                                         COLOR_SUCCESS);

                    static lmfs_inode_t ino;
                    int shape_ok = 0;
                    if (lmfs_read_inode(&lfs39, target_inode, &ino) == 0) {
                        int direct_all_set = 1;
                        for (int d = 0; d < 8; d++)
                            if (ino.direct[d] == 0)
                                direct_all_set = 0;
                        shape_ok = (ino.file_size == LMFS_TEST39_SIZE &&
                                    direct_all_set &&
                                    ino.indirect1 != 0);
                        printf("[lmfs] inode: file_size=%u direct_all_"
                               "set=%d indirect1=%u\n",
                               ino.file_size, direct_all_set,
                               ino.indirect1);
                    }
                    if (shape_ok) {
                        console_write_color(
                            "inode shape correct (file_size=10240, 8 "
                            "direct blocks + indirect1 used)\n",
                            COLOR_SUCCESS);
                    } else {
                        console_write_color(
                            "[lmfs] FAIL: inode shape after write is "
                            "wrong\n", COLOR_ERROR);
                    }

                    /* 绕开 lmfs_read()（还不存在），自己顺着
                     * direct[]/indirect1 把数据读回来核对。 */
                    static uint8_t readback[LMFS_TEST39_SIZE];
                    static uint8_t blk[LMFS_BLOCK_SIZE];
                    static uint32_t indirect_tbl[LMFS_INDIRECT1_PTRS];
                    int read_ok = 1;
                    uint32_t total_blocks =
                        (LMFS_TEST39_SIZE + LMFS_BLOCK_SIZE - 1)
                        / LMFS_BLOCK_SIZE;

                    if (shape_ok) {
                        uint32_t indirect_lba = lfs39.mount_lba
                            + lfs39.data_area_lba
                            + ino.indirect1 * LMFS_SECTORS_PER_BLOCK;
                        if (disk->read(disk, indirect_lba,
                                       LMFS_SECTORS_PER_BLOCK,
                                       indirect_tbl) != 0)
                            read_ok = 0;

                        for (uint32_t i = 0; i < total_blocks && read_ok;
                             i++) {
                            uint32_t block_num = (i < 8)
                                ? ino.direct[i]
                                : indirect_tbl[i - 8];
                            uint32_t block_lba = lfs39.mount_lba
                                + lfs39.data_area_lba
                                + block_num * LMFS_SECTORS_PER_BLOCK;
                            if (disk->read(disk, block_lba,
                                           LMFS_SECTORS_PER_BLOCK,
                                           blk) != 0) {
                                read_ok = 0;
                                break;
                            }
                            uint32_t offset = i * LMFS_BLOCK_SIZE;
                            uint32_t chunk = LMFS_TEST39_SIZE - offset;
                            if (chunk > LMFS_BLOCK_SIZE)
                                chunk = LMFS_BLOCK_SIZE;
                            memcpy(readback + offset, blk, chunk);
                        }
                    } else {
                        read_ok = 0;
                    }

                    int content_ok = read_ok &&
                        (memcmp(readback, pattern, LMFS_TEST39_SIZE) == 0);
                    if (content_ok) {
                        console_write_color(
                            "all 10240 bytes read back byte-for-byte "
                            "identical to what was written\n",
                            COLOR_SUCCESS);
                    } else {
                        console_write_color(
                            "[lmfs] FAIL: readback content does not "
                            "match what was written\n", COLOR_ERROR);
                    }
                }

                /* 边界检查：超过 LMFS_MAX_FILE_SIZE 应该被拒绝。 */
                if (lmfs_write(&lfs39, target_inode, pattern,
                                LMFS_MAX_FILE_SIZE + 1) != 0) {
                    console_write_color(
                        "write exceeding LMFS_MAX_FILE_SIZE correctly "
                        "rejected\n", COLOR_SUCCESS);
                } else {
                    console_write_color(
                        "[lmfs] FAIL: a write larger than "
                        "LMFS_MAX_FILE_SIZE was not rejected\n",
                        COLOR_ERROR);
                }
#undef LMFS_TEST39_SIZE
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY40 (第二月 · 第6周 · Day 40 of the 5.0 filesystem roadmap):
     *   "实现 read()。" 验收："read bigfile.txt 正常。"
     *
     *   跟 DAY38/39 同样的"幂等拿到一个文件"套路，这次目标是
     *   "bigfile.txt"（故意用计划原文点名的文件名——Day 41 之后这个
     *   名字在 Shell 里真的会被 `read bigfile.txt` 用到，见本段测试
     *   之后紧跟着的 tests/day40_test_read_shell.py）。内容不是
     *   Day 39 那种肉眼看不出意义的字节斜坡，换成一句可读短语反复
     *   拼接到 10240 字节——同样会跨过 8 个直接块、用上一级间接块，
     *   但这次是人也能看懂的内容，方便等下交互式 Shell 里
     *   `read bigfile.txt` 的输出直接拿肉眼核对。
     *
     *   验证分两层：
     *     1. 直接调用新写好的 lmfs_read()（不再像 Day 39 那样自己
     *        手动走 direct[]/indirect1——Day 39 当时没有 lmfs_read()
     *        可用，只能现场手写验证逻辑；今天这个函数真的存在了，
     *        用它本身就是对它的验证）。分三次、不对齐块边界的 chunk
     *        （3000+3000+4240=10240）依次读，模拟真实调用方"分批读、
     *        每次从上次结束的 offset 继续"的用法，不是只测一次性
     *        整个读完——跟 FAT 那边 Day 20 强调"file->pos 能正确
     *        跨多次调用续读"是同一个关注点。三次读回的内容拼起来，
     *        逐字节跟原始内容比对。
     *     2. offset 等于 file_size 时调用 lmfs_read()，确认返回 0
     *        （EOF，不是 -1 错误）——验收 lmfs.h 文档里特别强调的
     *        这条返回值约定。
     * ------------------------------------------------------------------ */
    console_write_color(
        "[lmfs] DAY40: lmfs_read() against LBA 100000 (bigfile.txt, "
        "chunked)...\n", COLOR_INFO);
    {
        static lmfs_fs_t lfs40;
        if (lmfs_mount(disk, LMFS_FIXTURE_LBA, &lfs40) != 0) {
            console_write_color(
                "[lmfs] FAIL: re-mount for DAY40 test unexpectedly failed\n",
                COLOR_ERROR);
        } else {
            const char *fname = "bigfile.txt";
            uint32_t target_inode = 0;
            int have_inode = 0;

            static lmfs_dirlist_entry_t
                pre_entries[LMFS_DIRLIST_MAX_ENTRIES];
            uint32_t pre_count = 0;
            if (lmfs_readdir(&lfs40, LMFS_ROOT_INODE_NUM, pre_entries,
                              LMFS_DIRLIST_MAX_ENTRIES, &pre_count) == 0) {
                for (uint32_t i = 0; i < pre_count; i++) {
                    if (strcmp(pre_entries[i].name, fname) == 0) {
                        target_inode = pre_entries[i].inode_num;
                        have_inode = 1;
                        break;
                    }
                }
            }
            if (!have_inode) {
                if (lmfs_create(&lfs40, LMFS_ROOT_INODE_NUM, fname,
                                 &target_inode) == 0)
                    have_inode = 1;
            }

            if (!have_inode) {
                console_write_color(
                    "[lmfs] FAIL: couldn't get/create bigfile.txt\n",
                    COLOR_ERROR);
            } else {
#define LMFS_TEST40_SIZE 10240u
                static char content[LMFS_TEST40_SIZE];
                static const char phrase[] =
                    "The quick brown fox jumps over the lazy dog. ";
                uint32_t plen = (uint32_t)strlen(phrase);
                for (uint32_t i = 0; i < LMFS_TEST40_SIZE; i++)
                    content[i] = phrase[i % plen];

                if (lmfs_write(&lfs40, target_inode, content,
                                LMFS_TEST40_SIZE) != 0) {
                    console_write_color(
                        "[lmfs] FAIL: lmfs_write() for bigfile.txt "
                        "unexpectedly returned -1\n", COLOR_ERROR);
                } else {
                    console_write_color(
                        "bigfile.txt written (10240 bytes)\n",
                        COLOR_SUCCESS);

                    static char readback[LMFS_TEST40_SIZE];
                    uint32_t chunk_sizes[3] = { 3000, 3000, 4240 };
                    uint32_t offset = 0;
                    int chunks_ok = 1;

                    for (int c = 0; c < 3; c++) {
                        int n = lmfs_read(&lfs40, target_inode,
                                           readback + offset,
                                           chunk_sizes[c], offset);
                        printf("[lmfs] lmfs_read() chunk #%d at "
                               "offset=%u requested=%u -> %d\n",
                               c + 1, offset, chunk_sizes[c], n);
                        if (n != (int)chunk_sizes[c]) {
                            chunks_ok = 0;
                            break;
                        }
                        offset += (uint32_t)n;
                    }

                    if (chunks_ok &&
                        memcmp(readback, content, LMFS_TEST40_SIZE) == 0) {
                        console_write_color(
                            "chunked reads reassembled byte-for-byte "
                            "match\n", COLOR_SUCCESS);
                    } else {
                        console_write_color(
                            "[lmfs] FAIL: chunked readback content does "
                            "not match what was written\n", COLOR_ERROR);
                    }

                    int eof_n = lmfs_read(&lfs40, target_inode, readback,
                                           16, LMFS_TEST40_SIZE);
                    if (eof_n == 0) {
                        console_write_color(
                            "read at EOF correctly returned 0 (not an "
                            "error)\n", COLOR_SUCCESS);
                    } else {
                        printf("[lmfs] FAIL: lmfs_read() at EOF "
                               "returned %d, expected 0\n", eof_n);
                    }
                }
#undef LMFS_TEST40_SIZE
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY41 (第二月 · 第6周 · Day 41 of the 5.0 filesystem roadmap):
     *   "实现 delete() 与空间回收。" 验收："rm 后空间恢复。"
     *
     *   跟 DAY38/39/40 同样的"幂等拿到一个文件"套路，目标是
     *   "kday41.txt"。先写入 10KB+（复用跟 DAY39 一样的字节斜坡
     *   内容），确保这个文件同时用上了直接块和一级间接块——这样
     *   lmfs_delete() 内部的 free_inode_data() 才会真正把"direct[8]
     *   + indirect1 索引表里的每一项 + indirect1 索引块自己"这条
     *   完整路径走一遍，不是只测最简单的"只有 1 个直接块"的情况。
     *
     *   验证的核心是"空间恢复"要落到磁盘字节层面，不能只看函数
     *   返回值：删除前，先独立读磁盘记录下这个文件占用的全部 11
     *   个数据块（8 个直接 + 2 个间接引用的数据块 + 1 个间接索引块
     *   自己）在数据块位图里的 bit、以及这个 inode 在 inode 位图里
     *   的 bit——全部应该是 1（已分配）。调用 lmfs_delete() 之后，
     *   再独立读一遍同样这些 bit，全部应该变回 0（空闲）——这才是
     *   "rm 后空间恢复"真正的证据。另外验证：
     *     - lmfs_readdir(root) 和 lmfs_lookup() 都再也找不到这个
     *       文件了（目录项真的被清掉了，不是只释放了空间）。
     *     - 删除根目录（对 "." 调用 lmfs_delete()，它指向 inode 0，
     *       type=DIR）被正确拒绝——这个项目没有删除目录的概念。
     * ------------------------------------------------------------------ */
    console_write_color(
        "[lmfs] DAY41: lmfs_delete() against LBA 100000...\n",
        COLOR_INFO);
    {
        static lmfs_fs_t lfs41;
        if (lmfs_mount(disk, LMFS_FIXTURE_LBA, &lfs41) != 0) {
            console_write_color(
                "[lmfs] FAIL: re-mount for DAY41 test unexpectedly failed\n",
                COLOR_ERROR);
        } else {
            const char *fname = "kday41.txt";
            uint32_t target_inode = 0;
            int have_inode = 0;

            static lmfs_dirlist_entry_t
                pre_entries[LMFS_DIRLIST_MAX_ENTRIES];
            uint32_t pre_count = 0;
            if (lmfs_readdir(&lfs41, LMFS_ROOT_INODE_NUM, pre_entries,
                              LMFS_DIRLIST_MAX_ENTRIES, &pre_count) == 0) {
                for (uint32_t i = 0; i < pre_count; i++) {
                    if (strcmp(pre_entries[i].name, fname) == 0) {
                        target_inode = pre_entries[i].inode_num;
                        have_inode = 1;
                        break;
                    }
                }
            }
            if (!have_inode) {
                if (lmfs_create(&lfs41, LMFS_ROOT_INODE_NUM, fname,
                                 &target_inode) == 0)
                    have_inode = 1;
            }

            if (!have_inode) {
                console_write_color(
                    "[lmfs] FAIL: couldn't get/create kday41.txt\n",
                    COLOR_ERROR);
            } else {
#define LMFS_TEST41_SIZE 10240u
                static uint8_t payload[LMFS_TEST41_SIZE];
                for (uint32_t i = 0; i < LMFS_TEST41_SIZE; i++)
                    payload[i] = (uint8_t)(i & 0xFF);

                if (lmfs_write(&lfs41, target_inode, payload,
                                LMFS_TEST41_SIZE) != 0) {
                    console_write_color(
                        "[lmfs] FAIL: lmfs_write() for kday41.txt "
                        "unexpectedly returned -1\n", COLOR_ERROR);
                } else {
                    static lmfs_inode_t ino;
                    static uint32_t all_blocks[11];
                    int ok = (lmfs_read_inode(&lfs41, target_inode,
                                               &ino) == 0 &&
                              ino.indirect1 != LMFS_BLOCK_NONE);
                    if (ok) {
                        for (int d = 0; d < 8; d++)
                            all_blocks[d] = ino.direct[d];

                        static uint32_t indirect_tbl[LMFS_INDIRECT1_PTRS];
                        uint32_t indirect_lba = lfs41.mount_lba
                            + lfs41.data_area_lba
                            + ino.indirect1 * LMFS_SECTORS_PER_BLOCK;
                        ok = (disk->read(disk, indirect_lba,
                                         LMFS_SECTORS_PER_BLOCK,
                                         indirect_tbl) == 0);
                        if (ok) {
                            /* 10240 字节 = 10 个块：8 个直接 + 2 个
                             * 间接引用的数据块。 */
                            all_blocks[8] = indirect_tbl[0];
                            all_blocks[9] = indirect_tbl[1];
                            all_blocks[10] = ino.indirect1;
                        }
                    }

                    if (!ok) {
                        console_write_color(
                            "[lmfs] FAIL: couldn't determine the 11 "
                            "blocks kday41.txt uses\n", COLOR_ERROR);
                    } else {
                        static uint8_t bitmap_raw[LMFS_SECTOR_SIZE];
                        uint32_t data_bitmap_lba = lfs41.mount_lba
                            + lfs41.sb.data_bitmap_lba;

                        int before_ok = 1;
                        if (disk->read(disk, data_bitmap_lba, 1,
                                       bitmap_raw) != 0) {
                            before_ok = 0;
                        } else {
                            for (int i = 0; i < 11; i++) {
                                uint32_t b = all_blocks[i];
                                int bit = (bitmap_raw[b / 8] >> (b % 8))
                                          & 1;
                                if (!bit)
                                    before_ok = 0;
                            }
                        }

                        static uint8_t inode_bitmap_raw[LMFS_SECTOR_SIZE];
                        uint32_t inode_bitmap_lba = lfs41.mount_lba
                            + lfs41.sb.inode_bitmap_lba;
                        if (disk->read(disk, inode_bitmap_lba, 1,
                                       inode_bitmap_raw) != 0) {
                            before_ok = 0;
                        } else {
                            int bit = (inode_bitmap_raw[target_inode / 8]
                                       >> (target_inode % 8)) & 1;
                            if (!bit)
                                before_ok = 0;
                        }

                        printf("[lmfs] before delete: 11 data block "
                               "bits + 1 inode bit all allocated=%d\n",
                               before_ok);

                        if (lmfs_delete(&lfs41, LMFS_ROOT_INODE_NUM,
                                         fname) != 0) {
                            console_write_color(
                                "[lmfs] FAIL: lmfs_delete() unexpectedly "
                                "returned -1\n", COLOR_ERROR);
                        } else {
                            console_write_color(
                                "lmfs_delete() returned 0\n",
                                COLOR_SUCCESS);

                            uint32_t dummy;
                            int still_visible =
                                (lmfs_lookup(&lfs41, LMFS_ROOT_INODE_NUM,
                                             fname, &dummy) == 0);
                            if (!still_visible) {
                                console_write_color(
                                    "file no longer found via "
                                    "lmfs_lookup()\n", COLOR_SUCCESS);
                            } else {
                                console_write_color(
                                    "[lmfs] FAIL: lmfs_lookup() still "
                                    "finds the deleted file\n",
                                    COLOR_ERROR);
                            }

                            int after_ok = 1;
                            if (disk->read(disk, data_bitmap_lba, 1,
                                           bitmap_raw) != 0) {
                                after_ok = 0;
                            } else {
                                for (int i = 0; i < 11; i++) {
                                    uint32_t b = all_blocks[i];
                                    int bit = (bitmap_raw[b / 8]
                                               >> (b % 8)) & 1;
                                    if (bit)
                                        after_ok = 0;
                                }
                            }
                            if (disk->read(disk, inode_bitmap_lba, 1,
                                           inode_bitmap_raw) != 0) {
                                after_ok = 0;
                            } else {
                                int bit =
                                    (inode_bitmap_raw[target_inode / 8]
                                     >> (target_inode % 8)) & 1;
                                if (bit)
                                    after_ok = 0;
                            }

                            printf("[lmfs] after delete: 11 data block "
                                   "bits + 1 inode bit all freed=%d\n",
                                   after_ok);

                            if (before_ok && after_ok) {
                                console_write_color(
                                    "space correctly reclaimed on "
                                    "disk\n", COLOR_SUCCESS);
                            } else {
                                console_write_color(
                                    "[lmfs] FAIL: space was not "
                                    "correctly reclaimed on disk\n",
                                    COLOR_ERROR);
                            }
                        }
                    }
                }
#undef LMFS_TEST41_SIZE

                /* 删除目录应该被拒绝——对 "." 调用 lmfs_delete()，
                 * 它指向 inode 0（根目录自己，type=DIR）。 */
                if (lmfs_delete(&lfs41, LMFS_ROOT_INODE_NUM, ".") != 0) {
                    console_write_color(
                        "deleting a directory correctly rejected\n",
                        COLOR_SUCCESS);
                } else {
                    console_write_color(
                        "[lmfs] FAIL: lmfs_delete() should have "
                        "rejected deleting a directory\n", COLOR_ERROR);
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * DAY42 (第二月 · 第6周 · Day 42 of the 5.0 filesystem roadmap):
     *   "多级目录与路径解析。" 验收："可以创建并访问多级目录。"
     *
     *   跟 DAY38~41 同样的"幂等"套路（镜像可能是上次开机留下的），
     *   目标是三层嵌套目录 kday42/sub/deep 加里面的 leaf.txt。验证
     *   "创建"和"访问"两半，而且都落到磁盘字节上，不只看返回值：
     *
     *   创建：lmfs_mkdir() 连建三层；每个新目录必须自带 "." 和 ".."
     *     （"."->自己，".."->父目录——用 lmfs_lookup() 读回来核对，
     *     它们是真实目录项，不是解析器里的魔法）。leaf.txt 建在最深层，
     *     写入一句话。
     *   访问：lmfs_resolve_path() 走完整路径找到 leaf.txt；".." 一路
     *     爬回根（根的 ".." 还是根）；"/kday42//sub/./deep/" 这种带
     *     重复 '/'、"." 和结尾 '/' 的写法解析成同一个目录；读回 leaf.txt
     *     的内容和写入的逐字节一致。
     *   拒绝：不存在的中间目录、把文件当目录用（"leaf.txt/x"）、重名
     *     mkdir、保留名 "."、在文件下面 mkdir——全部必须失败。
     *   lmfs_resolve_parent()：把 "kday42/sub/deep/new.txt" 拆成
     *     (deep 的 inode, "new.txt")；裸名字用起点；"/x" 用根。
     *
     *   输出刻意压缩到几行（VGA 文本控制台没有回滚，见
     *   tests/lmfs_test_helpers.py 的说明）。
     * ------------------------------------------------------------------ */
    console_write_color(
        "[lmfs] DAY42: lmfs_mkdir()/lmfs_resolve_path() against LBA "
        "100000...\n", COLOR_INFO);
    {
        static lmfs_fs_t lfs42;
        if (lmfs_mount(disk, LMFS_FIXTURE_LBA, &lfs42) != 0) {
            console_write_color(
                "[lmfs] FAIL: re-mount for DAY42 test unexpectedly failed\n",
                COLOR_ERROR);
        } else {
            int total42 = 0, pass42 = 0;
#define CHK42(cond, label) do { total42++; if (cond) pass42++; \
    else printf("[lmfs] FAIL: DAY42 %s\n", (label)); } while (0)

            uint32_t d1 = 0, d2 = 0, d3 = 0, leaf = 0, r = 0;
            int made = day42_mkdir_idem(&lfs42, LMFS_ROOT_INODE_NUM,
                                         "kday42", &d1)
                    && day42_mkdir_idem(&lfs42, d1, "sub", &d2)
                    && day42_mkdir_idem(&lfs42, d2, "deep", &d3);
            CHK42(made, "mkdir chain kday42/sub/deep");
            printf("[lmfs] mkdir chain kday42/sub/deep -> inodes "
                   "%u/%u/%u\n", d1, d2, d3);

            if (made) {
                /* "." and ".." are real entries pointing where they should */
                CHK42(lmfs_lookup(&lfs42, d3, ".", &r) == 0 && r == d3,
                      "deep/. == deep");
                CHK42(lmfs_lookup(&lfs42, d3, "..", &r) == 0 && r == d2,
                      "deep/.. == sub");
                CHK42(lmfs_lookup(&lfs42, d2, "..", &r) == 0 && r == d1,
                      "sub/.. == kday42");
                CHK42(lmfs_lookup(&lfs42, d1, "..", &r) == 0 &&
                      r == LMFS_ROOT_INODE_NUM, "kday42/.. == root");

                /* leaf.txt at the deepest level, content written + read back */
                static const char msg[] =
                    "Hello from a nested directory!\n";
                uint32_t msg_len = (uint32_t)sizeof(msg) - 1;
                int have_leaf = (lmfs_lookup(&lfs42, d3, "leaf.txt",
                                              &leaf) == 0);
                if (!have_leaf)
                    have_leaf = (lmfs_create(&lfs42, d3, "leaf.txt",
                                              &leaf) == 0);
                CHK42(have_leaf && lmfs_write(&lfs42, leaf, msg,
                                               msg_len) == 0,
                      "create+write leaf.txt");

                CHK42(lmfs_resolve_path(&lfs42, LMFS_ROOT_INODE_NUM,
                          "kday42/sub/deep/leaf.txt", &r) == 0 && r == leaf,
                      "resolve kday42/sub/deep/leaf.txt");
                CHK42(lmfs_resolve_path(&lfs42, d3, "..", &r) == 0 &&
                      r == d2, "resolve deep + ..");
                CHK42(lmfs_resolve_path(&lfs42, d3, "../../..", &r) == 0 &&
                      r == LMFS_ROOT_INODE_NUM, "resolve deep + ../../..");
                CHK42(lmfs_resolve_path(&lfs42, d3, "/..", &r) == 0 &&
                      r == LMFS_ROOT_INODE_NUM, "absolute /.. stays at root");
                CHK42(lmfs_resolve_path(&lfs42, LMFS_ROOT_INODE_NUM,
                          "/kday42//sub/./deep/", &r) == 0 && r == d3,
                      "resolve /kday42//sub/./deep/");
                CHK42(lmfs_resolve_path(&lfs42, d1, "", &r) == 0 && r == d1,
                      "empty path == start");

                CHK42(lmfs_resolve_path(&lfs42, LMFS_ROOT_INODE_NUM,
                          "kday42/nosuch/deep", &r) != 0,
                      "missing middle dir rejected");
                CHK42(lmfs_resolve_path(&lfs42, LMFS_ROOT_INODE_NUM,
                          "kday42/sub/deep/leaf.txt/x", &r) != 0,
                      "file used as dir rejected");
                CHK42(lmfs_mkdir(&lfs42, d1, "sub", &r) != 0,
                      "duplicate mkdir rejected");
                CHK42(lmfs_mkdir(&lfs42, d1, ".", &r) != 0 &&
                      lmfs_mkdir(&lfs42, d1, "..", &r) != 0,
                      "reserved names . and .. rejected");
                CHK42(lmfs_mkdir(&lfs42, leaf, "x", &r) != 0,
                      "mkdir under a file rejected");

                uint32_t par = 0;
                char nm[LMFS_NAME_MAX + 1];
                CHK42(lmfs_resolve_parent(&lfs42, LMFS_ROOT_INODE_NUM,
                          "kday42/sub/deep/new.txt", &par, nm) == 0 &&
                      par == d3 && strcmp(nm, "new.txt") == 0,
                      "resolve_parent nested");
                CHK42(lmfs_resolve_parent(&lfs42, d2, "bare", &par, nm) == 0
                      && par == d2 && strcmp(nm, "bare") == 0,
                      "resolve_parent bare name uses start");
                CHK42(lmfs_resolve_parent(&lfs42, d3, "/top", &par, nm) == 0
                      && par == LMFS_ROOT_INODE_NUM &&
                      strcmp(nm, "top") == 0,
                      "resolve_parent /top uses root");
                CHK42(lmfs_resolve_parent(&lfs42, d3, "/", &par, nm) != 0,
                      "resolve_parent of '/' rejected");

                /* readdir of the middle directory: ".", "..", "deep" */
                static lmfs_dirlist_entry_t ents[LMFS_DIRLIST_MAX_ENTRIES];
                uint32_t n = 0;
                int saw_dot = 0, saw_dotdot = 0, saw_deep = 0;
                if (lmfs_readdir(&lfs42, d2, ents,
                                  LMFS_DIRLIST_MAX_ENTRIES, &n) == 0) {
                    for (uint32_t i = 0; i < n; i++) {
                        if (strcmp(ents[i].name, ".") == 0) saw_dot = 1;
                        if (strcmp(ents[i].name, "..") == 0) saw_dotdot = 1;
                        if (strcmp(ents[i].name, "deep") == 0 &&
                            ents[i].type == LMFS_TYPE_DIR) saw_deep = 1;
                    }
                }
                CHK42(saw_dot && saw_dotdot && saw_deep,
                      "readdir(sub) lists . .. deep/");

                /* leaf.txt content, byte for byte */
                static char back[64];
                int got = lmfs_read(&lfs42, leaf, back, sizeof(back), 0);
                CHK42(got == (int)msg_len && memcmp(back, msg, msg_len) == 0,
                      "leaf.txt content read back");
            }

            printf("[lmfs] DAY42 checks: %d/%d passed\n", pass42, total42);
            if (pass42 == total42) {
                console_write_color(
                    "multi-level directories: mkdir + path resolution verified\n",
                    COLOR_SUCCESS);
            } else {
                console_write_color(
                    "[lmfs] FAIL: DAY42 multi-level directory checks "
                    "failed (see FAIL lines above)\n", COLOR_ERROR);
            }
#undef CHK42
        }
    }

    /* ------------------------------------------------------------------
     * !! DAY2 临时改动 (禁用位置 2/2) — 月末 (Day 29) 前必须保持注释 !!
     * 与文件顶部说明及 [6/7] 步骤处的禁用位置 1/2 是同一组改动，必须
     * 同时恢复，不能只改一处（只改一处会导致未定义行为或黑屏）。
     * 这里正常应该调用 gui_init() 接管屏幕并进入 GUI 主循环：
     *
     *   gui_init();
     *   for (;;) {
     *       gui_refresh();
     *       asm volatile("pause");
     *   }
     *
     * 第一个月 (Day 1~28) 保持注释，只用文本控制台 + hlt 循环。
     * ------------------------------------------------------------------ */

    /* ------------------------------------------------------------------
     * DAY43 (Week 7 PRE task — "从此正式变成GUI模式！！你必须要实际调试！！"):
     * the two DAY2 switches above/below are restored TOGETHER, as this
     * file's top-of-file "非常重要" block demands:
     *
     *   (1) vga_init_graphics()  — switches the display to BGA 800x450.
     *       It used to sit at step [6/7] near the top of kmain(). It is
     *       now called HERE, after every boot self-test above has
     *       printed to the text console, for one reason: the moment the
     *       display is in graphics mode, console_*() output (0xB8000) is
     *       invisible. Running the DAY2..DAY42 self-tests first keeps
     *       their output readable on the text console (and on the QEMU
     *       monitor dump the tests/day*.py scripts use) and only then
     *       hands the screen to the desktop. It is still one call, made
     *       once, immediately before gui_init() — the "never one without
     *       the other" rule is about that pairing, not about the line
     *       number.
     *   (2) gui_init() + the gui_refresh() loop — the desktop.
     *
     * LUMA_TEXT_MODE (build.sh: `LUMAOS_TEXT=1 ./build.sh`) keeps the old
     * behaviour — stay on the text console and run text_shell_run() — as
     * the headless debugging build the roadmap explicitly allows ("如果你
     * 文本模式测试、调试方便，可以沿用纯bash调试"). Both builds share every
     * line of shell/filesystem code; they differ only in this tail.
     * ------------------------------------------------------------------ */
live_boot:               /* DAY61: live CD mode joins here (see the boot_is_cd() block above) */
#ifdef LUMA_TEXT_MODE
    console_write_color("\n[boot] Text-mode debug console ready.\n", COLOR_INFO);

    /* DAY14: hand off to the text-mode shell loop instead of an idle
     * "for(;;) hlt;" — see this file's DAY14 header comment (above
     * text_shell_run()'s definition) for the full rationale. This
     * call never returns; text_shell_run() itself loops forever. */
    text_shell_run();
#else
    console_write_color("\n[boot] Self-tests done, starting the desktop (BGA 800x450).\n",
                        COLOR_INFO);
    vga_init_graphics();
    gui_init();
    if (boot_is_cd()) live_apply_config();   /* DAY63: theme / resolution / icons from CD0/ETC/LUMAOS.CFG */
    for (;;) {
        gui_refresh();
        asm volatile("pause");
    }
#endif
}