/* kernel/core/config.c - DAY63: see config.h */
#include "config.h"
#include "../lib/string.h"

luma_config_t g_cfg;

void config_defaults(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.theme = g_cfg.width = g_cfg.height = g_cfg.desktop_icons = CFG_UNSET;
    strcpy(g_cfg.source, "defaults");
}

static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int streq_ci(const char *a, const char *b)
{
    while (*a && *b) { if (lower(*a) != lower(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static int parse_int(const char *s, int *end)
{
    int v = 0, i = 0;
    if (s[0] < '0' || s[0] > '9') return -1;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); if (v > 100000) return -1; i++; }
    *end = i;
    return v;
}

static void apply(const char *key, const char *val)
{
    if (streq_ci(key, "theme")) {
        if (streq_ci(val, "light")) g_cfg.theme = 1;
        else if (streq_ci(val, "dark")) g_cfg.theme = 0;
        else { g_cfg.errors++; return; }
    } else if (streq_ci(key, "resolution")) {
        int e1 = 0, e2 = 0;
        int w = parse_int(val, &e1);
        if (w < 0 || val[e1] != 'x') { g_cfg.errors++; return; }
        int h = parse_int(val + e1 + 1, &e2);
        if (h < 0 || val[e1 + 1 + e2] != 0) { g_cfg.errors++; return; }
        g_cfg.width = w; g_cfg.height = h;
    } else if (streq_ci(key, "desktop_icons")) {
        if (val[0] == '1' && !val[1]) g_cfg.desktop_icons = 1;
        else if (val[0] == '0' && !val[1]) g_cfg.desktop_icons = 0;
        else { g_cfg.errors++; return; }
    } else if (streq_ci(key, "hostname")) {
        unsigned n = 0;
        while (val[n] && n < sizeof g_cfg.hostname - 1) { g_cfg.hostname[n] = val[n]; n++; }
        g_cfg.hostname[n] = 0;
    } else {
        if (g_cfg.n_unknown < 4) {
            unsigned n = 0;
            while (key[n] && n < sizeof g_cfg.unknown[0] - 1) { g_cfg.unknown[g_cfg.n_unknown][n] = key[n]; n++; }
            g_cfg.unknown[g_cfg.n_unknown][n] = 0;
            g_cfg.n_unknown++;
        }
        return;
    }
    g_cfg.keys++;
}

int config_parse(const char *text, unsigned len)
{
    unsigned i = 0;
    char line[96];
    while (i < len) {
        unsigned n = 0;
        while (i < len && text[i] != '\n') { if (n < sizeof line - 1) line[n++] = text[i]; i++; }
        i++;                                   /* the '\n' */
        line[n] = 0;
        g_cfg.lines++;
        char *p = line;
        while (is_sp(*p)) p++;
        if (*p == 0 || *p == '#') continue;
        char *eq = p;
        while (*eq && *eq != '=') eq++;
        if (*eq != '=') { g_cfg.errors++; continue; }
        *eq = 0;
        char *key = p, *val = eq + 1;
        for (char *k = eq - 1; k >= key && is_sp(*k); k--) *k = 0;          /* trim key */
        while (is_sp(*val)) val++;
        for (char *c = val; *c; c++)                                         /* " # comment" after the value */
            if (*c == '#' && (c == val || is_sp(c[-1]))) { *c = 0; break; }
        for (char *e = val + strlen(val) - 1; e >= val && is_sp(*e); e--) *e = 0;   /* trim value */
        apply(key, val);
    }
    return g_cfg.keys;
}
