/* user/programs/create.c - DAY77c: `create.sw 文件...` —— 创建空文件（取代内置 create 和 touch.sw，二合一）
 * open(O_WRONLY|O_CREAT|O_EXCL) 再 close：文件已存在 -> "File exists"（不会动已有内容）；路径里的目录不存在 -> No such file；
 * 在光盘上 -> Read-only file system。出错继续处理后面的参数，退出码 1。 */
#include "../include/luma.h"

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) { dputs("usage: create.sw file...\n", STDERR_FILENO); return 2; }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT | O_EXCL);
        if (fd < 0) { dprintf(STDERR_FILENO, "create: cannot create '%s': %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        if (close(fd) != 0) { dprintf(STDERR_FILENO, "create: '%s': %s\n", argv[i], strerror(errno)); rc = 1; }
    }
    return rc;
}
