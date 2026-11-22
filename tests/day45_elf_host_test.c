/* tests/day45_elf_host_test.c — Day 45 host-side unit test of
 * elf_fnv1a() and elf32_load_segments() (kernel/exec/elf.c): "copy every
 * PT_LOAD to p_vaddr, zero .bss, read back".
 *
 *   gcc -g -fsanitize=address,undefined -Ikernel/exec \
 *       tests/day45_elf_host_test.c kernel/exec/elf.c -o /tmp/elf45_host_test \
 *       && /tmp/elf45_host_test tests/hello.sw tests/data.sw
 *
 * The loader writes to the ABSOLUTE addresses named in the Program Headers
 * (0x08048000 …), so the test maps real memory there with
 * mmap(MAP_FIXED_NOREPLACE) — a 64-bit host process can do that — and then
 * checks the bytes in that memory against the bytes of the "file":
 *   - real toolchain output (hello.sw: 1 PT_LOAD; data.sw: 4 PT_LOADs with
 *     .rodata/.data/.bss) if the paths are given on the command line;
 *   - synthetic ELFs for the edge cases: file sizes around the 512-byte
 *     chunk boundary, .bss over previously dirty memory, a reader that
 *     comes up short / fails, a fabricated elf_image_t that must be refused
 *     BEFORE any write (the refused addresses are deliberately NOT mapped, so
 *     a stray write would crash the test), tampered derived fields.
 * The reader callback records every (offset, length) it is asked for, so the
 * test also proves the loader never reads outside [p_offset, p_offset+filesz).
 * The "readback mismatch" path (no RAM at the load address) cannot be
 * produced on a host; tests/day45_test_elf_map.py exercises it in QEMU with
 * `-m 64`, where 0x08048000 is not RAM.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "elf.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define A   0x08048000u
#define RX  (PF_R | PF_X)
#define RW  (PF_R | PF_W)
#define ARENA_LO  0x08000000u        /* mapped: [ARENA_LO, ARENA_LO+ARENA_SZ) */
#define ARENA_SZ  0x02000000u        /* 32 MB covers 0x08048000 + a lot       */
#define MEM(a)    ((volatile unsigned char *)(uintptr_t)(a))

/* ---- tiny ELF builder (same layout as day44_elf_host_test.c) ------------- */
typedef struct { uint32_t type, off, vaddr, filesz, memsz, flags, align; } seg_t;

