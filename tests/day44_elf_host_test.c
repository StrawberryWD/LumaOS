/* tests/day44_elf_host_test.c — Day 44 host-side unit test of the Program
 * Header half of kernel/exec/elf.c (elf32_parse_phdr / elf32_check_phdrs /
 * elf_ptype_name) plus the whole "parse header -> check header -> parse
 * phdrs -> check phdrs" pipeline that elf_load() runs after it has read the
 * bytes from the VFS.
 *
 *   gcc -g -fsanitize=address,undefined -Ikernel/exec \
 *       tests/day44_elf_host_test.c kernel/exec/elf.c -o /tmp/elf44_host_test \
 *       && /tmp/elf44_host_test tests/hello.sw
 *
 * Every negative case is a hand-built ELF whose ONLY defect is the one named in
 * the test, and the test asserts the SPECIFIC error code — so a check that
 * silently disappears (or fires for the wrong reason) is caught. The wrap-around
 * cases (vaddr+memsz and offset+filesz overflowing 32 bits) are the ones a naive
 * "a + b <= limit" implementation gets wrong. Buffers handed to the parser are
 * exact-size heap blocks so ASan catches any out-of-bounds read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elf.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- tiny ELF builder --------------------------------------------------- */
typedef struct { uint32_t type, off, vaddr, filesz, memsz, flags, align; } seg_t;

