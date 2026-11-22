/* kernel/shell/cmd/fsck.c - DAY67: `fsck` / `soak` / `gfxstat`
 *
 * 计划.md Day 67：“已知 Bug 清零。长时间测试不崩溃，优化 GUI 使使用更流畅。”
 *
 *  fsck [卷]   一致性检查（只读）。LMFS：lmfs_fsck() 逐 inode 走一遍，和两张位图交叉对账
 *              （泄漏的块、被引用却标空闲的块、重复引用、越界指针、孤儿 inode、计数器不符）。
 *              FAT：读根目录，每个文件的簇链沿 FAT 走到结束标记，长度必须 >= 文件大小所需簇数，
 *              并且空闲簇数 <= 总簇数。
 *  soak [n]    长时间稳定性测试：n 轮（默认 50，最多 2000）“建文件 -> 写 -> 读回校验 -> 覆盖 -> 删除”，
 *              文件大小每轮不同；每 10 轮核对一次空闲块数没有漂移（LMFS 允许目录块增长），最后 fsck。
 *              目的是把“跑很久会不会坏”从人工点鼠标变成一条命令：Day 64 的 fstress 测的是“一次做对”，
 *              soak 测的是“反复做一千次，空间不泄漏、内容不串、系统不崩”。
 *  gfxstat     GUI 刷新统计：自上次 `gfxstat reset` 以来的帧数、写进显存的行数、平均每帧写了多少行。
 *              Day 67 的显示优化——BGA 后端也只写变化的行（以前每帧整屏 360KB），SVGA 后端本来就是这样。
 *              文本模式下没有帧，显示 0。
 */
#define LUMA_KERNEL_SIDE 1
#include "../../core/kernel.h"
#include "../../core/errno.h"
#include "../../drivers/drivers.h"
#include "../../fs/vfs/vfs_ops.h"
#include "../../fs/lmfs/lmfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../shell.h"
#include "cmdfmt.h"

static int lmfs_report_bad(const lmfs_fsck_report_t *r)
{
    return (int)(r->leaked_blocks | r->missing_blocks | r->dup_blocks | r->bad_ptrs |
                 r->orphan_inodes | r->bad_inodes | r->free_mismatch);
}

static void print_lmfs_report(terminal_t *t, const lmfs_fsck_report_t *r)
{
    tprintf(t, TERM_COL_DEFAULT, "  inodes in use %u (%u files, %u dirs); blocks in bitmap %u, referenced %u\n",
            r->inodes_used, r->files, r->dirs, r->blocks_used, r->blocks_referenced);
    tprintf(t, TERM_COL_DEFAULT, "  leaked %u, missing %u, duplicate %u, bad pointers %u, orphan inodes %u, bad inodes %u, counter mismatch %u\n",
            r->leaked_blocks, r->missing_blocks, r->dup_blocks, r->bad_ptrs, r->orphan_inodes, r->bad_inodes, r->free_mismatch);
}

/* 返回问题个数（0 = 干净），打印细节 */
static uint32_t fsck_volume(terminal_t *t, const char *slot, int quiet)
{
    filesystem_t *fs; const char *sub;
    if (vfs_ops_split(slot, &fs, &sub) < 0) { tprintf(t, TERM_COL_ERROR, "fsck: %s: no such volume\n", slot); return 1; }
    vfs_statfs_t st;
    if (vfs_ops_statfs(slot, &st) < 0) { tprintf(t, TERM_COL_ERROR, "fsck: %s: statfs failed\n", slot); return 1; }
    if (strcmp(st.fstype, "lmfs") == 0) {
        lmfs_fs_t *l = (lmfs_fs_t *)fs->fs_private;
        lmfs_fsck_report_t rep;
        int r = lmfs_fsck(l, &rep);
        if (r != 0) { tprintf(t, TERM_COL_ERROR, "fsck: %s: could not run (%d)\n", slot, r); return 1; }
        uint32_t bad = rep.leaked_blocks + rep.missing_blocks + rep.dup_blocks + rep.bad_ptrs +
                       rep.orphan_inodes + rep.bad_inodes + rep.free_mismatch;
        if (!quiet) { tprintf(t, TERM_COL_INFO, "fsck %s (lmfs):\n", slot); print_lmfs_report(t, &rep); }
        return bad;
    }
    if (strncmp(st.fstype, "fat", 3) == 0) {
        uint32_t problems = 0, files = 0, idx = 0;
        vfs_node_t dir = (vfs_root && vfs_root->fs == fs) ? *vfs_root : *fs->root, child;
        while (dir.fs->readdir(&dir, idx++, &child) == 0) {
            if (child.type == VFS_TYPE_DIR) continue;
            files++;
            if (child.size == 0) continue;
            /* 读一遍：簇链断裂 / 比声明的短，fat_read 会提前返回或报错 */
            file_t f; uint32_t got = 0; static uint8_t buf[2048];
            if (child.fs->open(&child, &f) != 0) { problems++; continue; }
            for (;;) {
                int n = child.fs->read(&f, buf, sizeof(buf));
                if (n <= 0) break;
                got += (uint32_t)n;
            }
            if (child.fs->close) child.fs->close(&f);
            if (got != child.size) {
                problems++;
                tprintf(t, TERM_COL_ERROR, "  %s: chain gives %u bytes, directory says %u\n", child.name, got, child.size);
            }
        }
        if (st.free_blocks > st.total_blocks) { problems++; terminal_puts(t, "  free clusters > total clusters\n", TERM_COL_ERROR); }
        if (!quiet) tprintf(t, TERM_COL_INFO, "fsck %s (%s): %u files read through their cluster chains, %u free of %u clusters\n",
                            slot, st.fstype, files, st.free_blocks, st.total_blocks);
        return problems;
    }
    if (!quiet) tprintf(t, TERM_COL_INFO, "fsck %s (%s): read-only medium, nothing to check\n", slot, st.fstype);
    return 0;
}

