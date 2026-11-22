/* user/lib/malloc.c - DAY73: 用户态动态内存 malloc / free / calloc / realloc / sbrk / brk
 *
 * ===================================================================================
 *  1. 内核给了什么：SYS_BRK（kernel/syscall/syscall.h 的 DAY73 一节）
 * ===================================================================================
 *   程序的地址空间里只有 ELF 段、栈。堆 = 段末尾之后的一片“按需映射”的区域，由 program break 划界：
 *   [堆起点, break) 里的页都已映射、可读写、初始全 0；break 之上不存在（访问 = #PF，程序被杀）。
 *   brk(addr) 移动 break：内核给新增的整页映射页框，或把多出的整页还给页框池。堆最大 8MB，
 *   而且整个系统的页框池只有 8MB，所以耗尽 -> -ENOMEM，malloc 返回 NULL，不会崩。
 *
 * ===================================================================================
 *  2. 分配器：边界标记 + 空闲链表 + “顶部块”（Doug Lea dlmalloc 的精简版）
 * ===================================================================================
 *   堆被切成首尾相接的“块（chunk）”，每块前面有 8 字节头：
 *
 *        +-----------+-----------+-----------------------------+
 *        | prev_size |   size|P  |  用户数据 ...（空闲时前 8 字节放 fd/bk 链表指针）|
 *        +-----------+-----------+-----------------------------+
 *        ^ 块起点                 ^ malloc 返回的指针 = 块起点 + 8（块起点 8 字节对齐 -> 用户指针也 8 对齐）
 *
 *     size      本块总大小（含头，8 的倍数，最小 16）；最低位 P = “前一块在使用”（PREV_INUSE）。
 *               一个块自己是否空闲，不存在自己头里，而是看“下一块”头里的 P 位——所以已分配块没有尾部开销。
 *     prev_size 只有当前一块空闲时才有意义：存前一块的大小，这样 free 时能从本块直接“往回走”找到前一块
 *               （这就是“边界标记 / boundary tag”，让合并相邻空闲块是 O(1)）。前一块在使用时，这 4 字节
 *               是前一块用户数据的最后 4 个字节（省空间）。
 *   空闲块用双向链表串起来（fd/bk 放在用户数据区，所以最小块 = 8 头 + 8 指针 = 16）。
 *   堆的最高处永远有一个“顶部块 top”：它是尚未分配的、紧贴 break 的空闲块，不进链表。
 *
 *   malloc(n)：
 *     1. 请求 n -> 块大小 nb = max(16, 向上取 8 的倍数(n + 8))。n 巨大（会溢出）-> ENOMEM。
 *     2. 在空闲链表里找“最合适”的块（best-fit：够大里最小的；恰好相等立即停）。找到 -> 拆开，
 *        剩下 >= 16 字节的另成一个空闲块，否则整块给出去。
 *     3. 链表里没有 -> 从 top 切。top 不够 -> sbrk 增长堆（一次至少 32KB，减少系统调用）。
 *        sbrk 失败 -> 返回 NULL，errno=ENOMEM，堆保持原样。
 *   free(p)：
 *     1. 校验：p 8 对齐、落在 [堆起点, top)、下一块的 P 位为 1（= 本块确实在用）。否则这是野指针或重复释放，
 *        打印 "free(): invalid pointer" 并 exit(134)（与其悄悄破坏堆，不如当场死掉）。
 *     2. 向后合并：前一块空闲 -> 从链表摘下，并入本块。向前合并：下一块是 top -> 本块并入 top；下一块空闲 -> 摘下并入。
 *        因此堆里永远没有两个相邻的空闲块（malloc_check 会验证）。
 *     3. 并入 top 后如果 top >= 128KB，把多余的页用负的 sbrk 还给内核（留 32KB），所以“先申请很多再全释放”
 *        之后，进程占的页框会回落。
 *   realloc：能原地解决就原地（缩小：切掉尾巴；增长：吞并后面的空闲块或 top），否则 malloc 新块 + memcpy + free 旧块。
 *   calloc：malloc 之后清零（a*b 先查溢出）。
 *
 *   复杂度：malloc 在空闲块很多时是 O(空闲块数)（线性扫描链表）；free 是 O(1)。对小程序足够，换成分级链表是以后的优化。
 *   线程安全：LumaOS 现在一个程序只有一个执行流，所以没有锁。
 *   不要在 malloc 之外用 sbrk 占用堆：分配器假定 break 就是它自己堆的末端；发现 break 被别人挪动，malloc 会失败而不是乱来。
 */
#include "../include/luma.h"

