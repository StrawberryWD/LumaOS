/* kernel/syscall/stdin.c - DAY77a: fd 0 的实现。设计见 stdin.h。 */
#define LUMA_KERNEL_SIDE 1
#include "stdin.h"
#include "syscall.h"
#include "../core/errno.h"
#include "../drivers/keyboard.h"
#include "../exec/elf_run.h"
#include "../gui/gui.h"

extern void elf_kill_ctrl_c(void);                 /* elf_fault.c */
extern void enable_interrupts(void);
extern void disable_interrupts(void);

#define LINE_MAX 256

static uint8_t g_line[LINE_MAX];
static int     g_len, g_pos;                       /* 已完成的一行 g_line[g_pos..g_len) 等着被取走 */
static int     g_mode;

void stdin_reset(void)
{
    g_len = g_pos = 0;
    g_mode = LUMA_TERM_COOKED;
    (void)keyboard_take_ctrl_c();                  /* 上一个程序留下的 Ctrl+C 不算 */
}

int32_t stdin_termmode(uint32_t mode)
{
    if (mode > LUMA_TERM_RAW_NB)
        return -LUMA_EINVAL;
    int old = g_mode;
    g_mode = (int)mode;
    g_len = g_pos = 0;
    return old;
}

static void echo(char c) { elf_run_write(&c, 1, 0); }

/* 取一个按键。block=0 且没有 -> 0。等待期间开中断（int 0x80 进来时 IF=0，不开键盘中断永远不来），
 * GUI 模式重绘一帧，Ctrl+C 终止程序。 */
static int get_key(int block)
{
    for (;;) {
        unsigned char k = keyboard_read();
        if (k)
            return k;
        if (keyboard_take_ctrl_c())
            elf_kill_ctrl_c();                     /* 不返回 */
        if (!block)
            return 0;
        enable_interrupts();
        if (g_gui_mode)
            gui_modal_pump();
        else
            __asm__ volatile ("pause");
        disable_interrupts();
    }
}

int32_t stdin_read(uint32_t ubuf, uint32_t len)
{
    if (len == 0)
        return 0;
    if (len > 4096)
        len = 4096;
    if (!luma_user_range_ok(ubuf, len))
        return -LUMA_EFAULT;
    uint8_t *dst = (uint8_t *)(uintptr_t)ubuf;

    if (g_mode != LUMA_TERM_COOKED) {
        int k = get_key(g_mode == LUMA_TERM_RAW);
        if (!k)
            return -LUMA_EAGAIN;
        uint32_t n = 0;
        dst[n++] = (uint8_t)k;
        while (n < len && (k = get_key(0)) != 0)
            dst[n++] = (uint8_t)k;
        return (int32_t)n;
    }

    if (g_pos >= g_len) {                          /* 要新的一行 */
        g_len = g_pos = 0;
        for (;;) {
            int k = get_key(1);
            if (k == KEY_ENTER || k == '\n') {
                echo('\n');
                g_line[g_len++] = '\n';
                break;
            } else if (k == KEY_BS || k == 0x7F) {
                if (g_len > 0) { g_len--; echo('\b'); }
            } else if (k == KEY_CTRL_U) {
                while (g_len > 0) { g_len--; echo('\b'); }
            } else if (k == 0x04) {                /* Ctrl+D */
                if (g_len == 0)
                    return 0;                      /* 行首：EOF */
                break;                             /* 行中：已输入的部分交给程序（不带换行） */
            } else if (k >= 0x20 && k < 0x7F) {
                if (g_len < LINE_MAX - 1) { g_line[g_len++] = (uint8_t)k; echo((char)k); }
            }                                      /* 其余（方向键、F 键、Tab…）忽略 */
        }
    }
    uint32_t n = (uint32_t)(g_len - g_pos);
    if (n > len)
        n = len;
    for (uint32_t i = 0; i < n; i++)
        dst[i] = g_line[g_pos + (int)i];
    g_pos += (int)n;
    return (int32_t)n;
}
