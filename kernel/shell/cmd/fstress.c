/* kernel/shell/cmd/fstress.c - DAY64: `fstress [count]` —— 文件系统压力测试
 *
 * 计划.md Day 64：“FS Stress Test：批量创建/读取/写入文件，全部通过”。
 *
 * 为什么是内核里的命令，而不是 Python 往键盘里灌几百条 `create`/`write`
 * -----------------------------------------------------------------
 * 压力测试要的是“成百上千次操作 + 逐字节校验”。从 QEMU 监视器发 sendkey 一秒钟只能敲几个键，
 * 几十个文件的往返要几十分钟，而且校验只能靠肉眼比屏幕文字。命令直接调 Day 64 新增的
 * vfs_ops 层（FAT / LMFS 同一套接口，返回统一错误码），文件内容是“由文件序号 + 代数 + 位置算出来的
 * 确定性字节流”，读回来逐字节比对，几十个文件几百毫秒跑完。
 *
 * 做什么（在当前目录所在的卷上；ISO9660 光盘是只读的，会直接拒绝）
 * ----------------------------------------------------------------
 *   1. 建 N 个文件 FST000.TMP ...（8.3 名字，FAT 和 LMFS 都合法）
 *   2. 每个写入不同大小的内容（0 字节、边界附近 511/512/513、1023/1024/1025、跨直接块/间接块的大文件）
 *   3. 全部读回、stat 大小、逐字节校验
 *   4. 每 3 个文件覆盖写一次（新大小 + 新内容），再校验——测“覆盖写不泄漏旧块、不残留旧尾巴”
 *   5. 删除一半，确认被删的 stat 得到 -ENOENT，剩下的内容仍然正确——测“删除不误伤邻居”
 *   6. 目录项数 == 起始数 + 剩下的文件数
 *   7. 统一错误码抽查：重复 create -> -EEXIST，读/写/删不存在的文件 -> -ENOENT，读目录 -> -EISDIR
 *   8. 删光，空闲块数、空闲 inode 数必须回到起点；LMFS 再跑一遍 lmfs_fsck()（位图和 inode 交叉对账）
 * 最后一行：“fstress: PASS ...” 或 “fstress: FAIL (n errors)”，测试脚本只认这一行。
 *
 * 用法：fstress [count]      count 默认 24，范围 1~100；FAT 根目录/LMFS 目录放不下时报 -ENOSPC。
 */
#define LUMA_KERNEL_SIDE 1
#include "../../core/kernel.h"
#include "../../core/errno.h"
#include "../../fs/vfs/vfs_ops.h"
#include "../../fs/lmfs/lmfs.h"
#include "../shell.h"
#include "cmdfmt.h"

#define FS_MAX_FILES   100
#define FS_BUF         49152u          /* 单个文件最大字节数（48KB：LMFS 里要用到一级间接块） */

static uint8_t g_wbuf[FS_BUF];
static uint8_t g_rbuf[FS_BUF];

static const uint32_t g_sizes[] = {
    0, 1, 13, 100, 511, 512, 513, 1000, 1023, 1024, 1025, 2047, 2048, 3000,
    4096, 5000, 8191, 8192, 8193, 9000, 12000, 20000
};
#define NSIZES ((uint32_t)(sizeof(g_sizes) / sizeof(g_sizes[0])))

/* 第 i 个文件第 gen 代的大小：每 7 个文件夹一个 40000 字节的大文件（直接块 8 个装不下，用到间接块） */
static uint32_t size_of(uint32_t i, uint32_t gen)
{
    if (i % 7 == 6) return 40000u + gen * 17u;
    return g_sizes[(i * 5 + gen * 3) % NSIZES];
}

/* 文件内容：确定性伪随机字节流，同时依赖序号、代数、位置。位置里混入 (j >> 8)，
 * 所以相邻 256 字节段互不相同，写错位置/串了文件都会被发现。 */
static void fill(uint8_t *b, uint32_t i, uint32_t gen, uint32_t n)
{
    for (uint32_t j = 0; j < n; j++)
        b[j] = (uint8_t)(((i * 131u + gen * 29u + j * 7u + (j >> 8) * 3u) ^ 0x5Au) & 0xFFu);
}