/* ---- sbrk / brk ---- */
int brk(void *addr)
{
    int r = luma_syscall1(SYS_BRK, (int)addr);
    if (r < 0) { errno = -r; return -1; }
    return 0;
}

void *sbrk(int incr)
{
    int cur = luma_syscall1(SYS_BRK, 0);                 /* 当前 break */
    if (cur < 0) { errno = -cur; return (void *)-1; }
    if (incr == 0)
        return (void *)cur;
    long long want = (long long)(unsigned)cur + incr;    /* 在 64 位里算，防回绕（只加减，不需要 libgcc） */
    if (want < 0 || want > 0x7FFFFFFFll) { errno = ENOMEM; return (void *)-1; }
    int r = luma_syscall1(SYS_BRK, (int)want);
    if (r < 0) { errno = -r; return (void *)-1; }
    return (void *)cur;
}

/* ---- 数据结构 ---- */
typedef struct chunk {
    unsigned prev_size;
    unsigned size;                 /* 总大小 | PREV_INUSE */
} chunk_t;
typedef struct fchunk {            /* 空闲块：头 + 链表指针 */
    unsigned prev_size, size;
    struct fchunk *fd, *bk;
} fchunk_t;

#define PREV_INUSE   1u
#define HDR          8u
#define MINCHUNK     16u
#define CSIZE(c)     ((c)->size & ~7u)
#define NEXT(c)      ((chunk_t *)((char *)(c) + CSIZE(c)))
#define PREV(c)      ((chunk_t *)((char *)(c) - (c)->prev_size))
#define MEM(c)       ((void *)((char *)(c) + HDR))
#define CHUNK(p)     ((chunk_t *)((char *)(p) - HDR))
#define GROW_MIN     (32u * 1024u)
#define TRIM_ABOVE   (128u * 1024u)
#define TRIM_KEEP    (32u * 1024u)

static chunk_t  *heap_base;        /* 第一个块（NULL = 堆还没初始化）*/
static chunk_t  *top;              /* 顶部块 */
static fchunk_t *free_list;        /* 空闲块链表头（不含 top）*/

static void list_insert(fchunk_t *f)
{
    f->bk = NULL; f->fd = free_list;
    if (free_list) free_list->bk = f;
    free_list = f;
}
static void list_remove(fchunk_t *f)
{
    if (f->bk) f->bk->fd = f->fd; else free_list = f->fd;
    if (f->fd) f->fd->bk = f->bk;
}

static int heap_init(void)
{
    char *cur = sbrk(0);
    if (cur == (char *)-1)
        return -1;
    unsigned pad = (8u - ((unsigned)cur & 7u)) & 7u;      /* break 起点是页对齐的；仍然保证 8 对齐 */
    char *p = sbrk((int)(GROW_MIN + pad));
    if (p == (char *)-1)
        return -1;
    heap_base = top = (chunk_t *)(p + pad);
    top->prev_size = 0;
    top->size = GROW_MIN | PREV_INUSE;
    return 0;
}

/* 让 top 至少有 need 字节（need 已含头）。成功返回 0。 */
static int top_grow(unsigned need)
{
    if (CSIZE(top) >= need)
        return 0;
    unsigned inc = need - CSIZE(top);
    if (inc < GROW_MIN) inc = GROW_MIN;
    inc = (inc + 4095u) & ~4095u;
    char *end = (char *)top + CSIZE(top);
    if (sbrk(0) != (void *)end) {                         /* break 被别人挪过 */
        errno = ENOMEM;
        return -1;
    }
    unsigned min_inc = (need - CSIZE(top) + 4095u) & ~4095u;
    if (sbrk((int)inc) == (void *)-1) {
        if (inc == min_inc || sbrk((int)min_inc) == (void *)-1) { errno = ENOMEM; return -1; }
        inc = min_inc;
    }
    top->size = (CSIZE(top) + inc) | (top->size & PREV_INUSE);
    return 0;
}

static void bad_pointer(const char *why)
{
    dputs("free(): ", STDERR_FILENO);
    dputs(why, STDERR_FILENO);
    dputs("\n", STDERR_FILENO);
    exit(134);
}

