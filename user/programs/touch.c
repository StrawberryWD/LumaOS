/* user/programs/touch.c - DAY77c: `touch.sw 文件...` —— 不存在就创建空文件；已存在则什么也不改
 * （LumaOS 的 LMFS/FAT 没有时间戳，所以“更新修改时间”这半件事无事可做）。
 * open(O_WRONLY|O_CREAT)：不带 O_TRUNC，已有内容不会被清掉；目录 -> EISDIR。出错继续处理后面的参数，退出码 1。 */
#include "../include/luma.h"

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) { dputs("usage: touch.sw file...\n", STDERR_FILENO); return 2; }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) { dprintf(STDERR_FILENO, "touch: cannot touch '%s': %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        if (close(fd) != 0) { dprintf(STDERR_FILENO, "touch: '%s': %s\n", argv[i], strerror(errno)); rc = 1; }
    }
    return rc;
}
