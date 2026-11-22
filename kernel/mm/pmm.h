/* kernel/mm/pmm.h - LumaOS 5.0
 *
 * DAY54 (Week 8, Day 54): "独立用户地址空间。"  验收："ELF 在独立虚拟地址空间运行。"
 *
 * 物理页框分配器（Physical Memory Manager）：把一段物理内存切成 4KB 的“页框”，按需发放/回收。
 * 独立地址空间需要它——程序的虚拟页要映射到“某个物理页框”，而不再是“同样数值的物理地址”。
 *
 * 页框池 = 物理内存 [8MB, 16MB)，共 2048 个页框，用 256 字节的位图记录（1 = 已分配）。
 * 为什么是这一段：
 *   - 内核镜像/.bss 在 0x10000 ~ ~0x1B0000，栈顶 0x200000，GUI 后备缓冲 0x300000、SVGA prev_frame
 *     0x400000、额外终端池 0x500000~0x58A000 —— 全都在 8MB 以下（见 elf.h / window1.c 的内存地图）；
 *   - 16MB 以上是“用户虚拟地址区”(ELF_LOAD_MIN_ADDR)，进程页目录会把它换成自己的映射，
 *     所以页框不能放在那里（否则页框自己在进程页目录里就看不见了）；
 *   - [8MB,16MB) 在内核页表里始终恒等映射，内核可以直接用物理地址读写页框（清零、拷贝 ELF 段）。
 *   QEMU 至少 -m 16 就有这段内存；-m 64 也行（今天的测试覆盖）。
 *
 * 分配出的页框已清零（程序看到的 .bss、新栈必须是 0；也避免泄漏上一个程序的数据）。
 * 释放做了检查：地址必须在池内、4KB 对齐、当前确实已分配，否则返回 -1 且不改位图——
 * 重复释放不会把别人正在用的页框放回池里。
 */
#ifndef LUMA_PMM_H
#define LUMA_PMM_H

#include <stdint.h>

#define PMM_BASE    0x00800000u
#define PMM_END     0x01000000u
#define PMM_FRAMES  ((PMM_END - PMM_BASE) / 4096u)     /* 2048 */

/* 分配一个已清零的页框，返回它的物理地址；池空返回 0（0 永远不是合法页框）。 */
uint32_t pmm_alloc(void);
/* 释放页框；成功 0，非法/重复释放 -1。 */
int      pmm_free(uint32_t pa);
uint32_t pmm_free_count(void);
uint32_t pmm_total(void);
/* 该物理地址是否是当前已分配的池内页框（诊断/测试）。 */
int      pmm_is_allocated(uint32_t pa);

#endif