/* c 是刚腾出来的块（c->size 的 P 位已正确）：并入前后空闲邻居，放进链表或并入 top。 */
static void release(chunk_t *c)
{
    if (!(c->size & PREV_INUSE)) {                        /* 向后合并 */
        chunk_t *p = PREV(c);
        list_remove((fchunk_t *)p);
        p->size = (CSIZE(p) + CSIZE(c)) | (p->size & PREV_INUSE);
        c = p;
    }
    chunk_t *n = NEXT(c);
    if (n == top) {                                       /* 并入 top */
        c->size = (CSIZE(c) + CSIZE(top)) | (c->size & PREV_INUSE);
        top = c;
        unsigned ts = CSIZE(top);
        if (ts >= TRIM_ABOVE) {                           /* 把多余的页还给内核 */
            unsigned give = (ts - TRIM_KEEP) & ~4095u;
            char *end = (char *)top + ts;
            if (sbrk(0) == (void *)end && sbrk(-(int)give) != (void *)-1)
                top->size = (ts - give) | (top->size & PREV_INUSE);
        }
        return;
    }
    chunk_t *nn = NEXT(n);
    if (!(nn->size & PREV_INUSE)) {                       /* 下一块也空闲：吞并 */
        list_remove((fchunk_t *)n);
        c->size = (CSIZE(c) + CSIZE(n)) | (c->size & PREV_INUSE);
        n = NEXT(c);
    }
    n->prev_size = CSIZE(c);
    n->size &= ~PREV_INUSE;
    list_insert((fchunk_t *)c);
}

static unsigned request2size(size_t n)
{
    unsigned nb = ((unsigned)n + HDR + 7u) & ~7u;
    return nb < MINCHUNK ? MINCHUNK : nb;
}

/* 把空闲块 c（已从链表摘下，大小 >= nb）切出 nb 字节给调用者，余下的 >= 16 字节另成空闲块。 */
static void *take(chunk_t *c, unsigned nb)
{
    unsigned cs = CSIZE(c);
    if (cs - nb >= MINCHUNK) {
        chunk_t *r = (chunk_t *)((char *)c + nb);
        c->size = nb | (c->size & PREV_INUSE);
        r->size = (cs - nb) | PREV_INUSE;                 /* r 前面是刚分配出去的 c */
        {
            chunk_t *n = NEXT(r);
            if (n == top) {                               /* r 紧贴 top：并入 top，不进链表 */
                r->size = (CSIZE(r) + CSIZE(top)) | PREV_INUSE;
                top = r;
            } else {
                n->prev_size = CSIZE(r);
                n->size &= ~PREV_INUSE;
                list_insert((fchunk_t *)r);
            }
        }
    } else {
        NEXT(c)->size |= PREV_INUSE;
    }
    return MEM(c);
}

void *malloc(size_t n)
{
    if (n > 0x7FFFFF00u) { errno = ENOMEM; return NULL; }
    if (!heap_base && heap_init() != 0) { errno = ENOMEM; return NULL; }
    unsigned nb = request2size(n);

    fchunk_t *best = NULL;
    for (fchunk_t *f = free_list; f; f = f->fd) {
        unsigned fs = CSIZE((chunk_t *)f);
        if (fs >= nb && (!best || fs < CSIZE((chunk_t *)best))) {
            best = f;
            if (fs == nb) break;
        }
    }
    if (best) {
        list_remove(best);
        return take((chunk_t *)best, nb);
    }
    if (top_grow(nb + MINCHUNK) != 0)                     /* top 切走后至少还剩 16 字节（保证 top 永远有头）*/
        return NULL;
    chunk_t *c = top;
    unsigned ts = CSIZE(top);
    chunk_t *nt = (chunk_t *)((char *)c + nb);
    nt->size = (ts - nb) | PREV_INUSE;
    c->size = nb | (c->size & PREV_INUSE);
    top = nt;
    return MEM(c);
}

void free(void *p)
{
    if (!p)
        return;
    if (!heap_base || ((unsigned)p & 7u) || (char *)p < (char *)heap_base + HDR || (char *)p >= (char *)top)
        bad_pointer("invalid pointer");
    chunk_t *c = CHUNK(p);
    unsigned cs = CSIZE(c);
    if (cs < MINCHUNK || (char *)c + cs > (char *)top || !(NEXT(c)->size & PREV_INUSE))
        bad_pointer("invalid pointer or double free");
    release(c);
}

