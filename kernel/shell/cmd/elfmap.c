/* kernel/shell/cmd/elfmap.c
 * LumaOS 5.0 — DAY45 (Week 7, Day 45): "解析 PT_LOAD。"
 *   验收："能将程序段加载到内存。"
 *
 * 用法:  elfmap <file>     例如  elfmap IDE3/data.sw   （LMFS）
 *                                elfmap IDE0/HELLO.SW  （FAT）
 *                                cd IDE3  然后  elfmap hello.sw
 *
 * 作用：先用 Day 44 的 elf_load() 验证文件，再用 Day 45 的
 * elf_load_segments() 把每个 PT_LOAD 段真的装进内存（拷贝文件部分、清零
 * .bss、回读校验——原理见 kernel/exec/elf.h 的 DAY45 注释），然后把结果
 * 展示出来，并且“自己再查一遍”：
 *
 *   1. 装载之前，把每个段的目标区域先填成 0xA5（每段最多 256 KB）。这是
 *      调试手段，不是加载器的一部分：它让“.bss 漏清零”直接表现成
 *      a5 a5 a5……，而不是碰巧等于 0 而蒙混过关。
 *   2. 装载之后，命令自己从内存重新读一遍：
 *        - 每个段文件部分的 FNV-1a 必须等于加载器记录的值（[ok]）；
 *        - .bss 区域必须全是 0（[ok]）。
 *   3. 打印每段开头的 16 字节、.bss 开头的 8 字节、入口点处的 16 字节。
 *      宿主机测试把这些数字与 readelf/文件字节、以及 QEMU 监视器 xp 直接
 *      读出来的物理内存逐一对照。
 *
 * 本命令只读磁盘；会改写的只有 ELF 声明的、已被 Day 44 限定在用户装载区
 * (16 MB ~ 240 MB) 内的内存。它只“放好”，不执行——跳到入口是 Day 46。
 * 每行 < 80 列，文本调试版与 GUI 终端都不会折行。
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../exec/elf.h"
#include "../../exec/elf_loader.h"
#include "../../lib/string.h"
#include "../shell.h"

#define POISON_BYTE  0xA5
#define POISON_MAX   (256u * 1024u)    /* 每段最多预填多少字节 */

/* 无符号十进制（本内核 printf 的 %u 会经过 int，>= 2^31 打出负号）。 */
static void put_dec(terminal_t *t, uint32_t v, unsigned char color)
{
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) terminal_putchar(t, tmp[--n], color);
}

/* 0x 前缀 + 固定位数十六进制 */
static void put_hex(terminal_t *t, uint32_t v, int digits, unsigned char color)
{
    static const char hexd[] = "0123456789abcdef";
    terminal_puts(t, "0x", color);
    for (int i = digits - 1; i >= 0; i--)
        terminal_putchar(t, hexd[(v >> (i * 4)) & 0xF], color);
}

/* 不带 0x、固定位数十六进制（地址标签、字节） */
static void put_hex_raw(terminal_t *t, uint32_t v, int digits, unsigned char color)
{
    static const char hexd[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--)
        terminal_putchar(t, hexd[(v >> (i * 4)) & 0xF], color);
}

/* 打印内存 [addr, addr+n) 的字节："aa bb cc ..."（volatile 读，真的读内存） */
static void put_bytes(terminal_t *t, uint32_t addr, uint32_t n, unsigned char color)
{
    volatile const unsigned char *p = (volatile const unsigned char *)(uintptr_t)addr;
    for (uint32_t i = 0; i < n; i++) {
        if (i) terminal_putchar(t, ' ', color);
        put_hex_raw(t, p[i], 2, color);
    }
}

static void fail_line(terminal_t *t, const char *path, int rc)
{
    terminal_puts(t, "elfmap: ", TERM_COL_ERROR);
    terminal_puts(t, path, TERM_COL_ERROR);
    terminal_puts(t, ": ", TERM_COL_ERROR);
    terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
    terminal_newline(t);
}

/* 调试用预填：把每个非空 PT_LOAD 的目标区域开头填成 0xA5。
 * 只在 elf_load() 已通过之后调用，所以地址一定在用户装载区内、不回绕。 */
