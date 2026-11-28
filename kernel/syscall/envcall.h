#ifndef LUMA_ENVCALL_H
#define LUMA_ENVCALL_H
#include <stdint.h>
int32_t envcall_get(uint32_t uname, uint32_t ubuf, uint32_t size);   /* 值的长度 / -ENOENT / -ERANGE / -EFAULT */
int32_t envcall_set(uint32_t uname, uint32_t uvalue);                /* 0 / -EINVAL / -E2BIG / -ENOSPC / -EFAULT */
int32_t envcall_unset(uint32_t uname);                               /* 0（不存在也算成功）/ -EINVAL */
int32_t envcall_at(uint32_t index, uint32_t ubuf, uint32_t size);    /* "NAME=VALUE" 的长度 / -ENOENT（越界）/ -ERANGE */
#endif
