/* kernel/shell/cmd/fatcache.c - DAY65: `fatcache` / `fatbench` —— FAT 缓存的开关、统计和对比测试
 *
 * 计划.md Day 65：“FAT Cache：缓存 FAT 表，加快顺序读取。”
 *
 * 原理（实现见 kernel/fs/fat/fat_cache.c、fat_vfs.c 里 fat_read_chain）
 * ----------------------------------------------------------------
 * FAT 文件系统里，“这个文件的下一个簇是几号”存在 FAT 表里。老代码每读一个簇，就要读一次 FAT 表
 * 所在的扇区（512 字节能放 256 项 FAT16 / 128 项 FAT32 / 341 项 FAT12），连续读 100 个簇，
 * 同一个 FAT 扇区被读 100 次，而且每次读盘都是一次 ATA 命令（几十微秒~毫秒）。三层优化：
 *   1. 扇区缓存（fat_cache.c）：32 槽直接映射、写透；FAT 表的读 99% 命中内存。
 *   2. 链游标（fat_vfs.c）：fat_read() 记住“文件第 k 个簇是几号”，顺序读下一块时不用从头走链。
 *   3. 连续簇合并读：簇链里相邻簇号连续时，一条 ATA 命令读多个簇（数据区本身的优化）。
 * 三层的正确性底线：写盘后缓存必须作废/更新（block.c 的写钩子 + 代数 g_fat_gen），否则读到旧链。
 *
 * 命令
 * ----
 *   fatcache            显示缓存开关和统计
 *   fatcache on|off     开/关缓存（off = 直通设备，用来量对照数据）
 *   fatcache reset      统计清零
 *   fatbench            在当前 FAT 卷上建一个 ~48KB 的文件，用 512 字节小块顺序读两遍：
 *                       缓存关 / 缓存开，比较设备读命令数、沿链步数和 CPU 周期，并逐字节校验
 *                       两遍读到的内容一致且等于写入的内容。输出最后一行 "fatbench: PASS/FAIL"。
 */
#define LUMA_KERNEL_SIDE 1
#include "../../core/kernel.h"
#include "../../core/errno.h"
#include "../../fs/vfs/vfs_ops.h"
#include "../../fs/fat/fat_vfs.h"
#include "../../fs/fat/fat_cache.h"
#include "../shell.h"
#include "cmdfmt.h"

#define BENCH_BYTES 48000u
#define BENCH_CHUNK 512u

static uint8_t g_bw[BENCH_BYTES];
static uint8_t g_br[BENCH_BYTES];

static void show_stats(terminal_t *t)
{
    tprintf(t, TERM_COL_DEFAULT, "FAT cache: %s, %u slots of 512 bytes (direct-mapped, write-through)\n",
            g_fatc_enabled ? "ON" : "OFF", FATC_SLOTS);
    tprintf(t, TERM_COL_DEFAULT, "  FAT sector lookups %u: hits %u, misses %u, device reads %u, device writes %u, invalidations %u\n",
            g_fatc_stats.lookups, g_fatc_stats.hits, g_fatc_stats.misses, g_fatc_stats.dev_reads,
            g_fatc_stats.dev_writes, g_fatc_stats.invalidations);
    tprintf(t, TERM_COL_DEFAULT, "  chain: %u steps walked, %u cursor hits; data reads: %u device commands\n",
            g_fat_read_chain_steps, g_fat_cursor_hits, g_fat_read_dev_calls);
}

void cmd_fatcache(terminal_t *t, int argc, char **argv)
{
    if (argc >= 2) {
        if (strcmp(argv[1], "on") == 0)       { g_fatc_enabled = 1; fatc_invalidate_all(); }
        else if (strcmp(argv[1], "off") == 0) { g_fatc_enabled = 0; fatc_invalidate_all(); }
        else if (strcmp(argv[1], "reset") == 0) { fatc_stats_reset(); g_fat_read_dev_calls = g_fat_read_chain_steps = g_fat_cursor_hits = 0; }
        else { terminal_puts(t, "usage: fatcache [on|off|reset]\n", TERM_COL_ERROR); return; }
    }
    show_stats(t);
}

typedef struct {
    uint32_t cycles, fat_dev_reads, fat_lookups, chain_steps, data_cmds, cursor_hits;
    int      ok;
} run_t;

/* 用 512 字节小块把整个文件顺序读一遍到 g_br（每块一次 fat_read，等价于老 `read` 命令的小块读取模式） */
static run_t bench_read(const char *path, uint32_t want)
{
    run_t r = {0};
    vfs_node_t node; file_t f;
    fatc_stats_reset();
    g_fat_read_dev_calls = g_fat_read_chain_steps = g_fat_cursor_hits = 0;
    fatc_invalidate_all();                    /* 冷启动：两次比较的起点一致 */
    uint32_t t0 = cmd_rdtsc_lo();
    if (fat_open(path, &node) != FAT_OK || !node.fs || node.fs->open(&node, &f) != 0) return r;
    uint32_t got = 0;
    while (got < want) {
        uint32_t n = want - got < BENCH_CHUNK ? want - got : BENCH_CHUNK;
        int k = node.fs->read(&f, g_br + got, n);
        if (k <= 0) break;
        got += (uint32_t)k;
    }
    if (node.fs->close) node.fs->close(&f);
    r.cycles = cmd_rdtsc_lo() - t0;
    r.fat_dev_reads = g_fatc_stats.dev_reads;
    r.fat_lookups = g_fatc_stats.lookups;
    r.chain_steps = g_fat_read_chain_steps;
    r.data_cmds = g_fat_read_dev_calls;
    r.cursor_hits = g_fat_cursor_hits;
    r.ok = (got == want);
    return r;
}

