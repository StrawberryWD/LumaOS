/* user/programs/exec.c - DAY76 自测程序：`cd IDE3` 然后 `run exec.sw`（只放 IDE3 夹具，光盘 LMFS 根目录已满）
 * 三个阶段由 argv[1] 区分，靠 execv 自己串起来：
 *   阶段 0（无参数）：分配堆、打开文件，然后 execv 自己进入阶段 1 —— 新进程的堆/fd 必须是全新的；
 *   阶段 1：把 execv 的每条失败路径都试一遍（旧程序必须原样继续），再 execv 进入阶段 2；
 *   阶段 2：打印后 execv("args.sw", ...)：args.sw 打印参数并以 0 退出。
 * 最后一行 "exec.sw: stage1 all checks OK" 由阶段 1 打印。 */
#include "../include/luma.h"

static int bad;
#define CHECK(c) do { if (!(c)) { dprintf(STDERR_FILENO, "FAIL line %d: %s (errno=%d)\n", __LINE__, #c, errno); bad++; } } while (0)

int main(int argc, char **argv)
{
    int stage = argc >= 2 ? atoi(argv[1]) : 0;
    if (stage == 0) {
        puts("exec.sw: stage 0, leaking a 100000-byte heap block and an open file on purpose");
        char *leak = malloc(100000);
        if (leak) memset(leak, 0x5A, 100000);
        FILE *f = fopen("fixture.txt", "r");
        printf("stage 0: heap=%s file=%s\n", leak ? "ok" : "FAIL", f ? "open" : "FAIL");
        char *av[] = { "exec.sw", "1", "two words", "", NULL };
        execv("exec.sw", av);
        puts("FAIL: execv returned"); return 9;
    }
    if (stage == 1) {
        printf("stage 1: argc=%d <%s> <%s> <%s>\n", argc, argv[1], argv[2], argv[3]);
        CHECK(argc == 4 && !strcmp(argv[2], "two words") && argv[3][0] == 0 && argv[4] == NULL);
        FILE *f = fopen("fixture.txt", "r");
        CHECK(f != NULL);
        int fd = open("fixture.txt", O_RDONLY);
        CHECK(fd == 4);                                     /* fopen 拿到 3，这是第二个 -> 4：阶段 0 忘记关的文件没有被带过来 */
        close(fd); fclose(f);
        char *p = malloc(64); CHECK(p != NULL); strcpy(p, "survives failed execv");

        char *one[] = { "x", NULL };
        errno = 0; CHECK(execv("no-such-prog.sw", one) == -1 && errno == ENOENT);
        errno = 0; CHECK(execv(".", one) == -1 && errno == EISDIR);
        errno = 0; CHECK(execv("fixture.txt", one) == -1 && errno == ENOEXEC);      /* 文本文件不是 ELF */
        errno = 0; CHECK(execv("", one) == -1 && errno == ENOENT);
        errno = 0; CHECK(execv(NULL, one) == -1 && errno == EFAULT);
        errno = 0; CHECK(execv((char *)0x1000, one) == -1 && errno == EFAULT);
        char *bad_argv[] = { "x", (char *)0x1000, NULL };
        errno = 0; CHECK(execv("args.sw", bad_argv) == -1 && errno == EFAULT);      /* argv 里有坏指针 */
        errno = 0; CHECK(execv("args.sw", (char **)0x1000) == -1 && errno == EFAULT);
        static char *many[40]; for (int i = 0; i < 39; i++) many[i] = "a"; many[39] = NULL;
        errno = 0; CHECK(execv("args.sw", many) == -1 && errno == E2BIG);           /* 39 个参数 */
        static char big[3000]; memset(big, 'b', sizeof big - 1);
        char *bigv[] = { "x", big, NULL };
        errno = 0; CHECK(execv("args.sw", bigv) == -1 && errno == E2BIG);           /* 字符串合计超过 2048 */
        static char longp[300]; memset(longp, 'p', sizeof longp - 1);
        errno = 0; CHECK(execv(longp, one) == -1 && errno == ENAMETOOLONG);

        CHECK(!strcmp(p, "survives failed execv"));          /* 失败的 execv 没碰旧程序的堆 */
        FILE *g = fopen("fixture.txt", "r"); CHECK(g != NULL); if (g) { char l[64]; CHECK(fgets(l, sizeof l, g) != NULL); fclose(g); }
        puts(bad ? "exec.sw: stage1 FAILED" : "exec.sw: stage1 all checks OK");
        free(p);
        if (bad) return 1;
        char *av[] = { "exec.sw", "2", NULL };
        execv("exec.sw", av);
        puts("FAIL: execv returned"); return 9;
    }
    if (stage == 3) { puts("stage 3: exiting 7"); return 7; }
    if (stage == 4) {
        char *one[] = { "x", NULL };
        if (execv("no-such.sw", one) == -1) { perror("stage 4: execv failed"); return 5; }
        return 9;
    }
    puts("exec.sw: stage 2, handing over to args.sw");
    char *av[] = { "args.sw", "from exec", "", "end", NULL };
    execv("args.sw", av);
    puts("FAIL: execv(args.sw) returned"); return 9;
}
