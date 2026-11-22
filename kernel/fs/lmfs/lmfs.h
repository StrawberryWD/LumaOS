/* kernel/fs/lmfs/lmfs.h
 * LumaOS 5.0 - LMFS On-Disk Structures & Mount
 *
 * DAY32 (第二月 · 第5周 · Day 32 of the 5.0 filesystem roadmap):
 *   "C 端实现 lmfs_mount(dev)。"
 *   验收："能读取超级块并验证版本/容量。"
 *
 *   Day 29/30 只产出了 doc/lmfs.txt 这份设计文档；今天是 LMFS 第一次
 *   在内核里落成真正的 C 代码。跟 kernel/fs/fat/fat12.h（Day 9）的
 *   角色完全对应：那份头文件把 FAT12 的磁盘结构（BPB、目录项）定义
 *   成 C 结构体，这份头文件把 doc/lmfs.txt 第二/七节已经设计好的
 *   lmfs_superblock_t / lmfs_inode_t 同样定义成 C 结构体——字段名、
 *   字节偏移、大小全部照抄文档，任何一处不一致都以 doc/lmfs.txt 为准
 *   去改这里，而不是反过来。
 *
 *   lmfs_inode_t 在 Day 32 先把结构体定义出来，Day 33 的
 *   lmfs_read_inode()（本文件下方）才真正把它读出来用——跟 fat12.h
 *   当年先定义 fat_dirent_t（Day 9）、几天后才有 fat12_readdir() 真正
 *   去遍历它（Day 12）是同一个"先定形状、后写函数"的顺序。
 *
 * Design notes:
 *   - 两个结构体都 __attribute__((packed))，原因跟 fat_common.h 的
 *     bpb_12_t/fat_dirent_t 完全一样：需要直接把从磁盘读到的原始
 *     字节缓冲区指针强转成这个类型使用（或者反过来，把结构体内容
 *     原样当字节写回磁盘——Day 31 的 tools/mkfs.lmfs 用 Python 的
 *     struct.pack 做的事，lmfs_mount() 这里用 C 的 packed struct
 *     覆盖到已读入的缓冲区上做等价的事），x86 上非对齐的多字节字段
 *     访问是硬件原生支持的，不需要额外处理。
 *   - LMFS_SUPERBLOCK_LBA(=1) 是"卷内相对 LBA"，不是磁盘绝对 LBA
 *     ——doc/lmfs.txt 第一节专门解释过这一点：磁盘绝对 LBA 1~5 早被
 *     loader.bin 占用了，LMFS 卷自己的编号从它自己的 LBA 0（卷首）
 *     开始算。所以 lmfs_mount() 的 `lba` 参数意思跟 fat12_mount()
 *     的 `lba` 参数完全一样：调用者传入的是"这个卷的卷首在磁盘上的
 *     绝对 LBA"，函数内部自己再加上 LMFS_SUPERBLOCK_LBA 算出超级块
 *     真正所在的磁盘绝对 LBA。
 */
#ifndef LUMA_LMFS_H
#define LUMA_LMFS_H

#include <stdint.h>
#include "../../drivers/block/block.h"   /* block_device_t，lmfs_mount() 用 */

/* ===== 常量（全部照抄 doc/lmfs.txt，两边必须保持一致） ===== */

#define LMFS_MAGIC            0x4C4D4653u   /* 'L''M''F''S'，doc 第二/三节 */
#define LMFS_VERSION_MAJOR    1              /* doc 第七节：当前主版本 */
#define LMFS_VERSION_MINOR    0              /* 当前次版本 */

#define LMFS_BLOCK_SIZE        1024   /* doc 第六节：LMFS 块大小（字节） */
#define LMFS_SECTOR_SIZE       BLOCK_SECTOR_SIZE          /* 512，来自 block.h */
#define LMFS_SECTORS_PER_BLOCK (LMFS_BLOCK_SIZE / LMFS_SECTOR_SIZE)  /* = 2 */

/* doc 第一节：卷内 LBA 0 = 保留扇区，卷内 LBA 1 = 超级块。这是"卷内
 * 相对"地址，lmfs_mount() 会加上调用者传入的卷首绝对 LBA 才是真正
 * 要读的磁盘扇区——见本文件顶部 Design notes 最后一条。 */
#define LMFS_SUPERBLOCK_LBA     1

#define LMFS_INODE_SIZE         64    /* doc 第七节 */
#define LMFS_INODES_PER_BLOCK   (LMFS_BLOCK_SIZE / LMFS_INODE_SIZE)  /* = 16 */
#define LMFS_INODES_PER_SECTOR  (LMFS_SECTOR_SIZE / LMFS_INODE_SIZE) /* = 8,
                                   DAY33: lmfs_read_inode() 按扇区读，不是按
                                   块读——见该函数声明上方注释 */

/* DAY34: 目录项大小——doc/lmfs.txt 第十二节：跟 inode 同样选 64 字节，
 * 图的是同一个"能整除块大小"的道理（1024/64=16，一个块正好装 16 个
 * 目录项），并不是巧合跟 LMFS_INODE_SIZE 数值相同。 */
#define LMFS_DIRENT_SIZE         64
#define LMFS_DIRENTS_PER_BLOCK   (LMFS_BLOCK_SIZE / LMFS_DIRENT_SIZE)  /* = 16 */
#define LMFS_DIRENTS_PER_SECTOR  (LMFS_SECTOR_SIZE / LMFS_DIRENT_SIZE) /* = 8,
                                   DAY38: lmfs_create() 定位一个已有块
                                   内的空闲槽位时，只需要写回槽位所在
                                   的那一个扇区，不需要整块重写 */

/* DAY34: 目录项里文件名字段的最大可用长度（不含结尾 NUL）。选 55 而
 * 不是 FAT 的 8.3（11 字符）——doc/lmfs.txt 第十二节：计划.md 文末
 * 「LumaOS 9.0 目录架构规范」里出现的目录/文件名（"Administrator"
 * 13 字符、"TaskManager.sw"/"FileManager.sw" 14 字符等）已经超出
 * 8.3 的容量，LMFS 作为 LumaOS 自己的原生文件系统，没有理由继续
 * 束缚在 FAT 的历史包袱里。 */
#define LMFS_NAME_MAX            55

/* DAY34: 一次 lmfs_readdir() 调用最多解码/返回多少个目录项——跟
 * FAT_DIRLIST_MAX_ENTRIES（fat_common.h）同样的" caller 提供多大的
 * 数组，就还给它多少"上限，数值也保持一致，方便未来 VFS 层用同一个
 * 固定大小的缓冲区应付 FAT 和 LMFS 两种文件系统。 */
#define LMFS_DIRLIST_MAX_ENTRIES  32

/* doc 第八节：inode type 字段取值约定 */
#define LMFS_TYPE_FREE   0
#define LMFS_TYPE_FILE   1
#define LMFS_TYPE_DIR    2

/* doc 第九节末尾：根目录固定是 0 号 inode */
#define LMFS_ROOT_INODE_NUM  0

/* DAY39: 一级间接块里能放多少个块号指针——间接块本身也是一个普通的
 * LMFS_BLOCK_SIZE 字节数据块，里面存的是 uint32_t 块号数组，
 * 1024/4=256 个。 */
#define LMFS_INDIRECT1_PTRS  (LMFS_BLOCK_SIZE / 4)

/* DAY39: lmfs_write()/lmfs_read()（Day 40）支持的单个文件最大字节数
 * ——8 个直接块 + 一级间接块能指向的 256 个块，不含二级间接块
 * （doc/lmfs.txt 第七节设计了 indirect2 字段，但整个 91 天计划里没
 * 有任何一天真正实现它——见 lmfs_write() 声明上方注释的说明）。 */
#define LMFS_MAX_FILE_SIZE \
    (8 * LMFS_BLOCK_SIZE + LMFS_INDIRECT1_PTRS * LMFS_BLOCK_SIZE)

