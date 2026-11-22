/* kernel/shell/cmd/resolution.c - LumaOS 5.0 (Day 58b)
 *
 *   resolution            show the current mode, the blend-pool usage and the list of modes
 *   resolution 1024x576   switch to a listed mode (GUI build only)
 *   resolution next|prev  cycle through the list
 *
 * Same code path as the Settings window's "<  800x450  >" control
 * (gui_set_resolution(): reprograms BGA / VMware SVGA, re-fits every window).
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../shell.h"
#include "../../lib/string.h"

static void put_int(terminal_t *t, int v, unsigned char col)
{
    char b[16]; int n = 0;
    if (v == 0) b[n++] = '0';
    while (v > 0 && n < 15) { b[n++] = (char)('0' + v % 10); v /= 10; }
    char o[16]; for (int i = 0; i < n; i++) o[i] = b[n - 1 - i]; o[n] = 0;
    terminal_puts(t, o, col);
}

static int parse_mode(const char *s, int *w, int *h)
{
    int a = 0, b = 0, i = 0;
    if (s[i] < '0' || s[i] > '9') return -1;
    while (s[i] >= '0' && s[i] <= '9') a = a * 10 + (s[i++] - '0');
    if (s[i] != 'x') return -1;
    i++;
    if (s[i] < '0' || s[i] > '9') return -1;
    while (s[i] >= '0' && s[i] <= '9') b = b * 10 + (s[i++] - '0');
    if (s[i]) return -1;
    *w = a; *h = b;
    return 0;
}

void cmd_resolution(terminal_t *t, int argc, char **argv)
{
    int cw = vga_gfx_width(), ch = vga_gfx_height();
    int n = vga_mode_count();

    if (argc >= 2) {
#ifdef LUMA_TEXT_MODE
        terminal_puts(t, "resolution: GUI build only (this is the text-mode debug console)\n", TERM_COL_ERROR);
        return;
#else
        int w = 0, h = 0, cur = vga_mode_find(cw, ch);
        if (strcmp(argv[1], "next") == 0 || strcmp(argv[1], "prev") == 0) {
            int nx = (cur + (argv[1][0] == 'n' ? 1 : n - 1)) % n;
            vga_mode_get(nx, &w, &h);
        } else if (parse_mode(argv[1], &w, &h) != 0 || vga_mode_find(w, h) < 0) {
            terminal_puts(t, "resolution: unknown mode (run 'resolution' for the list)\n", TERM_COL_ERROR);
            return;
        }
        if (gui_set_resolution(w, h) != 0) {
            terminal_puts(t, "resolution: the display refused that mode\n", TERM_COL_ERROR);
            return;
        }
        cw = w; ch = h;
        terminal_puts(t, "resolution set to ", TERM_COL_SUCCESS);
        put_int(t, cw, TERM_COL_SUCCESS); terminal_puts(t, "x", TERM_COL_SUCCESS); put_int(t, ch, TERM_COL_SUCCESS);
        terminal_puts(t, "\n", TERM_COL_SUCCESS);
        return;
#endif
    }
    terminal_puts(t, "Resolution : ", TERM_COL_INFO);
    put_int(t, cw, TERM_COL_DEFAULT); terminal_puts(t, "x", TERM_COL_DEFAULT); put_int(t, ch, TERM_COL_DEFAULT);
    terminal_puts(t, "\nBlend pool : ", TERM_COL_INFO);
    put_int(t, vga_gfx_blend_used(), TERM_COL_DEFAULT); terminal_puts(t, " / ", TERM_COL_DEFAULT);
    put_int(t, vga_gfx_blend_max(), TERM_COL_DEFAULT);
    terminal_puts(t, " entries used\nModes      :", TERM_COL_INFO);
    for (int i = 0; i < n; i++) {
        int w, h; vga_mode_get(i, &w, &h);
        terminal_puts(t, " ", TERM_COL_DEFAULT);
        put_int(t, w, TERM_COL_DEFAULT); terminal_puts(t, "x", TERM_COL_DEFAULT); put_int(t, h, TERM_COL_DEFAULT);
    }
    terminal_puts(t, "\n", TERM_COL_DEFAULT);
}
