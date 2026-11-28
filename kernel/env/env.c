/* kernel/env/env.c - DAY77c: 环境变量表。设计见 env.h。 */
#include "env.h"
#include "../core/errno.h"
#include "../lib/string.h"

static struct { int used; char name[ENV_NAME_MAX]; char val[ENV_VAL_MAX]; } g_env[ENV_MAX];

void env_init(void)
{
    memset(g_env, 0, sizeof g_env);
    env_set("PATH", ENV_DEFAULT_PATH);
}

int env_valid_name(const char *n)
{
    if (!n || !n[0]) return 0;
    if (n[0] >= '0' && n[0] <= '9') return 0;
    uint32_t len = 0;
    for (; n[len]; len++) {
        char c = n[len];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return 0;
    }
    return len < ENV_NAME_MAX;
}

static int find(const char *name)
{
    for (int i = 0; i < ENV_MAX; i++)
        if (g_env[i].used && strcmp(g_env[i].name, name) == 0) return i;
    return -1;
}

const char *env_lookup(const char *name)
{
    int i = find(name);
    return i < 0 ? 0 : g_env[i].val;
}

int env_get(const char *name, char *buf, uint32_t size)
{
    int i = find(name);
    if (i < 0) return -LUMA_ENOENT;
    uint32_t n = (uint32_t)strlen(g_env[i].val);
    if (n + 1 > size) return -LUMA_ERANGE;
    memcpy(buf, g_env[i].val, n + 1);
    return (int)n;
}

int env_set(const char *name, const char *value)
{
    if (!env_valid_name(name) || !value) return -LUMA_EINVAL;
    if (strlen(value) >= ENV_VAL_MAX) return -LUMA_E2BIG;
    int i = find(name);
    if (i < 0) {
        for (int k = 0; k < ENV_MAX; k++) if (!g_env[k].used) { i = k; break; }
        if (i < 0) return -LUMA_ENOSPC;
        g_env[i].used = 1;
        strcpy(g_env[i].name, name);
    }
    strcpy(g_env[i].val, value);
    return 0;
}

int env_unset(const char *name)
{
    if (!env_valid_name(name)) return -LUMA_EINVAL;
    int i = find(name);
    if (i >= 0) memset(&g_env[i], 0, sizeof g_env[i]);
    return 0;
}

int env_count(void)
{
    int n = 0;
    for (int i = 0; i < ENV_MAX; i++) if (g_env[i].used) n++;
    return n;
}

int env_at(uint32_t index, char *buf, uint32_t size)
{
    uint32_t seen = 0;
    for (int i = 0; i < ENV_MAX; i++) {
        if (!g_env[i].used) continue;
        if (seen++ != index) continue;
        uint32_t a = (uint32_t)strlen(g_env[i].name), b = (uint32_t)strlen(g_env[i].val);
        if (a + 1 + b + 1 > size) return -LUMA_ERANGE;
        memcpy(buf, g_env[i].name, a);
        buf[a] = '=';
        memcpy(buf + a + 1, g_env[i].val, b + 1);
        return (int)(a + 1 + b);
    }
    return -LUMA_ENOENT;
}

int env_expand(const char *line, char *out, uint32_t size)
{
    uint32_t o = 0;
    for (uint32_t i = 0; line[i]; ) {
        char c = line[i];
        if (c == '\\' && line[i + 1] == '$') {          /* \$ -> 字面 $ */
            if (o + 1 >= size) return -LUMA_ERANGE;
            out[o++] = '$'; i += 2; continue;
        }
        if (c == '$') {
            char name[ENV_NAME_MAX]; uint32_t n = 0, j = i + 1;
            int braces = (line[j] == '{');
            if (braces) j++;
            while (line[j] && n < ENV_NAME_MAX - 1 &&
                   ((line[j] >= 'A' && line[j] <= 'Z') || (line[j] >= 'a' && line[j] <= 'z') || line[j] == '_' ||
                    (n > 0 && line[j] >= '0' && line[j] <= '9')))
                name[n++] = line[j++];
            name[n] = 0;
            if (n > 0 && (!braces || line[j] == '}')) {
                if (braces) j++;
                const char *v = env_lookup(name);
                if (v) {
                    uint32_t l = (uint32_t)strlen(v);
                    if (o + l >= size) return -LUMA_ERANGE;
                    memcpy(out + o, v, l); o += l;
                }
                i = j; continue;
            }
        }
        if (o + 1 >= size) return -LUMA_ERANGE;
        out[o++] = c; i++;
    }
    out[o] = 0;
    return 0;
}