/* DAY34: 数据块号 0 是保留哨兵值，永远不会被分配给任何文件/目录
 * ——doc/lmfs.txt 第十二节：这样 inode 里 direct[i]==0 / indirect1==0
 * / indirect2==0 就能干净地表示"这个指针槽位没有指向任何块"，不需要
 * 另外找一个字段记录"这个 inode 实际用了几个直接块"。跟根目录固定用
 * 0 号 inode（"inode 0 永远有意义，代表根目录"）是同一枚硬币的另一
 * 面："数据块 0 永远没有意义，代表指针为空"——真正能分配给文件内容
 * 的数据块号从 1 开始。mkfs.lmfs 格式化时会把数据块位图的第 0 位
 * 直接标记为已占用（永久保留，不属于任何 inode），Day 36/37 的位图
 * 分配算法也要遵守"永远跳过块号 0"这条规则。 */
#define LMFS_BLOCK_NONE  0

/* ===== 超级块（doc/lmfs.txt 第二节，512 字节，卷内 LBA 1） ===== */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* 必须等于 LMFS_MAGIC，否则不是 LMFS 卷 */
    uint16_t version_major;      /* 主版本：跟本文件 LMFS_VERSION_MAJOR 不同
                                   * 就代表磁盘布局可能已经变了，必须拒绝挂载 */
    uint16_t version_minor;      /* 次版本：通常只是新增可选特性，暂不检查 */
    uint32_t inode_count;        /* 本卷 inode 总数 */
    uint32_t data_block_count;   /* 本卷数据块总数 */
    uint32_t inode_bitmap_lba;   /* inode 位图起始 LBA（卷内相对） */
    uint32_t data_bitmap_lba;    /* 数据块位图起始 LBA（卷内相对） */
    uint32_t feature_compat;     /* 可选特性标志，v1.0 恒为 0 */
    uint32_t feature_incompat;   /* 不兼容特性标志：非 0 且驱动不认识 -> 拒绝挂载 */
    uint32_t feature_ro_compat;  /* 只读兼容特性标志，v1.0 恒为 0 */
    uint8_t  reserved[476];      /* 预留，补齐到 512 字节 */
} lmfs_superblock_t;

/* ===== inode（doc/lmfs.txt 第七节，64 字节；Day 33 起真正被读取/使用） ===== */
typedef struct __attribute__((packed)) {
    uint32_t file_size;       /* 文件实际大小，单位：字节 */
    uint32_t created_time;    /* 创建时间戳；本项目暂无 RTC 驱动，固定 0 */
    uint8_t  type;            /* LMFS_TYPE_FREE / _FILE / _DIR */
    uint8_t  reserved0[3];    /* 对齐填充，补到 4 字节边界 */
    uint32_t direct[8];       /* 8 个直接块指针（块号，非 LBA） */
    uint32_t indirect1;       /* 一级间接块指针 */
    uint32_t indirect2;       /* 二级间接块指针 */
    uint8_t  reserved1[12];   /* 预留，补齐到 64 字节 */
} lmfs_inode_t;

/* 两个结构体的大小必须跟 doc/lmfs.txt 完全一致——用 _Static_assert
 * 在编译期就把偏差挡住，不用等到运行时用十六进制查看器才发现
 * "怎么读出来的字段全是错位的"。跟 tools/mkfs.lmfs 里
 * `assert struct.calcsize(...) == ...` 是同一个把关思路，只是这里
 * 是 C 编译期版本。 */
_Static_assert(sizeof(lmfs_superblock_t) == 512,
               "lmfs_superblock_t 必须是 512 字节，跟 doc/lmfs.txt 第二节不符");
_Static_assert(sizeof(lmfs_inode_t) == LMFS_INODE_SIZE,
               "lmfs_inode_t 必须是 64 字节，跟 doc/lmfs.txt 第七节不符");

/* ===== 目录项（doc/lmfs.txt 第十二节，64 字节；Day 34 起被读取/使用）
 *
 * 一个目录的数据（inode.direct[]/indirect1/indirect2 指向的那些块）
 * 里存的就是一连串这种定长记录，逐个槽位扫描，遇到 used==0 就跳过
 * （空闲/已删除槽位），直到扫完 inode.file_size 覆盖的全部块。跟
 * FAT 目录项固定 32 字节、按槽位线性扫描是同一个思路，只是 FAT 用
 * DIR_Name[0] 的哨兵字节（0x00/0xE5）表示空闲/结束，这里改用一个
 * 独立的 used 字节——因为 LMFS 的文件名是变长且可能真的以 NUL 开头
 * 都合法（虽然目前 mkfs.lmfs 从不生成这样的名字），不能像 FAT 那样
 * 把"名字第一个字节"兼职当空闲标记用。
 * ===== */
typedef struct __attribute__((packed)) {
    uint32_t inode_num;    /* 这个目录项指向的 inode 号 */
    uint8_t  used;         /* 0=空闲（未使用或已删除）1=已使用 */
    uint8_t  name_len;     /* name 里实际字符数，不含结尾 NUL */
    uint8_t  reserved[2];  /* 对齐/预留 */
    char     name[LMFS_NAME_MAX + 1]; /* NUL 结尾，最长 LMFS_NAME_MAX 个字符 */
} lmfs_dirent_t;            /* sizeof == 64 == LMFS_DIRENT_SIZE */

_Static_assert(sizeof(lmfs_dirent_t) == LMFS_DIRENT_SIZE,
               "lmfs_dirent_t 必须是 64 字节，跟 doc/lmfs.txt 第十二节不符");

/* lmfs_readdir() 解码后交给调用者的形式——跟 fat_dirlist_entry_t
 * （fat_common.h）是同一个角色：调用者不需要关心磁盘上原始的
 * lmfs_dirent_t/used/reserved 这些实现细节，只关心"这一项叫什么
 * 名字、是文件还是目录、指向哪个 inode、（如果是文件）多大"。 */
typedef struct {
    char     name[LMFS_NAME_MAX + 1];  /* NUL 结尾 */
    uint8_t  type;         /* LMFS_TYPE_FILE 或 LMFS_TYPE_DIR（FREE 槽位
                               在 lmfs_readdir() 内部就跳过，不会出现在
                               这里） */
    uint32_t inode_num;
    uint32_t size;         /* 对应 inode 的 file_size */
} lmfs_dirlist_entry_t;

/* ===== 挂载状态（Day 32 的实际交付物） ===== */
typedef struct {
    block_device_t     *dev;         /* 这个卷所在的块设备 */
    uint32_t             mount_lba;  /* 卷首（卷内 LBA 0）在磁盘上的绝对 LBA，
                                       * 跟 fat12_fs_t::mount_lba 是同一个概念：
                                       * Day 33 起读 inode 表/位图/数据块时，
                                       * 都要用 mount_lba + 卷内相对 LBA 才是
                                       * 真正要传给 dev->read() 的磁盘绝对 LBA。 */
    lmfs_superblock_t    sb;          /* 已解析、已验证过的超级块 */
    uint32_t             inode_table_lba;  /* DAY33: inode 表起始 LBA（卷内
                                       相对），挂载时算好缓存在这里，见
                                       lmfs_read_inode() 声明上方注释——
                                       doc/lmfs.txt 第九节故意没有把这个
                                       位置存进超级块字段本身，是靠这里
                                       "挂载时算一次、之后重复用"，而不是
                                       "存进超级块、靠 mkfs 和挂载两处代码
                                       各自保持一致"。 */
    uint32_t             data_area_lba;    /* DAY34: 数据区起始 LBA（卷内
                                       相对），跟 inode_table_lba 同一个
                                       "挂载时现算一次、缓存复用"的理由，
                                       算法见 lmfs_readdir() 声明上方
                                       注释——lmfs_readdir() 靠这个字段
                                       把 inode 里的"块号"换算成磁盘绝对
                                       LBA。 */

    /* ---- DAY66: 位图分配加速（全是内存里的状态，磁盘格式一个字节没变） ----
     *
     * Day 36/37 的分配器每次都从位图第 0 个扇区开始、逐字节找第一个 0 位。
     * 对 1.44MB 的测试卷（位图只有 1 个扇区）看不出问题，但对大卷是 O(卷大小)：
     * 256MB 卷有 64 个位图扇区，前 63 个都已满时，每次分配都要先读 63 个扇区。
     * 下面四样东西把它变成“通常 0~1 次扇区读取”：
     *   free_blocks / free_inodes  挂载时数一遍位图得到，之后分配/释放时加减。
     *                              ① 写满的卷立刻失败，不用扫位图；② `df` 直接读；
     *                              ③ fsck 用它交叉检查位图。
     *   blk_hint / ino_hint        “下一个空闲位大概在这”——上一次分配出去的号+1，
     *                              释放时取 min。下次从这里往后找，找到末尾再回头
     *                              从 0 找（next-fit）。
     *   bm_cache[2]                最近用过的数据位图扇区 / inode 位图扇区各缓存 1 个
     *                              （写透：改了内存立刻写盘，所以缓存永远不比磁盘旧）。
     * lmfs_mount() 会把这些全部初始化；结构体整体被 memset 清零后 cache.idx
     * 必须是 -1（0 是合法的扇区号），所以不能靠“清零 = 初始状态”。 */
    uint32_t  free_blocks;       /* 当前空闲的数据块数（不含永久保留的 0 号） */
    uint32_t  free_inodes;       /* 当前空闲的 inode 数 */
    uint32_t  blk_hint;          /* 下次找空闲数据块从这个块号开始 */
    uint32_t  ino_hint;          /* 下次找空闲 inode 从这个号开始 */
    struct {
        int32_t  idx;            /* 缓存的是位图的第几个扇区；-1 = 空 */
        uint8_t  buf[512];
    } bm_cache[2];               /* [0] = 数据块位图  [1] = inode 位图 */
} lmfs_fs_t;

