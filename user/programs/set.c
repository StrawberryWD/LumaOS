/* user/programs/set.c - DAY77c: 环境变量（设计见 kernel/env/env.h）
 *   set                 列出全部变量（NAME=VALUE，每行一个）
 *   set NAME            显示一个变量的值（没有 -> 退出码 1）
 *   set NAME=VALUE ...  设置；VALUE 里有空格时后面的词会用空格接起来（"set MSG=hello world" -> MSG=hello world）
 * 变量全系统共享：这里改了，shell 里 $NAME 和 PATH 立刻生效。 */
#include "../include/luma.h"

int main(int argc, char **argv)
{
    char buf[160];
    if (argc == 1) {
        for (unsigned i = 0; envat(i, buf, sizeof buf) >= 0; i++) puts(buf);
        return 0;
    }
    char *eq = strchr(argv[1], '=');
    if (!eq) {
        if (argc > 2) { dputs("usage: set [NAME[=VALUE]]\n", STDERR_FILENO); return 2; }
        char *v = getenv(argv[1]);
        if (!v) { dprintf(STDERR_FILENO, "set: %s: not defined\n", argv[1]); return 1; }
        puts(v);
        return 0;
    }
    char val[128]; size_t n = 0;
    for (int i = 1; i < argc; i++) {
        const char *part = (i == 1) ? eq + 1 : argv[i];
        size_t l = strlen(part);
        if (i > 1) { if (n + 1 >= sizeof val) goto too_long; val[n++] = ' '; }
        if (n + l >= sizeof val) goto too_long;
        memcpy(val + n, part, l); n += l;
    }
    val[n] = 0;
    *eq = 0;
    if (setenv(argv[1], val, 1) != 0) { dprintf(STDERR_FILENO, "set: cannot set '%s': %s\n", argv[1], strerror(errno)); return 1; }
    return 0;
too_long:
    dputs("set: value too long\n", STDERR_FILENO);
    return 1;
}
