/* kernel/exec/elf_loader.h
 * LumaOS 5.0 - elf_load(): 通过 VFS 打开并验证一个 ELF32 可执行文件
 *
 * DAY44 (Week 7, Day 44): "实现 elf_load()。" 验收："检查 Magic、打印
 * Entry，实现在 lmfs 上用 elf。"
 *
 * 原理见 elf.h 的 DAY44 注释（“加载的前半场：打开 → 读 → 验证”）。
 * 本头文件只声明与 VFS 打交道的那一层；纯内存的解析/校验函数在 elf.h。
 *
 * DAY45 (Week 7, Day 45): "解析 PT_LOAD。" 验收："能将程序段加载到内存。"
 *   新增 elf_load_segments()：加载的后半场（把段拷到 p_vaddr、清零 .bss）。
 *   原理见 elf.h 的 DAY45 注释。
 */
#ifndef LUMA_ELF_LOADER_H
#define LUMA_ELF_LOADER_H

#include <stdint.h>
#include "elf.h"
#include "../fs/vfs/vfs.h"

/* elf_load(path, img)
 *
 * path : 已经过 shell_resolve_path() 规范化的完整路径，形如
 *        "IDE0/HELLO.SW"（FAT）或 "IDE3/hello.sw"、"IDE3/bin/hello.sw"
 *        （LMFS，多级目录）。FAT 还是 LMFS 由挂载点的 fs->name 决定。
 * img  : 输出。成功时是一份已验证的程序描述；失败时内容不保证有意义。
 *
 * 做的事（全部只读，不修改磁盘，不把任何段装入内存——那是 Day 45）：
 *   1. 打开文件（目录 -> ELF_ERR_IS_DIR，打不开 -> ELF_ERR_OPEN）；
 *   2. 读开头 52 字节，elf32_parse_header() + elf32_check_header()
 *      （Magic / 32 位 / 小端 / i386 / ET_EXEC / Program Header 表在文件内）；
 *   3. 把 file_t.pos 定位到 e_phoff，逐项读出 e_phnum 个 32 字节的
 *      Program Header 并解析（>ELF_MAX_PHDRS -> ELF_ERR_TOO_MANY_PHDRS）；
 *   4. elf32_check_phdrs() 校验每个 PT_LOAD 与入口点。
 * 返回 ELF_OK，或第一个失败原因（ELF_ERR_*，elf_strerror() 可转成文字）。
 * 失败时 *img 的 ehdr 字段可能已经填好（Header 通过解析之后），调用者
 * 可据此打印诊断，但不得据此运行程序。 */
int elf_load(const char *path, elf_image_t *img);

/* elf_load_segments(path, img, st)                              [DAY45]
 *
 * 加载的后半场：把 elf_load() 验证过的 img 的所有 PT_LOAD 段装入内存
 * （拷贝文件部分 + 清零 .bss + 回读校验；原理见 elf.h 的 DAY45 注释）。
 *
 * path : 与传给 elf_load() 的同一个路径（FAT 或 LMFS，同一份代码）。
 * img  : elf_load() 的输出。成功时 img->loaded 被置 1；失败时为 0。
 *        本函数不信任 img——内部会重新校验一遍再写内存。
 * st   : 可选输出（可传 NULL）：每段的校验和、拷贝/清零字节数、回读失败位置。
 *
 * 做的事：重新打开文件；文件大小必须仍等于 img->file_size（否则
 * ELF_ERR_CHANGED）；然后把“VFS 文件”包成 elf_read_fn，交给 elf.c 的纯
 * 函数 elf32_load_segments()。只读磁盘，写的只是 img 里已验证的用户装载区。
 * 返回 ELF_OK 或第一个失败原因（ELF_ERR_*，elf_strerror() 可转成文字）。
 *
 * 注意：本函数只“放好”，不执行——跳到入口是 Day 46 的 elf_run()。 */
int elf_load_segments(const char *path, elf_image_t *img, elf_load_stats_t *st);

/* ---- 下面三个是 elf_load()/elfinfo 共用的小工具 ---- */

/* elf_open(path, node, file, why)
 * 打开 path（按挂载点的 fs->name 分派：LMFS -> lmfs_vfs_open_path，
 * 其余 -> fat_open），并 fs->open() 出 file。成功返回 ELF_OK，此时
 * node/file 有效（file->node 指向 *node，所以 *node 在 elf_close 前必须
 * 保持存在）。失败返回 ELF_ERR_OPEN / ELF_ERR_IS_DIR / ELF_ERR_IO，
 * 若 why 非 NULL 则 *why 指向一段说明文字（"not found" 或 FAT 的错误串）。 */
int elf_open(const char *path, vfs_node_t *node, file_t *file, const char **why);

/* elf_read_at(file, off, buf, n)
 * 把 file->pos 设为 off，循环 fs->read 直到读满 n 字节或遇 EOF。
 * 返回实际读到的字节数（可能 < n，表示文件在那里结束），读盘出错返回 -1。
 * 这就是“seek”：VFS 没有单独的 seek 接口，但 file_t.pos 是公开字段，
 * FAT 与 LMFS 的 read 都以 file->pos 作为起始偏移。 */
int elf_read_at(file_t *file, uint32_t off, void *buf, uint32_t n);

void elf_close(file_t *file);

#endif /* LUMA_ELF_LOADER_H */