static void poison_segments(const elf_image_t *img)
{
    for (uint32_t i = 0; i < img->ehdr.e_phnum; i++) {
        const elf32_phdr_t *p = &img->phdr[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0)
            continue;
        uint32_t n = p->p_memsz < POISON_MAX ? p->p_memsz : POISON_MAX;
        volatile unsigned char *d = (volatile unsigned char *)(uintptr_t)p->p_vaddr;
        for (uint32_t k = 0; k < n; k++)
            d[k] = POISON_BYTE;
    }
}

void cmd_elfmap(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: elfmap <file>  (e.g. elfmap IDE3/data.sw)\n",
                      TERM_COL_ERROR);
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "elfmap: no mount-slot given and no "
                         "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }

    /* 静态：elf_image_t 与统计结构合计约 450 字节，不放内核栈。 */
    static elf_image_t      img;
    static elf_load_stats_t st;

    /* ---- 前半场（Day 44）：打开 + 验证 ---- */
    int rc = elf_load(path_buf, &img);
    if (rc == ELF_ERR_OPEN || rc == ELF_ERR_IS_DIR || rc == ELF_ERR_IO ||
        rc == ELF_ERR_NULL) {
        fail_line(t, path_buf, rc);
        return;
    }
    terminal_puts(t, path_buf, TERM_COL_INFO);
    terminal_puts(t, ": ", TERM_COL_INFO);
    put_dec(t, img.file_size, TERM_COL_INFO);
    terminal_puts(t, " bytes\n", TERM_COL_INFO);
    if (rc != ELF_OK) {
        terminal_puts(t, "elfmap: FAIL - ", TERM_COL_ERROR);
        terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
        terminal_newline(t);
        /* 验证没通过 -> 预填和装载都还没开始，内存一个字节也没动。 */
        terminal_puts(t, "elfmap: image NOT loaded (memory untouched)\n",
                      TERM_COL_ERROR);
        return;
    }

    /* ---- 后半场（Day 45）：预填 -> 装入内存 ---- */
    poison_segments(&img);
    terminal_puts(t, "Pre-filled with 0xA5, loading ", TERM_COL_DEFAULT);
    put_dec(t, img.n_load, TERM_COL_DEFAULT);
    terminal_puts(t, " PT_LOAD segment(s):\n", TERM_COL_DEFAULT);

    rc = elf_load_segments(path_buf, &img, &st);
    if (rc != ELF_OK) {
        terminal_puts(t, "elfmap: FAIL - ", TERM_COL_ERROR);
        terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
        terminal_newline(t);
        if (rc == ELF_ERR_VERIFY) {
            terminal_puts(t, "  at ", TERM_COL_ERROR);
            put_hex(t, st.fail_addr, 8, TERM_COL_ERROR);
            terminal_puts(t, ": wrote ", TERM_COL_ERROR);
            put_hex(t, st.fail_want, 2, TERM_COL_ERROR);
            terminal_puts(t, ", read back ", TERM_COL_ERROR);
            put_hex(t, st.fail_got, 2, TERM_COL_ERROR);
            terminal_newline(t);
        }
        terminal_puts(t, "elfmap: image NOT loaded (do not run it)\n",
                      TERM_COL_ERROR);
        return;
    }

    /* ---- 命令自己再查一遍，并逐段展示 ---- */
    int all_ok = 1;
    for (uint32_t i = 0; i < img.ehdr.e_phnum; i++) {
        const elf32_phdr_t *p = &img.phdr[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0)
            continue;

        /* 从内存重算文件部分的 FNV-1a，与加载器记录的比较。 */
        volatile const unsigned char *m =
            (volatile const unsigned char *)(uintptr_t)p->p_vaddr;
        uint32_t h = ELF_FNV_SEED;
        for (uint32_t k = 0; k < p->p_filesz; k++) {
            unsigned char b = m[k];
            h = elf_fnv1a(h, &b, 1);
        }
        int sum_ok = (h == st.seg_sum[i]);

        /* .bss 必须全是 0。 */
        int bss_ok = 1;
        for (uint32_t k = p->p_filesz; k < p->p_memsz; k++) {
            if (m[k] != 0) { bss_ok = 0; break; }
        }
        if (!sum_ok || !bss_ok) all_ok = 0;

        terminal_puts(t, "  #", TERM_COL_INFO);
        put_dec(t, i, TERM_COL_INFO);
        terminal_putchar(t, ' ', TERM_COL_INFO);
        put_hex(t, p->p_vaddr, 8, TERM_COL_INFO);
        terminal_putchar(t, ' ', TERM_COL_INFO);
        terminal_putchar(t, (p->p_flags & PF_R) ? 'R' : ' ', TERM_COL_INFO);
        terminal_putchar(t, (p->p_flags & PF_W) ? 'W' : ' ', TERM_COL_INFO);
        terminal_putchar(t, (p->p_flags & PF_X) ? 'E' : ' ', TERM_COL_INFO);
        terminal_puts(t, " copy ", TERM_COL_DEFAULT);
        put_hex(t, p->p_filesz, 8, TERM_COL_DEFAULT);
        terminal_puts(t, " zero ", TERM_COL_DEFAULT);
        put_hex(t, p->p_memsz - p->p_filesz, 8, TERM_COL_DEFAULT);
        terminal_puts(t, " sum ", TERM_COL_DEFAULT);
        put_hex(t, h, 8, TERM_COL_DEFAULT);
        if (sum_ok && bss_ok)
            terminal_puts(t, " [ok]\n", TERM_COL_SUCCESS);
        else
            terminal_puts(t, sum_ok ? " [BSS NOT ZERO]\n" : " [SUM MISMATCH]\n",
                          TERM_COL_ERROR);

        /* 段开头最多 16 字节（读回的内存内容，不是文件内容）。 */
        if (p->p_filesz > 0) {
            uint32_t n = p->p_filesz < 16 ? p->p_filesz : 16;
            terminal_puts(t, "     ", TERM_COL_DEFAULT);
            put_hex_raw(t, p->p_vaddr, 8, TERM_COL_DEFAULT);
            terminal_puts(t, ": ", TERM_COL_DEFAULT);
            put_bytes(t, p->p_vaddr, n, TERM_COL_DEFAULT);
            terminal_newline(t);
        }
        /* .bss 开头最多 8 字节。 */
        if (p->p_memsz > p->p_filesz) {
            uint32_t z = p->p_memsz - p->p_filesz;
            uint32_t n = z < 8 ? z : 8;
            terminal_puts(t, "     bss ", TERM_COL_DEFAULT);
            put_hex_raw(t, p->p_vaddr + p->p_filesz, 8, TERM_COL_DEFAULT);
            terminal_puts(t, ": ", TERM_COL_DEFAULT);
            put_bytes(t, p->p_vaddr + p->p_filesz, n, TERM_COL_DEFAULT);
            terminal_newline(t);
        }
    }

    /* 入口点处的 16 字节（Day 46 的 elf_run() 将从这里开始执行）。
     * Day 44 已保证入口落在可执行段的“文件部分”内，但它离段尾可能不足
     * 16 字节，所以按该段剩余的文件字节数截断。 */
    {
        const elf32_phdr_t *es = &img.phdr[img.entry_seg];
        uint32_t room = es->p_filesz - (img.ehdr.e_entry - es->p_vaddr);
        uint32_t n = room < 16 ? room : 16;
        terminal_puts(t, "  entry ", TERM_COL_INFO);
        put_hex_raw(t, img.ehdr.e_entry, 8, TERM_COL_INFO);
        terminal_puts(t, ": ", TERM_COL_DEFAULT);
        put_bytes(t, img.ehdr.e_entry, n, TERM_COL_DEFAULT);
        terminal_newline(t);
    }

    if (!all_ok || !img.loaded) {
        terminal_puts(t, "elfmap: FAIL - self-check after loading failed\n",
                      TERM_COL_ERROR);
        return;
    }
    /* 统计行与结论行分开：每行都 < 80 列（数字再大也不会折行）。 */
    terminal_puts(t, "Total:    ", TERM_COL_DEFAULT);
    put_dec(t, st.segs, TERM_COL_DEFAULT);
    terminal_puts(t, " segment(s), ", TERM_COL_DEFAULT);
    put_dec(t, st.bytes_copied, TERM_COL_DEFAULT);
    terminal_puts(t, " B copied, ", TERM_COL_DEFAULT);
    put_dec(t, st.bytes_zeroed, TERM_COL_DEFAULT);
    terminal_puts(t, " B zeroed\n", TERM_COL_DEFAULT);
    terminal_puts(t, "elfmap: OK - image loaded, entry ", TERM_COL_SUCCESS);
    put_hex(t, img.ehdr.e_entry, 8, TERM_COL_SUCCESS);
    terminal_newline(t);
}
