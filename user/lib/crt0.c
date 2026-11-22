/* user/lib/crt0.c - DAY71: 用户程序的真正入口 _start（"C runtime 0"）
 *
 * 内核（elf_run）按 Day 46~49 的约定把控制权交给 ELF 的入口地址，并传 3 个参数（cdecl，栈上）：
 *   _start(const void *api, int argc, char **argv)
 * api 是 Day 46 的函数表，Day 51 起输出走系统调用，api 不再需要；argc/argv 是命令行。
 *
 * crt0 的工作：调 main，把 main 的返回值当退出码用 SYS_EXIT 交给内核。这样程序员写 `return 0;` 就够了，
 * 不用记得写 exit；而且不管程序栈有多深，SYS_EXIT 都能一步回到 shell（Day 52）。
 * _start 自己没有返回地址可依赖——我们永远不 ret，exit 不会返回。
 */
#include "../include/luma.h"

void _start(const void *api, int argc, char **argv)
{
    (void)api;
    exit(main(argc, argv));
}
