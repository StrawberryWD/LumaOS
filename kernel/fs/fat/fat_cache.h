/* kernel/fs/fat/fat_cache.h - DAY65: FAT 表扇区缓存
 *
 * 问题（Day 64 的压力测试和 Day 65 的 `fatbench` 把它量出来了）
 * ----------------------------------------------------------------
 * FAT 文件系统里“这个簇的下一个簇是谁”存在 FAT 表里。fat12/16/32_next_cluster() 每次调用都
 * 向设备发一条读命令把包含这一项的扇区读进来——而一个 512 字节的 FAT 扇区能装 128 个 FAT32 项
 * （256 个 FAT16 项，341 个 FAT12 项），读一个 128 簇长的文件，要做 128 次“读同一个扇区”。
 * 加上 fat_read() 每次调用都从文件头开始沿链表走到当前位置（顺序读第 k 块要走 k 步），
 * 读 N 个簇的文件总共 O(N²) 次 FAT 扇区读取：256KB（512 簇）的文件 ≈ 13 万次。
 *
 * 做法（三层，各解决一部分）
 * ---------------------------
 *   1. 本文件：FAT 扇区缓存。32 个槽，直接映射（槽号 = (lba + dev 指针) % 32），每槽 1 个扇区，
 *      写透（fatc_write() 先写设备、成功后才更新缓存）。读 FAT 项 99% 命中内存。
 *   2. fat_vfs.c 的“链位置游标”：记住上一次读到文件的第几个簇、那个簇号是多少，
 *      顺序读下一块时从游标继续走，不再从头数。
 *   3. fat_vfs.c 的“连续簇合并”：簇 n、n+1、n+2... 在磁盘上相邻时，一条读命令读完整段，
 *      而不是每簇一条（mkfs.fat/mcopy 写出来的文件基本都是连续的）。
 *
 * 一致性
 * ------
 * 缓存是写透的，但还有别的东西会往同一块盘上写：kmain 的自测、`write` 命令走的数据簇写、
 * 目录项写……它们不经过本缓存。所以 block.c 的 ide0 写函数在每次写盘后调用 fatc_note_write()：
 * 把与被写区间重叠的缓存槽作废，并让 g_fat_gen 加一（链位置游标看到代数变了就作废）。
 * 经本缓存自己写的扇区，在设备写成功后再“补”回缓存（见 fatc_write），不会因为作废而白白丢掉。
 * 这样不管谁写盘，缓存都不会读出旧数据。
 *
 * 开关：g_fatc_enabled = 0 时所有函数直通设备（`fatcache off`），用来量“没有缓存”的对照数据；
 * 统计：g_fatc_stats。
 */
#ifndef LUMA_FAT_CACHE_H
#define LUMA_FAT_CACHE_H

#include <stdint.h>
#include "../../drivers/block/block.h"

#define FATC_SLOTS 32

typedef struct {
    uint32_t lookups;       /* 经本缓存读的扇区数（命中 + 未命中） */
    uint32_t hits;          /* 命中，没碰设备 */
    uint32_t misses;        /* 未命中，去设备读 */
    uint32_t dev_reads;     /* 实际发给设备的读命令条数（FAT 表访问） */
    uint32_t dev_writes;    /* 经 fatc_write 写出的写命令条数 */
    uint32_t invalidations; /* 被 fatc_note_write 作废的槽数 */
} fatc_stats_t;

extern fatc_stats_t g_fatc_stats;
extern int          g_fatc_enabled;     /* 1 = 缓存开（默认），0 = 直通 */
extern uint32_t     g_fat_gen;          /* 任何写盘都加一；游标靠它判断自己是否过期 */

/* 读 `count`（1..8）个扇区的 FAT 表内容到 buf；返回 0 / -1，约定同 block_device_t::read。 */
int  fatc_read(block_device_t *dev, uint32_t lba, uint8_t count, void *buf);

/* 写 FAT 表扇区：先写设备，成功后更新缓存。返回 0 / -1。 */
int  fatc_write(block_device_t *dev, uint32_t lba, uint8_t count, const void *buf);

/* 设备层每次写盘之后调用（block.c）：作废重叠的槽，g_fat_gen++。 */
void fatc_note_write(uint32_t lba, uint32_t count);

void fatc_invalidate_all(void);
void fatc_stats_reset(void);

#endif /* LUMA_FAT_CACHE_H */
