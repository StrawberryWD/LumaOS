/* user/programs/fsops.c - DAY75 自测程序：`run fsops.sw <含 fixture.txt 的目录，如 RAM0>`
 * 直接测 readdir / stat / getcwd 三个系统调用，包括所有出错路径。最后一行给结论。 */
#include "../include/luma.h"

static int bad;
#define CHECK(c) do { if (!(c)) { dprintf(STDERR_FILENO, "FAIL line %d: %s (errno=%d)\n", __LINE__, #c, errno); bad++; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 2) { dputs("usage: fsops.sw <dir-with-fixture.txt>\n", STDERR_FILENO); return 2; }
    const char *dir = argv[1];
    char fx[160]; snprintf(fx, sizeof fx, "%s/fixture.txt", dir);

    /* 1. 卷列表：path == "" */
    dirent_t d; int nvol = 0, saw_ram0 = 0;
    for (unsigned i = 0; readdir("", i, &d) == 1; i++) { nvol++; if (!strcmp(d.name, "RAM0") || !strcmp(d.name, "IDE3")) saw_ram0 = 1; CHECK(d.type == DT_DIR && d.size == 0); }
    CHECK(nvol >= 2 && saw_ram0);
    CHECK(readdir("", 99, &d) == 0);                                  /* 越过最后一项：0 */
    printf("volumes: %d\n", nvol);

    /* 2. 目录项：个数一致、没有 . 和 ..、条目互不相同、含 fixture.txt 且大小 6120 */
    int n = 0, found = 0, dots = 0;
    static char seen[64][32];
    for (unsigned i = 0; readdir(dir, i, &d) == 1; i++) {
        if (n < 64) { for (int k = 0; k < n; k++) CHECK(strcmp(seen[k], d.name) != 0); strcpy(seen[n], d.name); }
        n++;
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) dots++;
        if (!strcasecmp(d.name, "fixture.txt")) { found++; CHECK(d.type == DT_FILE && d.size == 6120); }
    }
    CHECK(n > 3 && found == 1 && dots == 0);
    CHECK(readdir(dir, (unsigned)n, &d) == 0 && readdir(dir, 100000, &d) == 0);   /* 越界稳定返回 0 */
    printf("entries in %s: %d\n", dir, n);

    /* 3. stat */
    stat_t st;
    CHECK(stat(fx, &st) == 0 && st.type == DT_FILE && st.size == 6120);
    CHECK(stat(dir, &st) == 0 && st.type == DT_DIR && st.size == 0);
    errno = 0; CHECK(stat("no-such", &st) == -1 && errno == ENOENT);
    errno = 0; CHECK(stat("", &st) == -1 && errno == ENOENT);
    errno = 0; CHECK(stat(NULL, &st) == -1 && errno == EFAULT);
    errno = 0; CHECK(stat(fx, (stat_t *)0x1000) == -1 && errno == EFAULT);
    char longp[300]; memset(longp, 'a', sizeof longp - 1); longp[sizeof longp - 1] = 0;
    errno = 0; CHECK(stat(longp, &st) == -1 && errno == ENAMETOOLONG);
    errno = 0; CHECK(stat("NOVOL9/x", &st) == -1 && errno == ENOENT);

    /* 4. readdir 出错路径 */
    errno = 0; CHECK(readdir(fx, 0, &d) == -1 && errno == ENOTDIR);   /* 是文件 */
    errno = 0; CHECK(readdir("no-such", 0, &d) == -1 && errno == ENOENT);
    errno = 0; CHECK(readdir(dir, 0, (dirent_t *)0x1000) == -1 && errno == EFAULT);
    errno = 0; CHECK(readdir(dir, 0, NULL) == -1 && errno == EFAULT);
    errno = 0; CHECK(readdir(NULL, 0, &d) == -1 && errno == EFAULT);

    /* 5. getcwd */
    char cwd[160];
    CHECK(getcwd(cwd, sizeof cwd) == cwd && strlen(cwd) >= 2 && cwd[strlen(cwd) - 1] == '/');
    printf("cwd: %s\n", cwd);
    char tiny[3];
    errno = 0; CHECK(getcwd(tiny, sizeof tiny) == NULL && errno == ERANGE);
    errno = 0; CHECK(getcwd(cwd, 0) == NULL && errno == ERANGE);
    errno = 0; CHECK(getcwd((char *)0x1000, 100) == NULL && errno == EFAULT);
    char exact[160]; strcpy(exact, cwd);
    char fit[160]; CHECK(getcwd(fit, strlen(exact) + 1) == fit && !strcmp(fit, exact));   /* 刚好放得下（含 NUL）*/
    errno = 0; CHECK(getcwd(fit, strlen(exact)) == NULL && errno == ERANGE);               /* 少一个字节 */

    /* 6. 每次 readdir 重新解析路径：同一个目录连续读两遍结果一致；大量调用不出事 */
    int n2 = 0; for (unsigned i = 0; readdir(dir, i, &d) == 1; i++) n2++;
    CHECK(n2 == n);
    for (int i = 0; i < 300; i++) readdir(dir, (unsigned)(i % (n + 1)), &d);

    puts(bad ? "fsops.sw: FAILED" : "fsops.sw: all checks OK");
    return bad;
}