/* DAY66: 分配器的统计，给 `lmfsbench` 和测试用。全局一份（所有 LMFS 卷共用）。 */
typedef struct {
    uint32_t alloc_block_calls;  /* lmfs_alloc_block() 被调用次数 */
    uint32_t alloc_inode_calls;
    uint32_t free_block_calls;
    uint32_t free_inode_calls;
    uint32_t bm_sector_reads;    /* 真正发给设备的位图扇区读次数（缓存命中不算） */
    uint32_t bm_sector_writes;
    uint32_t bm_cache_hits;      /* 缓存命中次数 */
    uint32_t double_frees;       /* 释放一个本来就空闲的块/inode（逻辑错误的迹象，不计入 free_*） */
} lmfs_stats_t;
extern lmfs_stats_t g_lmfs_stats;
void lmfs_stats_reset(void);

/* DAY66: 1 = 快速分配（hint + 32 位字扫描 + 位图扇区缓存，默认）；0 = Day 36 的老算法
 * （从头逐字节扫、不用 hint、不用缓存）。老算法保留下来只做两件事：`lmfsbench` 拿它当
 * 对照组；测试用它验证新算法分配出来的块号序列与老算法一致。两种模式下 free_* 计数
 * 都保持正确，可以随时切换。 */
extern int g_lmfs_fast_alloc;

/* DAY66: lmfs_count_free(fs) —— 重新数一遍位图，刷新 fs->free_blocks / free_inodes
 * （lmfs_mount() 内部就是调它）。成功 0，读位图失败 -1。 */
int lmfs_count_free(lmfs_fs_t *fs);

/* DAY67: 一致性检查（`fsck` 命令）。逐个 inode 走一遍，对照两张位图：
 *   leaked_blocks   位图说已用、但没有任何 inode 引用（空间泄漏）
 *   missing_blocks  被 inode 引用、位图却说空闲（会被重复分配，最危险）
 *   dup_blocks      同一个块被引用两次以上
 *   bad_ptrs        指向 data_block_count 之外的块号
 *   orphan_inodes   位图说已分配、但没有任何目录项指向它（根目录除外）
 *   free_mismatch   fs->free_blocks / free_inodes 与位图实际数出来的不符
 * 只读，不修复。返回 0 = 检查完成（看 report 各计数是否全 0），-1 = 读盘失败，
 * -2 = 卷太大（数据块 > LMFS_FSCK_MAX_BLOCKS，内存里放不下标记表）。 */
#define LMFS_FSCK_MAX_BLOCKS  (256u * 1024u)
#define LMFS_FSCK_MAX_INODES  (64u * 1024u)
typedef struct {
    uint32_t inodes_used, files, dirs;
    uint32_t blocks_used;        /* 位图里置位的数据块数（含 0 号保留块） */
    uint32_t blocks_referenced;  /* inode 实际引用的块数（含间接索引块） */
    uint32_t leaked_blocks, missing_blocks, dup_blocks, bad_ptrs;
    uint32_t orphan_inodes, bad_inodes, free_mismatch;   /* bad_inodes: 位图说空闲、inode 表里却有内容（或反过来类型是 FREE 却被引用） */
} lmfs_fsck_report_t;
int lmfs_fsck(lmfs_fs_t *fs, lmfs_fsck_report_t *out);

/* DAY67: 目录按序号取第 index 个有效项（跳过空闲槽位，包含 "." 和 ".."）。
 * 成功 0；index 超出末尾返回 1（不是错误）；读盘失败/不是目录 -1。
 * 取代 Day 34 的“一次最多 32 项”：lmfs_vfs 的 readdir 不再截断大目录。 */
int lmfs_dir_entry_at(lmfs_fs_t *fs, uint32_t dir_inode_num, uint32_t index,
                      lmfs_dirlist_entry_t *out);

/* lmfs_mount(dev, lba, out_fs)
 *
 * 从 `dev` 读取卷首在磁盘绝对 LBA `lba` 处的 LMFS 卷：读取卷内 LBA
 * LMFS_SUPERBLOCK_LBA（即磁盘绝对 LBA = lba + 1）处的 512 字节超级块，
 * 做 Day 32 验收标准要求的两件事：
 *
 *   1. "验证版本"：magic 必须等于 LMFS_MAGIC；version_major 必须等于
 *      本文件的 LMFS_VERSION_MAJOR（主版本不同 = 磁盘布局可能不兼容，
 *      doc/lmfs.txt 第八节 version_major/minor 字段详解里说明过这个
 *      区分的必要性）；feature_incompat 必须是 0（v1.0 的镜像本来就
 *      不会置这个字段的任何一位，非 0 说明这是本驱动读不懂的更新
 *      版本卷，doc 第三节 feature_incompat 字段详解里"必须拒绝挂载"
 *      的约定）。
 *   2. "验证容量"：inode_count、data_block_count 都不能是 0（否则这个
 *      卷里连一个 inode / 一个数据块都没有，不是一个合法能用的卷）；
 *      inode_bitmap_lba 必须落在超级块之后，data_bitmap_lba 必须落在
 *      inode 位图之后——两个位图的先后顺序是 doc 第九节定死的卷布局，
 *      顺序不对说明超级块内容已经损坏或者根本不是 mkfs.lmfs 格式化出
 *      来的卷。
 *
 * 全部通过 -> 填好 *out_fs（dev、mount_lba、已验证的 sb），返回 0。
 * 任意一项不通过，或者 dev->read() 本身失败 -> *out_fs 清零，返回 -1
 * ——跟 fat12_mount() 的"失败就清零、成功才填"约定完全一致。
 *
 * `out_fs` 不能为 NULL。
 */
int lmfs_mount(block_device_t *dev, uint32_t lba, lmfs_fs_t *out_fs);

