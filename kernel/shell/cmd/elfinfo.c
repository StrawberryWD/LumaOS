/* kernel/shell/cmd/elfinfo.c
 * LumaOS 5.0 — DAY43 (Week 7, Day 43): "能解析 ELF Header"
 *
 * 用法:  elfinfo <file>        例如  elfinfo IDE0/HELLO.SW
 *                                    cd IDE0  然后  elfinfo hello.sw
 *
 * 作用：通过 VFS 打开文件，读出最前面的 52 字节，交给
 * kernel/exec/elf.c 的 elf32_parse_header() 解析，再用
 * elf32_check_header() 判断“LumaOS 能不能运行它”，把结果按 `readelf -h`
 * 的风格打印出来。它是 Day 44–48 的诊断工具：elf_load() / run 出问题时，
 * 先用 elfinfo 看一眼文件头对不对。
 *
 * 只读命令：不修改任何磁盘数据。路径规则与 `read` 完全相同
 * （shell_resolve_path：裸文件名 = 当前卷/当前目录；"IDE0/X" = 指定卷），
 * FAT 与 LMFS 的分派方式也沿用 read.c 的 fs->name 判断。
 * 输出每行 < 80 列，文本调试版与 GUI 终端都不会折行。
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../../fs/lmfs/lmfs_vfs.h"
#include "../../exec/elf.h"
#include "../../exec/elf_loader.h"
#include "../../lib/string.h"
#include "../shell.h"

/* 无符号十进制（不用 %u：本内核 printf 的 %u 经过 int 中转，>= 2^31 会
 * 打出负号；文件头里的偏移/入口可能是任意值，必须按无符号打印）。 */
static void put_dec(terminal_t *t, uint32_t v, unsigned char color)
{
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) terminal_putchar(t, tmp[--n], color);
}

/* 固定宽度十六进制，digits = 2 / 4 / 8，带 0x 前缀。 */
static void put_hex(terminal_t *t, uint32_t v, int digits, unsigned char color)
{
    static const char hexd[] = "0123456789abcdef";
    terminal_puts(t, "0x", color);
    for (int i = digits - 1; i >= 0; i--)
        terminal_putchar(t, hexd[(v >> (i * 4)) & 0xF], color);
}

/* "  Label:      " — 标签左对齐到固定列，值对齐好看。 */
static void label(terminal_t *t, const char *name)
{
    terminal_puts(t, "  ", TERM_COL_DEFAULT);
    terminal_puts(t, name, TERM_COL_INFO);
    int pad = 12 - (int)strlen(name);
    while (pad-- > 0) terminal_putchar(t, ' ', TERM_COL_DEFAULT);
}

static void line_dec_name(terminal_t *t, const char *name, uint32_t v,
                          const char *text)
{
    label(t, name);
    put_dec(t, v, TERM_COL_DEFAULT);
    if (text) {
        terminal_puts(t, " (", TERM_COL_DEFAULT);
        terminal_puts(t, text, TERM_COL_DEFAULT);
        terminal_putchar(t, ')', TERM_COL_DEFAULT);
    }
    terminal_newline(t);
}

static void line_hex(terminal_t *t, const char *name, uint32_t v, int digits)
{
    label(t, name);
    put_hex(t, v, digits, TERM_COL_DEFAULT);
    terminal_newline(t);
}

static void line_dec(terminal_t *t, const char *name, uint32_t v)
{
    label(t, name);
    put_dec(t, v, TERM_COL_DEFAULT);
    terminal_newline(t);
}

static void print_ident(terminal_t *t, const elf32_ehdr_t *h)
{
    static const char hexd[] = "0123456789abcdef";
    label(t, "Magic:");
    for (int i = 0; i < EI_NIDENT; i++) {
        terminal_putchar(t, hexd[h->e_ident[i] >> 4], TERM_COL_DEFAULT);
        terminal_putchar(t, hexd[h->e_ident[i] & 15], TERM_COL_DEFAULT);
        if (i != EI_NIDENT - 1) terminal_putchar(t, ' ', TERM_COL_DEFAULT);
    }
    terminal_newline(t);
}

/* 打开 path 并读出最多 want 字节到 buf；*out_size 返回文件大小，*out_got
 * 返回实际读到的字节数。成功返回 0；失败时已向终端打印原因，返回 -1。
 * DAY44：打开/读取改用 kernel/exec/elf_loader.c 的 elf_open()/
 * elf_read_at()（FAT 与 LMFS 的分派逻辑只保留一份，elf_load() 也用它）。 */
static int read_head(terminal_t *t, const char *path, void *buf, uint32_t want,
                     uint32_t *out_size, uint32_t *out_got)
{
    vfs_node_t  node;
    file_t      file;
    const char *why = "not found";
    int rc = elf_open(path, &node, &file, &why);
    if (rc == ELF_ERR_IS_DIR) {
        terminal_puts(t, "elfinfo: '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' is a directory\n", TERM_COL_ERROR);
        return -1;
    }
    if (rc != ELF_OK) {
        terminal_puts(t, "elfinfo: cannot open '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' (", TERM_COL_ERROR);
        terminal_puts(t, why, TERM_COL_ERROR);
        terminal_puts(t, ")\n", TERM_COL_ERROR);
        return -1;
    }

    *out_size = node.size;
    *out_got  = 0;
    if (node.size == 0) {               /* 0 字节文件：没有东西可读 */
        elf_close(&file);
        return 0;
    }

    int got = elf_read_at(&file, 0, buf, want);
    elf_close(&file);
    if (got < 0) {
        terminal_puts(t, "elfinfo: read error\n", TERM_COL_ERROR);
        return -1;
    }
    *out_got = (uint32_t)got;
    return 0;
}

