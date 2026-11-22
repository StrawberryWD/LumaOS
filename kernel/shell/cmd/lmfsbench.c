/* kernel/shell/cmd/lmfsbench.c - DAY66: `lmfsbench` —— LMFS 位图分配器：新旧算法对照 + 正确性
 *
 * 计划.md Day 66：“LMFS 位图优化：减少 O(n) 扫描。”
 *
 * 老算法（Day 36）：每次分配都从位图第 0 字节开始逐字节找第一个 0 位；每扫一个字节就可能重读位图扇区。
 * 卷越满，前面已用的位越多，每次分配越慢——整体 O(n) 次分配 × O(n) 扫描 = O(n²)。
 *
 * 新算法（lmfs.c 的 bm_alloc）三招，磁盘格式一个字节不变：
 *   1. 空闲计数（free_blocks / free_inodes）：挂载时数一遍，之后分配/释放时加减。“满了吗”变成 O(1)，
 *      不用扫完整张位图才知道没有空位；df（Day 68）也靠它。
 *   2. 提示位置（blk_hint / ino_hint）：记住“第一个空闲位不会在这之前”。分配后 hint = 刚分配的号 + 1；
 *      释放一个比 hint 小的号时 hint 退回到它。不变式：hint ≤ 真正的第一个空闲位，所以从 hint 开始扫
 *      找到的永远是“第一个空闲位”——和老算法分配出来的号一模一样（测试验证这一点），只是不重复扫前面的已用区。
 *   3. 32 位字扫描 + 位图扇区缓存：一次比较 32 位，全 1 的字直接跳过；刚用过的位图扇区缓存在内存里，
 *      连续分配不再每次读盘。写仍然立即写回（写透），断电不丢位图。
 *
 * `lmfsbench` 做的事（在当前 LMFS 卷上，结束时把卷恢复原样）：
 *   A. 分配序列对照：按同一个“分配 / 间隔释放 / 再分配”剧本，先用老算法跑一遍记录块号序列，释放干净，
 *      再用新算法跑一遍，两个序列必须逐个相等（inode 同理）。
 *   B. 计数：free_blocks / free_inodes 在每一步与位图实际数出来的一致（lmfs_count_free 对账）。
 *   C. 速度：连续分配 N 个块再全部释放，比较位图扇区读次数与 CPU 周期。
 *   D. 收尾：lmfs_fsck 全绿。
 * 最后一行 "lmfsbench: PASS/FAIL"。
 */
#define LUMA_KERNEL_SIDE 1
#include "../../core/kernel.h"
#include "../../core/errno.h"
#include "../../fs/vfs/vfs_ops.h"
#include "../../fs/lmfs/lmfs.h"
#include "../shell.h"
#include "cmdfmt.h"

#define NB 400u
static uint32_t g_seq[2][NB + NB / 4 + 8];     /* 400 + 100 + 1 */
static uint32_t g_iseq[2][48];
static uint32_t g_ffail, g_ifail;      /* 释放失败次数（调试/报告用） */
static uint32_t g_live[NB + NB / 4 + 8];

