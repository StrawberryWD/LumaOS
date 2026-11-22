/* kernel/shell/cmd/df.c - DAY68: `df` —— 各卷的容量 / 已用 / 空闲
 *
 * 计划.md Day 68：“df 命令：显示磁盘空间”。
 *
 * 数据从哪来：vfs_ops_statfs()（见 kernel/fs/vfs/vfs_ops.c）——
 *   LMFS   直接读挂载时数出来、之后随分配/释放维护的 free_blocks（Day 66），O(1)；
 *   FAT    把整张 FAT 数一遍（fat_vfs_statfs，Day 65 的扇区缓存让它很快）；
 *   ISO    光盘只读，没有“空闲”的概念，只显示类型和 ro。
 * 容量 = 块数 × 块大小，单位 KB（FAT 的块是簇，LMFS 的块是 1KB）。
 *
 * 用法：df            列出所有已挂载的卷
 *       df IDE3       只看一个卷（也可写 `df .`，表示当前目录所在的卷）
 */
#define LUMA_KERNEL_SIDE 1
#include "../../core/kernel.h"
#include "../../core/errno.h"
#include "../../fs/vfs/vfs_ops.h"
#include "../shell.h"
#include "cmdfmt.h"

/* 内核的 printf 没有 "%-6s"（左对齐）这种写法，遇到会把参数错位而崩溃（见 livecd.c 里同样的注释），手工补空格 */
static void padded(char *out, const char *s, uint32_t w)
{
    uint32_t k = 0;
    while (s[k] && k < w) { out[k] = s[k]; k++; }
    while (k < w) out[k++] = ' ';
    out[k] = '\0';
}

static void one(terminal_t *t, const char *slot)
{
    char c1[16], c2[16];
    vfs_statfs_t st;
    int r = vfs_ops_statfs(slot, &st);
    if (r < 0) { tprintf(t, TERM_COL_ERROR, "%s  %s\n", slot, luma_strerror(r)); return; }
    if (st.read_only && st.total_blocks == 0) {
        tprintf(t, TERM_COL_DEFAULT, "%s %s %10s %10s %10s %5s  ro\n", (padded(c1, slot, 6), c1), (padded(c2, st.fstype, 8), c2), "-", "-", "-", "-");
        return;
    }
    uint32_t kb_per = st.block_size / 1024u;
    if (kb_per == 0) kb_per = 1;
    uint32_t total = st.total_blocks * kb_per, freek = st.free_blocks * kb_per, used = total - freek;
    uint32_t pct = 0;
    if (total) {   /* 避免 64 位除法（内核不链接 libgcc）：先缩小到不会溢出的范围 */
        uint32_t u = used, tt = total;
        while (tt > 40000000u) { u >>= 1; tt >>= 1; }
        pct = (u * 100u + tt / 2) / tt;
    }
    tprintf(t, TERM_COL_DEFAULT, "%s %s %10u %10u %10u %4u%%%s\n", (padded(c1, slot, 6), c1), (padded(c2, st.fstype, 8), c2), total, used, freek, pct,
            st.read_only ? "  ro" : "");
    if (st.total_inodes)
        tprintf(t, TERM_COL_DEFAULT, "       inodes: %u used, %u free of %u\n",
                st.total_inodes - st.free_inodes, st.free_inodes, st.total_inodes);
}

void cmd_df(terminal_t *t, int argc, char **argv)
{
    terminal_puts(t, "Volume Type        Size(KB)    Used(KB)    Free(KB) Use%\n", TERM_COL_INFO);
    if (argc >= 2) {
        static char p[64];
        vfs_statfs_t probe;
        if (vfs_ops_statfs(argv[1], &probe) == 0) {       /* 直接就是卷名："RAM0" / "IDE3" / "CD0" */
            one(t, argv[1]);
            return;
        }
        if (shell_resolve_path(argv[1], p, sizeof(p)) != 0) {
            terminal_puts(t, "df: bad path\n", TERM_COL_ERROR);
            return;
        }
        { uint32_t sz; int isdir; int sr = vfs_ops_stat(p, &sz, &isdir);
          if (sr < 0 && strcmp(argv[1], ".") != 0) { tprintf(t, TERM_COL_ERROR, "df: %s: %s\n", argv[1], luma_strerror(sr)); return; } }
        char slot[8]; uint32_t n = 0;
        while (p[n] && p[n] != '/' && n < 7) { slot[n] = p[n]; n++; }
        slot[n] = '\0';
        one(t, slot);
        return;
    }
    for (uint32_t i = 0; i < vfs_mount_count; i++)
        one(t, vfs_mounts[i].name);
}
