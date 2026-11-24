/* user/programs/cat.c - DAY75: `cat.sw [-n] 文件...`  —— 把文件内容按顺序写到标准输出
 *
 * -n 给每一行加行号（"%6d  " 前缀，和 UNIX cat -n 一样；空行也编号；行号跨文件连续）。
 * 某个文件打不开不影响后面的文件：报告原因（strerror）、最终退出码 1。目录 -> "Is a directory"。
 * 没有文件参数：LumaOS 的 stdin 还不能读，所以报用法错误（退出码 2），而不是假装等输入。
 * 实现：fopen + 4096 字节块的 fread/fwrite（Day 74）；-n 时逐字节扫描换行。 */
#include "../include/luma.h"

int main(int argc, char **argv)
{
    int number = 0, first = 1, rc = 0, nfiles = 0;
    unsigned lineno = 1; int at_line_start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { number = 1; first = 2; }
    for (int i = first; i < argc; i++) nfiles++;
    if (nfiles == 0) {
        dputs("usage: cat.sw [-n] file...   (stdin is not available yet)\n", STDERR_FILENO);
        return 2;
    }
    static char buf[4096];
    for (int i = first; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) { dprintf(STDERR_FILENO, "cat: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
            if (!number) { fwrite(buf, 1, n, stdout); continue; }
            for (size_t k = 0; k < n; k++) {
                if (at_line_start) { printf("%6u  ", lineno++); at_line_start = 0; }
                putchar(buf[k]);
                if (buf[k] == '\n') at_line_start = 1;
            }
        }
        if (ferror(f)) { dprintf(STDERR_FILENO, "cat: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
        fclose(f);
    }
    return rc;
}
