/* kernel/env/env.h - DAY77c: 环境变量（NAME=VALUE 表）
 *
 * 做什么
 * ------
 * 内核里一张全局的小表，shell 和所有用户程序共享（相当于 DOS 的 master environment）：
 *   * shell 用 PATH 找外部命令：敲 `ls` -> 依次在 PATH 的每个目录里找 ls.sw（"." 表示当前目录）。
 *   * shell 在执行命令行前把 $NAME / ${NAME} 换成变量值（没定义 -> 空串）。
 *   * 用户程序用 getenv()/setenv()/unsetenv()/envat()（SYS_GETENV 25 … SYS_ENVAT 28）读写；
 *     set.sw 列出/设置，unset.sw 删除。程序改了变量，shell 立刻看得到——没有“子进程私有副本”这回事。
 *
 * 限制
 * ----
 *   最多 ENV_MAX(16) 个变量；名字 1~31 个字符，只能是 [A-Za-z0-9_] 且不以数字开头，区分大小写；
 *   值最长 ENV_VAL_MAX-1(127) 个字符。默认只有 PATH=".:RAM0:IDE3"（见 env_init）。
 *   PATH 的分隔符是 ':'（卷名里没有冒号）。
 */
#ifndef LUMA_ENV_H
#define LUMA_ENV_H

#include <stdint.h>

#define ENV_MAX       16
#define ENV_NAME_MAX  32      /* 含结尾 NUL */
#define ENV_VAL_MAX   128     /* 含结尾 NUL */
#define ENV_DEFAULT_PATH ".:RAM0:IDE3"

void env_init(void);                                         /* 清空并装入默认变量 */
int  env_valid_name(const char *name);                       /* 1 = 合法名字 */
int  env_get(const char *name, char *buf, uint32_t size);    /* 返回值长度；没有 -ENOENT；buf 放不下 -ERANGE */
const char *env_lookup(const char *name);                    /* 内核内部用：指向值的指针或 NULL（下次修改前有效） */
int  env_set(const char *name, const char *value);           /* 0；名字非法 -EINVAL；值太长 -E2BIG；表满 -ENOSPC */
int  env_unset(const char *name);                            /* 0（不存在也算成功，和 unset 一样） */
int  env_at(uint32_t index, char *buf, uint32_t size);       /* "NAME=VALUE" 写进 buf，返回长度；index 越界 -ENOENT */
int  env_count(void);

/* 把 line 里的 $NAME / ${NAME} 展开写进 out（容量 size）；"\$" 表示字面的 '$'。返回 0；放不下 -ERANGE */
int  env_expand(const char *line, char *out, uint32_t size);

#endif
