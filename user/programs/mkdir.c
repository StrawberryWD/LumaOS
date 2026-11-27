/* user/programs/mkdir.c - DAY77c: `mkdir.sw [-p] 目录...`
 * 不加 -p：只建最后一级，已存在 -> "File exists"，父目录不存在 -> 内核给的原因。
 * -p：逐级建（"IDE3/a/b/c"），已存在的目录不算错；第一级可以是卷名（"IDE3"），因为它已经存在。 */
#include "../include/luma.h"

static int make_parents(const char *path)
{
    char t[160]; fs_trim(t, path, sizeof t);
    for (size_t i = 1; ; i++) {
        if (t[i] != '/' && t[i] != 0) continue;
        char save = t[i]; t[i] = 0;
        if (!fs_is_dir(t) && mkdir(t) != 0) return -1;
        t[i] = save;
        if (save == 0) return 0;
    }
}

int main(int argc, char **argv)
{
    int parents = 0, first = 1, rc = 0;
    if (argc > 1 && strcmp(argv[1], "-p") == 0) { parents = 1; first = 2; }
    if (first >= argc) { dputs("usage: mkdir.sw [-p] dir...\n", STDERR_FILENO); return 2; }
    for (int i = first; i < argc; i++) {
        int r = parents ? make_parents(argv[i]) : mkdir(argv[i]);
        if (r != 0) { dprintf(STDERR_FILENO, "mkdir: cannot create directory '%s': %s\n", argv[i], strerror(errno)); rc = 1; }
    }
    return rc;
}