typedef struct {
    terminal_t *t;
    uint32_t    errors;
    uint32_t    dir_growth;
    uint32_t    bytes_w, bytes_r;
} ctx_t;

static void fail(ctx_t *c, const char *what, const char *path, int rc)
{
    c->errors++;
    if (c->errors <= 8)
        tprintf(c->t, TERM_COL_ERROR, "  [FAIL] %s %s: %s (%s)\n", what, path,
                rc < 0 ? luma_errname(rc) : "unexpected", rc < 0 ? luma_strerror(rc) : "bad result");
}

static void name_of(char *out, uint32_t i) { sprintf(out, "FST%03u.TMP", i); }

static int verify(ctx_t *c, const char *path, uint32_t i, uint32_t gen)
{
    uint32_t want = size_of(i, gen), sz = 0; int isdir = 0;
    int r = vfs_ops_stat(path, &sz, &isdir);
    if (r < 0) { fail(c, "stat", path, r); return -1; }
    if (isdir || sz != want) { c->errors++; if (c->errors <= 8) tprintf(c->t, TERM_COL_ERROR, "  [FAIL] size %s: %u != %u\n", path, sz, want); return -1; }
    int got = vfs_ops_read(path, g_rbuf, FS_BUF);
    if (got < 0) { fail(c, "read", path, got); return -1; }
    c->bytes_r += (uint32_t)got;
    if ((uint32_t)got != want) { c->errors++; if (c->errors <= 8) tprintf(c->t, TERM_COL_ERROR, "  [FAIL] read %s: %d bytes != %u\n", path, got, want); return -1; }
    fill(g_wbuf, i, gen, want);
    for (uint32_t j = 0; j < want; j++) {
        if (g_rbuf[j] != g_wbuf[j]) {
            c->errors++;
            if (c->errors <= 8) tprintf(c->t, TERM_COL_ERROR, "  [FAIL] content %s: first difference at byte %u\n", path, j);
            return -1;
        }
    }
    return 0;
}

static int write_file(ctx_t *c, const char *path, uint32_t i, uint32_t gen)
{
    uint32_t n = size_of(i, gen);
    fill(g_wbuf, i, gen, n);
    int w = vfs_ops_write(path, g_wbuf, n);
    if (w < 0) { fail(c, "write", path, w); return -1; }
    if ((uint32_t)w != n) { fail(c, "write(short)", path, 0); return -1; }
    c->bytes_w += n;
    return 0;
}

static void phase(ctx_t *c, const char *name, uint32_t errors_before)
{
    tprintf(c->t, c->errors == errors_before ? TERM_COL_DEFAULT : TERM_COL_ERROR,
            "  %s %s\n", c->errors == errors_before ? "[ok]  " : "[FAIL]", name);
}

