/* kernel/shell/cmd/elfload.c
 * LumaOS 5.0 — DAY44 (Week 7, Day 44): "实现 elf_load()"
 *   验收："检查 Magic、打印 Entry，实现在 lmfs 上用 elf。"
 *
 * 用法:  elfload <file>     例如  elfload IDE0/HELLO.SW   （FAT）
 *                                 elfload IDE3/hello.sw   （LMFS）
 *                                 cd IDE3  然后  elfload hello.sw
 *
 * 作用：调用 kernel/exec/elf_loader.c 的 elf_load()——打开文件、验证
 * Magic/Header、读出全部 Program Header 并逐项校验（原理见 elf.h 的
 * DAY44 注释）——然后把结果打印出来：Magic、Entry、段表（readelf -l 风格）、
 * 装载范围，以及入口点落在哪个段里。
 *
 * 与 elfinfo 的分工：elfinfo 是“只看 Header 的诊断工具”，对坏文件也尽量把
 * 字段打出来；elfload 是“加载器的前半场”，只回答一个问题——这个文件能不能
 * 被装载，能就给出装载计划，不能就说明第一个原因。
 * 本命令只读：不修改磁盘，也不把任何段装入内存——装入内存是 Day 45 的
 * `elfmap` 命令（elf_load_segments()）。
 * 每行 < 80 列，文本调试版与 GUI 终端都不会折行。
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../exec/elf.h"
#include "../../exec/elf_loader.h"
#include "../../lib/string.h"
#include "../shell.h"

/* 无符号十进制（不用 %u：本内核 printf 的 %u 经过 int 中转，>= 2^31 会
 * 打出负号）。 */
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

/* 不带前缀、不补零的十六进制（用于 Align 列：0x1000） */
static void put_hex_short(terminal_t *t, uint32_t v, unsigned char color)
{
    static const char hexd[] = "0123456789abcdef";
    char tmp[8];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = hexd[v & 0xF]; v >>= 4; }
    terminal_puts(t, "0x", color);
    while (n) terminal_putchar(t, tmp[--n], color);
}

static void pad_to(terminal_t *t, int used, int width)
{
    while (used++ < width) terminal_putchar(t, ' ', TERM_COL_DEFAULT);
}

static void fail(terminal_t *t, const char *path, int rc)
{
    terminal_puts(t, "elfload: ", TERM_COL_ERROR);
    terminal_puts(t, path, TERM_COL_ERROR);
    terminal_puts(t, ": ", TERM_COL_ERROR);
    terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
    terminal_newline(t);
}

void cmd_elfload(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: elfload <file>  (e.g. elfload IDE3/hello.sw)\n",
                      TERM_COL_ERROR);
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "elfload: no mount-slot given and no "
                         "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }

    /* 静态：elf_image_t 约 400 字节，与 elfinfo 同理不放内核栈。 */
    static elf_image_t img;
    int rc = elf_load(path_buf, &img);

    if (rc == ELF_ERR_OPEN || rc == ELF_ERR_IS_DIR || rc == ELF_ERR_IO ||
        rc == ELF_ERR_NULL) {
        fail(t, path_buf, rc);
        return;
    }

    /* 走到这里，文件已打开且读过头：先报 Magic 结论（“检查 Magic”）。 */
    terminal_puts(t, path_buf, TERM_COL_INFO);
    terminal_puts(t, ": ", TERM_COL_INFO);
    put_dec(t, img.file_size, TERM_COL_INFO);
    terminal_puts(t, " bytes\n", TERM_COL_INFO);

    if (rc == ELF_ERR_TOO_SHORT || rc == ELF_ERR_BAD_MAGIC) {
        terminal_puts(t, "  Magic:   BAD - ", TERM_COL_ERROR);
        terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
        terminal_newline(t);
        terminal_puts(t, "elfload: FAIL - not an ELF file\n", TERM_COL_ERROR);
        return;
    }
    terminal_puts(t, "  Magic:   OK (7f 45 4c 46 = 0x7F 'E' 'L' 'F')\n",
                  TERM_COL_DEFAULT);

    if (rc != ELF_OK) {
        /* Header 或 Program Header 没通过：只给出第一个原因。 */
        terminal_puts(t, "elfload: FAIL - ", TERM_COL_ERROR);
        terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
        terminal_newline(t);
        return;
    }

    /* ---- 成功：打印 Entry、段表、装载计划 ---- */
    terminal_puts(t, "  Entry:   ", TERM_COL_DEFAULT);
    put_hex(t, img.ehdr.e_entry, 8, TERM_COL_SUCCESS);
    terminal_puts(t, "  (in segment #", TERM_COL_DEFAULT);
    put_dec(t, img.entry_seg, TERM_COL_DEFAULT);
    terminal_puts(t, ", +", TERM_COL_DEFAULT);
    put_hex_short(t, img.ehdr.e_entry - img.phdr[img.entry_seg].p_vaddr,
                  TERM_COL_DEFAULT);
    terminal_puts(t, ")\n", TERM_COL_DEFAULT);

    terminal_puts(t, "Program Headers (", TERM_COL_INFO);
    put_dec(t, img.ehdr.e_phnum, TERM_COL_INFO);
    terminal_puts(t, "):\n", TERM_COL_INFO);
    terminal_puts(t, "  Type       Offset     VirtAddr   FileSiz    MemSiz     Flg Align\n",
                  TERM_COL_INFO);
    for (uint32_t i = 0; i < img.ehdr.e_phnum; i++) {
        const elf32_phdr_t *p = &img.phdr[i];
        const char *tn = elf_ptype_name(p->p_type);
        terminal_puts(t, "  ", TERM_COL_DEFAULT);
        terminal_puts(t, tn, TERM_COL_DEFAULT);
        pad_to(t, (int)strlen(tn), 11);
        put_hex(t, p->p_offset, 8, TERM_COL_DEFAULT); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_hex(t, p->p_vaddr,  8, TERM_COL_DEFAULT); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_hex(t, p->p_filesz, 8, TERM_COL_DEFAULT); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_hex(t, p->p_memsz,  8, TERM_COL_DEFAULT); terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        terminal_putchar(t, (p->p_flags & PF_R) ? 'R' : ' ', TERM_COL_DEFAULT);
        terminal_putchar(t, (p->p_flags & PF_W) ? 'W' : ' ', TERM_COL_DEFAULT);
        terminal_putchar(t, (p->p_flags & PF_X) ? 'E' : ' ', TERM_COL_DEFAULT);
        terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        put_hex_short(t, p->p_align, TERM_COL_DEFAULT);
        terminal_newline(t);
    }

    terminal_puts(t, "Load:     ", TERM_COL_DEFAULT);
    put_dec(t, img.n_load, TERM_COL_DEFAULT);
    terminal_puts(t, " PT_LOAD, ", TERM_COL_DEFAULT);
    put_hex(t, img.load_lo, 8, TERM_COL_DEFAULT);
    terminal_puts(t, "..", TERM_COL_DEFAULT);
    put_hex(t, img.load_hi, 8, TERM_COL_DEFAULT);
    terminal_puts(t, ", from file ", TERM_COL_DEFAULT);
    put_dec(t, img.total_filesz, TERM_COL_DEFAULT);
    terminal_puts(t, " B, in memory ", TERM_COL_DEFAULT);
    put_dec(t, img.total_memsz, TERM_COL_DEFAULT);
    terminal_puts(t, " B\n", TERM_COL_DEFAULT);

    terminal_puts(t, "elfload: OK - valid, loadable ELF32 i386 executable\n",
                  TERM_COL_SUCCESS);
}
