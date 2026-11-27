/* user/programs/cp.c - DAY77c: `cp.sw [-r] 源... 目标`
 * 目标是已有目录 -> 复制进去（保留名字）；多个源时目标必须是目录。目录需要 -r。
 * 复制一个文件 = fopen(源,"r") + fopen(目标,"w") + 4096 字节块 fread/fwrite；目标的 fclose 才真正写回磁盘，失败会报错并删掉半成品。
 * 单个文件最大 256KB（Day 77b 的写缓冲上限），更大 -> "File too large"。 */
#include "../include/luma.h"

int main(int argc, char **argv)
{
    int rec = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r' || *o == 'R') rec = 1;
            else { dprintf(STDERR_FILENO, "cp: invalid option -- '%c'\n", *o); return 2; }
        }
    }
    int nsrc = argc - i - 1;
    if (nsrc < 1) { dputs("usage: cp.sw [-r] source... dest\n", STDERR_FILENO); return 2; }
    const char *dst = argv[argc - 1];
    if (nsrc > 1 && !fs_is_dir(dst)) {
        dprintf(STDERR_FILENO, "cp: target '%s' is not a directory\n", dst);
        return 1;
    }
    for (; i < argc - 1; i++)
        if (fs_copy("cp", argv[i], dst, rec) != 0) rc = 1;
    return rc;
}