static void w16(unsigned char *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* Header(52) + n Program Headers at e_phoff=52, padded to `file_size` bytes. */
static unsigned char *build(const seg_t *s, int n, uint32_t entry, uint32_t file_size)
{
    unsigned char *b = calloc(1, file_size);
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

/* The elf_load() pipeline on an in-memory file. */
static int pipeline(const unsigned char *file, uint32_t size, elf_image_t *img)
{
    unsigned char *exact = malloc(size ? size : 1);          /* exact size for ASan */
    if (size) memcpy(exact, file, size);
    elf32_ehdr_t h;
    elf32_phdr_t ph[ELF_MAX_PHDRS];
    int rc = elf32_parse_header(exact, size, &h);
    if (rc == ELF_OK) rc = elf32_check_header(&h, size);
    if (rc == ELF_OK && h.e_phnum > ELF_MAX_PHDRS) rc = ELF_ERR_TOO_MANY_PHDRS;
    if (rc == ELF_OK) {
        for (uint32_t i = 0; i < h.e_phnum; i++)
            elf32_parse_phdr(exact + h.e_phoff + 32u * i, &ph[i]);
        rc = elf32_check_phdrs(&h, ph, h.e_phnum, size, img);
    }
    free(exact);
    return rc;
}

#define A  0x08048000u
#define RX (PF_R | PF_X)
#define RW (PF_R | PF_W)

static void expect(const char *name, const seg_t *s, int n, uint32_t entry, uint32_t size, int want)
{
    unsigned char *f = build(s, n, entry, size);
    elf_image_t img;
    int rc = pipeline(f, size, &img);
    CHECK(rc == want, "%-34s got %d (%s), want %d (%s)", name, rc, elf_strerror(rc), want, elf_strerror(want));
    free(f);
}

int main(int argc, char **argv)
{
    CHECK(sizeof(elf32_phdr_t) == 32, "phdr size");

    /* ---- 1. the real hello.sw through the full pipeline ------------------ */
    if (argc >= 2) {
        FILE *fp = fopen(argv[1], "rb");
        if (!fp) { perror("open"); return 2; }
        static unsigned char good[8192];
        size_t n = fread(good, 1, sizeof good, fp);
        fclose(fp);
        elf_image_t img;
        int rc = pipeline(good, (uint32_t)n, &img);
        CHECK(rc == ELF_OK, "hello.sw passes (got %d %s)", rc, elf_strerror(rc));
        CHECK(img.n_load == 1, "hello n_load=%u", img.n_load);
        /* Derived from the file itself (its first Program Header: p_vaddr @+8, p_filesz @+16, p_memsz @+20),
         * not hard-coded: hello.sw changes whenever the fixture program changes (Day 46, Day 49...). */
        uint32_t ph0 = good[28] | good[29] << 8 | good[30] << 16 | (uint32_t)good[31] << 24;   /* e_phoff */
        uint32_t pv = good[ph0 + 8] | good[ph0 + 9] << 8 | good[ph0 + 10] << 16 | (uint32_t)good[ph0 + 11] << 24;
        uint32_t pf = good[ph0 + 16] | good[ph0 + 17] << 8 | good[ph0 + 18] << 16 | (uint32_t)good[ph0 + 19] << 24;
        uint32_t pm = good[ph0 + 20] | good[ph0 + 21] << 8 | good[ph0 + 22] << 16 | (uint32_t)good[ph0 + 23] << 24;
        CHECK(img.load_lo == pv && img.load_hi == pv + pm, "hello range %08x..%08x (file: %08x..%08x)", img.load_lo, img.load_hi, pv, pv + pm);
        CHECK(img.total_filesz == pf && img.total_memsz == pm, "hello totals (file: filesz %x memsz %x)", pf, pm);
        CHECK(img.entry_seg == 0 && img.ehdr.e_entry == (uint32_t)(good[24] | good[25] << 8 | good[26] << 16 | (uint32_t)good[27] << 24), "hello entry seg (e_entry read from the file bytes)");
        CHECK(img.phdr[1].p_type == 0x6474e551u && !strcmp(elf_ptype_name(img.phdr[1].p_type), "GNU_STACK"), "GNU_STACK kept");
        CHECK(img.file_size == n, "file_size");

        /* struct layout == decoder (catches a wrong decode offset) */
        elf32_phdr_t viastruct, viaparse;
        memcpy(&viastruct, good + 52, 32);
        elf32_parse_phdr(good + 52, &viaparse);
        CHECK(!memcmp(&viastruct, &viaparse, 32), "parse_phdr == memcpy on LE host");
    }

    /* ---- 2. good synthetic ELFs ------------------------------------------ */
    {   /* text (RX) + data with .bss (RW, memsz > filesz) in a 6000-byte file */
        seg_t s[3] = {
            { PT_LOAD, 0x000, A,          0x200, 0x200, RX, 0x1000 },
            { PT_LOAD, 0x1000, A + 0x1000, 0x400, 0x2400, RW, 0x1000 },
            { 0x6474e551u, 0, 0, 0, 0, RW, 0x10 },
        };
        unsigned char *f = build(s, 3, A + 0x100, 6000);
        elf_image_t img;
        int rc = pipeline(f, 6000, &img);
        CHECK(rc == ELF_OK, "text+data+bss good (got %d %s)", rc, elf_strerror(rc));
        CHECK(img.n_load == 2 && img.load_lo == A && img.load_hi == A + 0x3400, "text+data range");
        CHECK(img.total_filesz == 0x600 && img.total_memsz == 0x2600, "text+data totals");
        free(f);

        seg_t z[2] = { { PT_LOAD, 0, A, 0x84, 0, RX, 0x1000 },       /* memsz==0 : ignored  */
                       { PT_LOAD, 0, A, 0x84, 0x84, RX, 0x1000 } };
        f = build(z, 2, A + 0x10, 400);
        rc = pipeline(f, 400, &img);
        CHECK(rc == ELF_OK && img.n_load == 1 && img.entry_seg == 1, "memsz==0 PT_LOAD ignored (rc=%d)", rc);
        free(f);

        seg_t e[1] = { { PT_LOAD, 0, A, 0x84, 0x84, RX, 0 } };       /* align 0 = no constraint */
        f = build(e, 1, A, 400);
        CHECK(pipeline(f, 400, &img) == ELF_OK, "align 0 ok");
        free(f);
    }

    /* ---- 3. one defect each ---------------------------------------------- */
    {
        seg_t ok = { PT_LOAD, 0, A, 0x84, 0x84, RX, 0x1000 };
        seg_t s[3];

        s[0] = ok; s[0].type = PT_INTERP;
        expect("PT_INTERP", s, 1, A + 0x10, 400, ELF_ERR_DYNAMIC);
        s[0] = ok; s[1] = ok; s[1].type = PT_DYNAMIC; s[0].filesz = 0x84;
        expect("PT_DYNAMIC after a LOAD", s, 2, A + 0x10, 400, ELF_ERR_DYNAMIC);
        s[0] = ok; s[0].filesz = 0x100; s[0].memsz = 0x80;
        expect("filesz > memsz", s, 1, A + 0x10, 400, ELF_ERR_SEG_SIZE);
        s[0] = ok; s[0].off = 300; s[0].filesz = 200; s[0].memsz = 200; s[0].vaddr = A + 300; /* keeps congruence */
        expect("data past EOF (off+filesz>size)", s, 1, A + 0x130, 400, ELF_ERR_SEG_FILE_RANGE);
        s[0] = ok; s[0].off = 0xFFFFFFF0u; s[0].filesz = 0x20; s[0].memsz = 0x20; s[0].vaddr = A + 0xFFFFFFF0u;
        expect("off+filesz wraps 2^32", s, 1, A + 0x10, 400, ELF_ERR_SEG_FILE_RANGE);
        s[0] = ok; s[0].vaddr = 0x00100000u;
        expect("vaddr below 16MB (kernel area)", s, 1, 0x00100010u, 400, ELF_ERR_SEG_ADDR);
        s[0] = ok; s[0].vaddr = 0x0F000000u;
        expect("vaddr at ELF_LOAD_MAX_ADDR", s, 1, 0x0F000010u, 400, ELF_ERR_SEG_ADDR);
        s[0] = ok; s[0].vaddr = 0xFFFFFFF0u; s[0].memsz = 0x20; s[0].filesz = 0x20; s[0].align = 0;
        expect("vaddr+memsz wraps 2^32", s, 1, 0xFFFFFFF0u, 400, ELF_ERR_SEG_ADDR);
        s[0] = ok; s[0].memsz = 0x0F000000u - A + 1;
        expect("memsz runs past the max", s, 1, A + 0x10, 400, ELF_ERR_SEG_ADDR);
        s[0] = ok; s[0].align = 0x1800;
        expect("align not a power of 2", s, 1, A + 0x10, 400, ELF_ERR_SEG_ALIGN);
        s[0] = ok; s[0].vaddr = A + 8;
        expect("vaddr != offset mod align", s, 1, A + 0x18, 400, ELF_ERR_SEG_ALIGN);
        s[0] = ok; s[1] = ok; s[1].vaddr = A + 0x40; s[1].off = 0x40; s[1].filesz = s[1].memsz = 0x20;
        expect("two PT_LOADs overlap", s, 2, A + 0x10, 400, ELF_ERR_SEG_OVERLAP);
        s[0] = ok;
        expect("entry past filesz (in .bss)", s, 1, A + 0x84, 400, ELF_ERR_ENTRY_NOT_EXEC);
        s[0] = ok; s[0].memsz = 0x200;
        expect("entry in bss part of RX seg", s, 1, A + 0x100, 400, ELF_ERR_ENTRY_NOT_EXEC);
        s[0] = ok; s[0].flags = RW;
        expect("entry in a non-X segment", s, 1, A + 0x10, 400, ELF_ERR_ENTRY_NOT_EXEC);
        s[0] = ok;
        expect("entry before every segment", s, 1, A - 4, 400, ELF_ERR_ENTRY_NOT_EXEC);
        s[0] = ok; s[0].type = PT_NOTE;
        expect("no PT_LOAD at all", s, 1, A + 0x10, 400, ELF_ERR_NO_LOAD);
        s[0] = ok; s[0].memsz = 0; s[0].filesz = 0;
        expect("only an empty PT_LOAD", s, 1, A + 0x10, 400, ELF_ERR_NO_LOAD);
    }

    /* ---- 4. table-size limits -------------------------------------------- */
    {
        seg_t many[9];
        for (int i = 0; i < 9; i++) many[i] = (seg_t){ PT_NOTE, 0, 0, 0, 0, 0, 0 };
        many[0] = (seg_t){ PT_LOAD, 0, A, 0x84, 0x84, RX, 0x1000 };
        expect("9 program headers > ELF_MAX_PHDRS", many, 9, A + 0x10, 600, ELF_ERR_TOO_MANY_PHDRS);
        expect("8 program headers is fine", many, 8, A + 0x10, 600, ELF_OK);

        elf_image_t img;
        elf32_ehdr_t h; memset(&h, 0, sizeof h);
        elf32_phdr_t arr[ELF_MAX_PHDRS + 1]; memset(arr, 0, sizeof arr);
        CHECK(elf32_check_phdrs(&h, arr, 9, 600, &img) == ELF_ERR_TOO_MANY_PHDRS, "check_phdrs n=9 direct");
        CHECK(elf32_check_phdrs(NULL, arr, 1, 600, &img) == ELF_ERR_NULL, "NULL h");
        CHECK(elf32_check_phdrs(&h, NULL, 1, 600, &img) == ELF_ERR_NULL, "NULL ph");
        CHECK(elf32_check_phdrs(&h, arr, 1, 600, NULL) == ELF_ERR_NULL, "NULL img");
    }

    /* ---- 5. names -------------------------------------------------------- */
    CHECK(!strcmp(elf_ptype_name(PT_LOAD), "LOAD") && !strcmp(elf_ptype_name(PT_NOTE), "NOTE")
          && !strcmp(elf_ptype_name(0x12345678u), "?"), "ptype names");
    for (int e = ELF_ERR_TOO_MANY_PHDRS; e >= ELF_ERR_ENTRY_NOT_EXEC; e--)
        CHECK(strcmp(elf_strerror(e), "unknown error") != 0, "strerror(%d) defined", e);
    for (int e = ELF_ERR_OPEN; e >= ELF_ERR_IO; e--)
        CHECK(strcmp(elf_strerror(e), "unknown error") != 0, "strerror(%d) defined", e);

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