static void w16(unsigned char *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* Header + n Program Headers at e_phoff=52; the rest of the file is a
 * position-dependent byte pattern so any misplaced byte is detectable. */
static unsigned char *build(const seg_t *s, int n, uint32_t entry, uint32_t file_size)
{
    unsigned char *b = malloc(file_size);
    for (uint32_t i = 0; i < file_size; i++) b[i] = (unsigned char)((i * 7 + 1) & 0xFF);
    memset(b, 0, 52 + 32 * n);
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 1; b[5] = 1; b[6] = 1;
    w16(b + 16, ET_EXEC); w16(b + 18, EM_386); w32(b + 20, 1);
    w32(b + 24, entry); w32(b + 28, 52); w32(b + 32, 0); w32(b + 36, 0);
    w16(b + 40, 52); w16(b + 42, 32); w16(b + 44, n); w16(b + 46, 40);
    for (int i = 0; i < n; i++) {
        unsigned char *p = b + 52 + 32 * i;
        w32(p + 0, s[i].type);  w32(p + 4, s[i].off);   w32(p + 8, s[i].vaddr);
        w32(p + 12, s[i].vaddr); w32(p + 16, s[i].filesz); w32(p + 20, s[i].memsz);
        w32(p + 24, s[i].flags); w32(p + 28, s[i].align);
    }
    return b;
}

/* ---- the "file" reader: records every request ---------------------------- */
typedef struct {
    const unsigned char *buf;
    uint32_t size;
    int      calls;
    uint32_t off[256], len[256];
    int      fail_at_call;          /* 1-based; 0 = never                    */
    int      fail_mode;             /* 0: return -1, 1: return a short count */
} file_t_;

static int rd(void *ctx, uint32_t off, void *out, uint32_t n)
{
    file_t_ *f = ctx;
    if (f->calls < 256) { f->off[f->calls] = off; f->len[f->calls] = n; }
    f->calls++;
    if (f->fail_at_call && f->calls == f->fail_at_call)
        return f->fail_mode == 0 ? -1 : (int)(n / 2);
    if (off >= f->size) return 0;
    uint32_t avail = f->size - off;
    if (n > avail) n = avail;
    memcpy(out, f->buf + off, n);
    return (int)n;
}

/* parse + check (what elf_load() does), producing the elf_image_t that
 * elf32_load_segments() receives. */
static int make_img(const unsigned char *file, uint32_t size, elf_image_t *img)
{
    elf32_ehdr_t h;
    elf32_phdr_t ph[ELF_MAX_PHDRS];
    int rc = elf32_parse_header(file, size, &h);
    if (rc == ELF_OK) rc = elf32_check_header(&h, size);
    if (rc == ELF_OK && h.e_phnum > ELF_MAX_PHDRS) rc = ELF_ERR_TOO_MANY_PHDRS;
    if (rc == ELF_OK) {
        for (uint32_t i = 0; i < h.e_phnum; i++)
            elf32_parse_phdr(file + h.e_phoff + 32u * i, &ph[i]);
        rc = elf32_check_phdrs(&h, ph, h.e_phnum, size, img);
    }
    return rc;
}

static void fill_arena(uint32_t lo, uint32_t n, unsigned char v) { memset((void *)(uintptr_t)lo, v, n); }

/* Every live PT_LOAD of img: memory == file bytes, .bss == 0. Returns #bad bytes. */
static uint32_t verify_mem(const elf_image_t *img, const unsigned char *file)
{
    uint32_t bad = 0;
    for (uint32_t i = 0; i < img->ehdr.e_phnum; i++) {
        const elf32_phdr_t *p = &img->phdr[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;
        for (uint32_t k = 0; k < p->p_filesz; k++)
            if (MEM(p->p_vaddr + k)[0] != file[p->p_offset + k]) bad++;
        for (uint32_t k = p->p_filesz; k < p->p_memsz; k++)
            if (MEM(p->p_vaddr + k)[0] != 0) bad++;
    }
    return bad;
}

static unsigned char *slurp(const char *path, uint32_t *n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(2); }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    unsigned char *b = malloc(sz ? sz : 1);
    if (fread(b, 1, sz, fp) != (size_t)sz) { perror("read"); exit(2); }
    fclose(fp);
    *n = (uint32_t)sz;
    return b;
}

/* run the whole load of an in-memory file; returns rc; img and st are filled in */
static int do_load(const unsigned char *file, uint32_t size, elf_image_t *img,
                   elf_load_stats_t *st, file_t_ *f)
{
    memset(f, 0, sizeof *f);
    f->buf = file; f->size = size;
    int rc = make_img(file, size, img);
    if (rc != ELF_OK) return rc;
    return elf32_load_segments(img, rd, f, st);
}

/* No read may leave [p_offset, p_offset+filesz) of some live segment, and the
 * reads of one segment must be consecutive 512-byte chunks (last one shorter). */
static int reads_are_tight(const elf_image_t *img, const file_t_ *f)
{
    int ci = 0;
    for (uint32_t i = 0; i < img->ehdr.e_phnum; i++) {
        const elf32_phdr_t *p = &img->phdr[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0 || p->p_filesz == 0) continue;
        uint32_t done = 0;
        while (done < p->p_filesz) {
            uint32_t want = p->p_filesz - done; if (want > ELF_LOAD_CHUNK) want = ELF_LOAD_CHUNK;
            if (ci >= f->calls || f->off[ci] != p->p_offset + done || f->len[ci] != want) return 0;
            done += want; ci++;
        }
    }
    return ci == f->calls;
}

int main(int argc, char **argv)
{
    /* ---- FNV-1a known vectors ------------------------------------------- */
    CHECK(elf_fnv1a(ELF_FNV_SEED, "", 0) == 0x811C9DC5u, "fnv empty");
    CHECK(elf_fnv1a(ELF_FNV_SEED, "a", 1) == 0xE40C292Cu, "fnv 'a'");
    CHECK(elf_fnv1a(ELF_FNV_SEED, "foobar", 6) == 0xBF9CF968u, "fnv 'foobar'");
    {   /* feeding in pieces == feeding at once */
        uint32_t h = elf_fnv1a(ELF_FNV_SEED, "foo", 3);
        h = elf_fnv1a(h, "bar", 3);
        CHECK(h == 0xBF9CF968u, "fnv incremental");
    }

    /* ---- map the memory the loader will write ---------------------------- */
    void *m = mmap((void *)(uintptr_t)ARENA_LO, ARENA_SZ, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m == MAP_FAILED || (uintptr_t)m != ARENA_LO) {
        printf("cannot map the arena at 0x%08x (%s) — host test skipped the load tests\n",
               ARENA_LO, "MAP_FIXED_NOREPLACE failed");
        return 2;
    }

    elf_image_t img;
    elf_load_stats_t st;
    file_t_ f;

    /* ---- 1. real toolchain output ---------------------------------------- */
    for (int ai = 1; ai < argc; ai++) {
        uint32_t size;
        unsigned char *file = slurp(argv[ai], &size);
        fill_arena(A - 0x1000, 0x10000, 0xA5);                 /* dirty RAM */
        int rc = do_load(file, size, &img, &st, &f);
        CHECK(rc == ELF_OK, "%s loads (got %d %s)", argv[ai], rc, elf_strerror(rc));
        CHECK(img.loaded == 1, "%s: loaded flag", argv[ai]);
        CHECK(verify_mem(&img, file) == 0, "%s: memory == file bytes, .bss zero", argv[ai]);
        CHECK(st.segs == img.n_load, "%s: stats.segs %u vs n_load %u", argv[ai], st.segs, img.n_load);
        CHECK(st.bytes_copied == img.total_filesz, "%s: bytes_copied", argv[ai]);
        CHECK(st.bytes_zeroed == img.total_memsz - img.total_filesz, "%s: bytes_zeroed", argv[ai]);
        CHECK(reads_are_tight(&img, &f), "%s: reads stay inside the segments' file ranges", argv[ai]);
        for (uint32_t i = 0; i < img.ehdr.e_phnum; i++) {
            const elf32_phdr_t *p = &img.phdr[i];
            if (p->p_type != PT_LOAD || p->p_memsz == 0) { CHECK(st.seg_sum[i] == 0, "%s: seg_sum[%u] of a non-LOAD is 0", argv[ai], i); continue; }
            CHECK(st.seg_sum[i] == elf_fnv1a(ELF_FNV_SEED, file + p->p_offset, p->p_filesz),
                  "%s: seg_sum[%u]", argv[ai], i);
        }
        /* bytes in the page padding between segments stay untouched (0xA5) */
        {
            uint32_t lo = A - 0x1000, hi = A - 0x1000 + 0x10000, stray = 0;
            for (uint32_t a = lo; a < hi; a++) {
                int in_seg = 0;
                for (uint32_t i = 0; i < img.ehdr.e_phnum; i++) {
                    const elf32_phdr_t *p = &img.phdr[i];
                    if (p->p_type == PT_LOAD && p->p_memsz && a >= p->p_vaddr && a - p->p_vaddr < p->p_memsz) in_seg = 1;
                }
                if (!in_seg && MEM(a)[0] != 0xA5) stray++;
            }
            CHECK(stray == 0, "%s: %u byte(s) outside every segment were modified", argv[ai], stray);
        }
        if (strstr(argv[ai], "hello")) {
            /* Derived from the file, not hard-coded (hello.sw changes with the fixture program). */
            CHECK(img.n_load == 1 && st.bytes_copied == img.phdr[0].p_filesz && st.bytes_zeroed == 0, "hello.sw numbers");
            {
                /* the bytes at the entry in memory == the bytes at the corresponding file offset */
                uint32_t eoff = img.phdr[img.entry_seg].p_offset + (img.ehdr.e_entry - img.phdr[img.entry_seg].p_vaddr);
                int same = 1;
                for (uint32_t k = 0; k < 8; k++) if (MEM(img.ehdr.e_entry)[k] != file[eoff + k]) same = 0;
                CHECK(same, "hello.sw: 8 bytes at the entry in memory == file bytes at the entry offset");
            }
        }
        if (strstr(argv[ai], "data")) {
            CHECK(img.n_load == 4, "data.sw has 4 PT_LOAD (got %u)", img.n_load);
            CHECK(st.bytes_zeroed > 0x2000, "data.sw has a real .bss (%u)", st.bytes_zeroed);
        }
        free(file);
    }

    /* ---- 2. chunk boundaries ---------------------------------------------- */
    {
        static const uint32_t sizes[] = { 1, 4, 511, 512, 513, 1023, 1024, 1025, 1500, 4096 };
        for (unsigned si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
            uint32_t fsz = sizes[si], msz = fsz + 3000;
            seg_t s[2] = {
                { PT_LOAD, 0x1000, A + 0x1000, fsz, msz, RW, 0x1000 },
                { PT_LOAD, 0, A, 0x100, 0x100, RX, 0x1000 },
            };
            uint32_t total = 0x1000 + fsz + 64;
            unsigned char *file = build(s, 2, A + 0x80, total);
            fill_arena(A, 0x8000, 0xA5);
            int rc = do_load(file, total, &img, &st, &f);
            CHECK(rc == ELF_OK, "filesz=%u loads (got %d %s)", fsz, rc, elf_strerror(rc));
            CHECK(verify_mem(&img, file) == 0, "filesz=%u: memory == file, bss zero", fsz);
            CHECK(reads_are_tight(&img, &f), "filesz=%u: reads tight (%d calls)", fsz, f.calls);
            CHECK(MEM(A + 0x1000 + msz)[0] == 0xA5, "filesz=%u: byte after memsz untouched", fsz);
            CHECK(st.bytes_copied == fsz + 0x100 && st.bytes_zeroed == 3000, "filesz=%u: stats", fsz);
            free(file);
        }
    }

    /* ---- 3. stale data is cleared: second program over the first one ----- */
    {
        seg_t s1[1] = { { PT_LOAD, 0, A, 0x1000, 0x1000, RX, 0x1000 } };
        seg_t s2[1] = { { PT_LOAD, 0, A, 0x100, 0x1000, RX, 0x1000 } };   /* 0xF00 of .bss */
        unsigned char *f1 = build(s1, 1, A + 0x80, 0x1000);
        unsigned char *f2 = build(s2, 1, A + 0x80, 0x100);
        CHECK(do_load(f1, 0x1000, &img, &st, &f) == ELF_OK, "stale: first program loads");
        CHECK(MEM(A + 0x800)[0] == f1[0x800], "stale: first program's data is in memory");
        CHECK(do_load(f2, 0x100, &img, &st, &f) == ELF_OK, "stale: second program loads");
        uint32_t nz = 0;
        for (uint32_t k = 0x100; k < 0x1000; k++) if (MEM(A + k)[0] != 0) nz++;
        CHECK(nz == 0, "stale: %u byte(s) of the first program survived in the second one's .bss", nz);
        free(f1); free(f2);
    }

    /* ---- 4. reader failures -> ELF_ERR_IO, loaded stays 0 ------------------ */
    {
        seg_t s[1] = { { PT_LOAD, 0, A, 0x1800, 0x1800, RX, 0x1000 } };      /* 4 chunks */
        unsigned char *file = build(s, 1, A + 0x80, 0x1800);
        for (int mode = 0; mode < 2; mode++) {
            for (int at = 1; at <= 4; at++) {
                memset(&f, 0, sizeof f); f.buf = file; f.size = 0x1800;
                f.fail_at_call = at; f.fail_mode = mode;
                CHECK(make_img(file, 0x1800, &img) == ELF_OK, "io: make_img");
                img.loaded = 1;                                              /* stale 1 must be reset */
                int rc = elf32_load_segments(&img, rd, &f, &st);
                CHECK(rc == ELF_ERR_IO, "io mode %d at chunk %d -> %d (%s)", mode, at, rc, elf_strerror(rc));
                CHECK(img.loaded == 0, "io mode %d at chunk %d: loaded==0", mode, at);
            }
        }
        /* the file is shorter than img says (truncated after validation) */
        memset(&f, 0, sizeof f); f.buf = file; f.size = 0x1000;
        CHECK(make_img(file, 0x1800, &img) == ELF_OK, "io: make_img (truncation)");
        int rc = elf32_load_segments(&img, rd, &f, &st);
        CHECK(rc == ELF_ERR_IO && img.loaded == 0, "truncated file -> ELF_ERR_IO (got %d)", rc);
        free(file);
    }

    /* ---- 5. a fabricated / tampered elf_image_t is refused BEFORE any write.
     *         (Those addresses are not mapped: a write would crash the test.) */
    {
        seg_t ok = { PT_LOAD, 0, A, 0x84, 0x84, RX, 0x1000 };
        unsigned char *file = build(&ok, 1, A + 0x10, 400);
        CHECK(make_img(file, 400, &img) == ELF_OK, "fab: base image");
        elf_image_t base = img;
        int rc;

        img = base; img.phdr[0].p_vaddr = 0x00100000u; img.phdr[0].p_paddr = 0x00100000u;
        memset(&f, 0, sizeof f); f.buf = file; f.size = 400;
        rc = elf32_load_segments(&img, rd, &f, &st);
        CHECK(rc == ELF_ERR_SEG_ADDR && f.calls == 0 && img.loaded == 0, "fab: vaddr in the kernel area -> SEG_ADDR, no read (%d)", rc);

        img = base; img.phdr[0].p_vaddr = 0xFFFFFFF0u; img.phdr[0].p_memsz = 0x20; img.phdr[0].p_filesz = 0x20; img.phdr[0].p_align = 0;
        img.ehdr.e_entry = 0xFFFFFFF0u;
        rc = elf32_load_segments(&img, rd, &f, &st);
        CHECK(rc == ELF_ERR_SEG_ADDR && img.loaded == 0, "fab: vaddr+memsz wraps -> SEG_ADDR (%d)", rc);

        img = base; img.phdr[0].p_filesz = 0x1000;                           /* > memsz and past EOF */
        rc = elf32_load_segments(&img, rd, &f, &st);
        CHECK(rc == ELF_ERR_SEG_SIZE && img.loaded == 0, "fab: filesz > memsz -> SEG_SIZE (%d)", rc);

        img = base; img.phdr[0].p_type = PT_INTERP;
        rc = elf32_load_segments(&img, rd, &f, &st);
        CHECK(rc == ELF_ERR_DYNAMIC && img.loaded == 0, "fab: PT_INTERP -> DYNAMIC (%d)", rc);

        img = base; img.ehdr.e_phnum = 9;
        rc = elf32_load_segments(&img, rd, &f, &st);
        CHECK(rc == ELF_ERR_TOO_MANY_PHDRS && img.loaded == 0, "fab: e_phnum=9 -> TOO_MANY_PHDRS (%d)", rc);

        img = base; img.ehdr.e_machine = EM_X86_64;
        rc = elf32_load_segments(&img, rd, &f, &st);
        CHECK(rc == ELF_ERR_BAD_MACHINE && img.loaded == 0, "fab: wrong CPU -> BAD_MACHINE (%d)", rc);

        img = base; img.ehdr.e_entry = A + 0x84;                             /* entry in .bss/after the file part */
        rc = elf32_load_segments(&img, rd, &f, &st);
        CHECK(rc == ELF_ERR_ENTRY_NOT_EXEC && img.loaded == 0, "fab: entry outside X segment (%d)", rc);

        memset(&img, 0, sizeof img);                                         /* never validated at all */
        rc = elf32_load_segments(&img, rd, &f, &st);
        CHECK(rc != ELF_OK && img.loaded == 0, "fab: all-zero image refused (%d %s)", rc, elf_strerror(rc));

        /* tampered DERIVED fields do not matter: they are recomputed */
        img = base; img.n_load = 99; img.load_lo = 1; img.load_hi = 2; img.total_filesz = 7; img.entry_seg = 5;
        fill_arena(A, 0x1000, 0xA5);
        memset(&f, 0, sizeof f); f.buf = file; f.size = 400;
        rc = elf32_load_segments(&img, rd, &f, &st);
        CHECK(rc == ELF_OK && img.loaded == 1, "tamper: derived fields recomputed (%d)", rc);
        CHECK(img.n_load == 1 && img.load_lo == A && img.load_hi == A + 0x84 && img.total_filesz == 0x84 && img.entry_seg == 0,
              "tamper: n_load/load_lo/load_hi/total/entry_seg corrected");
        CHECK(verify_mem(&img, file) == 0, "tamper: memory still correct");
        free(file);
    }

    /* ---- 6. segment kinds the loader must skip ----------------------------- */
    {
        seg_t s[4] = {
            { PT_LOAD, 0, A, 0x100, 0x100, RX, 0x1000 },
            { PT_NOTE, 0x100, 0, 0x20, 0x20, 0, 4 },                         /* not loaded */
            { PT_LOAD, 0x200, A + 0x1000, 0, 0, RW, 0x1000 },                /* memsz==0: ignored */
            { 0x6474e551u, 0, 0, 0, 0, RW, 0x10 },                           /* GNU_STACK */
        };
        unsigned char *file = build(s, 4, A + 0x80, 0x400);
        fill_arena(A, 0x2000, 0xA5);
        int rc = do_load(file, 0x400, &img, &st, &f);
        CHECK(rc == ELF_OK && st.segs == 1, "skip: only the one live PT_LOAD is placed (got %d, segs %u)", rc, st.segs);
        CHECK(MEM(A + 0x1000)[0] == 0xA5, "skip: empty PT_LOAD wrote nothing");
        CHECK(st.seg_sum[1] == 0 && st.seg_sum[2] == 0 && st.seg_sum[3] == 0, "skip: no checksum for skipped phdrs");
        free(file);
    }

    /* ---- 7. NULL handling / optional stats -------------------------------- */
    {
        seg_t ok = { PT_LOAD, 0, A, 0x84, 0x84, RX, 0x1000 };
        unsigned char *file = build(&ok, 1, A + 0x10, 400);
        memset(&f, 0, sizeof f); f.buf = file; f.size = 400;
        CHECK(make_img(file, 400, &img) == ELF_OK, "null: base");
        CHECK(elf32_load_segments(NULL, rd, &f, &st) == ELF_ERR_NULL, "null img");
        CHECK(elf32_load_segments(&img, NULL, &f, &st) == ELF_ERR_NULL, "null reader");
        CHECK(elf32_load_segments(&img, rd, &f, NULL) == ELF_OK && img.loaded == 1, "stats may be NULL");
        free(file);
    }

    /* ---- 8. names --------------------------------------------------------- */
    CHECK(strcmp(elf_strerror(ELF_ERR_CHANGED), "unknown error") != 0, "strerror CHANGED");
    CHECK(strcmp(elf_strerror(ELF_ERR_VERIFY),  "unknown error") != 0, "strerror VERIFY");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
