/* user/programs/fileio.c - DAY74 自测程序：`run fileio.sw <fixture路径> [leak]`
 *
 * 验收："用户程序读文件。"  读 doc/fixture.txt（120 行，每行 "Line NNN: pack my box with five dozen liquor jugs.\n"，
 * 共 6120 字节；构建时装进 RAM0、IDE3、光盘 CD0 和 FAT 卷），用 fgets / fgetc / fread / read / fseek / ungetc 各读一遍，
 * 并检查各种出错路径。同目录下如果有 empty.txt（0 字节）也测它。
 * 参数 "leak"：故意不关闭 3 个文件就退出——测试脚本接着再跑一次，证明内核在程序结束时回收了 fd。
 */
#include "../include/luma.h"

#define LINES 120
#define LINELEN 51
#define TOTAL (LINES * LINELEN)

static int bad;
#define CHECK(c) do { if (!(c)) { dprintf(STDERR_FILENO, "FAIL line %d: %s (errno=%d)\n", __LINE__, #c, errno); bad++; } } while (0)

static char expect_buf[TOTAL + 1];
static void make_expected(void)
{
    for (int i = 1; i <= LINES; i++)
        snprintf(expect_buf + (i - 1) * LINELEN, LINELEN + 1, "Line %03d: pack my box with five dozen liquor jugs.\n", i);
}

