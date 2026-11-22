/* kernel/mm/paging.h - LumaOS 5.0
 *
 * DAY53 (Week 8, Day 53): "开启分页，建立 Kernel Page Table。"  验收："开启分页后系统仍稳定。"
 *
 * ===================================================================================
 *  什么是分页（x86 32 位，二级页表）
 * ===================================================================================
 *  开启分页之前，程序和内核用的地址就是物理内存地址。开启之后，CPU 每次访问内存都要先把
 *  “线性地址”（程序里看到的地址）通过页表翻译成物理地址，再去访问 RAM：
 *
 *      线性地址 32 位 = [ 目录下标 10 位 | 页表下标 10 位 | 页内偏移 12 位 ]
 *
 *      CR3 ──► 页目录 PD（4KB，1024 个 4 字节的项 PDE）
 *                 PDE[目录下标] ──► 页表 PT（4KB，1024 个项 PTE）
 *                                      PTE[页表下标] ──► 物理页框（4KB）
 *                                                         + 页内偏移 = 物理地址
 *
 *  每个 PDE/PTE 低 12 位是标志，高 20 位是下一级（或页框）的物理地址的高 20 位：
 *      bit0 P  存在        bit1 RW 可写        bit2 US 用户可访问（Day 55 用）
 *      bit3 PWT 直写       bit4 PCD 禁用缓存    bit5 A 已访问     bit6 D 已写（仅 PTE）
 *      bit7 PS 仅 PDE：=1 表示这个 PDE 直接映射一整块 4MB（“大页”），不再有页表（需要 CR4.PSE）
 *  访问 P=0 的项 -> CPU 触发 #PF（缺页异常，向量 14），把出错的线性地址放进 CR2，错误码压栈：
 *      bit0 =0 页不存在 / =1 权限不符   bit1 =1 写访问   bit2 =1 用户态访问   bit3 保留位被置
 *      bit4 =1 取指令
 *
 *  为什么要分页：Day 54 起每个程序有自己的页目录（CR3），同一个线性地址 0x08048000 在不同程序里
 *  指向不同的物理页——这才是“独立地址空间”；US 位 + Ring 3 才让“用户程序无法直接访问内核”
 *  由硬件强制（Day 55）。今天先做地基：内核自己的页表，并且“开了分页系统和以前一模一样”。
 *
 * ===================================================================================
 *  今天的内核页表（恒等映射：线性地址 == 物理地址）
 * ===================================================================================
 *   - 为什么恒等：内核、驱动、ELF 装载区（程序 p_vaddr = 物理地址）、显存、PCI 设备都按物理
 *     地址在用。恒等映射让它们在开启分页后一条指令都不用改，这是“开启后仍稳定”的关键。
 *     （Day 54 给程序独立页目录时，内核部分的恒等映射被每个页目录共享一份拷贝。）
 *   - 0x00000000 ~ 0x0FFFFFFF（256MB）：用 4KB 页（64 张页表）映射，页框 = 地址。
 *       例外：第 0 页 [0, 4KB) 不映射（P=0）——NULL 指针保护。内核里任何 `*(int*)0` 或
 *       NULL+小偏移的访问都会立刻 #PF，而不是悄悄读写 BIOS 中断向量表。
 *   - 0x10000000 ~ 0xFFFFFFFF：用 4MB 大页映射（只占页目录里的 960 项，不需要页表），
 *     PWT|PCD 置位（不缓存）——这一段是 PCI 设备的 MMIO（显存/VMware SVGA/Bochs VGA 的 LFB
 *     在 0xE0000000~0xFD000000 一带，见 vga_gfx.c），设备寄存器和显存不能被 CPU 缓存。
 *   - 页表本身放在内核 .bss 里（build.sh 把 .bss 链接到 1MB 起），共 4KB + 64×4KB = 260KB。
 *   - CR0.WP=1：Ring 0 的写也要遵守页表的 RW 位（今天没有只读页，为 Day 54 以后做准备）。
 *
 *  开启顺序（paging_init）：填好页表 -> CR4.PSE=1（允许大页）-> CR3=页目录物理地址 ->
 *  CR0.PG=1。PG 置位的那条指令之后，CPU 就开始用页表取指令；因为 eip 所在页恒等映射，
 *  下一条指令还在同一个地址，程序感觉不到变化。
 *
 *  缺页处理：interrupts.c 把向量 14 交给 paging_page_fault()，它在内核态的缺页一律 panic：
 *  显示出错地址（CR2）、错误码、出错指令地址；不会悄悄返回（返回会无限重试同一条指令）。
 *  panic 画面是文本模式控制台；GUI 模式下画面不可见，但系统同样停在那里（Day 56 再做用户态异常）。
 *
 * ===================================================================================
 *  今天的局限
 * ===================================================================================
 *   - 内核代码段/只读数据还没标成只读（需要知道段边界，等有了内存管理器再做）。
 *   - 没有物理页框分配器：页表是静态数组；Day 54 的用户页要先有页框分配。
 *   - 4GB 以上的物理地址（PAE）、PGE 全局页、NX 位都不用。
 *   - 256MB 以内的 4KB 页映射并不检查那里是否真的有 RAM（QEMU -m 64 时 64MB 之上的页仍是
 *     “存在”的，访问只是读到 0xFF/写入被丢弃，与分页前的行为一致）。
 */