void cmd_fstress(terminal_t *t, int argc, char **argv)
{
    static char dir[64];
    static char path[96];
    char nm[16];
    ctx_t c = { t, 0, 0, 0, 0 };
    uint32_t count = 24;

    if (argc >= 2 && (cmd_parse_uint(argv[1], &count) != 0 || count < 1 || count > FS_MAX_FILES)) {
        terminal_puts(t, "usage: fstress [count]   (count 1..100, default 24)\n", TERM_COL_ERROR);
        return;
    }
    if (shell_resolve_path(".", dir, sizeof(dir)) != 0) {
        terminal_puts(t, "fstress: no current filesystem\n", TERM_COL_ERROR);
        return;
    }

    vfs_statfs_t before;
    int r = vfs_ops_statfs(dir, &before);
    if (r < 0) { tprintf(t, TERM_COL_ERROR, "fstress: %s: %s\n", dir, luma_strerror(r)); return; }
    if (before.read_only) {
        tprintf(t, TERM_COL_ERROR, "fstress: %s is read-only (%s); cd to a writable volume first\n", dir, before.fstype);
        return;
    }
    uint32_t base_entries = 0, dir_sz0 = 0, dir_sz1 = 0; int isd;
    (void)vfs_ops_dir_count(dir, &base_entries);
    (void)vfs_ops_stat(dir, &dir_sz0, &isd);

    tprintf(t, TERM_COL_INFO, "fstress: %u files on %s (%s, %u free blocks of %u bytes)\n",
            count, dir, before.fstype, before.free_blocks, before.block_size);
    uint32_t t0 = cmd_rdtsc_lo();
    uint32_t e;

    /* 1 create */
    e = c.errors;
    for (uint32_t i = 0; i < count; i++) {
        name_of(nm, i); cmd_join(path, sizeof(path), dir, nm);
        r = vfs_ops_create(path);
        if (r < 0) {
            if (r == -LUMA_ENOSPC && i >= 4) {
                /* 目录/卷容量到顶不是文件系统 bug：用已经建好的 i 个文件继续（FAT 根目录只有一个目录区） */
                tprintf(t, TERM_COL_INFO, "  directory full after %u files (%s) - continuing with %u\n", i, luma_strerror(r), i);
                count = i;
                break;
            }
            fail(&c, "create", path, r);
            if (r == -LUMA_EEXIST)
                terminal_puts(t, "  (leftover FSTnnn.TMP files? delete them with `rm` and run again)\n", TERM_COL_ERROR);
            if (r == -LUMA_ENOSPC) break;
        }
    }
    phase(&c, "create", e);

    /* 2 write gen 0 */
    e = c.errors;
    for (uint32_t i = 0; i < count; i++) { name_of(nm, i); cmd_join(path, sizeof(path), dir, nm); (void)write_file(&c, path, i, 0); }
    phase(&c, "write", e);

    /* 3 verify gen 0 */
    e = c.errors;
    for (uint32_t i = 0; i < count; i++) { name_of(nm, i); cmd_join(path, sizeof(path), dir, nm); (void)verify(&c, path, i, 0); }
    phase(&c, "read back + verify", e);

    /* 4 overwrite every 3rd with gen 1 */
    e = c.errors;
    for (uint32_t i = 0; i < count; i += 3) {
        name_of(nm, i); cmd_join(path, sizeof(path), dir, nm);
        if (write_file(&c, path, i, 1) == 0) (void)verify(&c, path, i, 1);
    }
    phase(&c, "overwrite + verify", e);

    /* 5 delete the odd ones; survivors intact */
    e = c.errors;
    uint32_t alive = 0;
    for (uint32_t i = 0; i < count; i++) {
        name_of(nm, i); cmd_join(path, sizeof(path), dir, nm);
        if (i % 2 == 1) {
            r = vfs_ops_delete(path);
            if (r < 0) fail(&c, "delete", path, r);
            uint32_t sz; int d;
            r = vfs_ops_stat(path, &sz, &d);
            if (r != -LUMA_ENOENT) fail(&c, "deleted file still visible:", path, r);
        } else {
            alive++;
        }
    }
    for (uint32_t i = 0; i < count; i += 2) {
        name_of(nm, i); cmd_join(path, sizeof(path), dir, nm);
        (void)verify(&c, path, i, (i % 3 == 0) ? 1 : 0);
    }
    phase(&c, "delete half + survivors intact", e);

    /* 6 directory entry count */
    e = c.errors;
    uint32_t entries = 0;
    r = vfs_ops_dir_count(dir, &entries);
    if (r < 0) fail(&c, "dir_count", dir, r);
    else if (entries != base_entries + alive) {
        c.errors++;
        tprintf(t, TERM_COL_ERROR, "  [FAIL] directory has %u entries, expected %u\n", entries, base_entries + alive);
    }
    phase(&c, "directory listing complete", e);

    /* 7 unified error codes */
    e = c.errors;
    name_of(nm, 0); cmd_join(path, sizeof(path), dir, nm);
    if (vfs_ops_create(path) != -LUMA_EEXIST) fail(&c, "create-existing should be EEXIST:", path, 0);
    static char missing[96];
    cmd_join(missing, sizeof(missing), dir, "NOFILE.TMP");
    if (vfs_ops_read(missing, g_rbuf, FS_BUF) != -LUMA_ENOENT) fail(&c, "read-missing should be ENOENT:", missing, 0);
    if (vfs_ops_write(missing, g_wbuf, 1) != -LUMA_ENOENT)     fail(&c, "write-missing should be ENOENT:", missing, 0);
    if (vfs_ops_delete(missing) != -LUMA_ENOENT)               fail(&c, "delete-missing should be ENOENT:", missing, 0);
    if (vfs_ops_read(dir, g_rbuf, FS_BUF) != -LUMA_EISDIR)     fail(&c, "read-directory should be EISDIR:", dir, 0);
    phase(&c, "error codes (EEXIST/ENOENT/EISDIR)", e);

    /* 8 cleanup + accounting */
    e = c.errors;
    for (uint32_t i = 0; i < count; i += 2) {
        name_of(nm, i); cmd_join(path, sizeof(path), dir, nm);
        r = vfs_ops_delete(path);
        if (r < 0) fail(&c, "delete", path, r);
    }
    vfs_statfs_t after;
    r = vfs_ops_statfs(dir, &after);
    if (r < 0) fail(&c, "statfs", dir, r);
    else {
        /* 目录只会长不会缩（删文件只清目录项，不还目录块）：LMFS 目录峰值条目数（起始 + count + "." ".."）
         * 每 16 项占一个 1KB 目录块，所以空闲块允许比起点少 0..峰值块数 个——这是设计，不是泄漏。
         * 真正的泄漏由下面 fsck 的 leaked_blocks / free_mismatch 逐块对账来抓，这里只做粗界。
         * FAT 没有这个现象（根目录固定区域），要求严格相等。 */
        uint32_t allow = 0;
        if (after.fstype && strcmp(after.fstype, "lmfs") == 0) allow = (base_entries + count + 2 + 15) / 16;
        (void)dir_sz1; (void)dir_sz0;
        int lost = (int)before.free_blocks - (int)after.free_blocks;
        if (lost < 0 || (uint32_t)lost > allow) {
            c.errors++;
            tprintf(t, TERM_COL_ERROR, "  [FAIL] free blocks %u -> %u (allowed loss %u; space leaked or double-freed)\n", before.free_blocks, after.free_blocks, allow);
        }
        c.dir_growth = lost > 0 ? (uint32_t)lost : 0;
    }
    if (r >= 0) {
        if (after.free_inodes != before.free_inodes) {
            c.errors++;
            tprintf(t, TERM_COL_ERROR, "  [FAIL] free inodes %u -> %u\n", before.free_inodes, after.free_inodes);
        }
    }
    entries = 0;
    (void)vfs_ops_dir_count(dir, &entries);
    if (entries != base_entries) { c.errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] %u entries left, expected %u\n", entries, base_entries); }
    if (after.fstype && strcmp(after.fstype, "lmfs") == 0) {
        filesystem_t *fs; const char *sub;
        if (vfs_ops_split(dir, &fs, &sub) == 0) {
            lmfs_fsck_report_t rep;
            int fr = lmfs_fsck((lmfs_fs_t *)fs->fs_private, &rep);
            if (fr != 0) { c.errors++; tprintf(t, TERM_COL_ERROR, "  [FAIL] fsck could not run (%d)\n", fr); }
            else if (rep.leaked_blocks | rep.missing_blocks | rep.dup_blocks | rep.bad_ptrs |
                     rep.orphan_inodes | rep.bad_inodes | rep.free_mismatch) {
                c.errors++;
                tprintf(t, TERM_COL_ERROR, "  [FAIL] fsck: leaked=%u missing=%u dup=%u badptr=%u orphan=%u badino=%u freemis=%u\n",
                        rep.leaked_blocks, rep.missing_blocks, rep.dup_blocks, rep.bad_ptrs,
                        rep.orphan_inodes, rep.bad_inodes, rep.free_mismatch);
            }
        }
    }
    phase(&c, "cleanup, free space back to baseline, fsck", e);

    uint32_t mcycles = (cmd_rdtsc_lo() - t0) >> 20;
    if (c.errors == 0)
        tprintf(t, TERM_COL_SUCCESS, "fstress: PASS (%u files, %u KB written, %u KB read, 0 errors, ~%u Mcycles)\n",
                count, c.bytes_w / 1024, c.bytes_r / 1024, mcycles);
    else
        tprintf(t, TERM_COL_ERROR, "fstress: FAIL (%u errors)\n", c.errors);
}