void *calloc(size_t a, size_t b)
{
    unsigned long long total = (unsigned long long)a * b;       /* 32x32->64 乘法不需要 libgcc */
    if (total > 0x7FFFFF00u) { errno = ENOMEM; return NULL; }
    void *p = malloc((size_t)total);
    if (p) memset(p, 0, (size_t)total);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    if (n > 0x7FFFFF00u) { errno = ENOMEM; return NULL; }
    if (!heap_base || ((unsigned)p & 7u) || (char *)p < (char *)heap_base + HDR || (char *)p >= (char *)top)
        bad_pointer("invalid pointer (realloc)");
    chunk_t *c = CHUNK(p);
    unsigned cs = CSIZE(c), nb = request2size(n);
    if (!(NEXT(c)->size & PREV_INUSE))
        bad_pointer("invalid pointer or double free (realloc)");

    if (nb <= cs) {                                       /* 缩小（或不变）：切掉尾巴 */
        if (cs - nb >= MINCHUNK) {
            chunk_t *r = (chunk_t *)((char *)c + nb);
            c->size = nb | (c->size & PREV_INUSE);
            r->size = (cs - nb) | PREV_INUSE;
            release(r);                                   /* r 前面是 c（在用），release 会处理 top/后邻居 */
        }
        return p;
    }
    chunk_t *nx = NEXT(c);
    if (nx == top) {                                      /* 后面就是 top：向它借 */
        unsigned need = nb + MINCHUNK;                    /* 借完 top 至少还剩 16 */
        unsigned have = cs + CSIZE(top);
        if (have < need) {
            if (top_grow(need - cs) != 0) return NULL;    /* 原块不变 */
            have = cs + CSIZE(top);
        }
        chunk_t *nt = (chunk_t *)((char *)c + nb);
        nt->size = (have - nb) | PREV_INUSE;
        c->size = nb | (c->size & PREV_INUSE);
        top = nt;
        return p;
    }
    if (!(NEXT(nx)->size & PREV_INUSE) && cs + CSIZE(nx) >= nb) {   /* 后面是够大的空闲块：吞并 */
        list_remove((fchunk_t *)nx);
        unsigned total = cs + CSIZE(nx);
        chunk_t *after = (chunk_t *)((char *)c + total);
        if (total - nb >= MINCHUNK) {
            chunk_t *r = (chunk_t *)((char *)c + nb);
            c->size = nb | (c->size & PREV_INUSE);
            r->size = (total - nb) | PREV_INUSE;
            if (after == top) { r->size = (CSIZE(r) + CSIZE(top)) | PREV_INUSE; top = r; }
            else { after->prev_size = CSIZE(r); after->size &= ~PREV_INUSE; list_insert((fchunk_t *)r); }
        } else {
            c->size = total | (c->size & PREV_INUSE);
            after->size |= PREV_INUSE;
        }
        return p;
    }
    void *q = malloc(n);                                  /* 只能搬家 */
    if (!q) return NULL;
    memcpy(q, p, cs - HDR);
    free(p);
    return q;
}

/* ---- 统计与自检 ---- */
void malloc_info(malloc_info_t *mi)
{
    mi->heap = mi->used = mi->free_bytes = mi->top = mi->nused = mi->nfree = 0;
    if (!heap_base) return;
    mi->heap = (size_t)((char *)top + CSIZE(top) - (char *)heap_base);
    mi->top = CSIZE(top);
    for (chunk_t *c = heap_base; c != top; c = NEXT(c)) {
        if (NEXT(c)->size & PREV_INUSE) { mi->used += CSIZE(c); mi->nused++; }
        else                            { mi->free_bytes += CSIZE(c); mi->nfree++; }
    }
}

/* 返回 0 = 堆完好；1 块大小非法；2 块越过 top；3 PREV_INUSE/prev_size 与前一块不符；4 两个相邻空闲块（没合并）；
 * 5 空闲链表与堆里的空闲块不一致；6 top 非法 */
int malloc_check(void)
{
    if (!heap_base) return 0;
    if (CSIZE(top) < MINCHUNK || ((unsigned)top & 7u)) return 6;
    unsigned nfree = 0; int prev_free = 0; chunk_t *prev = NULL;
    for (chunk_t *c = heap_base; c != top; prev = c, c = NEXT(c)) {
        unsigned cs = CSIZE(c);
        if (cs < MINCHUNK || (cs & 7u)) return 1;
        if ((char *)c + cs > (char *)top) return 2;
        if (prev) {
            if (prev_free && (c->size & PREV_INUSE)) return 3;
            if (!prev_free && !(c->size & PREV_INUSE)) return 3;
            if (prev_free && c->prev_size != CSIZE(prev)) return 3;
        } else if (!(c->size & PREV_INUSE)) return 3;
        int this_free = !(NEXT(c)->size & PREV_INUSE);
        if (this_free && prev_free) return 4;
        if (this_free) nfree++;
        prev_free = this_free;
    }
    if (prev_free || !(top->size & PREV_INUSE)) return 4;          /* 空闲块直接贴着 top 应已并入 top */
    unsigned listed = 0;
    for (fchunk_t *f = free_list; f; f = f->fd) {
        listed++;
        if (f->fd && f->fd->bk != f) return 5;
        if (NEXT((chunk_t *)f)->size & PREV_INUSE) return 5;
        if (listed > nfree) return 5;
    }
    return listed == nfree ? 0 : 5;
}