/* lmfs_read_inode(fs, inode_num, out_inode)
 *
 * DAY33 (第二月 · 第5周 · Day 33 of the 5.0 filesystem roadmap):
 *   "实现 lmfs_read_inode()。" 验收："能读取 inode 0。"
 *
 * 按 inode 号从 `fs` 已挂载的卷里读出对应的 lmfs_inode_t，写进
 * *out_inode。inode 表的位置本身不是超级块里的一个字段（doc/lmfs.txt
 * 第九节的设计决定：省掉一个"两处数据要保持一致"的冗余字段），是
 * lmfs_mount() 挂载时用 "data_bitmap_lba + 数据块位图占用的扇区数"
 * 现算出来、缓存进 fs->inode_table_lba 的（见 lmfs_fs_t 定义处的
 * 注释）——lmfs_read_inode() 直接用这个缓存值，不会每次调用都重新
 * 算一遍。
 *
 * 定位方式：inode 表里每个 inode 固定 64 字节（LMFS_INODE_SIZE），
 * 一个 512 字节扇区正好装 8 个（LMFS_INODES_PER_SECTOR）——这跟
 * doc/lmfs.txt 第七节"inode 大小选 64 字节，为的是能整除块大小"是
 * 同一个思路在扇区这一级的自然结果（64 同样整除 512）。所以：
 *
 *     扇区号（inode 表内，从 0 开始） = inode_num / 8
 *     扇区内偏移                     = (inode_num % 8) * 64
 *
 * 读那一个扇区（fs->mount_lba + fs->inode_table_lba + 扇区号），
 * 从读回来的 512 字节缓冲区里把对应的 64 字节切出来，就是这个 inode
 * ——不需要把整个 inode 表都读进内存，一次 dev->read() 只读 1 个
 * 扇区，跟 fat12_readdir() 逐簇读目录区、不一次性整卷读入是同一个
 * "按需读、不贪多"的原则。
 *
 * 校验：inode_num 必须 < fs->sb.inode_count（doc 第三节"验证容量"
 * 里 inode_count 本来就是这个卷的 inode 总数，超出这个数的 inode 号
 * 在这个卷上根本不存在，读了也是垃圾数据）；fs 必须是已经
 * lmfs_mount() 成功过的状态（fs->dev 非 NULL）。
 *
 * 成功返回 0 并填好 *out_inode；越界、fs 未挂载、或者 dev->read()
 * 本身失败，返回 -1，*out_inode 清零——跟 lmfs_mount() 的"失败就
 * 清零"约定一致。
 *
 * `fs` 和 `out_inode` 都不能为 NULL。
 */
int lmfs_read_inode(lmfs_fs_t *fs, uint32_t inode_num,
                     lmfs_inode_t *out_inode);

/* lmfs_readdir(fs, dir_inode_num, out_entries, max_entries, out_count)
 *
 * DAY34 (第二月 · 第5周 · Day 34 of the 5.0 filesystem roadmap):
 *   "实现 lmfs_readdir()。" 验收："能列出根目录。"
 *
 * 读 `dir_inode_num` 这个目录 inode 的数据，把里面每一个"已使用"的
 * lmfs_dirent_t 解码成 lmfs_dirlist_entry_t，写进 out_entries[]。
 *
 * 定位数据块：inode.direct[0..7] 是这个目录数据所在的块号（块号，不
 * 是 LBA）；块号 -> 磁盘绝对 LBA 的换算是
 * `fs->mount_lba + fs->data_area_lba + 块号 * LMFS_SECTORS_PER_BLOCK`
 * ——fs->data_area_lba 是 lmfs_mount() 挂载时用 doc/lmfs.txt 第九节
 * 的公式（inode 表起始 LBA + inode 表占用的扇区数，再向上取整成偶数）
 * 现算出来、缓存进 fs->data_area_lba 的，算法必须跟 tools/mkfs.lmfs
 * 的 build_layout() 完全一致，两边独立算出同一个位置。
 *
 * v1.0 范围：本次实现只走 direct[8]（8 个直接块 = 最多 8*16=128 个
 * 目录项，8KB 目录内容），不处理 indirect1/indirect2——doc/lmfs.txt
 * 第七节设计 inode 时，间接块本来就是给大文件预留的，日常使用的
 * 目录（哪怕未来 IDE/System/SysApp/ 这种目录，里面文件也就几个到
 * 几十个）远用不到需要间接块才能装下的规模；真正哪天有目录大到需要
 * 间接块，再来扩展这个函数，不提前做这个复杂度。
 * direct[i] == LMFS_BLOCK_NONE(0) 表示"没有更多块了"，遇到就停止
 * 扫描（不是错误——见 LMFS_BLOCK_NONE 的注释：块号 0 是保留哨兵值，
 * 真正的数据块从块号 1 开始，所以 0 能安全地当"空指针"用）。
 *
 * 每个块内部：LMFS_DIRENTS_PER_BLOCK(=16) 个 lmfs_dirent_t 逐一检查
 * used 字段，used==0 的槽位跳过（空闲/已删除），used==1 的槽位解码
 * 成一条 out_entries[]。跟 fat12_readdir() 的"线性扫描全部槽位，
 * 跳过空闲的"是同一个思路，只是空闲标记字段不同（见 lmfs_dirent_t
 * 定义处的注释）。
 *
 * 写满 max_entries 个或者扫描完这个目录全部数据（含遇到
 * LMFS_BLOCK_NONE）后停止，把实际解码出的条目数写进 *out_count（0
 * 是合法结果：一个空目录）。
 *
 * 校验：dir_inode_num 对应的 inode 必须 type==LMFS_TYPE_DIR（对一个
 * 普通文件调用 lmfs_readdir() 没有意义，直接拒绝，不会把文件内容
 * 错当目录项解析）；其余校验（inode 号越界、fs 未挂载）复用
 * lmfs_read_inode() 自己的检查。
 *
 * 成功返回 0；dir_inode_num 不是目录、lmfs_read_inode() 失败、或者
 * 扫描途中 dev->read() 失败，返回 -1（已解码的条目和 *out_count 保留
 * 失败前的状态，不回滚——跟 fat12_readdir() "读到一半失败，之前解码
 * 的条目仍然有效"是同一个约定）。
 *
 * `fs`、`out_entries`、`out_count` 都不能为 NULL。`out_entries` 至少
 * 要有 `max_entries` 个 lmfs_dirlist_entry_t 的空间。
 */
int lmfs_readdir(lmfs_fs_t *fs, uint32_t dir_inode_num,
                  lmfs_dirlist_entry_t *out_entries,
                  uint32_t max_entries, uint32_t *out_count);

/* lmfs_alloc_block(fs, out_block_num)
 *
 * DAY36 (第二月 · 第6周 · Day 36 of the 5.0 filesystem roadmap):
 *   "实现数据块 bitmap 分配器。" 验收："连续分配多个块，bitmap 正确变化。"
 *
 * 在 `fs` 挂载的卷里找一个空闲数据块（数据块位图里对应 bit 是 0 的
 * 那个），把它在位图里标记为已占用并写回磁盘，把块号写进
 * *out_block_num。
 *
 * 扫描范围：从卷内 LBA fs->sb.data_bitmap_lba 开始，逐扇区读数据块
 * 位图（扇区数由 doc/lmfs.txt 第九节的公式算出——跟 lmfs_mount() 算
 * inode_table_lba 用的是同一条公式），按字节-然后-位的顺序线性找
 * 第一个 0（全 0xFF 的字节整体跳过，不逐位判断，因为位图通常是大片
 * 连续已分配）。
 *
 * 块号 0（LMFS_BLOCK_NONE，doc 第十二节的保留哨兵值）永远不会被分配
 * 出去——正常情况下 mkfs.lmfs 早就把它在位图里标记成已占用，这里的
 * 判断只是防御性的最后一道防线（防的是 Day 41 空间回收哪天有 bug
 * 意外把这一位清掉了）。
 *
 * 只写回位图里真正改动的那一个扇区，不是整张位图——位图可能跨好几
 * 个扇区，没必要每次分配都把全部扇区重写一遍。写盘失败直接返回 -1，
 * 不会留下"内存里标记了、磁盘上没写"的半成品状态；`fs->dev->write`
 * 是 NULL（只读后端）时同样直接拒绝，跟 fat_alloc_cluster() 开头的
 * 检查是同一个理由。
 *
 * 成功返回 0 并填好 *out_block_num；`fs`/`out_block_num` 为 NULL、
 * `fs` 未挂载、位图区域读写失败、或者整个卷已经没有空闲数据块
 * （卷写满了），返回 -1。
 *
 * DAY36 范围之外：本函数只管"找一个空闲块、标记它"，不管数据块
 * 之前的内容（新分配的块内容是磁盘上原来剩下的任意字节，不保证是
 * 全零——清零是调用者的责任，比如 Day 38 create()/Day 39 write() 在
 * 真正往这个块里写有意义的数据之前，各自决定要不要先清零），也不
 * 负责把分配到的块号填进任何 inode 的 direct[]/indirect1/indirect2
 * ——那是调用方（Day 38/39）拿到块号之后自己做的事，分配器本身不
 * 关心这个块最终会被谁用来存什么。释放数据块（分配的反向操作）是
 * Day 41（"delete() 与空间回收"）的任务，本函数不提供。
 */
