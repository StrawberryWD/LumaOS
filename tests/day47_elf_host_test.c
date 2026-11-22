/* tests/day47_elf_host_test.c - Day 46 + 47 (+48 output counters) host unit test of elf_run()'s C logic
 * (Day 47: user stack preparation / inspection / overflow guard / stack-vs-segment check).
 *
 *   gcc -O1 -Wall -Ikernel/exec -Ikernel/gui -Ikernel/lib -Ikernel/core \
 *       -Ikernel/drivers -Ikernel/arch -Ikernel/fs/vfs tests/day47_elf_host_test.c \
 *       kernel/exec/elf.c kernel/exec/elf_run.c -o /tmp/elf47_host_test && /tmp/elf47_host_test
 *
 * The assembly trampoline (elf_enter) and the terminal are replaced by stubs here, so this
 * checks only what elf_run.c itself decides: argument checks, "loaded must be 1", the
 * re-validation of a (possibly tampered) image, the entry address / api table it hands to the
 * trampoline, exit-code propagation, the loaded->0 rule, and the api->puts/putchar bridge.
 * The trampoline itself (registers, DF, IF, segments, esp) is tested in QEMU:
 * tests/day46_test_elf_run.py.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "elf_run.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } \
                           printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- stubs ---- */
static char     out_buf[8192];
static unsigned out_len;
void terminal_putchar(terminal_t *t, char c, unsigned char color)
{ (void)t; (void)color; if (out_len < sizeof(out_buf) - 1) out_buf[out_len++] = c; }
void terminal_puts(terminal_t *t, const char *s, unsigned char color)
{ while (*s) terminal_putchar(t, *s++, color); }

static int      enter_calls;
static uint32_t enter_entry;
static const luma_api_t *enter_api;
static int      enter_ret = 42;
static const char *enter_say;          /* what the fake program prints through api->puts */
static uint32_t enter_stack_top;
static void (*enter_hook)(void);          /* lets a test scribble on the stack "while the program runs" */
uint32_t elf_exit_via;
/* DAY55: the api table is no longer in elf_run.c: it lives in the user runtime page (kernel/exec/urt.*), its
 * functions are Ring 3 stubs that use SYS_WRITE. On the host we model those stubs with elf_run_write(), the
 * SYS_WRITE backend, including the stub's 4096-character cap and NULL tolerance. */
extern int elf_run_write(const char *buf, uint32_t n, int to_stderr);
static void host_puts(const char *s)
{ if (!s) return; uint32_t n = 0; while (n < 4096 && s[n]) n++; elf_run_write(s, n, 0); }
static void host_putchar(int c) { char ch = (char)c; elf_run_write(&ch, 1, 0); }
static void host_exit(int code) { (void)code; }
static const luma_api_t host_api = { LUMA_API_MAGIC, LUMA_API_VERSION, host_puts, host_putchar, host_exit };
uint32_t urt_api_va(void)      { return 0x0A000000u; }
uint32_t urt_ret_stub_va(void) { return 0x0A000040u; }
uint32_t elf_ret_stub_va;
elf_fault_t elf_fault_last;   /* DAY56 */
void elf_sys_exit(int code, int via) { (void)code; (void)via; }
/* DAY49: the trampoline now also receives argc/argv; the stub records them and snapshots what the
 * program would see (argv[] pointers + strings live in the user stack region, mapped by main()). */
static int       enter_argc;
static char    **enter_argv;
static char      snap[32][64];
static uint32_t  snap_ptr[33];
static int       snap_ok;
static uint32_t  spare_word;
int elf_enter(uint32_t entry, const void *api, uint32_t stack_top, int argc, char **argv)
{
    enter_calls++;
    enter_stack_top = stack_top;
    enter_argc = argc; enter_argv = argv;
    spare_word = *(volatile uint32_t *)(uintptr_t)(stack_top - 4);
    snap_ok = 1;
    /* The target is 32-bit: argv[] entries are 4-byte words (this host test is 64-bit, so read them as uint32_t). */
    const volatile uint32_t *w = (const volatile uint32_t *)argv;
    for (int i = 0; i < argc && i < 32; i++) {
        snap_ptr[i] = w[i];
        strncpy(snap[i], (const char *)(uintptr_t)w[i], 63); snap[i][63] = 0;
    }
    snap_ptr[argc] = w[argc];
    if (enter_hook) enter_hook();
    enter_entry = entry;
    (void)api; enter_api = &host_api;
    if (enter_say) enter_api->puts(enter_say);
    enter_api->putchar('!');
    return enter_ret;
}

