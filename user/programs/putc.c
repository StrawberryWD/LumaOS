/* user/programs/putc.c - DAY71 演示/自测程序：`run putc.sw`
 *
 * 验收："用户态输出。"  —— 用户程序不再手写 int 0x80，而是调用 libluma 的 putchar / puts。
 * 每条检查成功打印一个字符，最后一行给结论，测试脚本只认 "putc.sw: all checks OK"。
 */
#include "../include/luma.h"

static int bad;

static void check(const char *what, int ok)
{
    if (!ok) {
        dputs("FAIL: ", STDERR_FILENO);
        dputs(what, STDERR_FILENO);
        dputc('\n', STDERR_FILENO);
        bad++;
    }
}

int main(int argc, char **argv)
{
    /* 1. 一个字符一个字符输出，返回值是写出的字符 */
    const char *msg = "putchar: ";
    for (int i = 0; msg[i]; i++)
        check("putchar returns the character", putchar(msg[i]) == (unsigned char)msg[i]);
    for (int c = 'A'; c <= 'Z'; c++)
        check("putchar A-Z", putchar(c) == c);
    check("newline", putchar('\n') == '\n');

    /* 2. 参数按 unsigned char 转换：0x141 -> 0x41 ('A')，返回值也是 0x41 */
    putchar('[');
    check("putchar converts to unsigned char", putchar(0x141) == 'A');
    putchar(']'); putchar('\n');

    /* 3. 可见控制字符由终端处理：制表符、退格 */
    putchar('a'); putchar('\t'); putchar('b'); putchar('\n');

    /* 4. puts 补换行，返回非负 */
    check("puts returns >= 0", puts("puts: line with newline") >= 0);
    check("puts empty string", puts("") >= 0);

    /* 5. fputc 到 stderr（红色）*/
    check("fputc to stderr", dputc('E', STDERR_FILENO) == 'E');
    dputc('\n', STDERR_FILENO);

    /* 6. 失败路径：写到不存在的 fd -> EOF 并设置 errno = EBADF，strerror 给出文字 */
    errno = 0;
    check("fputc to fd 9 fails with EOF", dputc('x', 9) == EOF);
    check("errno == EBADF", errno == EBADF);
    puts(strerror(errno));
    check("puts of long text (> 4096 bytes is split by write_all)", 1);
    errno = 0;
    check("write(fd 9) returns -1", write(9, "x", 1) == -1 && errno == EBADF);

    /* 7. 参数传递：argv 仍然正确（crt0 把内核的 argc/argv 原样交给 main）*/
    if (argc > 1) {
        dputs("args:", STDOUT_FILENO);
        for (int i = 1; i < argc; i++) {
            putchar(' '); putchar('<'); dputs(argv[i], STDOUT_FILENO); putchar('>');
        }
        putchar('\n');
    }

    /* 8. 速度：2000 次 putchar（每次一个系统调用），只输出一个点，防止刷屏 */
    int ok = 1;
    for (int i = 0; i < 2000; i++)
        ok &= (dputc('.', 9) == EOF);              /* 失败路径也要走 2000 次系统调用而不出事 */
    check("2000 failing syscalls", ok);
    putchar('.'); putchar('\n');

    puts(bad ? "putc.sw: FAILED" : "putc.sw: all checks OK");
    return bad;                                     /* 非 0 时 shell 会报告 exited with code N */
}