int lmfs_alloc_block(lmfs_fs_t *fs, uint32_t *out_block_num);

/* lmfs_alloc_inode(fs, type, out_inode_num)
 *
 * DAY37 (第二月 · 第6周 · Day 37 of the 5.0 filesystem roadmap):
 *   "实现 inode 分配器。" 验收："创建 inode 后返回合法 inode 号。"
 *
 * 在 `fs` 挂载的卷里找一个空闲 inode 号（inode 位图里对应 bit 是 0
 * 的那个），标记为已分配，并把 inode 表里这个槽位的内容初始化成一个
 * 刚分配、空的、type 为调用者指定的 `type` 的合法 inode（file_size=0、
 * created_time=0、8 个直接块指针和两个间接块指针全部是 0），把分配到
 * 的 inode 号写进 *out_inode_num。
 *
 * 跟 lmfs_alloc_block()（Day 36）结构上几乎一样（同样的"字节->位"
 * 两级线性扫描、同样全 0xFF 字节整体跳过的小优化、同样只写回真正
 * 翻转了的那一个位图扇区），但有一处关键差异，也是这个函数比
 * lmfs_alloc_block() 多做的事：lmfs_alloc_block() 分配完一个数据块
 * 之后，数据块本身的内容是不管的（谁用谁负责清零/填内容）；
 * lmfs_alloc_inode() 却必须顺带初始化 inode 表项的内容，不能只
 * 翻转位图——因为 doc/lmfs.txt 第八节（type 字段详解）当初定下的
 * 不变式是"type==LMFS_TYPE_FREE(0) 要跟位图里这一位是 0 保持一致"，
 * 如果只翻转位图、不碰 inode 内容，位图会说"已分配"而这个 inode 的
 * type 还停在 FREE(0)，两套"是否已分配"的记录就对不上了；数据块
 * 没有这种第二套记录（数据块的字节内容本来就没有"这块是不是空闲"
 * 的自描述字段），所以不存在同样的问题。
 *
 * 为什么需要一个 `type` 参数：分配出来的 inode 到底是文件还是目录，
 * 只有调用者知道（Day 38 的 create() 传 LMFS_TYPE_FILE，未来做
 * mkdir 一类操作会传 LMFS_TYPE_DIR）；`type` 必须是 LMFS_TYPE_FILE
 * 或 LMFS_TYPE_DIR 之一，传 LMFS_TYPE_FREE 或其他值直接拒绝（FREE
 * 不是一个"分配出来"的目标状态，自相矛盾）。
 *
 * 写盘顺序是刻意安排的：先把 inode 表项的新内容写成功，最后才把
 * 位图那一位置位并写回——不是反过来。这样如果 inode 表那一步写盘
 * 失败，位图仍然停在"未分配"，不会出现"位图说这个号已经分配、
 * inode 内容却是旧的/错的"这种更难排查的半成品状态；代价是如果
 * 位图那最后一步写盘失败，inode 内容已经写成了新的但位图没跟上，
 * 这是本函数已知、未处理的边界情况（这个项目目前没有日志/事务机制，
 * 不在这里新增）。
 *
 * 成功返回 0 并填好 *out_inode_num；`fs`/`out_inode_num` 为 NULL、
 * `type` 不是 FILE/DIR 之一、`fs` 未挂载、只读后端（dev->write ==
 * NULL）、位图或 inode 表区域读写失败、或者整个 inode 表已经分配满，
 * 返回 -1。
 *
 * 范围之外，留给 Day 38：本函数只负责"给我一个空白的、属于某个
 * type 的新 inode"，不会把这个 inode 号填进任何目录的数据块（也就是
 * 不会创建任何 lmfs_dirent_t）——把新 inode 和一个文件名、挂到某个
 * 目录下，是 Day 38（"实现 create() 与目录项"）的事，本函数单独
 * 调用完，这个新 inode 在磁盘上是"存在但没有任何目录项指向它"的
 * 状态，直到调用者把它链接进某个目录。
 */
int lmfs_alloc_inode(lmfs_fs_t *fs, uint8_t type, uint32_t *out_inode_num);

/* lmfs_create(fs, parent_inode_num, name, out_inode_num)
 *
 * DAY38 (第二月 · 第6周 · Day 38 of the 5.0 filesystem roadmap):
 *   "实现 create() 与目录项。" 验收："create test.txt 成功。"
 *
 * 在 `parent_inode_num`（必须是一个目录）里新建一个名为 `name` 的
 * 普通文件：分配一个新 inode（内部调用 Day 37 的 lmfs_alloc_inode()，
 * type=LMFS_TYPE_FILE），在父目录的数据里找一个空闲的 lmfs_dirent_t
 * 槽位（没有空闲槽位、父目录的 8 个直接块也还没用满时，会先调用
 * Day 36 的 lmfs_alloc_block() 给父目录多分配一个块），把新 inode
 * 号和文件名写进这个槽位。成功时把新 inode 号写进 *out_inode_num。
 *
 * 这是 Day 32~37 这一串底层分配器第一次被真正"串起来用一遍"：
 * mount -> read_inode -> （扫目录找重名/找空位）-> alloc_block（如果
 * 父目录现有的块都满了）-> alloc_inode -> 把两者的结果写成一条
 * dirent。本函数不包含这条链路里任何一环的实现，只是按顺序调用。
 *
 * 校验（按顺序，全部通过之前不做任何磁盘写入——名字不合法、父目录
 * 不是目录、重名、父目录已经满了，这几种情况下函数不会留下任何
 * 副作用，不会有"分配了一半又失败"的半成品状态；只有下面"实际新增
 * 一个块"这一步之后才有本函数自己承认的已知未处理失败窗口，见下）：
 *   1. name 非 NULL、长度在 1~LMFS_NAME_MAX(55) 之间、不含 '/'（单
 *      一路径分量，跟 fat_create_file() 的同名检查一致——多级路径
 *      解析归 Day 42 的 lmfs_resolve_parent()，本函数自己永远只接受
 *      一个不带路径分隔符的纯文件名；DAY42 起 "."/".." 也被拒绝——
 *      它们是 mkdir 自动生成的保留名）。
 *   2. parent_inode_num 对应的 inode 存在且 type==LMFS_TYPE_DIR。
 *   3. 父目录现有数据块（只扫 direct[8]，跟 lmfs_readdir() 同样的
 *      v1.0 范围限定）里没有任何一条已使用的 dirent 的名字跟 `name`
 *      完全相同（大小写敏感的精确匹配——LMFS 不像 FAT 8.3 那样强制
 *      转大写归一化，这是 Day 34 选择长文件名设计时就带来的差异，
 *      两个大小写不同的名字在 LMFS 里是两个不同的文件）。重名直接
 *      拒绝，不分配任何新 inode/块。
 *   4. 父目录现有块里有空闲 dirent 槽位，或者父目录至少还有一个
 *      空的 direct[] 指针槽位（可以分配一个新块）——两者都没有，
 *      父目录已经彻底满了（最多 8 个块 × 16 个槽位 = 128 个目录项，
 *      doc/lmfs.txt 第十二节提到的上限），拒绝。
 *
 * 已知、未处理的失败窗口：如果校验全部通过、需要新分配一个块挂到
 * 父目录上，而这一步之后（父目录 inode 的 file_size/direct[] 已经
 * 写回磁盘）、真正写入新 dirent 或分配新 inode 这几步里任何一步失败
 * ——父目录会多出一个指向某个块的指针，这个块已经被正确清零成全部
 * 是空闲 dirent 槽位，只是这次 create() 调用本身失败了，不会留下
 * 一条指向不存在 inode 的脏 dirent（因为 dirent 本身要等到最后才
 * 写），代价是如果失败发生在这里，父目录会"虚占"一个块（没有任何
 * 实际数据，但也没有被当成完全空闲——不是数据损坏，只是空间没有
 * 100% 物尽其用）；这个项目目前没有日志/事务机制（doc/lmfs.txt
 * 开头"关于 LMFS"milestone 提到的 v3.0 特性，v1.0 阶段不做），
 * 这是沿用 Day 36/37 同样"不做事务、只保证不产生真正损坏的状态"
 * 的既有取舍。
 *
 * 成功返回 0 并填好 *out_inode_num；上述任一校验不通过、`fs`/
 * `name`/`out_inode_num` 为 NULL、只读后端（dev->write==NULL）、或者
 * 底层读写/分配器调用失败，返回 -1。
 *
 * 范围之外：本函数只建普通文件（LMFS_TYPE_FILE）——建目录（未来的
 * mkdir 一类操作，需要 LMFS_TYPE_DIR 且新目录自己也要有"."/".."
 * 两条初始 dirent，仿照 tools/mkfs.lmfs 当初给根目录写的那两条）不
 * 在今天的范围内，计划里也没有专门的一天安排它；创建出来的文件
 * file_size 固定是 0、没有任何数据块——真正写内容是 Day 39
 * （"实现 write()"）的任务。
 */