/* ---- a hand-built, valid, loaded image: one RX segment at 0x08048000 ---- */
static void mk(elf_image_t *img)
{
    memset(img, 0, sizeof(*img));
    img->file_size = 0x200;
    elf32_ehdr_t *h = &img->ehdr;
    memcpy(h->e_ident, "\x7f""ELF\x01\x01\x01", 7);
    h->e_type = ET_EXEC; h->e_machine = EM_386; h->e_version = 1;
    h->e_entry = 0x08048080; h->e_phoff = 52; h->e_ehsize = 52;
    h->e_phentsize = 32; h->e_phnum = 2;
    elf32_phdr_t *p = &img->phdr[0];
    p->p_type = PT_LOAD; p->p_offset = 0; p->p_vaddr = p->p_paddr = 0x08048000;
    p->p_filesz = p->p_memsz = 0x100; p->p_flags = PF_R | PF_X; p->p_align = 0x1000;
    p = &img->phdr[1];
    p->p_type = PT_LOAD; p->p_offset = 0x100; p->p_vaddr = p->p_paddr = 0x08049100;
    p->p_filesz = 0x40; p->p_memsz = 0x240; p->p_flags = PF_R | PF_W; p->p_align = 0x1000;
    img->loaded = 1;
}

int main(void)
{
    /* Day 47: elf_run() fills/reads the user stack at its absolute address -> map real memory there. */
    void *stk = mmap((void *)(uintptr_t)ELF_USER_STACK_BASE, ELF_USER_STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (stk == MAP_FAILED || (uintptr_t)stk != ELF_USER_STACK_BASE) { printf("cannot map the stack area\n"); return 2; }
    elf_image_t img;
    terminal_t *T = (terminal_t *)0x1000;      /* never dereferenced: stubs ignore it */
    int code = -999, rc;

    /* ---- argument checks ---- */
    mk(&img);
    CHECK(elf_run(NULL, T, &code, NULL) == ELF_ERR_NULL, "NULL img -> ELF_ERR_NULL");
    CHECK(elf_run(&img, NULL, &code, NULL) == ELF_ERR_NULL, "NULL terminal -> ELF_ERR_NULL");
    CHECK(enter_calls == 0, "nothing executed after the NULL cases");

    /* ---- loaded gate ---- */
    mk(&img); img.loaded = 0; code = -999;
    rc = elf_run(&img, T, &code, NULL);
    CHECK(rc == ELF_ERR_NOT_LOADED, "loaded==0 -> ELF_ERR_NOT_LOADED (got %d)", rc);
    CHECK(code == 0, "exit_code zeroed on failure (got %d)", code);
    CHECK(enter_calls == 0, "loaded==0: trampoline NOT called");
    mk(&img); img.loaded = 2;
    CHECK(elf_run(&img, T, &code, NULL) == ELF_ERR_NOT_LOADED && enter_calls == 0, "loaded==2 (not exactly 1) refused too");

    /* ---- tampered images are re-validated (loaded==1 is not enough) ---- */
    mk(&img); img.ehdr.e_machine = 62;
    CHECK(elf_run(&img, T, &code, NULL) == ELF_ERR_BAD_MACHINE && enter_calls == 0, "tampered e_machine refused");
    mk(&img); img.ehdr.e_entry = 0x08049100;        /* inside a RW (non-exec) segment */
    rc = elf_run(&img, T, &code, NULL);
    CHECK(rc == ELF_ERR_ENTRY_NOT_EXEC && enter_calls == 0, "entry in non-exec segment refused (got %d)", rc);
    mk(&img); img.ehdr.e_entry = 0x08048100;        /* one past the exec segment's file part */
    CHECK(elf_run(&img, T, &code, NULL) == ELF_ERR_ENTRY_NOT_EXEC && enter_calls == 0, "entry one past segment end refused");
    mk(&img); img.ehdr.e_entry = 0x00010000;        /* kernel memory */
    CHECK(elf_run(&img, T, &code, NULL) != ELF_OK && enter_calls == 0, "entry in kernel memory refused");
    mk(&img); img.phdr[0].p_vaddr = 0x00100000;     /* segment moved onto kernel stack area */
    CHECK(elf_run(&img, T, &code, NULL) != ELF_OK && enter_calls == 0, "segment outside the user load area refused");
    mk(&img); img.entry_seg = 1;                    /* lying entry_seg field must not matter... */
    rc = elf_run(&img, T, &code, NULL);
    CHECK(rc == ELF_OK && enter_calls == 1 && enter_entry == 0x08048080,
          "wrong img.entry_seg is ignored: validation recomputes it (rc %d, entry %08x)", rc, enter_entry);

    /* ---- the good path ---- */
    enter_calls = 0; out_len = 0; enter_ret = 42; enter_say = "hi, ";
    mk(&img); code = -999;
    rc = elf_run(&img, T, &code, NULL);
    CHECK(rc == ELF_OK, "valid loaded image -> ELF_OK");
    CHECK(enter_calls == 1, "trampoline called exactly once");
    CHECK(enter_entry == 0x08048080, "entry passed = e_entry (0x%08x)", enter_entry);
    CHECK(code == 42, "exit code = trampoline's return value (%d)", code);
    CHECK(img.loaded == 0, "img.loaded cleared after running (memory image is no longer pristine)");
    CHECK(enter_api && enter_api->magic == LUMA_API_MAGIC && enter_api->version == LUMA_API_VERSION,
          "api table: magic 'LAPI', version %u", enter_api ? enter_api->version : 0);
    CHECK(out_len == 5 && memcmp(out_buf, "hi, !", 5) == 0, "api->puts/putchar reach the terminal (\"%.*s\")", (int)out_len, out_buf);

    /* running again without re-loading is refused */
    enter_calls = 0;
    CHECK(elf_run(&img, T, &code, NULL) == ELF_ERR_NOT_LOADED && enter_calls == 0, "second run without reload refused");

    /* negative / large exit codes pass through unchanged */
    mk(&img); enter_ret = -1;
    elf_run(&img, T, &code, NULL);
    CHECK(code == -1, "exit code -1 preserved");
    mk(&img); enter_ret = (int)0x80000000;
    elf_run(&img, T, &code, NULL);
    CHECK(code == (int)0x80000000, "exit code INT_MIN preserved");

    /* exit_code pointer is optional */
    mk(&img);
    CHECK(elf_run(&img, T, NULL, NULL) == ELF_OK, "exit_code == NULL allowed");

    /* ---- api outside a run: puts after elf_run returned must be a no-op ---- */
    const luma_api_t *saved = enter_api;
    out_len = 0;
    saved->puts("late");
    saved->putchar('x');
    CHECK(out_len == 0, "api->puts/putchar after the program finished print nothing");

    /* ---- api->puts robustness: NULL and unterminated strings ---- */
    static const char *capture_api_say;
    (void)capture_api_say;
    mk(&img); enter_say = NULL; out_len = 0; enter_ret = 0;
    /* fake program that calls puts(NULL) and an unterminated 8000-byte buffer */
    {
        static char big[8000];
        memset(big, 'z', sizeof(big));          /* no NUL anywhere */
        enter_say = big;
        elf_run(&img, T, &code, NULL);
        CHECK(out_len <= 4097 + 1, "unterminated string is capped (printed %u chars)", out_len);
    }


    /* ======================= DAY 47 ======================= */
    volatile unsigned char *stk8 = (volatile unsigned char *)(uintptr_t)ELF_USER_STACK_BASE;
    elf_run_info_t info;
    enter_say = NULL; enter_ret = 7; enter_calls = 0;

    /* stack_top handed to the trampoline; untouched stack -> used 0, no overflow, via ret */
    mk(&img); memset((void *)(uintptr_t)ELF_USER_STACK_BASE, 0x11, ELF_USER_STACK_SIZE);   /* stale garbage from a previous run */
    rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && enter_calls == 1, "D47 run ok (rc %d)", rc);
    CHECK(enter_stack_top == ELF_USER_STACK_TOP && enter_stack_top == 0x0A000000u, "stack_top passed to elf_enter = 0x0A000000");
    CHECK(info.stack_top == 0x0A000000u && info.stack_size == 0x10000u, "info top/size");
    CHECK(info.stack_used == 4 && info.overflow == 0 && info.via_exit == 0, "argc==0: only the spare word [TOP-4]=0 is written: used 4 (got %u)", info.stack_used);
    int refilled = 1;
    for (uint32_t i = 0; i < ELF_USER_STACK_SIZE - 4; i++) if (stk8[i] != ELF_STACK_FILL) { refilled = 0; break; }   /* last word = Day 49 spare 0 */
    CHECK(refilled, "stack was re-filled with 0xA5 over its whole size before the run (stale 0x11 gone)");

    /* usage measured from the lowest modified byte */
    static void *hook_addr; static unsigned char hook_val;
    void hook_write(void) { *(volatile unsigned char *)hook_addr = hook_val; }
    enter_hook = hook_write; hook_val = 0x42;
    hook_addr = (void *)(uintptr_t)(ELF_USER_STACK_TOP - 100);
    mk(&img); rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && info.stack_used == 100 && !info.overflow, "write 100 B below the top: used == 100 (got %u)", info.stack_used);
    hook_addr = (void *)(uintptr_t)(ELF_USER_STACK_BASE + 1000);
    mk(&img); rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && info.stack_used == ELF_USER_STACK_SIZE - 1000 && !info.overflow, "deep use outside the guard: no overflow (used %u)", info.stack_used);

    /* guard area: [BASE, BASE+256) */
    hook_addr = (void *)(uintptr_t)(ELF_USER_STACK_BASE + ELF_STACK_GUARD);          /* first byte after the guard */
    mk(&img); rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && !info.overflow, "write at BASE+256 (just outside the guard): no overflow");
    hook_addr = (void *)(uintptr_t)(ELF_USER_STACK_BASE + ELF_STACK_GUARD - 1);      /* last guard byte */
    mk(&img); code = -999; rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_ERR_STACK_OVERFLOW && info.overflow == 1 && info.stack_used == ELF_USER_STACK_SIZE - ELF_STACK_GUARD + 1,
          "write at BASE+255 (last guard byte): ELF_ERR_STACK_OVERFLOW, used %u", info.stack_used);
    CHECK(code == 7 && img.loaded == 0, "overflow: the program's exit code (%d) is still delivered, loaded cleared", code);
    hook_addr = (void *)(uintptr_t)ELF_USER_STACK_BASE; hook_val = ELF_STACK_FILL;   /* writing the fill value itself is invisible: documented limit */
    mk(&img); rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && info.stack_used == 4, "limit: writing the fill value 0xA5 cannot be detected (baseline 4)");
    enter_hook = NULL;
    mk(&img); rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && info.stack_used == 4 && !info.overflow, "after an overflow the next run starts clean (stack re-filled)");

    /* via_exit comes from the trampoline's variable */
    elf_exit_via = 1;
    mk(&img); rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && info.via_exit == 1, "info.via_exit reflects elf_exit_via == 1 (api->exit path)");
    elf_exit_via = 0;

    /* info == NULL still fine; info zeroed on early failures */
    mk(&img); img.loaded = 0; memset(&info, 0x77, sizeof info);
    rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_ERR_NOT_LOADED && info.stack_used == 0 && info.via_exit == 0 && info.stack_size == 0, "info zeroed on failure");

    /* api table: exit field */
    CHECK(enter_api && enter_api->version == 2 && enter_api->exit == host_exit, "api version 2 carries an exit entry");

    /* segments may not overlap the stack (checked by elf32_check_phdrs, hence at load time too) */
    enter_calls = 0;
    mk(&img); img.phdr[1].p_vaddr = ELF_USER_STACK_BASE; img.phdr[1].p_offset = 0; img.phdr[1].p_filesz = 0x40; img.phdr[1].p_memsz = 0x100;
    rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_ERR_SEG_STACK && enter_calls == 0, "segment at the stack base -> ELF_ERR_SEG_STACK, nothing executed (got %d)", rc);
    mk(&img); img.phdr[1].p_vaddr = ELF_USER_STACK_BASE - 0x1000; img.phdr[1].p_offset = 0; img.phdr[1].p_filesz = 0x40; img.phdr[1].p_memsz = 0x1001;
    CHECK(elf_run(&img, T, &code, &info) == ELF_ERR_SEG_STACK && enter_calls == 0, "segment 1 byte into the stack -> rejected");
    mk(&img); img.phdr[1].p_vaddr = ELF_USER_STACK_TOP - 0x10; img.phdr[1].p_offset = 0; img.phdr[1].p_filesz = 0x10; img.phdr[1].p_memsz = 0x10; img.phdr[1].p_align = 0x10;
    CHECK(elf_run(&img, T, &code, &info) == ELF_ERR_SEG_STACK && enter_calls == 0, "segment at the very top of the stack -> rejected");
    mk(&img); img.phdr[1].p_vaddr = ELF_USER_STACK_BASE - 0x1000; img.phdr[1].p_offset = 0; img.phdr[1].p_filesz = 0x40; img.phdr[1].p_memsz = 0x1000;
    rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && enter_calls == 1, "segment ending exactly at the stack base is fine (rc %d)", rc);
    mk(&img); img.phdr[1].p_vaddr = ELF_USER_STACK_TOP; img.phdr[1].p_offset = 0; img.phdr[1].p_filesz = 0x40; img.phdr[1].p_memsz = 0x40; img.phdr[1].p_align = 0x1000;
    enter_calls = 0;
    rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_ERR_SEG_STACK && enter_calls == 0, "DAY55: segment starting at the stack top is on the runtime page: refused (rc %d)", rc);
    mk(&img); img.phdr[1].p_vaddr = ELF_USER_STACK_TOP + 0x1000; img.phdr[1].p_offset = 0; img.phdr[1].p_filesz = 0x40; img.phdr[1].p_memsz = 0x40; img.phdr[1].p_align = 0x1000;
    enter_calls = 0;
    rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && enter_calls == 1, "segment starting just above the runtime page is fine (rc %d)", rc);


    /* ======================= DAY 48 (elf_run_info_t.out_total / out_last, used by `run`) ======================= */
    enter_hook = NULL; enter_say = "hello\n";
    mk(&img); rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && info.out_total == 7 && info.out_last == '!', "out_total counts puts + putchar chars (7), out_last == '!' (got %u, %c)", info.out_total, (int)info.out_last);
    enter_say = NULL;
    mk(&img); rc = elf_run(&img, T, &code, &info);
    CHECK(rc == ELF_OK && info.out_total == 1 && info.out_last == '!', "counters restart for every run (1, '!')");
    enter_say = "ab\n";
    mk(&img); rc = elf_run(&img, T, &code, &info);
    CHECK(info.out_total == 4, "3-char string + putchar = 4");
    enter_say = NULL;

    /* ======================= DAY 49 (argc/argv delivery) ======================= */
    {
        enter_say = NULL; enter_hook = NULL; enter_ret = 0;
        const uint32_t TOP = ELF_USER_STACK_TOP;
        /* argc==0 behaves like Day 47: frame_top == TOP, argv -> NULL word at TOP-4 */
        mk(&img); rc = elf_run(&img, T, &code, &info);
        CHECK(rc == ELF_OK && enter_argc == 0 && enter_stack_top == TOP && (uint32_t)(uintptr_t)enter_argv == TOP - 4 && spare_word == 0,
              "argc==0: frame_top==TOP, argv->NULL at TOP-4");

        const char *a1[] = { "hello.sw", "luma" };
        mk(&img); rc = elf_run_args(&img, T, 2, a1, &code, &info);
        CHECK(rc == ELF_OK && enter_argc == 2, "argc 2 delivered");
        CHECK(!strcmp(snap[0], "hello.sw") && !strcmp(snap[1], "luma"), "argv strings intact (%s, %s)", snap[0], snap[1]);
        CHECK(snap_ptr[2] == 0, "argv[argc] == NULL");
        CHECK(enter_stack_top % 16 == 0 && (enter_stack_top - 20 + 4) % 16 == 0, "frame_top 16-aligned; (esp+4) at entry 16-aligned");
        CHECK((uint32_t)(uintptr_t)enter_argv % 4 == 0 && (uint32_t)(uintptr_t)enter_argv < TOP && (uint32_t)(uintptr_t)enter_argv >= enter_stack_top,
              "argv array is 4-aligned, inside the stack, above frame_top");
        CHECK(snap_ptr[0] >= (uint32_t)(uintptr_t)enter_argv && snap_ptr[1] < TOP && snap_ptr[0] < snap_ptr[1], "strings above the pointer array, argv[0] lowest");
        CHECK(spare_word == 0, "[frame_top-4] is zero");
        CHECK(info.stack_used >= 2 * 4 + 9 + 5 && info.stack_used < 100, "stack_used covers strings+array (%u)", info.stack_used);

        /* empty argument and strings with spaces survive */
        const char *a2[] = { "p", "", "two words", "x" };
        mk(&img); rc = elf_run_args(&img, T, 4, a2, &code, &info);
        CHECK(rc == ELF_OK && !strcmp(snap[1], "") && !strcmp(snap[2], "two words") && !strcmp(snap[3], "x") && snap_ptr[4] == 0, "empty + spaced args intact");

        /* limits: exactly at the limit OK, one over -> ELF_ERR_ARGS and the program does not run */
        static const char *many[ELF_ARGS_MAX_COUNT + 1];
        for (uint32_t i = 0; i <= ELF_ARGS_MAX_COUNT; i++) many[i] = "a";
        mk(&img); enter_calls = 0; rc = elf_run_args(&img, T, (int)ELF_ARGS_MAX_COUNT, many, &code, &info);
        CHECK(rc == ELF_OK && enter_calls == 1 && enter_argc == (int)ELF_ARGS_MAX_COUNT && snap_ptr[ELF_ARGS_MAX_COUNT] == 0, "exactly ELF_ARGS_MAX_COUNT args accepted");
        mk(&img); enter_calls = 0; rc = elf_run_args(&img, T, (int)ELF_ARGS_MAX_COUNT + 1, many, &code, &info);
        CHECK(rc == ELF_ERR_ARGS && enter_calls == 0, "one arg too many -> ELF_ERR_ARGS, not run");
        mk(&img); enter_calls = 0; rc = elf_run_args(&img, T, -1, many, &code, &info);
        CHECK(rc == ELF_ERR_ARGS && enter_calls == 0, "negative argc -> ELF_ERR_ARGS");

        static char big[ELF_ARGS_MAX_BYTES + 8];
        memset(big, 'x', sizeof big);
        big[ELF_ARGS_MAX_BYTES - 1] = 0;                       /* strlen+1 == MAX_BYTES */
        const char *a3[] = { big };
        mk(&img); enter_calls = 0; rc = elf_run_args(&img, T, 1, a3, &code, &info);
        CHECK(rc == ELF_OK && enter_calls == 1, "one string of exactly ELF_ARGS_MAX_BYTES (with NUL) accepted (rc %d)", rc);
        big[ELF_ARGS_MAX_BYTES - 1] = 'x'; big[ELF_ARGS_MAX_BYTES] = 0;   /* one byte more */
        mk(&img); enter_calls = 0; rc = elf_run_args(&img, T, 1, a3, &code, &info);
        CHECK(rc == ELF_ERR_ARGS && enter_calls == 0, "one byte over ELF_ARGS_MAX_BYTES -> ELF_ERR_ARGS");
        const char *a4[] = { "ok", NULL };
        mk(&img); enter_calls = 0; rc = elf_run_args(&img, T, 2, a4, &code, &info);
        CHECK(rc == ELF_ERR_ARGS && enter_calls == 0, "NULL element in argv -> ELF_ERR_ARGS");
        mk(&img); enter_calls = 0; rc = elf_run_args(&img, T, 1, NULL, &code, &info);
        CHECK(rc == ELF_ERR_NULL && enter_calls == 0, "argv == NULL with argc>0 -> ELF_ERR_NULL");

        /* a failed args check must leave the image loaded for a retry? (loaded stays 1: nothing ran) */
        mk(&img); rc = elf_run_args(&img, T, -1, many, &code, &info);
        CHECK(img.loaded == 1, "ELF_ERR_ARGS: image still loaded (nothing ran)");
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
