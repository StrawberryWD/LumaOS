/* kernel/shell/cmd/errno.c - DAY70: `errno` —— 查统一错误码表
 *
 * 计划.md Day 70：“统一错误码：所有函数返回统一错误码。”
 *
 * 全系统的约定（详见 kernel/core/errno.h）：返回 -LUMA_Exxx；成功是 0 或非负结果。
 *   errno            列出整张表（号、符号名、文字）
 *   errno 2 / -2     按号查
 *   errno ENOENT     按名字查（不区分大小写，开头的 E 可省略：`errno noent`）
 *   errno -t         自检：表里每个号 luma_strerror/luma_errname/luma_errno_from_name 往返一致，
 *                    FAT 老式错误码折算正确；最后一行 "errno: PASS"。
 */
#define LUMA_KERNEL_SIDE 1
#include "../../core/kernel.h"
#include "../../core/errno.h"
#include "../../fs/fat/fat_vfs.h"
#include "../shell.h"
#include "cmdfmt.h"

static void line(terminal_t *t, int e)
{
    char nm[16]; uint32_t k = 0;
    const char *n = luma_errname(e);
    while (n[k] && k < 14) { nm[k] = n[k]; k++; }
    while (k < 13) nm[k++] = ' ';
    nm[k] = '\0';
    tprintf(t, TERM_COL_DEFAULT, "%4d  %s %s\n", e, nm, luma_strerror(e));
}

static int selftest(terminal_t *t)
{
    int bad = 0, n = 0;
    for (int i = 0; luma_errno_nth(i) >= 0; i++) {
        int e = luma_errno_nth(i);
        n++;
        if (luma_errno_from_name(luma_errname(e)) != e) { bad++; tprintf(t, TERM_COL_ERROR, "  [FAIL] name round trip %d\n", e); }
        if (strcmp(luma_strerror(e), luma_strerror(-e)) != 0) { bad++; tprintf(t, TERM_COL_ERROR, "  [FAIL] sign %d\n", e); }
        if (strcmp(luma_strerror(e), "Unknown error") == 0) { bad++; tprintf(t, TERM_COL_ERROR, "  [FAIL] %d has no text\n", e); }
        if (i > 0 && luma_errno_nth(i) <= luma_errno_nth(i - 1)) { bad++; tprintf(t, TERM_COL_ERROR, "  [FAIL] table not ascending at %d\n", i); }
    }
    if (luma_errno_from_name("noent") != LUMA_ENOENT) { bad++; terminal_puts(t, "  [FAIL] 'noent'\n", TERM_COL_ERROR); }
    if (luma_errno_from_name("EBOGUS") != -1)          { bad++; terminal_puts(t, "  [FAIL] 'EBOGUS'\n", TERM_COL_ERROR); }
    if (strcmp(luma_strerror(0), "Success") != 0)      { bad++; terminal_puts(t, "  [FAIL] strerror(0)\n", TERM_COL_ERROR); }
    if (strcmp(luma_strerror(9999), "Unknown error") != 0) { bad++; terminal_puts(t, "  [FAIL] strerror(9999)\n", TERM_COL_ERROR); }
    if (luma_err_from_fat(FAT_ERR_NOT_FOUND) != -LUMA_ENOENT) { bad++; terminal_puts(t, "  [FAIL] fat NOT_FOUND\n", TERM_COL_ERROR); }
    if (luma_err_from_fat(FAT_ERR_IO) != -LUMA_EIO)           { bad++; terminal_puts(t, "  [FAIL] fat IO\n", TERM_COL_ERROR); }
    if (luma_err_from_fat(FAT_ERR_CHAIN_BROKEN) != -LUMA_EUCLEAN) { bad++; terminal_puts(t, "  [FAIL] fat CHAIN\n", TERM_COL_ERROR); }
    if (luma_err_from_fat(0) != 0)                            { bad++; terminal_puts(t, "  [FAIL] fat ok\n", TERM_COL_ERROR); }
    if (bad == 0) tprintf(t, TERM_COL_SUCCESS, "errno: PASS (%d entries, names/texts/signs/FAT mapping consistent)\n", n);
    else          tprintf(t, TERM_COL_ERROR, "errno: FAIL (%d)\n", bad);
    return bad;
}

void cmd_errno(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        for (int i = 0; luma_errno_nth(i) >= 0; i++) line(t, luma_errno_nth(i));
        return;
    }
    if (strcmp(argv[1], "-t") == 0) { (void)selftest(t); return; }
    uint32_t v = 0;
    const char *a = argv[1];
    if (*a == '-') a++;
    if (*a >= '0' && *a <= '9') {
        if (cmd_parse_uint(a, &v) != 0 || v > 100000) { terminal_puts(t, "errno: bad number\n", TERM_COL_ERROR); return; }
        if (strcmp(luma_errname((int)v), "E?") == 0) { tprintf(t, TERM_COL_DEFAULT, "%4u  (not defined) %s\n", v, luma_strerror((int)v)); return; }
        line(t, (int)v);
        return;
    }
    int e = luma_errno_from_name(argv[1]);
    if (e < 0) { tprintf(t, TERM_COL_ERROR, "errno: unknown error name '%s'\n", argv[1]); return; }
    line(t, e);
}