int lmfs_create(lmfs_fs_t *fs, uint32_t parent_inode_num, const char *name,
                 uint32_t *out_inode_num);

/* lmfs_lookup(fs, parent_inode_num, name, out_inode_num)
 *
 * 在 `parent_inode_num`（必须是一个目录）的数据里找名字正好是
 * `name` 的一条已使用 lmfs_dirent_t，把它的 inode 号写进
 * *out_inode_num。是 lmfs_create() 内部"查重名"那段扫描逻辑的只读
 * 版本抽出来的小工具——Day 39 的 lmfs_vfs_write()、以及可预见的
 * Day 40 lmfs_read() 路径解析，都需要同一个"给个文件名，找到它的
 * inode 号"的操作，不该在每个地方各写一遍。
 *
 * 大小写敏感的精确匹配，范围限定（只扫 direct[8]）跟 lmfs_readdir()
 * /lmfs_create() 完全一致——理由见那两个函数声明上方的注释，不再
 * 重复。
 *
 * 找到返回 0 并填好 *out_inode_num；`fs`/`name`/`out_inode_num` 为
 * NULL、parent_inode_num 不是目录、或者没有任何一条已使用的目录项
 * 匹配这个名字，返回 -1。
 */
int lmfs_lookup(lmfs_fs_t *fs, uint32_t parent_inode_num, const char *name,
                 uint32_t *out_inode_num);

/* lmfs_write(fs, inode_num, buf, size)
 *
 * DAY39 (第二月 · 第6周 · Day 39 of the 5.0 filesystem roadmap):
 *   "实现 write()，支持直接块与间接块。" 验收："写入 10KB 文件并验证。"
 *
 * 把 `inode_num`（必须是一个普通文件）的内容整个覆盖成 `buf` 的前
 * `size` 字节——固定从字节偏移 0 开始覆盖整个文件，不是追加，新的
 * file_size 就是 `size`。这跟 fat_write()（kernel/fs/fat/fat_vfs.c，
 * Day 25）"整个覆盖、不是追加"的语义完全一致，也是这个项目 Shell
 * `write` 命令（kernel/shell/cmd/write.c）一直以来的既定含义——两种
 * 文件系统上 `write` 命令的行为应该一样，不该因为底层是 FAT 还是
 * LMFS 就表现不同。
 *
 * 为什么是"直接块与间接块"：10KB = 10240 字节，8 个直接块只能装
 * 8192 字节，必须用上一级间接块（indirect1）才能装下剩下的 2048
 * 字节（还需要 2 个数据块）——这正是验收标准特意选 10KB 而不是
 * 随便一个小于 8KB 的数字的原因：逼着这个函数真的把一级间接块这条
 * 路径走一遍，不能只实现直接块就蒙混过关。只支持到一级间接块，不
 * 支持二级间接块（indirect2）——doc/lmfs.txt 第七节当初设计 inode
 * 时虽然留了这个字段，但翻遍计划.md 全文，91 天里没有任何一天真正
 * 要求实现二级间接块的读写；本函数能支持的最大文件大小是
 * LMFS_MAX_FILE_SIZE（8 个直接块 + 256 个一级间接块指针 ≈ 264KB），
 * 超出这个大小直接拒绝，不会尝试走 indirect2。
 *
 * DAY41 UPDATE：Day 39 刚发布时，这里原本写的是"旧内容会被泄漏，
 * 等 Day 41 delete() 落地后回头修"——跟 fat_write() 自己 Day 25~26
 * 的历史（泄漏到 Day 27 才修）完全对应。现在 Day 41 已经落地：本
 * 函数内部在开始分配任何新块之前，会先用 lmfs_delete()/
 * lmfs_free_inode() 共享的同一套释放逻辑，把这个 inode 原来占用的
 * 全部数据块（如果有的话）释放干净，再开始写新内容——旧内容不会
 * 再泄漏了。细节、为什么是"先释放旧的、再分配新的"这个顺序而不是
 * 反过来，见 lmfs_delete() 声明上方注释。
 *
 * 块分配/写入顺序（为什么、不是任意选的）：先把每一个数据块的内容
 * 真正写到磁盘上，再写一级间接块自己的索引表内容，最后才把 inode
 * 的 file_size/direct[]/indirect1 一次性更新并写回——全程只在内存里
 * 累积新的指针值，直到确认一切都已经成功写盘，才在最后一步把 inode
 * 改成指向这些新内容。这样任何一步中途失败，旧的 inode 内容在磁盘
 * 上原封不动（虽然新分配、已经写了内容的块会被浪费/孤立，这个项目
 * 没有日志/事务机制，不打算为这类边界情况加回滚），不会出现"inode
 * 已经指向某个块、那个块里其实是写了一半的内容"这种更难排查的半
 * 成品状态——跟 Day 37/38 一路延续下来的"内容先、指针/位图后"是
 * 同一个原则。
 *
 * 成功返回 0；`fs`/`buf`（当 size 非 0 时）/`out` 相关参数为 NULL、
 * inode_num 对应的 inode 不存在或不是 LMFS_TYPE_FILE、size 超过
 * LMFS_MAX_FILE_SIZE、只读后端（dev->write==NULL）、或者任意一次
 * 底层分配器/读写调用失败（含卷已经写满），返回 -1。
 *
 * `size` 可以是 0（把文件清空：file_size 变 0，direct[]/indirect1
 * 全部变回 LMFS_BLOCK_NONE——合法，不分配任何块，呼应
 * lmfs_create() 新建文件本来就是这个起始状态）。
 */
int lmfs_write(lmfs_fs_t *fs, uint32_t inode_num, const void *buf,
               uint32_t size);

