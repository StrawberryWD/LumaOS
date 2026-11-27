/* user/programs/rm.c - DAY77c: `rm.sw [-r] [-f] 路径...`
 * 文件直接删；目录需要 -r（递归：先删里面的，再 rmdir）；-f 路径不存在不报错。
 * 拒绝删除 "." ".." 和卷根（"IDE3"）。只读卷（CD0）-> "Read-only file system"。 */
#include "../include/luma.h"

int main(int argc, char **argv)
{
    int rec = 0, force = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'r' || *o == 'R') rec = 1;
            else if (*o == 'f') force = 1;
            else { dprintf(STDERR_FILENO, "rm: invalid option -- '%c'\n", *o); return 2; }
        }
    }
    if (i >= argc) { if (force) return 0; dputs("usage: rm.sw [-r] [-f] path...\n", STDERR_FILENO); return 2; }
    for (; i < argc; i++)
        if (fs_remove("rm", argv[i], rec, force) != 0) rc = 1;
    return rc;
}