/* 一轮剧本。返回分配到的块数（写进 seq）；全部分配的块在结束前释放。 */
static int script(lmfs_fs_t *fs, uint32_t *seq, uint32_t *iseq, uint32_t nblk, uint32_t *ninode_out)
{
    uint32_t n = 0, live = 0, k;
    /* 1) 连续分配 nblk 个块 */
    for (k = 0; k < nblk; k++) {
        uint32_t b;
        if (lmfs_alloc_block(fs, &b) != 0) return -1;
        seq[n++] = b; g_live[live++] = b;
    }
    /* 2) 释放每第 3 个（留下“洞”），记下释放顺序对后续分配的影响 */
    uint32_t keep = 0;
    for (k = 0; k < live; k++) {
        if (k % 3 == 1) { if (lmfs_free_block(fs, g_live[k]) != 0) return -1; }
        else g_live[keep++] = g_live[k];
    }
    live = keep;
    /* 3) 再分配 nblk/4 个：必须先填洞（按块号从小到大） */
    for (k = 0; k < nblk / 4; k++) {
        uint32_t b;
        if (lmfs_alloc_block(fs, &b) != 0) return -1;
        seq[n++] = b; g_live[live++] = b;
    }
    /* 4) 释放最小的块号再分配一次：hint 必须退回去 */
    uint32_t lo = g_live[0], lo_i = 0;
    for (k = 1; k < live; k++) if (g_live[k] < lo) { lo = g_live[k]; lo_i = k; }
    if (lmfs_free_block(fs, lo) != 0) return -1;
    g_live[lo_i] = g_live[--live];
    { uint32_t b; if (lmfs_alloc_block(fs, &b) != 0) return -1; seq[n++] = b; g_live[live++] = b; }
    /* 5) inode：分配 40 个、释放 1/2、再分配 */
    uint32_t ni = 0, ino[48], ilive = 0;
    for (k = 0; k < 40; k++) {
        uint32_t i;
        if (lmfs_alloc_inode(fs, 1 /* FILE */, &i) != 0) break;
        iseq[ni++] = i; ino[ilive++] = i;
    }
    for (k = 0; k < ilive; k += 2) (void)lmfs_free_inode(fs, ino[k]);
    for (k = 0; k < 6 && ni < 46; k++) {
        uint32_t i;
        if (lmfs_alloc_inode(fs, 1, &i) != 0) break;
        iseq[ni++] = i; ino[ilive++] = i;
    }
    /* 6) 全部释放（inode 里已经被释放的那些不要重复释放） */
    for (k = 0; k < live; k++) if (lmfs_free_block(fs, g_live[k]) != 0) g_ffail++;
    for (k = 0; k < ilive; k++) { if (!(k < 40 && (k % 2) == 0)) if (lmfs_free_inode(fs, ino[k]) != 0) g_ifail++; }
    *ninode_out = ni;
    return (int)n;
}