void cmd_fatbench(terminal_t *t, int argc, char **argv)
{
    (void)argc; (void)argv;
    static char dir[64], path[96];
    vfs_statfs_t st;
    if (shell_resolve_path(".", dir, sizeof(dir)) != 0 || vfs_ops_statfs(dir, &st) < 0) {
        terminal_puts(t, "fatbench: no current filesystem\n", TERM_COL_ERROR); return;
    }
    if (strncmp(st.fstype, "fat", 3) != 0) {
        tprintf(t, TERM_COL_ERROR, "fatbench: %s is %s, not FAT; cd IDE0/IDE1/IDE2 first\n", dir, st.fstype); return;
    }
    cmd_join(path, sizeof(path), dir, "FATBENCH.TMP");
    int was = g_fatc_enabled;
    (void)vfs_ops_delete(path);
    int r = vfs_ops_create(path);
    if (r < 0) { tprintf(t, TERM_COL_ERROR, "fatbench: create: %s\n", luma_strerror(r)); return; }
    for (uint32_t i = 0; i < BENCH_BYTES; i++) g_bw[i] = (uint8_t)((i * 7u + (i >> 9) * 13u) ^ 0xA5u);
    r = vfs_ops_write(path, g_bw, BENCH_BYTES);
    if (r != (int)BENCH_BYTES) {
        tprintf(t, TERM_COL_ERROR, "fatbench: write: %s\n", r < 0 ? luma_strerror(r) : "short write");
        (void)vfs_ops_delete(path); return;
    }
    tprintf(t, TERM_COL_INFO, "fatbench: %s (%s, cluster %u bytes), %u bytes read in %u-byte pieces\n",
            path, st.fstype, st.block_size, BENCH_BYTES, BENCH_CHUNK);

    g_fatc_enabled = 0;
    run_t off = bench_read(path, BENCH_BYTES);
    int same_off = off.ok && memcmp(g_br, g_bw, BENCH_BYTES) == 0;
    memset(g_br, 0, BENCH_BYTES);
    g_fatc_enabled = 1;
    run_t on = bench_read(path, BENCH_BYTES);
    int same_on = on.ok && memcmp(g_br, g_bw, BENCH_BYTES) == 0;
    g_fatc_enabled = was;
    fatc_invalidate_all();

    tprintf(t, TERM_COL_DEFAULT, "  cache OFF: %6u Kcycles, FAT-sector lookups %4u (device reads %4u), chain steps %4u, data cmds %3u\n",
            off.cycles >> 10, off.fat_lookups, off.fat_dev_reads, off.chain_steps, off.data_cmds);
    tprintf(t, TERM_COL_DEFAULT, "  cache ON : %6u Kcycles, FAT-sector lookups %4u (device reads %4u), chain steps %4u, data cmds %3u, cursor hits %u\n",
            on.cycles >> 10, on.fat_lookups, on.fat_dev_reads, on.chain_steps, on.data_cmds, on.cursor_hits);
    (void)vfs_ops_delete(path);

    uint32_t errors = 0;
    if (!same_off) { errors++; terminal_puts(t, "  [FAIL] cache-off read differs from what was written\n", TERM_COL_ERROR); }
    if (!same_on)  { errors++; terminal_puts(t, "  [FAIL] cache-on read differs from what was written\n", TERM_COL_ERROR); }
    if (on.fat_dev_reads > off.fat_dev_reads) { errors++; terminal_puts(t, "  [FAIL] cache made FAT device reads worse\n", TERM_COL_ERROR); }
    if (on.fat_dev_reads > 4 + off.fat_dev_reads / 8) { errors++; terminal_puts(t, "  [FAIL] cache did not cut FAT device reads by 8x\n", TERM_COL_ERROR); }
    if (on.chain_steps > off.chain_steps) { errors++; terminal_puts(t, "  [FAIL] chain steps increased\n", TERM_COL_ERROR); }
    if (errors == 0)
        tprintf(t, TERM_COL_SUCCESS, "fatbench: PASS (FAT device reads %u -> %u, chain steps %u -> %u, contents identical)\n",
                off.fat_dev_reads, on.fat_dev_reads, off.chain_steps, on.chain_steps);
    else
        tprintf(t, TERM_COL_ERROR, "fatbench: FAIL (%u errors)\n", errors);
}