void cmd_elfinfo(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: elfinfo <file>  (e.g. elfinfo IDE0/HELLO.SW)\n",
                      TERM_COL_ERROR);
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "elfinfo: no mount-slot given and no "
                         "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }

    /* 静态缓冲：ELF Header 只有 52 字节，多留一点余量；放静态区而不是
     * 栈上，与 read.c 的做法一致（内核栈很小，.bss 预算见 PRE-3 注释，
     * 这里总共才 ~120 字节）。 */
    static uint8_t      head[64];
    static elf32_ehdr_t eh;
    uint32_t size = 0, got = 0;

    if (read_head(t, path_buf, head, ELF32_EHDR_SIZE, &size, &got) != 0)
        return;

    terminal_puts(t, path_buf, TERM_COL_INFO);
    terminal_puts(t, ": ", TERM_COL_INFO);
    put_dec(t, size, TERM_COL_INFO);
    terminal_puts(t, " bytes\n", TERM_COL_INFO);

    int rc = elf32_parse_header(head, got, &eh);
    if (rc == ELF_ERR_TOO_SHORT || rc == ELF_ERR_BAD_MAGIC) {
        /* 连 ELF 的门都没进：不是 ELF 文件。顺便把开头 4 字节亮出来，
         * 方便对照（例如文本文件会显示 ASCII 码）。 */
        terminal_puts(t, "not an ELF file: ", TERM_COL_ERROR);
        terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
        terminal_newline(t);
        if (got > 0) {
            static const char hexd[] = "0123456789abcdef";
            terminal_puts(t, "  first bytes: ", TERM_COL_DEFAULT);
            for (uint32_t i = 0; i < got && i < 4; i++) {
                terminal_putchar(t, hexd[head[i] >> 4], TERM_COL_DEFAULT);
                terminal_putchar(t, hexd[head[i] & 15], TERM_COL_DEFAULT);
                terminal_putchar(t, ' ', TERM_COL_DEFAULT);
            }
            terminal_newline(t);
        }
        return;
    }

    terminal_puts(t, "ELF Header:\n", TERM_COL_INFO);
    print_ident(t, &eh);
    line_dec_name(t, "Class:",   eh.e_ident[EI_CLASS],   elf_class_name(eh.e_ident[EI_CLASS]));

    if (rc == ELF_ERR_NOT_32BIT) {
        /* 64 位文件：Header 布局不同，后面的字段不能按 32 位解释。 */
        terminal_puts(t, "  (not ELF32 - remaining fields not decoded)\n",
                      TERM_COL_ERROR);
        terminal_puts(t, "check: FAIL - ", TERM_COL_ERROR);
        terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
        terminal_newline(t);
        return;
    }

    line_dec_name(t, "Data:",    eh.e_ident[EI_DATA],    elf_data_name(eh.e_ident[EI_DATA]));
    if (rc == ELF_ERR_NOT_LE) {
        terminal_puts(t, "  (not little-endian - remaining fields not decoded)\n",
                      TERM_COL_ERROR);
        terminal_puts(t, "check: FAIL - ", TERM_COL_ERROR);
        terminal_puts(t, elf_strerror(rc), TERM_COL_ERROR);
        terminal_newline(t);
        return;
    }

    line_dec  (t, "Version:", eh.e_ident[EI_VERSION]);
    line_dec_name(t, "OS/ABI:", eh.e_ident[EI_OSABI], elf_osabi_name(eh.e_ident[EI_OSABI]));
    line_dec_name(t, "Type:",    eh.e_type,    elf_type_name(eh.e_type));
    line_dec_name(t, "Machine:", eh.e_machine, elf_machine_name(eh.e_machine));
    line_hex  (t, "Entry:",   eh.e_entry, 8);
    line_dec  (t, "PH off:",  eh.e_phoff);
    line_dec  (t, "PH size:", eh.e_phentsize);
    line_dec  (t, "PH num:",  eh.e_phnum);
    line_dec  (t, "SH off:",  eh.e_shoff);
    line_dec  (t, "SH size:", eh.e_shentsize);
    line_dec  (t, "SH num:",  eh.e_shnum);
    line_dec  (t, "SH strndx:", eh.e_shstrndx);
    line_hex  (t, "Flags:",   eh.e_flags, 8);
    line_dec  (t, "EH size:", eh.e_ehsize);

    int ck = elf32_check_header(&eh, size);
    if (ck == ELF_OK) {
        terminal_puts(t, "check: OK - valid ELF32 i386 executable\n",
                      TERM_COL_SUCCESS);
    } else {
        terminal_puts(t, "check: FAIL - ", TERM_COL_ERROR);
        terminal_puts(t, elf_strerror(ck), TERM_COL_ERROR);
        terminal_newline(t);
    }
}