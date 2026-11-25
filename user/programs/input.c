/* user/programs/input.c - DAY77a 自测程序：读键盘（stdin）。`cd IDE3` 后：
 *   run input.sw          提示 "name? "，读一行，打印 "hello <name>"；再读行直到 Ctrl+D，统计行数/字符数
 *   run input.sw raw      TERM_RAW：每个按键打印一行 "key 0xNN"，读到 'q' 结束
 *   run input.sw nb       TERM_RAW_NB：没有按键时 read 返回 -1/EAGAIN，数一数，yield；读到 'q' 结束
 *   run input.sw cat      把 stdin 原样复制到 stdout，直到 EOF
 *   run input.sw wait     等输入（给 Ctrl+C 测试用）
 *   run input.sw bad      read(0) 的错误路径：坏指针 EFAULT、len 0 返回 0、termmode(9) EINVAL */
#include "../include/luma.h"

int main(int argc, char **argv)
{
    const char *mode = argc >= 2 ? argv[1] : "";
    if (!strcmp(mode, "raw")) {
        int old = termmode(TERM_RAW);
        printf("raw: old mode %d\n", old);
        for (;;) {
            unsigned char b[8];
            int n = read(0, b, sizeof b);
            if (n <= 0) { printf("raw: read returned %d\n", n); return 1; }
            for (int i = 0; i < n; i++) {
                printf("key 0x%02x\n", b[i]);
                if (b[i] == 'q') { termmode(TERM_COOKED); return 0; }
            }
        }
    }
    if (!strcmp(mode, "nb")) {
        termmode(TERM_RAW_NB);
        int polls = 0;
        for (;;) {
            unsigned char b;
            int n = read(0, &b, 1);
            if (n < 0 && errno == EAGAIN) { polls++; yield(); continue; }
            if (n == 1) { printf("nb: key 0x%02x after %s polls\n", b, polls > 0 ? "some" : "no"); if (b == 'q') return 0; polls = 0; }
        }
    }
    if (!strcmp(mode, "cat")) {
        char b[64]; int n;
        while ((n = read(0, b, sizeof b)) > 0) write(1, b, (size_t)n);
        printf("[EOF n=%d]\n", n);
        return 0;
    }
    if (!strcmp(mode, "wait")) {
        puts("waiting for input (Ctrl+C to kill)");
        char b[8]; read(0, b, sizeof b);
        puts("FAIL: not killed"); return 1;
    }
    if (!strcmp(mode, "bad")) {
        int bad = 0;
        errno = 0; if (read(0, (void *)0x1000, 10) != -1 || errno != EFAULT) { puts("FAIL efault"); bad++; }
        errno = 0; if (read(0, NULL, 10) != -1 || errno != EFAULT) { puts("FAIL null"); bad++; }
        char c; if (read(0, &c, 0) != 0) { puts("FAIL len0"); bad++; }
        errno = 0; if (termmode(9) != -1 || errno != EINVAL) { puts("FAIL termmode(9)"); bad++; }
        errno = 0; if (termmode(-1) != -1 || errno != EINVAL) { puts("FAIL termmode(-1)"); bad++; }
        if (termmode(TERM_RAW) != TERM_COOKED || termmode(TERM_COOKED) != TERM_RAW) { puts("FAIL termmode ret"); bad++; }
        errno = 0; if (read(1, &c, 1) != -1 || errno != EBADF) { puts("FAIL read(1)"); bad++; }
        puts(bad ? "input.sw bad: FAILED" : "input.sw bad: all checks OK");
        return bad;
    }
    /* 默认：提示 + 读一行；然后数行 */
    char name[64];
    fputs("name? ", stdout);
    if (!fgets(name, sizeof name, stdin)) { puts("no input"); return 1; }
    size_t L = strlen(name); if (L && name[L - 1] == '\n') name[L - 1] = 0;
    printf("hello <%s>\n", name);
    char line[128]; int lines = 0, chars = 0;
    while (fgets(line, sizeof line, stdin)) { lines++; chars += (int)strlen(line); }
    printf("then %d lines, %d chars\n", lines, chars);
    int c = getchar();                                  /* Ctrl+D 之后还能继续读 */
    printf("after EOF getchar -> %d\n", c);
    return 0;
}
