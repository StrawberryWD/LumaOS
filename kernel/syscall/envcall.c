/* kernel/syscall/envcall.c - DAY77c: 环境变量系统调用（getenv / setenv / unsetenv / envat）。设计见 kernel/env/env.h。
 * 和 fd.c / fsops.c 一样：用户指针先验证再用；字符串先拷进内核缓冲区。 */
#define LUMA_KERNEL_SIDE 1
#include "envcall.h"
#include "syscall.h"
#include "../env/env.h"
#include "../core/errno.h"
#include "../lib/string.h"

static int copy_str(char *dst, uint32_t usrc, uint32_t max)       /* 返回长度或 -errno */
{
    for (uint32_t i = 0; i < max; i++) {
        if (!luma_user_range_ok(usrc + i, 1))
            return -LUMA_EFAULT;
        char c = *(const volatile char *)(uintptr_t)(usrc + i);
        dst[i] = c;
        if (c == 0)
            return (int)i;
    }
    return -LUMA_E2BIG;
}

static int32_t put_user(uint32_t ubuf, uint32_t size, const char *s, uint32_t n)   /* 写 n+1 字节 */
{
    if (size < n + 1) return -LUMA_ERANGE;
    if (!luma_user_range_ok(ubuf, n + 1)) return -LUMA_EFAULT;
    memcpy((void *)(uintptr_t)ubuf, s, n + 1);
    return (int32_t)n;
}

int32_t envcall_get(uint32_t uname, uint32_t ubuf, uint32_t size)
{
    char name[ENV_NAME_MAX], val[ENV_VAL_MAX];
    int r = copy_str(name, uname, sizeof name);
    if (r < 0) return r == -LUMA_E2BIG ? -LUMA_ENOENT : r;
    r = env_get(name, val, sizeof val);
    if (r < 0) return r;
    return put_user(ubuf, size, val, (uint32_t)r);
}

int32_t envcall_set(uint32_t uname, uint32_t uvalue)
{
    char name[ENV_NAME_MAX], val[ENV_VAL_MAX];
    int r = copy_str(name, uname, sizeof name);
    if (r < 0) return r == -LUMA_E2BIG ? -LUMA_EINVAL : r;
    r = copy_str(val, uvalue, sizeof val);
    if (r < 0) return r;                              /* 值太长：-E2BIG */
    return env_set(name, val);
}

int32_t envcall_unset(uint32_t uname)
{
    char name[ENV_NAME_MAX];
    int r = copy_str(name, uname, sizeof name);
    if (r < 0) return r == -LUMA_E2BIG ? -LUMA_EINVAL : r;
    return env_unset(name);
}

int32_t envcall_at(uint32_t index, uint32_t ubuf, uint32_t size)
{
    char tmp[ENV_NAME_MAX + ENV_VAL_MAX + 1];
    int r = env_at(index, tmp, sizeof tmp);
    if (r < 0) return r;
    return put_user(ubuf, size, tmp, (uint32_t)r);
}