int main(int argc, char **argv)
{
    if (argc < 2) { dputs("usage: fileio.sw <path-of-fixture.txt> [leak]\n", STDERR_FILENO); return 2; }
    const char *path = argv[1];
    int leak = argc > 2 && argv[2][0] == 'l';
    make_expected();
    malloc_info_t m0; malloc_info(&m0);

    /* ---- 1. 打开失败的各种原因 ---- */
    errno = 0; CHECK(fopen("no-such-file.txt", "r") == NULL && errno == ENOENT);
    errno = 0; CHECK(fopen("", "r") == NULL && errno == ENOENT);
    errno = 0; CHECK(fopen("/", "r") == NULL && errno == EISDIR);                 /* 当前卷的根目录 */
    errno = 0; CHECK(fopen(NULL, "r") == NULL && errno == EINVAL);
    errno = 0; CHECK(fopen(path, "w") == NULL && errno == ENOSYS);                /* 写：还没有 */
    errno = 0; CHECK(fopen(path, "r+") == NULL && errno == ENOSYS);
    errno = 0; CHECK(fopen(path, "x") == NULL && errno == EINVAL);
    errno = 0; CHECK(open(path, 1) == -1 && errno == ENOSYS);                     /* O_WRONLY */
    char longp[300]; memset(longp, 'a', sizeof longp - 1); longp[sizeof longp - 1] = 0;
    errno = 0; CHECK(fopen(longp, "r") == NULL && errno == ENAMETOOLONG);
    errno = 0; CHECK(open((const char *)0x1000, O_RDONLY) == -1 && errno == EFAULT);      /* 内核地址/未映射 */
    errno = 0; CHECK(open((const char *)0, O_RDONLY) == -1 && errno == EFAULT);
    errno = 0; CHECK(fopen("NOVOL9/x", "r") == NULL && errno == ENOENT);

    /* ---- 2. 打开、大小、fseek/ftell ---- */
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) { puts("fileio.sw: FAILED (cannot open fixture)"); return 1; }
    CHECK(fileno(f) >= 3);
    CHECK(ftell(f) == 0);
    CHECK(fseek(f, 0, SEEK_END) == 0 && ftell(f) == TOTAL);
    CHECK(fgetc(f) == EOF && feof(f));
    CHECK(fseek(f, 0, SEEK_SET) == 0 && !feof(f) && ftell(f) == 0);               /* fseek 清 EOF */

    /* ---- 3. fgets 逐行：120 行，每行内容正确；之后 NULL + feof ---- */
    char line[128]; int ok = 1, n = 0;
    while (fgets(line, sizeof line, f)) {
        if (n >= LINES || strncmp(line, expect_buf + n * LINELEN, LINELEN) != 0 || strlen(line) != LINELEN) { ok = 0; break; }
        n++;
    }
    CHECK(ok && n == LINES);
    CHECK(feof(f) && !ferror(f));
    CHECK(fgets(line, sizeof line, f) == NULL);                                   /* EOF 粘住 */
    CHECK(ftell(f) == TOTAL);
    printf("lines: %d bytes: %d\n", n, (int)ftell(f));
    char small[10]; rewind(f);                                                    /* 缓冲小于一行：fgets 读 9 个字符就停 */
    CHECK(fgets(small, sizeof small, f) == small && strcmp(small, "Line 001:") == 0 && ftell(f) == 9);
    CHECK(fgets(small, sizeof small, f) != NULL && ftell(f) == 18);

    /* ---- 4. fgetc 全文 ---- */
    rewind(f); ok = 1;
    for (int i = 0; i < TOTAL; i++) if (fgetc(f) != (unsigned char)expect_buf[i]) { ok = 0; break; }
    CHECK(ok && fgetc(f) == EOF && feof(f));

    /* ---- 5. fread：各种块大小（<缓冲、=缓冲、>缓冲、不整除）---- */
    static const int sizes[] = { 1, 7, 51, 100, 511, 512, 513, 600, 1024, 4096, 6120 };
    static char big[TOTAL + 64];
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        int sz = sizes[k];
        rewind(f); int got = 0, good = 1;
        for (;;) {
            size_t r = fread(big + got, 1, (size_t)sz, f);
            got += (int)r;
            if ((int)r < sz) break;
        }
        if (got != TOTAL || memcmp(big, expect_buf, TOTAL) != 0) good = 0;
        if (!good) dprintf(STDERR_FILENO, "fread chunk %d: got %d\n", sz, got);
        CHECK(good && feof(f) && !ferror(f));
    }
    rewind(f);
    CHECK(fread(big, 51, LINES + 10, f) == LINES);                                /* 项数：只有 120 个完整的 */
    CHECK(memcmp(big, expect_buf, TOTAL) == 0);
    CHECK(fread(big, 0, 5, f) == 0 && fread(big, 5, 0, f) == 0);
    rewind(f); CHECK(fread(big, 1000, 7, f) == 6 && feof(f));                     /* 6120 = 6 x 1000 + 120 零头 */

    /* ---- 6. 随机 fseek ---- */
    CHECK(fseek(f, 51 * 60, SEEK_SET) == 0 && fgets(line, sizeof line, f) && strncmp(line, expect_buf + 51 * 60, LINELEN) == 0);   /* 第 61 行 */
    CHECK(fseek(f, -51, SEEK_CUR) == 0 && fgets(line, sizeof line, f) && strncmp(line, expect_buf + 51 * 60, LINELEN) == 0);       /* 回退一行，缓冲被正确扣掉 */
    CHECK(fseek(f, 10, SEEK_CUR) == 0 && ftell(f) == 51 * 61 + 10);
    CHECK(fseek(f, -1, SEEK_END) == 0 && fgetc(f) == '\n' && fgetc(f) == EOF);
    CHECK(fseek(f, TOTAL, SEEK_SET) == 0 && fgetc(f) == EOF);
    CHECK(fseek(f, TOTAL + 1000, SEEK_SET) == 0 && fgetc(f) == EOF && ftell(f) == TOTAL + 1000);   /* 越过文件尾合法 */
    errno = 0; CHECK(fseek(f, -1, SEEK_SET) == -1 && errno == EINVAL);
    errno = 0; CHECK(fseek(f, 0, 9) == -1 && errno == EINVAL);
    CHECK(fseek(f, 0, SEEK_SET) == 0 && fgetc(f) == 'L');                         /* 失败的 fseek 之后流仍可用（位置未变）*/
    unsigned seed = 7; ok = 1;
    for (int i = 0; i < 200; i++) {                                               /* 200 次随机位置+随机长度 */
        seed = seed * 1103515245u + 12345u;
        int pos = (int)((seed >> 8) % TOTAL), len = (int)((seed >> 20) % 700) + 1;
        if (pos + len > TOTAL) len = TOTAL - pos;
        if (fseek(f, pos, SEEK_SET) != 0 || (int)fread(big, 1, (size_t)len, f) != len || memcmp(big, expect_buf + pos, (size_t)len) != 0)
            { ok = 0; break; }
    }
    CHECK(ok);

    /* ---- 7. ungetc ---- */
    rewind(f);
    int c1 = fgetc(f), c2 = fgetc(f);
    CHECK(c1 == 'L' && c2 == 'i' && ftell(f) == 2);
    CHECK(ungetc(c2, f) == c2 && ftell(f) == 1);
    CHECK(ungetc('Z', f) == EOF);                                                 /* 只保证退回 1 个 */
    CHECK(fgetc(f) == 'i' && fgetc(f) == 'n' && ftell(f) == 3);
    CHECK(ungetc('Q', f) == 'Q' && fread(big, 1, 3, f) == 3 && big[0] == 'Q' && big[1] == 'e' && big[2] == ' ');
    CHECK(ungetc(EOF, f) == EOF);

    /* ---- 8. 两个流各自独立的位置；fd 重用规则 ---- */
    FILE *g = fopen(path, "r");
    CHECK(g != NULL && fileno(g) != fileno(f));
    if (g) {
        rewind(f); fseek(f, 100, SEEK_SET);
        CHECK(fgetc(g) == 'L' && ftell(g) == 1 && ftell(f) == 100);
        int gfd = fileno(g);
        CHECK(fclose(g) == 0);
        FILE *h = fopen(path, "r");
        CHECK(h != NULL && fileno(h) == gfd);                                     /* 最小的空闲号 */
        if (h) fclose(h);
    }
    errno = 0; CHECK(close(fileno(f) + 5) == -1 && errno == EBADF);               /* 没打开过的 fd */

    /* ---- 9. 底层 read/lseek/close 的出错路径 ---- */
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 3);
    errno = 0; CHECK(read(fd, (void *)0x1000, 16) == -1 && errno == EFAULT);     /* 坏缓冲区：-EFAULT，位置不变 */
    errno = 0; CHECK(read(fd, (void *)0, 16) == -1 && errno == EFAULT);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);
    CHECK(read(fd, big, 0) == 0);
    CHECK(read(fd, big, 10) == 10 && memcmp(big, expect_buf, 10) == 0 && lseek(fd, 0, SEEK_CUR) == 10);
    static char huge[100000];
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, huge, sizeof huge) == TOTAL);   /* 大于文件：短读 */
    CHECK(read(fd, huge, 10) == 0);                                               /* 文件尾 */
    CHECK(lseek(fd, -5, SEEK_END) == TOTAL - 5);
    errno = 0; CHECK(lseek(fd, -TOTAL - 1, SEEK_END) == -1 && errno == EINVAL);
    CHECK(close(fd) == 0);
    errno = 0; CHECK(close(fd) == -1 && errno == EBADF);                          /* 重复 close */
    errno = 0; CHECK(read(fd, big, 1) == -1 && errno == EBADF);                   /* 已关闭 */
    errno = 0; CHECK(read(0, big, 1) == -1 && errno == EBADF);                    /* stdin 没有数据 */
    errno = 0; CHECK(read(1, big, 1) == -1 && errno == EBADF);
    errno = 0; CHECK(close(0) == -1 && errno == EBADF);                           /* 标准流不能关 */
    errno = 0; CHECK(read(-1, big, 1) == -1 && errno == EBADF);
    errno = 0; CHECK(read(1000, big, 1) == -1 && errno == EBADF);

    /* ---- 10. 标准流 ---- */
    CHECK(fgetc(stdin) == EOF && ferror(stdin));                                  /* stdin 今天没有数据 */
    clearerr(stdin);
    errno = 0; CHECK(fclose(stdout) == EOF && errno == EBADF);
    errno = 0; CHECK(fputc('x', f) == EOF && errno == EBADF);                     /* 只读流不能写 */
    CHECK(fprintf(stdout, "stdout via fprintf: %d\n", 74) == 23);
    CHECK(fputs("fputs to stdout\n", stdout) >= 0 && fwrite("fwrite 4x3\n", 1, 11, stdout) == 11);
    errno = ENOENT; perror("perror demo");                                        /* -> stderr: "perror demo: No such file or directory" */

    /* ---- 11. 空文件（如果同目录有 empty.txt）---- */
    {
        char ep[160]; int k = (int)strlen(path); while (k > 0 && path[k - 1] != '/') k--;
        memcpy(ep, path, (size_t)k); strcpy(ep + k, "empty.txt");
        FILE *e = fopen(ep, "r");
        if (e) {
            CHECK(fgetc(e) == EOF && feof(e) && ftell(e) == 0 && fread(big, 1, 10, e) == 0);
            CHECK(fseek(e, 0, SEEK_END) == 0 && ftell(e) == 0);
            fclose(e);
            puts("empty file: ok");
        } else puts("empty file: (none in this volume)");
    }
    CHECK(fclose(f) == 0);

    /* ---- 12. EMFILE：把 fd 表填满，再全部关掉；循环 fopen/fclose 不泄漏堆 ---- */
    FILE *fs[40]; int opened = 0;
    for (; opened < 40; opened++) { fs[opened] = fopen(path, "r"); if (!fs[opened]) break; }
    CHECK(opened == 16 && errno == EMFILE);                                       /* 每个程序最多 16 个普通文件 */
    printf("open files before EMFILE: %d\n", opened);
    for (int i = 0; i < opened; i++) CHECK(fclose(fs[i]) == 0);
    for (int i = 0; i < 50; i++) { FILE *t = fopen(path, "r"); if (!t) { CHECK(0); break; } fgetc(t); fclose(t); }
    malloc_info_t m1; malloc_info(&m1);
    CHECK(m1.used == m0.used && m1.nused == m0.nused);                            /* fopen/fclose 没有泄漏 */
    CHECK(malloc_check() == 0);

    if (leak) {                                                                   /* 故意不关：内核在程序结束时回收 */
        FILE *a = fopen(path, "r"), *b = fopen(path, "r"); int d = open(path, O_RDONLY);
        CHECK(a && b && d >= 3);
        fgetc(a);
        puts("leaking 3 open files on purpose");
    }
    puts(bad ? "fileio.sw: FAILED" : "fileio.sw: all checks OK");
    return bad;
}