/* lmfs_read(fs, inode_num, buf, size, offset)
 *
 * DAY40 (第二月 · 第6周 · Day 40 of the 5.0 filesystem roadmap):
 *   "实现 read()。" 验收："read bigfile.txt 正常。"
 *
 * 从 `inode_num`（必须是一个普通文件）的第 `offset` 字节开始，最多
 * 读 `size` 字节到 `buf`，返回真正读到的字节数。
 *
 * 跟这个模块里其余函数（mount/read_inode/readdir/alloc_block/
 * alloc_inode/create/write，全部 0=成功 -1=失败）不一样，本函数的
 * 返回值约定照抄 fat_read()（kernel/fs/fat/fat_vfs.c，Day 19）：
 * 返回值是"真正读到几个字节"（0 <= n <= size），不是 0/-1——因为读
 * 操作天然有"合法的部分结果"这种中间状态（读到文件末尾、或者调用者
 * 请求的 size 比剩余字节数大），用 0/-1 表达不了"读到了 N 个字节，
 * 不多不少，这是正常情况不是错误"，fat_read() 当年就是因为同一个
 * 理由选了这个约定，LMFS 这边直接照抄而不是另起一套——两种文件
 * 系统的 fs->read() 要喂给同一个 vfs_read_fn 函数指针用（见
 * kernel/fs/vfs/vfs.h），本来就必须遵守同一份契约，不可能各算各的。
 *
 * `offset >= file_size`：返回 0——vfs_read_fn 自己文档里"0 == EOF，
 * 不是错误"的约定（kernel/fs/vfs/vfs.h），不是失败。
 *
 * 定位算法：跟 Day 39 lmfs_write() 内部走的是同一套 direct[8] + 
 * indirect1（256 个指针）的块号查找逻辑，只是这次是读不是写——
 * `offset` 换算成"第几个块、块内第几个字节"，块号 < 8 直接查
 * direct[]，>= 8 的部分第一次用到时才去读一级间接块自己的索引表
 * （只读一次，缓存在本函数内部的栈/静态变量里，不会每跨一个块就
 * 重新读一遍索引表）；读出来的内容按需要的字节数拷进 buf，必要时
 * 跨越多个块循环，直到凑够 `size` 字节或者碰到文件末尾。
 *
 * 不支持超过 LMFS_MAX_FILE_SIZE 的文件（整个项目目前也没有任何函数
 * 能写出这么大的文件——lmfs_write() 自己会拒绝，见该函数声明上方
 * 注释），所以本函数不需要、也没有处理 indirect2（二级间接块）的
 * 读取路径。
 *
 * `fs`/`buf`（size 非 0 时）为 NULL、inode_num 对应的 inode 不存在
 * 或不是 LMFS_TYPE_FILE、或者底层 dev->read() 调用失败，返回 -1
 * （负数，调用者按 "< 0 就是出错" 判断，不需要跟某个具体的负数值
 * 比较——这个项目目前没有像 fat_error_t 那样为 LMFS 建一整套独立
 * 错误码，道理跟 lmfs_vfs_create()/lmfs_vfs_write() 已经说过的一样，
 * 还没有必要）。已经拷进 buf 的部分（如果读到一半才失败）不保证
 * 有效，调用者应该忽略返回负数时 buf 的内容，这点也跟 fat_read()
 * 的约定一致。
 */
int lmfs_read(lmfs_fs_t *fs, uint32_t inode_num, void *buf, uint32_t size,
              uint32_t offset);

/* lmfs_free_block(fs, block_num)
 *
 * DAY41 (第二月 · 第6周 · Day 41 of the 5.0 filesystem roadmap):
 *   "实现 delete() 与空间回收。" 验收："rm 后空间恢复。"
 *
 * lmfs_alloc_block()（Day 36）的反向操作：把数据块位图里 `block_num`
 * 对应的 bit 清回 0（空闲）。只管这一个 bit，不管这个块原来的内容
 * （不清零、不覆盖）——跟 lmfs_alloc_block() 自己"分配出来的块内容
 * 不保证是什么，谁用谁负责清零"是同一个"只管位图、不管内容"的分工，
 * 方向相反。
 *
 * `block_num == LMFS_BLOCK_NONE(0)`——这个永久保留的哨兵值从来不是
 * 一次真正的分配，直接拒绝（释放一个从未被分配过的块号没有意义，
 * 调用这个函数传 0 大概率是调用者的逻辑错误，不是想释放"块号 0"）。
 * `block_num >= fs->sb.data_block_count`——超出这个卷的数据块总数，
 * 同样拒绝。
 *
 * 成功返回 0；`fs` 为 NULL、`fs` 未挂载、block_num 不合法（上述两种
 * 情况）、只读后端、或者底层读写位图扇区失败，返回 -1。
 */
int lmfs_free_block(lmfs_fs_t *fs, uint32_t block_num);

/* lmfs_free_inode(fs, inode_num)
 *
 * DAY41 同上。lmfs_alloc_inode()（Day 37）的反向操作：把 inode 位图
 * 里 `inode_num` 对应的 bit 清回 0，同时把 inode 表里这个槽位的内容
 * 整个重置成空闲状态（type=LMFS_TYPE_FREE、file_size/created_time/
 * 全部块指针清零——内容上就是 tools/mkfs.lmfs 里 build_free_inode()
 * 产出的那个样子）。两步都做、不是只翻位图——理由跟 lmfs_alloc_
 * inode() 当初"分配时必须同时初始化内容"是同一条不变式的反向应用：
 * doc/lmfs.txt 第八节定下的"type==FREE 要跟位图这一位是 0 保持
 * 一致"，释放的时候同样不能只改一半。
 *
 * `inode_num == LMFS_ROOT_INODE_NUM(0)`——根目录永远不会被释放，直接
 * 拒绝（这个项目没有"删除根目录"的概念，调用这个函数传 0 一定是
 * 调用者的逻辑错误）。`inode_num >= fs->sb.inode_count`——超出这个
 * 卷的 inode 总数，同样拒绝。
 *
 * 不负责释放这个 inode 原本占用的数据块——那是 lmfs_delete() 调用
 * 本函数之前就该做完的事（见该函数声明上方注释里的顺序说明），
 * 本函数只管 inode 这一侧。
 *
 * 成功返回 0；`fs` 为 NULL、`fs` 未挂载、inode_num 不合法（上述两种
 * 情况）、只读后端、或者底层读写失败，返回 -1。
 */
int lmfs_free_inode(lmfs_fs_t *fs, uint32_t inode_num);

/* lmfs_delete(fs, parent_inode_num, name)
 *
 * DAY41 同上。在 `parent_inode_num`（必须是一个目录）里删除名为
 * `name` 的普通文件：目录项标记为空闲、文件占用的全部数据块和它
 * 自己的 inode 都释放回各自的位图。
 *
 * 操作顺序——跟 fat_delete_file()（Day 26）刻意相反，原因是两种
 * 文件系统的磁盘结构本质不同：
 * ------------------------------------------------------------------
 *   FAT 的目录项自己就存着簇链起点（DIR_FstClusHI/LO），目录项是
 *   "可见性标记"和"数据指针记录"合二为一的唯一记录——所以
 *   fat_delete_file() 必须先走簇链释放数据，最后才标记目录项删除：
 *   反过来做的话，目录项一旦被标记删除，簇链起点这个唯一记录就
 *   丢了，没法再找到要释放的是哪些簇。
 *
 *   LMFS 把这两个角色拆开了：目录项（lmfs_dirent_t）只存"名字 ->
 *   inode 号"，真正的数据块指针（direct[]/indirect1）存在 inode
 *   表里，用 inode 号就能直接找到，不依赖目录项是否还在。这意味着
 *   LMFS 可以、而且应该先清掉目录项（让文件立刻从 lmfs_readdir()/
 *   lmfs_lookup() 的结果里消失），再去释放数据块和 inode——如果
 *   反过来先释放数据块/inode、最后才清目录项，一旦中途失败在
 *   "inode 已经释放、目录项还没清"这一步，这个已经被释放、位图显示
 *   空闲的 inode 号就可能被后续某次 lmfs_create() 重新分配给一个
 *   全新、完全无关的文件，而旧的目录项还继续指着这同一个 inode 号
 *   ——变成两个不同名字的文件暗地里共享同一个 inode，是真正的数据
 *   混淆，不只是空间泄漏。先清目录项再释放，就不存在这个问题：
 *   目录项一旦清掉，这个 inode 号在被成功释放、重新分配出去之前，
 *   没有任何路径能再找到它，后续任何一步失败最多是"这块空间该收
 *   回没收回"（已知、可接受的泄漏，这个项目没有日志/事务机制），
 *   不会是两个文件共享一个 inode 这种更严重的问题。
 * ------------------------------------------------------------------
 *
 * 具体步骤：
 *   1. 在父目录现有的块里扫描，找到名字匹配 `name` 的已使用目录项
 *      （记下它的位置：哪个块、块内第几个槽位），读出它的 inode 号。
 *      没找到，返回 -1。
 *   2. 读这个 inode，必须是 LMFS_TYPE_FILE——目录（比如根目录自己
 *      的 "."/".."，两条目录项都指向 inode 0）不允许被删除，这个
 *      项目没有删除目录的概念（计划.md 91 天里也没有任何一天要求
 *      实现 mkdir，自然也不需要对应的删除）。不是文件，返回 -1。
 *   3. 把这条目录项标记为空闲（used=0），写回磁盘——到这一步，
 *      `rm` 的用户可见效果已经生效：`ls`/`lmfs_lookup()` 立刻再也
 *      找不到这个文件了。
 *   4. 释放这个 inode 的全部数据块（direct[8] 里每一个非 
 *      LMFS_BLOCK_NONE 的块、indirect1 索引表里每一个非 
 *      LMFS_BLOCK_NONE 的块、以及 indirect1 索引块自己）。
 *   5. 释放这个 inode 本身（lmfs_free_inode()）。
 *
 * 第 3 步成功之后，`name` 已经不可见——第 4/5 步任何一步失败都
 * 返回 -1，但那只代表"空间没有完全回收干净"，不代表删除没有生效
 * （文件已经找不到了）；这个项目目前没有办法在一次函数调用里区分
 * "删除生效但回收不完整"和"删除彻底失败"这两种不同程度的 -1，这是
 * 已知的、可以接受的简化（跟这个模块其余函数一贯"失败就是 -1，不
 * 细分原因"的风格一致）。
 *
 * 成功返回 0；`fs`/`name` 为 NULL、parent_inode_num 不是目录、名字
 * 不存在、目标不是普通文件、或者任意一步底层读写/释放调用失败，
 * 返回 -1。
 */
