/* tests/day43_elf_host_test.c — Day 43 host-side unit test of kernel/exec/elf.c
 *
 *   gcc -g -fsanitize=address,undefined -Ikernel/exec \
 *       tests/day43_elf_host_test.c kernel/exec/elf.c -o /tmp/elf_host_test \
 *       && /tmp/elf_host_test tests/hello.sw
 *
 * Same pattern as tests/day15_fat_mount_host_test.c: the parser has no kernel
 * dependencies, so it is compiled for the host (with ASan/UBSan) and fed real
 * and mutated headers. Truncation tests use EXACT-size heap buffers so that a
 * read past the end of the supplied length is caught by AddressSanitizer.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "elf.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned char *dup_exact(const unsigned char *src, size_t n)
{
    unsigned char *p = malloc(n ? n : 1);
    if (n) memcpy(p, src, n);
    return p;
}

static int parse_check(const unsigned char *img, size_t len, elf32_ehdr_t *h)
{
    unsigned char *exact = dup_exact(img, len);
    int rc = elf32_parse_header(exact, (uint32_t)len, h);
    if (rc == ELF_OK) rc = elf32_check_header(h, (uint32_t)len);
    free(exact);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s hello.sw\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    unsigned char good[8192];
    size_t n = fread(good, 1, sizeof good, f);
    fclose(f);

    elf32_ehdr_t h;
    memset(&h, 0, sizeof h);

    /* sizes locked by _Static_assert, re-check at run time for the log */
    CHECK(sizeof(elf32_ehdr_t) == 52 && sizeof(elf32_phdr_t) == 32 && sizeof(elf32_shdr_t) == 40,
          "struct sizes");

    /* 1. the real file */
    CHECK(parse_check(good, n, &h) == ELF_OK, "good file must pass");
    CHECK(h.e_type == ET_EXEC && h.e_machine == EM_386 && h.e_ehsize == 52, "good fields");
    CHECK(h.e_entry == (uint32_t)(good[24] | good[25] << 8 | good[26] << 16 | (uint32_t)good[27] << 24), "entry 0x%08x == e_entry bytes of the file", h.e_entry);
    CHECK(h.e_phoff == 52 && h.e_phentsize == 32 && h.e_phnum == (good[44] | good[45] << 8) && h.e_phnum >= 1, "ph table fields (phnum read from the file bytes)");

    /* 2. field offsets agree with the packed struct (catches a wrong decode offset) */
    {
        elf32_ehdr_t raw;
        memcpy(&raw, good, sizeof raw);          /* x86-64 host is little-endian */
        CHECK(raw.e_entry == h.e_entry && raw.e_phoff == h.e_phoff && raw.e_shoff == h.e_shoff &&
              raw.e_shnum == h.e_shnum && raw.e_shstrndx == h.e_shstrndx &&
              raw.e_type == h.e_type && raw.e_machine == h.e_machine &&
              raw.e_flags == h.e_flags && raw.e_phnum == h.e_phnum,
              "decode offsets vs packed struct");
        CHECK(offsetof(elf32_ehdr_t, e_entry) == 24 && offsetof(elf32_ehdr_t, e_shstrndx) == 50,
              "struct offsets");
        CHECK(offsetof(elf32_phdr_t, p_flags) == 24, "phdr p_flags offset (ELF32: after p_memsz)");
    }

    /* 3. every truncated length 0..51 -> TOO_SHORT, no over-read (ASan) */
    for (size_t L = 0; L < 52; L++)
        CHECK(parse_check(good, L, &h) == ELF_ERR_TOO_SHORT, "len %zu", L);

    /* 4. exactly 52 bytes parses (check then fails: table is outside the file) */
    CHECK(elf32_parse_header(good, 52, &h) == ELF_OK, "52 bytes parse");
    CHECK(elf32_check_header(&h, 52) == ELF_ERR_PH_RANGE, "52-byte file has no room for phdrs");

    /* 5. targeted single-field mutations */
    struct { const char *name; size_t off; unsigned char val; int want; } t[] = {
        { "magic0",  0, 0x00, ELF_ERR_BAD_MAGIC },
        { "magic3",  3, 'X',  ELF_ERR_BAD_MAGIC },
        { "class64", 4, 2,    ELF_ERR_NOT_32BIT },
        { "class0",  4, 0,    ELF_ERR_NOT_32BIT },
        { "big",     5, 2,    ELF_ERR_NOT_LE },
        { "data0",   5, 0,    ELF_ERR_NOT_LE },
        { "identver",6, 2,    ELF_ERR_BAD_IDENT_VER },
        { "type_dyn",16, 3,   ELF_ERR_BAD_TYPE },
        { "type_rel",16, 1,   ELF_ERR_BAD_TYPE },
        { "arm",     18, 40,  ELF_ERR_BAD_MACHINE },
        { "ver",     20, 2,   ELF_ERR_BAD_VERSION },
        { "ehsize",  40, 64,  ELF_ERR_BAD_EHSIZE },
        { "phentsz", 42, 56,  ELF_ERR_BAD_PHENTSIZE },
        { "phnum0",  44, 0,   ELF_ERR_NO_PHDRS },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        unsigned char b[8192]; memcpy(b, good, n); b[t[i].off] = t[i].val;
        int rc = parse_check(b, n, &h);
        CHECK(rc == t[i].want, "%s: got %d (%s), want %d", t[i].name, rc, elf_strerror(rc), t[i].want);
    }
    {   /* entry == 0, phoff beyond file, phoff overlapping the header, overflow-ish phoff */
        unsigned char b[8192];
        memcpy(b, good, n); memset(b + 24, 0, 4);
        CHECK(parse_check(b, n, &h) == ELF_ERR_NO_ENTRY, "entry 0");
        memcpy(b, good, n); b[28] = 0x00; b[29] = 0x10;           /* phoff = 0x1000 */
        CHECK(parse_check(b, n, &h) == ELF_ERR_PH_RANGE, "phoff beyond file");
        memcpy(b, good, n); b[28] = 4; b[29] = b[30] = b[31] = 0; /* phoff = 4 (inside header) */
        CHECK(parse_check(b, n, &h) == ELF_ERR_PH_RANGE, "phoff inside header");
        memcpy(b, good, n); b[28] = b[29] = b[30] = b[31] = 0xFF; /* phoff = 0xFFFFFFFF */
        CHECK(parse_check(b, n, &h) == ELF_ERR_PH_RANGE, "phoff 0xFFFFFFFF (no overflow)");
        memcpy(b, good, n); b[44] = b[45] = 0xFF;                 /* phnum = 65535 */
        CHECK(parse_check(b, n, &h) == ELF_ERR_PH_RANGE, "phnum 65535 table too big");
    }

    /* 6. NULL handling */
    CHECK(elf32_parse_header(NULL, 52, &h) == ELF_ERR_NULL, "null buf");
    CHECK(elf32_parse_header(good, 52, NULL) == ELF_ERR_NULL, "null out");
    CHECK(elf32_check_header(NULL, 0) == ELF_ERR_NULL, "null hdr");

    /* 7. brute force: every byte of the header set to 00/80/FF — must never crash
     *    (ASan/UBSan clean) and must never report OK for a mutation that the
     *    targeted tests above classify as invalid-by-construction (magic/class/data). */
    int ok_after_mut = 0;
    for (size_t i = 0; i < 52; i++) {
        unsigned char vals[3] = { 0x00, 0x80, 0xFF };
        for (int v = 0; v < 3; v++) {
            unsigned char b[8192]; memcpy(b, good, n); b[i] = vals[v];
            int rc = parse_check(b, n, &h);
            if (rc == ELF_OK) ok_after_mut++;
            if (i < 4 && vals[v] != good[i])
                CHECK(rc == ELF_ERR_BAD_MAGIC, "byte %zu mutated must be BAD_MAGIC", i);
        }
    }
    printf("(info) %d of 156 single-byte mutations still accepted - those are the fields\n"
           "       the loader does not need to reject (e_flags, e_shoff, e_shnum, padding...)\n",
           ok_after_mut);

    /* 8. name helpers never return NULL */
    for (int e = 1; e >= -20; e--) CHECK(elf_strerror(e) != NULL, "strerror %d", e);
    CHECK(elf_type_name(0xFFFF)[0] == '?' && elf_machine_name(0xFFFF)[0] == '?', "unknown names");

    printf(fails ? "\nHOST TEST FAILED (%d)\n" : "\nHOST TEST ALL PASS\n", fails);
    return fails ? 1 : 0;
}