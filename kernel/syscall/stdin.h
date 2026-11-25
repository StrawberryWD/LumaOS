/* kernel/syscall/stdin.h - LumaOS 5.0 DAY77a: 标准输入（fd 0）
 *
 * 用户程序 read(0, buf, n) 走到这里。两种模式（SYS_TERMMODE 切换，每个程序开始时都是 COOKED）：
 *
 *   LUMA_TERM_COOKED（默认，“行缓冲”）：内核替程序做行编辑——可见字符回显到终端，Backspace 删一个字符，
 *       Ctrl+U 删整行，Enter 结束一行（行尾带 '\n'）；read 一次最多返回这一行的内容，一行没取完下次接着取。
 *       Ctrl+D：行首 = 文件结束（read 返回 0）；行中 = 把已输入的部分当作一行交给程序（不带换行）。
 *       方向键、F 键、Tab 等“特殊键”在这种模式下被忽略。
 *   LUMA_TERM_RAW（“原始”）：不回显、不编辑；read 阻塞到至少有一个按键，然后把键盘缓冲里现有的全部按键
 *       （最多 len 个）一次交出，每个键 1 字节：可见字符/Enter(0x0D)/Backspace(0x08)/Esc 是 ASCII，
 *       方向键等特殊键是 keyboard.h 里 0x80 以上的 KEY_* 码。
 *   LUMA_TERM_RAW_NB：同 RAW，但没有按键时立刻返回 -EAGAIN，不阻塞（GUI 程序的事件循环用）。
 *
 * Ctrl+C：在等待输入时按下会终止程序（退出码 130，和平时 Ctrl+C 杀程序一样），不会被程序当作一个字符读到。
 * 等待时 GUI 模式会继续重绘界面和鼠标；文本模式（测试用）的终端输出照旧在程序结束时才刷到屏幕上。
 */
#ifndef LUMA_STDIN_H
#define LUMA_STDIN_H
#include <stdint.h>

#define LUMA_TERM_COOKED 0
#define LUMA_TERM_RAW    1
#define LUMA_TERM_RAW_NB 2

int32_t stdin_read(uint32_t ubuf, uint32_t len);   /* fd 0 的 read；返回字节数、0 = EOF、-errno */
int32_t stdin_termmode(uint32_t mode);             /* SYS_TERMMODE：返回旧模式，-EINVAL */
void    stdin_reset(void);                         /* 每个程序开始/结束时：回到 COOKED、丢掉半行 */

#endif