int lmfs_delete(lmfs_fs_t *fs, uint32_t parent_inode_num, const char *name);
/* ===================================================================
 * DAY42 (第二月 · 第6周 · Day 42 of the 5.0 filesystem roadmap):
 *   "多级目录与路径解析。" 验收："可以创建并访问多级目录。"
 *
 * 前 41 天里，LMFS 所有"按名字找东西"的函数（lmfs_lookup/lmfs_create/
 * lmfs_delete）都只认【一个目录 inode + 一个纯文件名】，路径解析全靠
 * lmfs_vfs.c 把"<挂载点>/<文件名>"切一刀、父目录硬编码成根目录——
 * 所以整个 LMFS 实际上一直是"扁平"的。Day 42 补上两块拼图：
 *
 *   1. lmfs_mkdir()：真正创建一个子目录（LMFS_TYPE_DIR 的 inode，
 *      自带 "." 和 ".." 两条目录项，跟 tools/mkfs.lmfs 当初给根目录
 *      写的那两条是同一个形状）。
 *   2. lmfs_resolve_path()/lmfs_resolve_parent()：把 "a/b/c" 这样的
 *      多级路径，一段一段用 lmfs_lookup() 往下走，换算成最终的
 *      inode 号（或者"最后一段的父目录 inode + 最后一段的名字"）。
 *
 * 磁盘格式【完全没有改】——目录本来就是"数据块里一串 lmfs_dirent_t"，
 * 子目录和根目录在磁盘上是同一种东西，所以 Day 42 不动 mkfs.lmfs、
 * 不动超级块/inode/dirent 的任何字段。
 * =================================================================== */

/* lmfs_mkdir(fs, parent_inode_num, name, out_inode_num)
 *
 * 在 `parent_inode_num`（必须是目录）里新建名为 `name` 的子目录，
 * 成功时把新目录的 inode 号写进 *out_inode_num（可传 NULL）。
 *
 * 跟 lmfs_create() 共用同一套校验与"挂进父目录"的实现（内部是同一个
 * 静态函数 create_node()，只是 type 不同），多做的只有"目录自己要有
 * 内容"这一步：
 *   - 分配一个 DIR 类型的 inode；
 *   - 分配 1 个数据块，写入两条目录项：
 *         "."  -> 新目录自己的 inode
 *         ".." -> parent_inode_num
 *   - 把新 inode 的 file_size=1024、direct[0]=该块 写回；
 *   - 最后才把 (name -> 新 inode) 登记进父目录（"内容先、可见性后"，
 *     跟 Day 37/38/39 同一条写盘顺序原则）。
 *
 * name 规则同 lmfs_create()：1~55 字符、不含 '/'；另外 "." 和 ".."
 * 是保留名（它们由 mkdir 自动生成），不允许 create/mkdir。
 *
 * 成功返回 0；任何校验失败（重名、父目录不是目录、父目录满了、名字
 * 非法）、只读后端或底层读写/分配失败返回 -1。失败时如果新 inode/块
 * 已经分配出去，会尽力释放（best-effort，这个项目没有事务/日志）。
 */
int lmfs_mkdir(lmfs_fs_t *fs, uint32_t parent_inode_num, const char *name,
               uint32_t *out_inode_num);

/* lmfs_resolve_path(fs, start_inode_num, path, out_inode_num)
 *
 * 路径解析：从 `start_inode_num`（必须是目录；相对路径的起点）出发，
 * 按 '/' 切分 `path`，每一段调用 lmfs_lookup() 往下走一层，最终的
 * inode 号写进 *out_inode_num。
 *
 * 规则：
 *   - 以 '/' 开头：忽略 start_inode_num，从根目录（inode 0）开始。
 *   - 连续的 '/'、结尾的 '/' 都被忽略；空路径 "" 返回 start_inode_num。
 *   - "." 和 ".." 不做特殊处理——它们在磁盘上就是目录里真实存在的两条
 *     目录项（根目录的 ".." 指向自己，所以 "/.." 还是根），解析器只
 *     是把它们当普通名字去 lookup。这正是 Day 34 把 "."/".." 做成真实
 *     目录项而不是"魔法字符串"的回报。
 *   - 中间每一段必须是目录；最后一段可以是文件或目录（调用者自己检查
 *     想要的类型）。
 *   - 任何一段长度超过 LMFS_NAME_MAX、或找不到 -> 失败。
 *
 * 成功返回 0；失败（某段不存在/中间段不是目录/参数为 NULL/读盘失败）
 * 返回 -1。
 */
int lmfs_resolve_path(lmfs_fs_t *fs, uint32_t start_inode_num,
                      const char *path, uint32_t *out_inode_num);

/* lmfs_resolve_parent(fs, start_inode_num, path, out_parent_inode,
 *                     out_name)
 *
 * 给 create/mkdir/delete 用的"只解析到倒数第二段"：把 `path` 拆成
 * "目录部分" + "最后一段名字"，目录部分用 lmfs_resolve_path() 解析成
 * 父目录 inode（必须真的是目录），最后一段名字拷进 out_name。
 *
 *     "a/b/c.txt"  -> parent = inode(a/b), name = "c.txt"
 *     "c.txt"      -> parent = start_inode_num, name = "c.txt"
 *     "/c.txt"     -> parent = 根目录,          name = "c.txt"
 *     "a/b/"       -> 结尾的 '/' 被忽略，等价 "a/b"
 *
 * 最后一段为空（路径是 "" 或 "/"）、路径总长超过内部缓冲区（127）、
 * 名字超过 LMFS_NAME_MAX、父目录解析失败或不是目录，返回 -1。最后一段
 * 可以是 "." 或 ".."——要不要拒绝由调用者决定（create/mkdir/delete 自己
 * 都会拒绝）。
 */
int lmfs_resolve_parent(lmfs_fs_t *fs, uint32_t start_inode_num,
                        const char *path, uint32_t *out_parent_inode,
                        char out_name[LMFS_NAME_MAX + 1]);

#endif /* LUMA_LMFS_H */
