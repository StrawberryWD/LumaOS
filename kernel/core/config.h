/* kernel/core/config.h - DAY63: system configuration read from the CD (ETC/LUMAOS.CFG).
 * Plain text, one `key=value` per line, '#' comments. Parsed into g_cfg; the GUI applies theme /
 * resolution / desktop_icons once the desktop is up (live_apply_config()). Unknown keys are kept in a
 * small list so `config` can show them (typos are visible instead of silently ignored). */
#ifndef LUMA_CONFIG_H
#define LUMA_CONFIG_H

#define CFG_UNSET (-1)
typedef struct {
    int  loaded;                 /* a config file was found and parsed */
    char source[40];             /* "CD0/ETC/LUMAOS.CFG" or "defaults" */
    int  theme;                  /* CFG_UNSET, 0 = dark, 1 = light */
    int  width, height;          /* CFG_UNSET or the parsed size */
    int  desktop_icons;          /* CFG_UNSET, 0, 1 */
    char hostname[32];
    int  lines, keys, errors;    /* parse statistics */
    char unknown[4][24];         /* first unknown keys */
    int  n_unknown;
} luma_config_t;

extern luma_config_t g_cfg;

void config_defaults(void);
/* Parse a text buffer (not necessarily NUL-terminated). Returns the number of recognised keys. */
int  config_parse(const char *text, unsigned len);
#endif
