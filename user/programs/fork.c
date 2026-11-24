/* user/programs/fork.c - DAY77 自测程序：`cd IDE3` 然后 `run fork.sw`（`run fork.sw orphan`：父进程先退出的场景）
 * 每个场景自成一体；最后一行 "fork.sw: all checks OK"。 */
#include "../include/luma.h"

static int bad;
#define CHECK(c) do { if (!(c)) { dprintf(STDERR_FILENO, "FAIL line %d: %s (errno=%d)\n", __LINE__, #c, errno); bad++; } } while (0)

static int g = 5;                               /* 全局变量：子进程改它，父进程的不能变 */

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "orphan")) {
        int pid = fork();
        if (pid == 0) { puts("orphan: child starts"); yield(); puts("orphan: child still runs after the parent exited"); return 3; }
        puts("orphan: parent exits first");
        return 0;
    }

    CHECK(getpid() == 1);                                           /* 没 fork 过的程序是 1 */
    errno = 0; CHECK(wait(NULL) == -1 && errno == ECHILD);

    /* 1. 基本：一次调用两次返回，地址空间互相独立 */
    char *h = malloc(64); strcpy(h, "parent heap");
    int pid = fork();
    if (pid == 0) {
        CHECK(getpid() == 2);
        g = 99; strcpy(h, "child heap");
        puts("1: child runs, exits 42");
        return 42;
    }
    CHECK(pid == 2);
    puts("1: parent forked, child has not run yet");
    int st = 0, w = wait(&st);
    CHECK(w == 2 && WIFEXITED(st) && WEXITSTATUS(st) == 42);
    CHECK(g == 5 && !strcmp(h, "parent heap"));                     /* 父进程的全局变量和堆没被子进程改动 */
    printf("1: wait -> pid %d status %d\n", w, WEXITSTATUS(st));

    /* 2. 三个子进程，退出码各不同，wait 全部收回，第四次 ECHILD */
    int sum = 0;
    for (int i = 0; i < 3; i++) {
        int c = fork();
        if (c == 0) return 10 + i;
        CHECK(c > 0);
    }
    for (int i = 0; i < 3; i++) { int s2; int p2 = wait(&s2); CHECK(p2 > 0 && WIFEXITED(s2)); sum += WEXITSTATUS(s2); }
    CHECK(sum == 10 + 11 + 12);
    errno = 0; CHECK(wait(NULL) == -1 && errno == ECHILD);
    puts("2: three children reaped");

    /* 3. fd 表复制：各自独立的读位置 */
    int fd = open("fixture.txt", O_RDONLY); CHECK(fd >= 3);
    char a[11], b[11];
    CHECK(read(fd, a, 10) == 10);                                   /* 父：位置 10 */
    int c = fork();
    if (c == 0) {
        CHECK(read(fd, b, 10) == 10);                               /* 子：从 10 读到 20 */
        int sumc = 0; for (int i = 0; i < 10; i++) sumc += (unsigned char)b[i];
        close(fd);
        return sumc & 0xFF;
    }
    int s3; CHECK(wait(&s3) == c);
    CHECK(read(fd, b, 10) == 10);                                   /* 父：也是从 10 读到 20（子进程的读没移动父进程的位置） */
    int sump = 0; for (int i = 0; i < 10; i++) sump += (unsigned char)b[i];
    CHECK(WEXITSTATUS(s3) == (sump & 0xFF));
    CHECK(close(fd) == 0);
    puts("3: independent file offsets");

    /* 4. yield 轮流：父 p，子 c，交替 */
    c = fork();
    if (c == 0) { for (int i = 0; i < 3; i++) { putchar('c'); yield(); } return 0; }
    for (int i = 0; i < 3; i++) { putchar('p'); yield(); }
    CHECK(wait(NULL) == c);
    puts("");
    puts("4: yield done (expect pcpcpc above)");

    /* 5. 子进程里 execv */
    c = fork();
    if (c == 0) {
        char *av[] = { "args.sw", "forked", "child", NULL };
        execv("args.sw", av);
        puts("FAIL: execv returned"); return 9;
    }
    int s5; CHECK(wait(&s5) == c && WIFEXITED(s5) && WEXITSTATUS(s5) == 0);
    puts("5: child execv'd args.sw");

    /* 6. 子进程崩溃（写 NULL）：被杀死，父进程活着并能看到 */
    c = fork();
    if (c == 0) { *(volatile int *)0 = 1; return 0; }
    int s6; CHECK(wait(&s6) == c && WIFSIGNALED(s6));
    puts("6: crashed child reaped, parent alive");

    /* 7. 孙进程：子进程自己 fork 并 wait */
    c = fork();
    if (c == 0) {
        int gc = fork();
        if (gc == 0) return 7;
        int s; if (wait(&s) != gc || WEXITSTATUS(s) != 7) return 1;
        return 8;
    }
    int s7; CHECK(wait(&s7) == c && WEXITSTATUS(s7) == 8);
    puts("7: grandchild ok");

    /* 8. 进程表满：根进程 + 7 个子进程，第 8 个 fork 失败 EAGAIN；收回后又能 fork */
    int kids = 0;
    for (;;) {
        int k = fork();
        if (k == 0) return 0;
        if (k < 0) { CHECK(errno == EAGAIN); break; }
        kids++;
    }
    CHECK(kids == 7);
    for (int i = 0; i < kids; i++) CHECK(wait(NULL) > 0);
    int again = fork(); if (again == 0) return 0;
    CHECK(again > 0 && wait(NULL) == again);
    printf("8: process table full after %d children, usable again\n", kids);

    /* 9. 子进程的堆独立增长 */
    c = fork();
    if (c == 0) { char *big = malloc(300000); if (!big) return 1; memset(big, 7, 300000); return big[299999] == 7 ? 0 : 2; }
    int s9; CHECK(wait(&s9) == c && WEXITSTATUS(s9) == 0);
    puts("9: child grew its own heap");

    free(h);
    puts(bad ? "fork.sw: FAILED" : "fork.sw: all checks OK");
    return bad ? 1 : 0;
}