void cmd_lmfsbench(terminal_t *t, int argc, char **argv)
{
    (void)argc; (void)argv;
    static char dir[64];
    filesystem_t *fsx; const char *sub;
    if (shell_resolve_path(".", dir, sizeof(dir)) != 0 || vfs_ops_split(dir, &fsx, &sub) < 0 ||
        !fsx->name || strcmp(fsx->name, "lmfs") != 0) {
        terminal_puts(t, "lmfsbench: current volume is not LMFS; cd RAM0 / IDE3 first\n", TERM_COL_ERROR);
        return;
    }
    lmfs_fs_t *fs = (lmfs_fs_t *)fsx->fs_private;
    /* 先和磁盘对账：开机自检（Day 32~42 的 lmfs 测试）用的是它们自己的 lmfs_mount 实例，会在 VFS 挂载之后
     * 改同一块磁盘，所以 VFS 这个实例里的空闲计数 / hint 可能已经过期。重新数一遍，hint 归零（0 永远 <= 第一个空闲位）。 */
    if (lmfs_count_free(fs) != 0) { terminal_puts(t, "lmfsbench: cannot read the bitmaps\n", TERM_COL_ERROR); return; }
    fs->blk_hint = 0; fs->ino_hint = 0;
    lmfs_fsck_report_t base;
    int base_ok = (lmfs_fsck(fs, &base) == 0);   /* 开机自检可能留下过孤儿：只要求“没有比开始时更糟”，见末尾 */
    int was_fast = g_lmfs_fast_alloc;
    uint32_t errors = 0;
    uint32_t fb0 = fs->free_blocks, fi0 = fs->free_inodes;
    uint32_t nblk = NB;
    if (fb0 < nblk + 64) nblk = fb0 > 80 ? fb0 - 64 : 0;
    if (nblk < 16 || fi0 < 64) { terminal_puts(t, "lmfsbench: volume too full to benchmark\n", TERM_COL_ERROR); return; }
    tprintf(t, TERM_COL_INFO, "lmfsbench: %s, %u free blocks, %u free inodes, script of %u blocks\n", dir, fb0, fi0, nblk);

    uint32_t nseq[2] = {0, 0}, ninode[2] = {0, 0}, cyc[2] = {0, 0}, rds[2] = {0, 0}, hits[2] = {0, 0};
    for (int mode = 0; mode < 2; mode++) {            /* 0 = 老算法，1 = 新算法 */
        g_lmfs_fast_alloc = mode;
        lmfs_stats_reset(); g_ffail = g_ifail = 0;
        uint32_t t0 = cmd_rdtsc_lo();
        int n = script(fs, g_seq[mode], g_iseq[mode], nblk, &ninode[mode]);
        cyc[mode] = cmd_rdtsc_lo() - t0;
        rds[mode] = g_lmfs_stats.bm_sector_reads;
        hits[mode] = g_lmfs_stats.bm_cache_hits;
        if (n < 0) { errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] script failed in %s mode\n", mode ? "fast" : "old"); n = 0; }
        nseq[mode] = (uint32_t)n;
        /* B. 计数对账 */
        if (g_ffail || g_ifail) tprintf(t, TERM_COL_ERROR, "  [FAIL] %s: %u block frees and %u inode frees returned an error\n", mode ? "fast" : "old", g_ffail, g_ifail);
        uint32_t fb = fs->free_blocks, fi = fs->free_inodes;
        if (fb != fb0 || fi != fi0) { errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] %s: free counters %u/%u != start %u/%u\n", mode ? "fast" : "old", fb, fi, fb0, fi0); }
        if (lmfs_count_free(fs) != 0) errors++;
        if (fs->free_blocks != fb || fs->free_inodes != fi) { errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] %s: counters disagree with the bitmap\n", mode ? "fast" : "old"); }
        if (fs->free_blocks != fb0) { errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] %s: volume not restored (%u != %u)\n", mode ? "fast" : "old", fs->free_blocks, fb0); }
    }
    /* A. 序列相等 */
    uint32_t diff = 0;
    if (nseq[0] != nseq[1] || ninode[0] != ninode[1]) diff = 1;
    for (uint32_t i = 0; !diff && i < nseq[0]; i++) if (g_seq[0][i] != g_seq[1][i]) { diff = i + 1; }
    for (uint32_t i = 0; !diff && i < ninode[0]; i++) if (g_iseq[0][i] != g_iseq[1][i]) { diff = i + 1; }
    if (diff) {
        errors++;
        tprintf(t, TERM_COL_ERROR, "  [FAIL] allocation order differs between old and fast allocator (at #%u of %u/%u)\n", diff, nseq[0], nseq[1]);
        if (diff <= nseq[0] && diff <= nseq[1])
            tprintf(t, TERM_COL_ERROR, "         old block %u, fast block %u (previous: %u / %u)\n", g_seq[0][diff - 1], g_seq[1][diff - 1],
                    diff > 1 ? g_seq[0][diff - 2] : 0, diff > 1 ? g_seq[1][diff - 2] : 0);
    }
    else tprintf(t, TERM_COL_DEFAULT, "  allocation order identical: %u block numbers, %u inode numbers\n", nseq[0], ninode[0]);

    /* C. 速度 */
    tprintf(t, TERM_COL_DEFAULT, "  old : %6u Kcycles, bitmap sector reads %5u\n", cyc[0] >> 10, rds[0]);
    tprintf(t, TERM_COL_DEFAULT, "  fast: %6u Kcycles, bitmap sector reads %5u (cache hits %u)\n", cyc[1] >> 10, rds[1], hits[1]);
    if (rds[1] * 4 > rds[0]) { errors++; terminal_puts(t, "  [FAIL] fast allocator did not cut bitmap reads by 4x\n", TERM_COL_ERROR); }

    /* D. fsck */
    g_lmfs_fast_alloc = was_fast;
    lmfs_fsck_report_t rep;
    if (lmfs_fsck(fs, &rep) != 0) { errors++; terminal_puts(t, "  [FAIL] fsck could not run\n", TERM_COL_ERROR); }
    else if (rep.free_mismatch | rep.missing_blocks | rep.dup_blocks | rep.bad_ptrs | rep.bad_inodes ||
             !base_ok || rep.leaked_blocks != base.leaked_blocks || rep.orphan_inodes != base.orphan_inodes) {
        errors++;
        tprintf(t, TERM_COL_ERROR, "  [FAIL] fsck: leaked=%u missing=%u dup=%u badptr=%u orphan=%u badino=%u freemis=%u (double frees %u)\n",
                rep.leaked_blocks, rep.missing_blocks, rep.dup_blocks, rep.bad_ptrs, rep.orphan_inodes,
                rep.bad_inodes, rep.free_mismatch, g_lmfs_stats.double_frees);
    }
    if (errors == 0 && (base.leaked_blocks || base.orphan_inodes))
        tprintf(t, TERM_COL_INFO, "  (volume already had %u leaked blocks / %u orphan inodes before the run - left by the boot self-tests, unchanged by it)\n", base.leaked_blocks, base.orphan_inodes);
    if (errors == 0)
        tprintf(t, TERM_COL_SUCCESS, "lmfsbench: PASS (same allocation order, bitmap reads %u -> %u, counters exact, fsck no worse)\n", rds[0], rds[1]);
    else
        tprintf(t, TERM_COL_ERROR, "lmfsbench: FAIL (%u errors)\n", errors);
}