#ifndef LUMA_PAGING_H
#define LUMA_PAGING_H

#include <stdint.h>

#define PAGE_SIZE        4096u
#define PAGE_LARGE_SIZE  0x400000u            /* 4MB 大页 */

/* 页目录项 / 页表项标志 */
#define PTE_P    0x001u
#define PTE_RW   0x002u
#define PTE_US   0x004u
#define PTE_PWT  0x008u
#define PTE_PCD  0x010u
#define PTE_A    0x020u
#define PTE_D    0x040u
#define PDE_PS   0x080u

/* 内核恒等映射的分界（见上面的说明） */
#define PAGING_SMALL_LIMIT   0x10000000u      /* 以下 4KB 页；以上 4MB 大页 */

/* 缺页错误码位 */
#define PF_PRESENT   0x01u
#define PF_WRITE     0x02u
#define PF_USER      0x04u
#define PF_RSVD      0x08u
#define PF_FETCH     0x10u

/* 开启分页。必须在 idt_init() 之后调用（缺页要有人处理）。只能调用一次。 */
void paging_init(void);

/* 分页是否已开启（读 CR0.PG）。 */
int paging_enabled(void);

/* 查表：把线性地址 va 翻译成物理地址。
 * 成功返回 0，*pa 是物理地址（含页内偏移），*flags 是该映射的有效标志（PDE 与 PTE 的 RW/US 相与；
 * 大页时 *flags 含 PDE_PS）；该地址没有映射返回 -1（*pa、*flags 不改）。
 * 不访问 va 本身，所以对未映射地址是安全的。 */
int paging_translate(uint32_t va, uint32_t *pa, uint32_t *flags);

/* DAY54：同上，但查任意页目录 pd（指针 = 它的物理地址，内核恒等映射）。paging_translate() 查的是
 * 内核页目录；paging_translate_current() 查 CR3 当前指向的那个（系统调用检查用户指针用它）。 */
int paging_translate_pd(const uint32_t *pd, uint32_t va, uint32_t *pa, uint32_t *flags);
int paging_translate_current(uint32_t va, uint32_t *pa, uint32_t *flags);
const char *paging_exception_name(uint32_t vec);   /* DAY56: CPU 异常向量的名字 */
uint32_t *paging_kernel_pd(void);          /* 内核页目录（进程页目录从它拷贝共享部分）*/
uint32_t  paging_kernel_pd_phys(void);
void      paging_load_cr3(uint32_t pd_phys);
uint32_t  paging_current_cr3(void);

/* 诊断用的快照。 */
typedef struct {
    uint32_t cr0, cr3, cr4;
    uint32_t pd_phys;          /* 页目录的物理地址（= CR3 的高 20 位部分）*/
    uint32_t pde_small;        /* 指向页表的 PDE 个数（每个 4MB，里面是 4KB 页）*/
    uint32_t pde_large;        /* 4MB 大页的 PDE 个数 */
    uint32_t pages_mapped;     /* 4KB 页里 P=1 的个数 */
    uint32_t page_faults;      /* 迄今处理过的缺页次数（panic 前的那次不计，所以正常恒为 0）*/
} paging_info_t;
void paging_get_info(paging_info_t *out);

/* #PF 入口（interrupts.c 调用）。内核态缺页 -> panic，不返回。
 * err = 错误码，eip/cs = 出错指令的位置。 */
void paging_page_fault(uint32_t err, uint32_t eip, uint32_t cs);

/* 内核态 CPU 异常（向量 0~31，除了 #PF）的统一 panic：显示向量号、名字、错误码、eip。 */
void paging_exception_panic(uint32_t vec, uint32_t err, uint32_t eip, uint32_t cs);

#endif
