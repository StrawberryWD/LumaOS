/* user/programs/rmdir.c - DAY77c: `rmdir.sw 目录...` —— 只删空目录（SYS_RMDIR）。
 * 非空 -> "Directory not empty"；不是目录 -> "Not a directory"；卷根 -> "Device or resource busy"；CD -> "Read-only file system"。 */
#include "../include/luma.h"

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) { dputs("usage: rmdir.sw dir...\n", STDERR_FILENO); return 2; }
    for (int i = 1; i < argc; i++)
        if (rmdir(argv[i]) != 0) { dprintf(STDERR_FILENO, "rmdir: failed to remove '%s': %s\n", argv[i], strerror(errno)); rc = 1; }
    return rc;
}
