/* user/programs/unset.c - DAY77c: `unset.sw NAME...` —— 删除环境变量（不存在也不算错）。 */
#include "../include/luma.h"

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) { dputs("usage: unset.sw NAME...\n", STDERR_FILENO); return 2; }
    for (int i = 1; i < argc; i++)
        if (unsetenv(argv[i]) != 0) { dprintf(STDERR_FILENO, "unset: '%s': %s\n", argv[i], strerror(errno)); rc = 1; }
    return rc;
}