void cmd_fsck(terminal_t *t, int argc, char **argv)
{
    static char p[64];
    char slot[16]; uint32_t n = 0;
    if (shell_resolve_path(argc >= 2 ? argv[1] : ".", p, sizeof(p)) != 0) { terminal_puts(t, "fsck: bad path\n", TERM_COL_ERROR); return; }
    while (p[n] && p[n] != '/' && n < sizeof(slot) - 1) { slot[n] = p[n]; n++; }
    slot[n] = '\0';
    uint32_t bad = fsck_volume(t, slot, 0);
    if (bad == 0) tprintf(t, TERM_COL_SUCCESS, "fsck: %s is clean\n", slot);
    else          tprintf(t, TERM_COL_ERROR, "fsck: %s has %u problem(s)\n", slot, bad);
}

/* ---------------- soak ---------------- */
#define SK_BUF 24576u
static uint8_t g_sw[SK_BUF], g_sr[SK_BUF];

static void sk_fill(uint8_t *b, uint32_t seed, uint32_t n)
{
    uint32_t x = seed * 2654435761u + 12345u;
    for (uint32_t i = 0; i < n; i++) { x = x * 1103515245u + 12345u; b[i] = (uint8_t)(x >> 16); }
}

void cmd_soak(terminal_t *t, int argc, char **argv)
{
    static char dir[64], path[96];
    char nm[16];
    uint32_t rounds = 50;
    if (argc >= 2 && (cmd_parse_uint(argv[1], &rounds) != 0 || rounds < 1 || rounds > 2000)) {
        terminal_puts(t, "usage: soak [rounds]   (1..2000, default 50)\n", TERM_COL_ERROR); return;
    }
    if (shell_resolve_path(".", dir, sizeof(dir)) != 0) { terminal_puts(t, "soak: no current filesystem\n", TERM_COL_ERROR); return; }
    vfs_statfs_t before, now;
    int r = vfs_ops_statfs(dir, &before);
    if (r < 0) { tprintf(t, TERM_COL_ERROR, "soak: %s\n", luma_strerror(r)); return; }
    if (before.read_only) { tprintf(t, TERM_COL_ERROR, "soak: %s is read-only\n", dir); return; }
    /* LMFS 的空闲块计数在开机自检之后可能过期（见 lmfsbench 的说明），先对账 */
    int is_lmfs = strcmp(before.fstype, "lmfs") == 0;
    filesystem_t *fsx = 0; const char *sub;
    if (is_lmfs && vfs_ops_split(dir, &fsx, &sub) == 0) {
        lmfs_fs_t *l = (lmfs_fs_t *)fsx->fs_private;
        (void)lmfs_count_free(l); l->blk_hint = 0; l->ino_hint = 0;
        (void)vfs_ops_statfs(dir, &before);
    }
    uint32_t base_bad = 0;
    { char slot[16]; uint32_t k = 0; while (dir[k] && dir[k] != '/' && k < 15) { slot[k] = dir[k]; k++; } slot[k] = 0; base_bad = fsck_volume(t, slot, 1); }
    tprintf(t, TERM_COL_INFO, "soak: %u rounds on %s (%s)\n", rounds, dir, before.fstype);

    uint32_t errors = 0, kb = 0, files = 0;
    const uint32_t NF = 6;
    uint32_t max_lost = 0;
    for (uint32_t rd = 0; rd < rounds && errors < 8; rd++) {
        uint32_t szs[6];
        for (uint32_t i = 0; i < NF; i++) {
            szs[i] = (rd * 977u + i * 3571u + (i == 5 ? 20000u : 0u)) % (i == 5 ? SK_BUF : 6000u);
            sprintf(nm, "SK%u.TMP", i); cmd_join(path, sizeof(path), dir, nm);
            (void)vfs_ops_delete(path);
            if ((r = vfs_ops_create(path)) < 0) { errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] round %u create %s: %s\n", rd, nm, luma_strerror(r)); break; }
            sk_fill(g_sw, rd * 8 + i, szs[i]);
            if ((r = vfs_ops_write(path, g_sw, szs[i])) != (int)szs[i]) { errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] round %u write %s: %d\n", rd, nm, r); break; }
            files++; kb += szs[i] / 1024;
        }
        for (uint32_t i = 0; i < NF && errors < 8; i++) {
            sprintf(nm, "SK%u.TMP", i); cmd_join(path, sizeof(path), dir, nm);
            sk_fill(g_sw, rd * 8 + i, szs[i]);
            int got = vfs_ops_read(path, g_sr, SK_BUF);
            if (got != (int)szs[i] || memcmp(g_sr, g_sw, szs[i]) != 0) {
                errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] round %u verify %s (got %d, want %u)\n", rd, nm, got, szs[i]);
            }
        }
        /* 覆盖：每个文件换一个新大小/内容，再校验——测“覆盖写不残留旧尾巴 / 不泄漏旧块” */
        for (uint32_t i = 0; i < NF && errors < 8; i += 2) {
            sprintf(nm, "SK%u.TMP", i); cmd_join(path, sizeof(path), dir, nm);
            uint32_t n2 = (szs[i] * 5u + 77u) % 9000u;
            sk_fill(g_sw, rd * 8 + i + 4000, n2);
            if (vfs_ops_write(path, g_sw, n2) != (int)n2 || vfs_ops_read(path, g_sr, SK_BUF) != (int)n2 || memcmp(g_sr, g_sw, n2) != 0) {
                errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] round %u overwrite %s\n", rd, nm);
            }
        }
        for (uint32_t i = 0; i < NF; i++) {
            sprintf(nm, "SK%u.TMP", i); cmd_join(path, sizeof(path), dir, nm);
            if ((r = vfs_ops_delete(path)) < 0) { errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] round %u delete %s: %s\n", rd, nm, luma_strerror(r)); }
        }
        if ((rd % 10) == 9 || rd + 1 == rounds) {
            if (vfs_ops_statfs(dir, &now) == 0) {
                int lost = (int)before.free_blocks - (int)now.free_blocks;
                /* 目录项只会让 LMFS 目录多长一块（<=1），FAT 必须严格相等 */
                uint32_t allow = is_lmfs ? 2u : 0u;
                if (lost < 0 || (uint32_t)lost > allow) {
                    errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] round %u: free blocks %u -> %u (drift)\n", rd, before.free_blocks, now.free_blocks);
                }
                if (lost > 0 && (uint32_t)lost > max_lost) max_lost = (uint32_t)lost;
            }
            tprintf(t, TERM_COL_DEFAULT, "  round %u/%u ok so far (%u files, %u KB)\n", rd + 1, rounds, files, kb);
        }
    }
    uint32_t bad = 0;
    { char slot[16]; uint32_t k = 0; while (dir[k] && dir[k] != '/' && k < 15) { slot[k] = dir[k]; k++; } slot[k] = 0; bad = fsck_volume(t, slot, 1); }
    if (bad > base_bad) { errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] fsck: %u problems (%u before the run)\n", bad, base_bad); }
    if (errors == 0) tprintf(t, TERM_COL_SUCCESS, "soak: PASS (%u rounds, %u files, %u KB, no leaks, fsck no worse)\n", rounds, files, kb);
    else             tprintf(t, TERM_COL_ERROR, "soak: FAIL (%u errors)\n", errors);
}

/* ---------------- gfxstat ---------------- */
void cmd_gfxstat(terminal_t *t, int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) { vga_gfx_stats_reset(); terminal_puts(t, "gfxstat: counters reset\n", TERM_COL_INFO); return; }
    uint32_t f, w, tot;
    vga_gfx_stats(&f, &w, &tot);
    uint32_t avg = f ? w / f : 0;
    uint32_t pct = tot ? (w * 100u) / tot : 0;
    tprintf(t, TERM_COL_DEFAULT, "gfxstat: %u frames, %u rows written (%u%% of %u full-screen rows), avg %u rows/frame\n", f, w, pct, tot, avg);
}
