/* user/programs/malloc.c - DAY73 自测程序：`run malloc.sw`
 * 验收："用户态动态内存。"  每条检查失败打印 "FAIL line N" 到 stderr；最后一行给结论。
 * 末尾的 "heap: ..." 行供测试脚本和人看。 */
#include "../include/luma.h"

static int bad;
#define CHECK(c) do { if (!(c)) { dprintf(STDERR_FILENO, "FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)
#define HEAPOK() CHECK(malloc_check() == 0)

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return (rng >> 8) & 0xFFFFFF; }

static void fill(unsigned char *p, size_t n, unsigned char seed) { for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(seed + i * 7); }
static int verify(const unsigned char *p, size_t n, unsigned char seed)
{ for (size_t i = 0; i < n; i++) if (p[i] != (unsigned char)(seed + i * 7)) return 0; return 1; }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* 1. sbrk / brk 基本行为 */
    void *b0 = sbrk(0);
    CHECK(b0 != (void *)-1 && ((unsigned)b0 & 0xFFF) == 0);          /* 堆起点页对齐 */
    char *p = sbrk(100);
    CHECK(p == b0);
    CHECK(sbrk(0) == (char *)b0 + 100);
    p[0] = 'a'; p[99] = 'z';                                          /* 刚映射的页可写 */
    CHECK(brk((char *)b0 + 200) == 0 && sbrk(0) == (char *)b0 + 200);
    errno = 0; CHECK(brk((char *)b0 - 4096) == -1 && errno == EINVAL);  /* 不能缩到堆起点以下 */
    errno = 0; CHECK(sbrk(0x7FFFFFF0) == (void *)-1 && errno == ENOMEM);
    CHECK(brk(b0) == 0 && sbrk(0) == b0);                            /* 还原 */

    /* 2. 基本分配：对齐、互不重叠、内容保持 */
    enum { N = 40 };
    unsigned char *a[N]; size_t sz[N];
    for (int i = 0; i < N; i++) {
        sz[i] = 1 + (size_t)(i * 37 % 200);
        a[i] = malloc(sz[i]);
        CHECK(a[i] != NULL && ((unsigned)a[i] & 7) == 0);
        fill(a[i], sz[i], (unsigned char)i);
    }
    HEAPOK();
    for (int i = 0; i < N; i++) CHECK(verify(a[i], sz[i], (unsigned char)i));
    for (int i = 0; i < N; i += 2) free(a[i]);                        /* 释放偶数号：留下空洞 */
    HEAPOK();
    for (int i = 1; i < N; i += 2) CHECK(verify(a[i], sz[i], (unsigned char)i));   /* 奇数号不受影响 */
    for (int i = 1; i < N; i += 2) free(a[i]);
    HEAPOK();
    malloc_info_t mi; malloc_info(&mi);
    CHECK(mi.nused == 0 && mi.nfree == 0 && mi.used == 0);            /* 全释放 -> 全部并回 top（合并生效）*/

    /* 3. 释放后复用：同样大小的块应得到同一地址 */
    void *x = malloc(64), *y = malloc(64), *z = malloc(64);
    free(y);
    void *y2 = malloc(64);
    CHECK(y2 == y);                                                   /* 空洞被复用 */
    free(x); free(y2); free(z);
    HEAPOK();

    /* 4. 合并：三块相邻，乱序释放，然后能装下一块 3 倍大小的而不用增长堆 */
    void *c1 = malloc(1000), *c2 = malloc(1000), *c3 = malloc(1000), *guard = malloc(16);
    malloc_info(&mi); size_t heap_before = mi.heap;
    free(c1); free(c3); free(c2);                                     /* c3 先于 c2：c2 同时与前后合并 */
    HEAPOK();
    void *big = malloc(3000);
    CHECK(big == c1);                                                 /* 三块合成一块，从 c1 的位置开始 */
    malloc_info(&mi); CHECK(mi.heap == heap_before);
    free(big); free(guard);
    HEAPOK();

    /* 5. calloc：清零；乘法溢出 -> NULL */
    unsigned char *z0 = malloc(512); memset(z0, 0xAA, 512); free(z0);
    unsigned char *cz = calloc(64, 8);                                /* 很可能复用刚才脏的块：必须清零 */
    int zero = 1; for (int i = 0; i < 512; i++) if (cz[i]) zero = 0;
    CHECK(zero);
    free(cz);
    errno = 0; CHECK(calloc(0x10000, 0x10000) == NULL && errno == ENOMEM);   /* 2^32 溢出 */
    errno = 0; CHECK(calloc(0x40000000u, 8) == NULL && errno == ENOMEM);

    /* 6. realloc：增长保留内容、缩小保留前缀、NULL/0 的特殊情形 */
    unsigned char *r = malloc(100); fill(r, 100, 5);
    r = realloc(r, 5000); CHECK(r && verify(r, 100, 5));              /* 增长（可能原地也可能搬家）*/
    unsigned char *keep = malloc(32);                                 /* 挡在后面，强迫下面这次搬家 */
    r = realloc(r, 20000); CHECK(r && verify(r, 100, 5));
    r = realloc(r, 40); CHECK(r && verify(r, 40, 5));                 /* 缩小 */
    HEAPOK();
    void *rn = realloc(NULL, 24);                                     /* 等同 malloc */
    CHECK(rn != NULL);
    CHECK(realloc(r, 0) == NULL);                                     /* 等同 free */
    free(keep); free(rn);
    HEAPOK();
    unsigned char *g = malloc(10); fill(g, 10, 9);
    for (int k = 0; k < 50; k++) { g = realloc(g, 10 + (size_t)k * 13); CHECK(g && verify(g, 10, 9)); }   /* 逐步增长（常原地）*/
    free(g);
    HEAPOK();

    /* 7. 随机压力：3000 次 malloc/free/realloc，所有块带图案，随时校验，定期检查堆结构 */
    enum { SLOTS = 64 };
    unsigned char *s[SLOTS]; size_t ss[SLOTS]; unsigned char sd[SLOTS];
    for (int i = 0; i < SLOTS; i++) s[i] = NULL;
    int stress_ok = 1;
    for (int op = 0; op < 3000; op++) {
        int i = (int)(rnd() % SLOTS);
        unsigned what = rnd() % 3;
        if (s[i] && !verify(s[i], ss[i], sd[i])) { stress_ok = 0; break; }
        if (!s[i] || what == 0) {
            if (s[i]) free(s[i]);
            ss[i] = 1 + rnd() % 3000; sd[i] = (unsigned char)rnd();
            s[i] = malloc(ss[i]); if (!s[i]) { stress_ok = 0; break; }
            fill(s[i], ss[i], sd[i]);
        } else if (what == 1) {
            free(s[i]); s[i] = NULL;
        } else {
            size_t ns = 1 + rnd() % 3000;
            unsigned char *q = realloc(s[i], ns); if (!q) { stress_ok = 0; break; }
            size_t keepn = ns < ss[i] ? ns : ss[i];
            if (!verify(q, keepn, sd[i])) { stress_ok = 0; break; }
            s[i] = q; ss[i] = ns; fill(s[i], ns, sd[i]);
        }
        if (op % 100 == 0 && malloc_check() != 0) { stress_ok = 0; break; }
    }
    CHECK(stress_ok);
    for (int i = 0; i < SLOTS; i++) free(s[i]);
    HEAPOK();
    malloc_info(&mi); CHECK(mi.nused == 0);

    /* 8. 大块 + 归还：申请 2MB，用满，释放；堆应缩回（break 回落）*/
    char *brk_a = sbrk(0);
    unsigned char *huge = malloc(2 * 1024 * 1024);
    CHECK(huge != NULL);
    if (huge) { fill(huge, 2 * 1024 * 1024, 1); CHECK(verify(huge, 2 * 1024 * 1024, 1)); }
    char *brk_b = sbrk(0);
    CHECK(brk_b - brk_a >= 2 * 1024 * 1024 - 64 * 1024);     /* 顶部原有的空闲块抵掉一部分 */
    free(huge);
    char *brk_c = sbrk(0);
    CHECK(brk_c < brk_b && (brk_c - brk_a) <= 64 * 1024);   /* 还给了内核 */
    HEAPOK();

    /* 9. 耗尽：一直要 256KB 直到 NULL（errno=ENOMEM），然后全释放，再次能分配 */
    enum { MAXB = 64 };
    void *blk[MAXB]; int nb = 0;
    while (nb < MAXB) { blk[nb] = malloc(256 * 1024); if (!blk[nb]) break; ((char *)blk[nb])[0] = 1; nb++; }
    CHECK(nb > 4 && nb < MAXB);                                       /* 8MB 上限必然先到 */
    CHECK(errno == ENOMEM);
    HEAPOK();
    for (int i = 0; i < nb; i++) free(blk[i]);
    HEAPOK();
    void *again = malloc(1024 * 1024); CHECK(again != NULL); free(again);
    printf("exhaustion: got %d blocks of 256KB before ENOMEM\n", nb);

    /* 10. 块的 8 字节对齐 + malloc(0) 可 free */
    void *z1 = malloc(0); void *z2 = malloc(0);
    CHECK(z1 && z2 && z1 != z2); free(z1); free(z2);
    HEAPOK();

    malloc_info(&mi);
    printf("heap: %u bytes, used %u in %u blocks, free %u in %u blocks, top %u\n",
           (unsigned)mi.heap, (unsigned)mi.used, (unsigned)mi.nused, (unsigned)mi.free_bytes, (unsigned)mi.nfree, (unsigned)mi.top);

    /* 11. 故意重复释放：库应报错并以 134 退出（测试脚本据此判断；用 `run malloc.sw crash` 触发）*/
    if (argc > 1 && argv[1][0] == 'c') {
        void *d = malloc(40); free(d); free(d);
        puts("NOT REACHED");
    }
    if (argc > 1 && argv[1][0] == 'w') {                              /* 越过 break 写：应触发 #PF，程序被杀 */
        volatile char *e = (char *)sbrk(0) + 4096 * 4;
        *e = 1;
        puts("NOT REACHED");
    }
    puts(bad ? "malloc.sw: FAILED" : "malloc.sw: all checks OK");
    return bad;
}
