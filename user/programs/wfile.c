/* user/programs/wfile.c - DAY77b 自测程序：写文件。
 *   run wfile.sw <目录>          全部检查（目录要可写：IDE3 或 RAM0），最后一行 "wfile.sw: all checks OK"
 *   run wfile.sw <目录> leak     写文件但“忘记 close”就退出（内核要在程序退出时写回）
 *   run wfile.sw <目录> check    检查 leak 留下的文件，以及上次运行留下的 persist.txt（重启后验证真的落盘）
 *   run wfile.sw <目录> rofs <只读路径>  在光盘上写 -> EROFS */
#include "../include/luma.h"

static int bad;
#define CHECK(c) do { if (!(c)) { dprintf(STDERR_FILENO, "FAIL line %d: %s (errno=%d)\n", __LINE__, #c, errno); bad++; } } while (0)

static char dir[100];
static const char *P(const char *name) { static char b[4][160]; static int i; i = (i + 1) & 3; snprintf(b[i], 160, "%s/%s", dir, name); return b[i]; }

static int put(const char *name, const char *text)           /* 一次性写文件（覆盖） */
{
    int fd = open(P(name), O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    int n = (int)write(fd, text, strlen(text));
    int c = close(fd);
    return (n == (int)strlen(text) && c == 0) ? 0 : -1;
}
static int slurp(const char *name, char *out, int cap)       /* 读整个文件，返回长度/-1 */
{
    int fd = open(P(name), O_RDONLY); if (fd < 0) return -1;
    int n = (int)read(fd, out, cap - 1); close(fd);
    if (n >= 0) out[n] = 0;
    return n;
}
static int size_of(const char *name) { stat_t st; return stat(P(name), &st) == 0 ? (int)st.size : -1; }

int main(int argc, char **argv)
{
    if (argc < 2) { dputs("usage: wfile.sw <dir> [leak|check|rofs <path>]\n", STDERR_FILENO); return 2; }
    strncpy(dir, argv[1], sizeof dir - 1);
    char buf[512];

    if (argc >= 3 && !strcmp(argv[2], "leak")) {
        int fd = open(P("leak.txt"), O_WRONLY | O_CREAT | O_TRUNC);
        write(fd, "written, never closed", 21);
        puts("leak: exiting without close");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[2], "check")) {
        CHECK(slurp("leak.txt", buf, sizeof buf) == 21 && !strcmp(buf, "written, never closed"));
        CHECK(slurp("persist.txt", buf, sizeof buf) == 15 && !strcmp(buf, "survives reboot"));
        puts(bad ? "wfile.sw check: FAILED" : "wfile.sw check: all checks OK");
        return bad;
    }
    if (argc >= 4 && !strcmp(argv[2], "rofs")) {
        errno = 0; CHECK(open(argv[3], O_WRONLY) == -1 && errno == EROFS);
        errno = 0; CHECK(open(argv[3], O_WRONLY | O_CREAT) == -1 && errno == EROFS);
        errno = 0; CHECK(unlink(argv[3]) == -1 && errno == EROFS);
        errno = 0; CHECK(mkdir("CD0/ETC/newdir") == -1 && errno == EROFS);
        errno = 0; CHECK(fopen(argv[3], "w") == NULL && errno == EROFS);
        puts(bad ? "wfile.sw rofs: FAILED" : "wfile.sw rofs: all checks OK");
        return bad;
    }

    /* 1. 创建、写、关闭，再读回；stat 看到大小 */
    int fd = open(P("w1.txt"), O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 3);
    CHECK(write(fd, "hello ", 6) == 6 && write(fd, "world\n", 6) == 6);
    CHECK(close(fd) == 0);
    CHECK(size_of("w1.txt") == 12 && slurp("w1.txt", buf, sizeof buf) == 12 && !strcmp(buf, "hello world\n"));
    puts("1: create/write/close/read back");

    /* 2. flags 的错误路径 */
    errno = 0; CHECK(open(P("w1.txt"), O_WRONLY | O_CREAT | O_EXCL) == -1 && errno == EEXIST);
    errno = 0; CHECK(open(P("missing.txt"), O_WRONLY) == -1 && errno == ENOENT);
    errno = 0; CHECK(open(dir, O_WRONLY) == -1 && errno == EISDIR);
    errno = 0; CHECK(open(P("nodir/x.txt"), O_WRONLY | O_CREAT) == -1 && errno == ENOENT);
    errno = 0; CHECK(open(P("w1.txt"), O_RDONLY | O_TRUNC) == -1 && errno == EINVAL);
    errno = 0; CHECK(open(P("w1.txt"), 3) == -1 && errno == EINVAL);
    errno = 0; CHECK(open(P("w1.txt"), O_WRONLY | 0x10000) == -1 && errno == EINVAL);
    puts("2: open error paths");

    /* 3. O_APPEND 和 O_TRUNC */
    fd = open(P("w1.txt"), O_WRONLY | O_APPEND); CHECK(fd >= 3);
    CHECK(write(fd, "more", 4) == 4); CHECK(close(fd) == 0);
    CHECK(slurp("w1.txt", buf, sizeof buf) == 16 && !strcmp(buf, "hello world\nmore"));
    fd = open(P("w1.txt"), O_WRONLY | O_TRUNC); CHECK(fd >= 3);
    CHECK(write(fd, "x", 1) == 1); CHECK(close(fd) == 0);
    CHECK(slurp("w1.txt", buf, sizeof buf) == 1 && buf[0] == 'x');
    fd = open(P("w1.txt"), O_WRONLY | O_TRUNC); CHECK(close(fd) == 0);              /* 只截断不写：文件变空 */
    CHECK(size_of("w1.txt") == 0);
    puts("3: append / truncate");

    /* 4. 局部写 + lseek + 零填充 + 读写打开 */
    CHECK(put("w2.txt", "0123456789") == 0);
    fd = open(P("w2.txt"), O_RDWR); CHECK(fd >= 3);
    CHECK(lseek(fd, 3, SEEK_SET) == 3 && write(fd, "ABC", 3) == 3);
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, 20) == 10 && !strncmp(buf, "012ABC6789", 10));   /* 读写打开：能读到刚写的 */
    CHECK(lseek(fd, 0, SEEK_END) == 10);
    CHECK(lseek(fd, 14, SEEK_SET) == 14 && write(fd, "Z", 1) == 1);                   /* 越过文件尾再写：补零 */
    CHECK(close(fd) == 0);
    CHECK(size_of("w2.txt") == 15 && slurp("w2.txt", buf, sizeof buf) == 15);
    CHECK(!strncmp(buf, "012ABC6789", 10) && buf[10] == 0 && buf[13] == 0 && buf[14] == 'Z');
    fd = open(P("w2.txt"), O_WRONLY); CHECK(fd >= 3);
    errno = 0; CHECK(read(fd, buf, 4) == -1 && errno == EBADF); CHECK(close(fd) == 0);   /* 只写打开不能读 */
    fd = open(P("w2.txt"), O_RDONLY); CHECK(fd >= 3);
    errno = 0; CHECK(write(fd, "q", 1) == -1 && errno == EBADF); CHECK(close(fd) == 0);  /* 只读打开不能写 */
    puts("4: lseek / overwrite / zero fill / O_RDWR");

    /* 5. write 的错误路径 */
    fd = open(P("w3.txt"), O_WRONLY | O_CREAT | O_TRUNC); CHECK(fd >= 3);
    errno = 0; CHECK(write(fd, (void *)0x1000, 10) == -1 && errno == EFAULT);
    errno = 0; CHECK(write(fd, NULL, 10) == -1 && errno == EFAULT);
    CHECK(write(fd, buf, 0) == 0);
    errno = 0; CHECK(write(fd + 5, "x", 1) == -1 && errno == EBADF);
    errno = 0; CHECK(write(99, "x", 1) == -1 && errno == EBADF);
    CHECK(close(fd) == 0);
    errno = 0; CHECK(close(fd) == -1 && errno == EBADF);                              /* 重复 close */
    CHECK(size_of("w3.txt") == 0);
    puts("5: write error paths");

    /* 6. fsync：close 之前别的 open 就能读到 */
    fd = open(P("w4.txt"), O_WRONLY | O_CREAT | O_TRUNC); CHECK(fd >= 3);
    CHECK(write(fd, "synced", 6) == 6);
    CHECK(fsync(fd) == 0);
    CHECK(slurp("w4.txt", buf, sizeof buf) == 6 && !strcmp(buf, "synced"));
    CHECK(close(fd) == 0);
    errno = 0; CHECK(fsync(fd) == -1 && errno == EBADF);
    puts("6: fsync");

    /* 7. 同时最多 3 个写打开 */
    int w[4];
    for (int i = 0; i < 3; i++) { char nm[16]; snprintf(nm, sizeof nm, "m%d.txt", i); w[i] = open(P(nm), O_WRONLY | O_CREAT | O_TRUNC); CHECK(w[i] >= 3); }
    errno = 0; w[3] = open(P("m3.txt"), O_WRONLY | O_CREAT | O_TRUNC); CHECK(w[3] == -1 && errno == EMFILE);
    errno = 0; CHECK(size_of("m3.txt") == -1);                                        /* 失败的 open 没有留下文件 */
    for (int i = 0; i < 3; i++) CHECK(close(w[i]) == 0);
    int rfd = open(P("m0.txt"), O_RDONLY); CHECK(rfd >= 3); close(rfd);               /* 只读 open 不受写打开数量限制 */
    w[0] = open(P("m0.txt"), O_WRONLY); CHECK(w[0] >= 3); close(w[0]);                /* 关掉后又能写打开 */
    puts("7: write-fd limit");

    /* 8. stdio：w / a / r+ / w+ / a+ / x */
    FILE *f = fopen(P("s1.txt"), "w"); CHECK(f != NULL);
    CHECK(fprintf(f, "line %d\n", 1) == 7 && fputs("line 2\n", f) >= 0 && fputc('!', f) == '!' && fwrite("abc", 1, 3, f) == 3);
    errno = 0; CHECK(fgetc(f) == EOF && errno == EBADF);                              /* "w" 流不能读 */
    CHECK(fclose(f) == 0);
    CHECK(slurp("s1.txt", buf, sizeof buf) == 18 && !strcmp(buf, "line 1\nline 2\n!abc"));
    f = fopen(P("s1.txt"), "a"); CHECK(f != NULL); CHECK(fputs("+tail", f) >= 0); CHECK(fclose(f) == 0);
    CHECK(slurp("s1.txt", buf, sizeof buf) == 23 && !strcmp(buf, "line 1\nline 2\n!abc+tail"));
    f = fopen(P("s1.txt"), "r+"); CHECK(f != NULL);
    CHECK(fgets(buf, sizeof buf, f) != NULL && !strcmp(buf, "line 1\n"));              /* 读一行（预读了整个缓冲）*/
    CHECK(fputs("LINE", f) >= 0);                                                     /* 紧接着写：写在逻辑位置 7，不是预读到的末尾 */
    CHECK(fclose(f) == 0);
    CHECK(slurp("s1.txt", buf, sizeof buf) == 23 && !strcmp(buf, "line 1\nLINE 2\n!abc+tail"));
    f = fopen(P("s2.txt"), "w+"); CHECK(f != NULL);
    CHECK(fputs("round trip\n", f) >= 0); rewind(f);
    CHECK(fgets(buf, sizeof buf, f) != NULL && !strcmp(buf, "round trip\n")); CHECK(fclose(f) == 0);
    f = fopen(P("s2.txt"), "a+"); CHECK(f != NULL); CHECK(fputs("appended\n", f) >= 0); rewind(f);
    CHECK(fgets(buf, sizeof buf, f) != NULL && !strcmp(buf, "round trip\n"));
    CHECK(fgets(buf, sizeof buf, f) != NULL && !strcmp(buf, "appended\n")); CHECK(fclose(f) == 0);
    errno = 0; CHECK(fopen(P("s2.txt"), "wx") == NULL && errno == EEXIST);
    errno = 0; CHECK(fopen(P("nope.txt"), "r+") == NULL && errno == ENOENT);
    errno = 0; CHECK(fopen(P("s2.txt"), "z") == NULL && errno == EINVAL);
    f = fopen(P("s2.txt"), "r"); CHECK(f != NULL);
    errno = 0; CHECK(fputc('x', f) == EOF && errno == EBADF); CHECK(fclose(f) == 0);   /* "r" 流不能写 */
    f = fopen(P("s3.txt"), "wb"); CHECK(f != NULL && fflush(f) == 0 && fclose(f) == 0); CHECK(size_of("s3.txt") == 0);
    puts("8: stdio modes");

    /* 9. 大文件：200000 字节分块写、读回校验；写到上限给 EFBIG */
    unsigned char *big = malloc(262144 + 4096); CHECK(big != NULL);
    for (int i = 0; i < 262144 + 4096; i++) big[i] = (unsigned char)(i * 7 + (i >> 8));
    fd = open(P("big.bin"), O_WRONLY | O_CREAT | O_TRUNC); CHECK(fd >= 3);
    int total = 0; while (total < 200000) { int n = (int)write(fd, big + total, 200000 - total > 30000 ? 30000 : 200000 - total); if (n <= 0) break; total += n; }
    CHECK(total == 200000 && close(fd) == 0 && size_of("big.bin") == 200000);
    unsigned char *back = malloc(200000); CHECK(back != NULL);
    fd = open(P("big.bin"), O_RDONLY); int got = 0; while (got < 200000) { int n = (int)read(fd, back + got, 200000 - got); if (n <= 0) break; got += n; } close(fd);
    CHECK(got == 200000 && !memcmp(back, big, 200000));
    fd = open(P("big.bin"), O_WRONLY | O_TRUNC); CHECK(fd >= 3);
    total = 0; for (;;) { int n = (int)write(fd, big + (total % 1000), 50000); if (n <= 0) break; total += n; }
    CHECK(total == 262144);                                                            /* 短写到 256KB 上限，再写 -> EFBIG */
    errno = 0; CHECK(write(fd, "x", 1) == -1 && errno == EFBIG);
    CHECK(close(fd) == 0 && size_of("big.bin") == 262144);
    CHECK(unlink(P("big.bin")) == 0);
    free(back); free(big);
    puts("9: large file / EFBIG");

    /* 10. unlink */
    CHECK(unlink(P("w4.txt")) == 0 && size_of("w4.txt") == -1);
    errno = 0; CHECK(unlink(P("w4.txt")) == -1 && errno == ENOENT);
    errno = 0; CHECK(unlink(dir) == -1 && errno == EISDIR);
    errno = 0; CHECK(unlink("") == -1 && errno == ENOENT);
    errno = 0; CHECK(unlink(NULL) == -1 && errno == EFAULT);
    puts("10: unlink");

    /* 11. mkdir */
    int mr = mkdir(P("wdir"));
    CHECK(mr == 0 || (mr == -1 && errno == EEXIST));                                   /* 上一次运行可能留下了 */
    stat_t st; CHECK(stat(P("wdir"), &st) == 0 && st.type == DT_DIR);
    errno = 0; CHECK(mkdir(P("wdir")) == -1 && errno == EEXIST);
    errno = 0; CHECK(mkdir(P("nodir/sub")) == -1 && errno == ENOENT);
    CHECK(put("wdir/inner.txt", "inside") == 0 && size_of("wdir/inner.txt") == 6);
    CHECK(unlink(P("wdir/inner.txt")) == 0);
    puts("11: mkdir");

    /* 12. rename */
    CHECK(put("r1.txt", "rename me") == 0);
    CHECK(rename(P("r1.txt"), P("r2.txt")) == 0);
    CHECK(size_of("r1.txt") == -1 && slurp("r2.txt", buf, sizeof buf) == 9 && !strcmp(buf, "rename me"));
    CHECK(put("r3.txt", "old content that is longer") == 0);
    CHECK(rename(P("r2.txt"), P("r3.txt")) == 0);                                      /* 覆盖已有文件 */
    CHECK(size_of("r2.txt") == -1 && slurp("r3.txt", buf, sizeof buf) == 9 && !strcmp(buf, "rename me"));
    CHECK(rename(P("r3.txt"), P("r3.txt")) == 0 && size_of("r3.txt") == 9);            /* 同一路径：什么都不做 */
    errno = 0; CHECK(rename(P("nothere"), P("x")) == -1 && errno == ENOENT);
    errno = 0; CHECK(rename(P("wdir"), P("wdir2")) == -1 && errno == EISDIR);
    errno = 0; CHECK(rename(P("r3.txt"), P("wdir")) == -1 && errno == EISDIR);         /* 目标是目录 */
    errno = 0; CHECK(rename(P("r3.txt"), P("nodir/y")) == -1 && errno == ENOENT);
    CHECK(size_of("r3.txt") == 9);                                                     /* 失败的 rename 没有动源文件 */
    CHECK(unlink(P("r3.txt")) == 0);
    puts("12: rename");

    /* 13. fork：子进程不继承写打开的文件 */
    fd = open(P("fk.txt"), O_WRONLY | O_CREAT | O_TRUNC); CHECK(fd >= 3);
    int c = fork();
    if (c == 0) { errno = 0; int r = (int)write(fd, "child", 5); return (r == -1 && errno == EBADF) ? 0 : 1; }
    int st13 = 0; CHECK(wait(&st13) == c && WIFEXITED(st13) && WEXITSTATUS(st13) == 0);
    CHECK(write(fd, "parent", 6) == 6 && close(fd) == 0);
    CHECK(slurp("fk.txt", buf, sizeof buf) == 6 && !strcmp(buf, "parent"));
    puts("13: fork does not share write fds");

    /* 14. 给“重启后检查”留一个文件；清理其它 */
    CHECK(put("persist.txt", "survives reboot") == 0);
    unlink(P("w1.txt")); unlink(P("w2.txt")); unlink(P("w3.txt")); unlink(P("s1.txt")); unlink(P("s2.txt")); unlink(P("s3.txt"));
    unlink(P("fk.txt")); unlink(P("m0.txt")); unlink(P("m1.txt")); unlink(P("m2.txt"));

    puts(bad ? "wfile.sw: FAILED" : "wfile.sw: all checks OK");
    return bad ? 1 : 0;
}
