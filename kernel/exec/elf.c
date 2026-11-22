/* kernel/exec/elf.c
 * LumaOS 5.0 - ELF32 Header / Program Header 解析与校验
 *
 * DAY43：Header 的解析与校验。原理与设计说明见 elf.h 的文件头注释。
 * DAY44：新增 elf32_parse_phdr()/elf32_check_phdrs()——对 Program Header
 *        表的逐项解析与“值不值得加载”的校验（规则与理由见 elf.h 的
 *        DAY44 注释）。仍然是纯内存函数：不碰 VFS、不碰磁盘。
 * DAY45：新增 elf_fnv1a()/elf32_load_segments()——把已验证的 PT_LOAD 段
 *        搬进内存（拷贝 + .bss 清零 + 回读校验）。字节从调用者给的读函数
 *        来，所以本文件仍然不碰 VFS，仍然可以在宿主机上做单元测试。
 * 本文件只依赖 <stdint.h>，不依赖内核其它部分，宿主机可直接编译测试。
 */
#include "elf.h"

/* 小端读取：显式逐字节组装，与 CPU 字节序、对齐无关。 */
static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int elf32_parse_header(const void *buf, uint32_t len, elf32_ehdr_t *out)
{
    if (buf == (const void *)0 || out == (elf32_ehdr_t *)0)
        return ELF_ERR_NULL;
    if (len < ELF32_EHDR_SIZE)
        return ELF_ERR_TOO_SHORT;

    const unsigned char *p = (const unsigned char *)buf;

    /* 先拷出 16 字节 e_ident —— 无论后面成败，调用者都能打印它。 */
    for (int i = 0; i < EI_NIDENT; i++)
        out->e_ident[i] = p[i];

    /* 1) 魔数：7F 'E' 'L' 'F'。 */
    if (p[EI_MAG0] != ELFMAG0 || p[EI_MAG1] != ELFMAG1 ||
        p[EI_MAG2] != ELFMAG2 || p[EI_MAG3] != ELFMAG3)
        return ELF_ERR_BAD_MAGIC;

    /* 2) 字长：后面 Header 的字段宽度取决于它（32 位 Header 52 字节，
     *    64 位 Header 64 字节，字段偏移完全不同）。 */
    if (p[EI_CLASS] != ELFCLASS32)
        return ELF_ERR_NOT_32BIT;

    /* 3) 字节序：必须先确认是小端，才有资格按小端去解释多字节整数。 */
    if (p[EI_DATA] != ELFDATA2LSB)
        return ELF_ERR_NOT_LE;

    /* 4) 现在可以安全地解释 e_ident 之后的全部字段。 */
    out->e_type      = rd16(p + 16);
    out->e_machine   = rd16(p + 18);
    out->e_version   = rd32(p + 20);
    out->e_entry     = rd32(p + 24);
    out->e_phoff     = rd32(p + 28);
    out->e_shoff     = rd32(p + 32);
    out->e_flags     = rd32(p + 36);
    out->e_ehsize    = rd16(p + 40);
    out->e_phentsize = rd16(p + 42);
    out->e_phnum     = rd16(p + 44);
    out->e_shentsize = rd16(p + 46);
    out->e_shnum     = rd16(p + 48);
    out->e_shstrndx  = rd16(p + 50);
    return ELF_OK;
}

int elf32_check_header(const elf32_ehdr_t *h, uint32_t file_size)
{
    if (h == (const elf32_ehdr_t *)0)
        return ELF_ERR_NULL;

    if (h->e_ident[EI_VERSION] != EV_CURRENT) return ELF_ERR_BAD_IDENT_VER;
    if (h->e_version           != EV_CURRENT) return ELF_ERR_BAD_VERSION;
    if (h->e_machine           != EM_386)     return ELF_ERR_BAD_MACHINE;
    /* ET_DYN 也会在这里被拒绝：现代 gcc 默认产出 PIE（类型是 ET_DYN），
     * 它要求加载器做重定位。LumaOS 的 .sw 用 `-static -no-pie` 链接成
     * 固定地址的 ET_EXEC，加载器因此不需要重定位。 */
    if (h->e_type              != ET_EXEC)    return ELF_ERR_BAD_TYPE;
    if (h->e_ehsize            != ELF32_EHDR_SIZE) return ELF_ERR_BAD_EHSIZE;
    if (h->e_entry             == 0)          return ELF_ERR_NO_ENTRY;

    if (h->e_phnum == 0)
        return ELF_ERR_NO_PHDRS;
    if (h->e_phentsize != ELF32_PHDR_SIZE)
        return ELF_ERR_BAD_PHENTSIZE;

    /* Program Header 表必须整张落在文件里：
     *   phoff + phnum*phentsize <= file_size
     * 写成 “phoff <= size && tbl <= size - phoff” 避免 32 位加法溢出
     * （phnum 与 phentsize 都是 16 位，相乘最大 65535*65535 < 2^32，
     *  本身不会溢出）。同时表不能压在 ELF Header 里（phoff >= 52）。 */
    uint32_t tbl = (uint32_t)h->e_phnum * (uint32_t)h->e_phentsize;
    if (h->e_phoff < ELF32_EHDR_SIZE ||
        h->e_phoff > file_size ||
        tbl > file_size - h->e_phoff)
        return ELF_ERR_PH_RANGE;

    return ELF_OK;
}

/* ===================================================================
 * DAY44: Program Header
 * =================================================================== */

void elf32_parse_phdr(const void *buf, elf32_phdr_t *out)
{
    const unsigned char *p = (const unsigned char *)buf;
    /* 32 位 Program Header 的字段顺序（与 64 位版不同！）：
     * type offset vaddr paddr filesz memsz flags align，各 4 字节。 */
    out->p_type   = rd32(p +  0);
    out->p_offset = rd32(p +  4);
    out->p_vaddr  = rd32(p +  8);
    out->p_paddr  = rd32(p + 12);
    out->p_filesz = rd32(p + 16);
    out->p_memsz  = rd32(p + 20);
    out->p_flags  = rd32(p + 24);
    out->p_align  = rd32(p + 28);
}

/* 一个 PT_LOAD 是否“有内容要装入内存”（memsz==0 的段规范允许存在，忽略） */
static int is_live_load(const elf32_phdr_t *p)
{
    return p->p_type == PT_LOAD && p->p_memsz != 0;
}

int elf32_check_phdrs(const elf32_ehdr_t *h, const elf32_phdr_t *ph, uint32_t n,
                      uint32_t file_size, elf_image_t *img)
{
    if (h == (const elf32_ehdr_t *)0 || ph == (const elf32_phdr_t *)0 ||
        img == (elf_image_t *)0)
        return ELF_ERR_NULL;
    if (n > ELF_MAX_PHDRS)
        return ELF_ERR_TOO_MANY_PHDRS;

    /* 先整表扫一遍动态链接标记：不管它排在第几项，都应该报“要动态
     * 链接”这个根因，而不是被前面某个 PT_LOAD 的次要错误抢先。 */
    for (uint32_t i = 0; i < n; i++)
        if (ph[i].p_type == PT_INTERP || ph[i].p_type == PT_DYNAMIC)
            return ELF_ERR_DYNAMIC;

    uint32_t n_load = 0, lo = 0xFFFFFFFFu, hi = 0, tf = 0, tm = 0;

    for (uint32_t i = 0; i < n; i++) {
        const elf32_phdr_t *p = &ph[i];
        if (!is_live_load(p))
            continue;

        /* filesz > memsz：文件里的内容比内存里的段还大，没有意义。 */
        if (p->p_filesz > p->p_memsz)
            return ELF_ERR_SEG_SIZE;

        /* 文件内容范围：先判 offset 再做减法，永不回绕。 */
        if (p->p_offset > file_size || p->p_filesz > file_size - p->p_offset)
            return ELF_ERR_SEG_FILE_RANGE;

        /* 内存范围必须整体落在 [MIN, MAX)：同样“先比上界再减”。 */
        if (p->p_vaddr < ELF_LOAD_MIN_ADDR || p->p_vaddr >= ELF_LOAD_MAX_ADDR ||
            p->p_memsz > ELF_LOAD_MAX_ADDR - p->p_vaddr)
            return ELF_ERR_SEG_ADDR;

        /* DAY47: 不得压在用户栈上（栈是内核为程序留的，不是程序可以占的）。
         * 上面已保证 p_vaddr+p_memsz 不回绕，这里可以直接比较区间。 */
        if (p->p_vaddr < ELF_USER_STACK_TOP + 0x1000u &&      /* DAY55: 栈正上方那一页是运行时页（urt.h） */
            ELF_USER_STACK_BASE < p->p_vaddr + p->p_memsz)
            return ELF_ERR_SEG_STACK;

        /* 对齐：p_align 为 0/1 表示无约束；否则必须是 2 的幂，且
         * p_vaddr ≡ p_offset (mod p_align)。 */
        if (p->p_align > 1) {
            uint32_t m = p->p_align - 1;
            if (p->p_align & m)
                return ELF_ERR_SEG_ALIGN;
            if ((p->p_vaddr & m) != (p->p_offset & m))
                return ELF_ERR_SEG_ALIGN;
        }

        /* 与之前已验证过的 PT_LOAD 比较内存区间。前面的段已经通过了
         * 地址范围检查，所以 q->p_vaddr + q->p_memsz 不会回绕。 */
        for (uint32_t j = 0; j < i; j++) {
            const elf32_phdr_t *q = &ph[j];
            if (!is_live_load(q))
                continue;
            if (p->p_vaddr < q->p_vaddr + q->p_memsz &&
                q->p_vaddr < p->p_vaddr + p->p_memsz)
                return ELF_ERR_SEG_OVERLAP;
        }

        n_load++;
        if (p->p_vaddr < lo) lo = p->p_vaddr;
        if (p->p_vaddr + p->p_memsz > hi) hi = p->p_vaddr + p->p_memsz;
        tf += p->p_filesz;       /* 至多 8 项、每项 < 240MB，不会回绕 */
        tm += p->p_memsz;
    }

    if (n_load == 0)
        return ELF_ERR_NO_LOAD;

    /* 入口点：必须落在某个“可执行的、且来自文件”的 PT_LOAD 中。 */
    int entry_seg = -1;
    for (uint32_t i = 0; i < n; i++) {
        const elf32_phdr_t *p = &ph[i];
        if (!is_live_load(p) || !(p->p_flags & PF_X))
            continue;
        if (h->e_entry >= p->p_vaddr && h->e_entry - p->p_vaddr < p->p_filesz) {
            entry_seg = (int)i;
            break;
        }
    }
    if (entry_seg < 0)
        return ELF_ERR_ENTRY_NOT_EXEC;

    img->file_size = file_size;
    img->ehdr = *h;
    for (uint32_t i = 0; i < n; i++)
        img->phdr[i] = ph[i];
    img->n_load       = n_load;
    img->load_lo      = lo;
    img->load_hi      = hi;
    img->total_filesz = tf;
    img->total_memsz  = tm;
    img->entry_seg    = (uint32_t)entry_seg;
    img->loaded       = 0;          /* DAY45: 校验通过 != 已装入内存 */
    return ELF_OK;
}

/* ===================================================================
 * DAY45: 把 PT_LOAD 段装入内存
 * =================================================================== */

uint32_t elf_fnv1a(uint32_t h, const void *buf, uint32_t n)
{
    const unsigned char *p = (const unsigned char *)buf;
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;                 /* FNV 32 位素数；只用 32 位乘法 */
    }
    return h;
}

/* 记录一次回读失败的位置，供 elfmap 打印。 */
static int verify_fail(elf_load_stats_t *st, uint32_t addr, uint32_t want, uint32_t got)
{
    st->fail_addr = addr;
    st->fail_want = want;
    st->fail_got  = got;
    return ELF_ERR_VERIFY;
}

int elf32_load_segments(elf_image_t *img, elf_read_fn rd, void *ctx,
                        elf_load_stats_t *st)
{
    if (img == (elf_image_t *)0 || rd == (elf_read_fn)0)
        return ELF_ERR_NULL;

    elf_load_stats_t local;
    if (st == (elf_load_stats_t *)0)
        st = &local;
    {   /* 不依赖 string.h：逐字节清零统计结构 */
        unsigned char *z = (unsigned char *)st;
        for (uint32_t i = 0; i < (uint32_t)sizeof(*st); i++)
            z[i] = 0;
    }
    img->loaded = 0;

    /* ---- 第 0 步：不盲信调用者传来的 img，重新校验一遍 ----
     * 目标地址/长度马上要被当作“可以写”的依据，所以必须保证它们满足
     * Day 44 的全部规则（用户装载区内、不回绕、互不重叠……）。校验结果
     * 写进局部的 chk，通过后整体取代 *img（派生字段以重新计算的为准）。 */
    int rc = elf32_check_header(&img->ehdr, img->file_size);
    if (rc != ELF_OK)
        return rc;
    elf_image_t chk;
    rc = elf32_check_phdrs(&img->ehdr, img->phdr, img->ehdr.e_phnum,
                           img->file_size, &chk);
    if (rc != ELF_OK)
        return rc;
    chk.loaded = 0;
    *img = chk;

    /* ---- 第 1 步：逐个 PT_LOAD 搬运 ---- */
    unsigned char buf[ELF_LOAD_CHUNK];

    for (uint32_t i = 0; i < img->ehdr.e_phnum; i++) {
        const elf32_phdr_t *p = &img->phdr[i];
        if (!is_live_load(p))
            continue;

        /* 目的地：p_vaddr（无分页：线性地址 == 物理地址）。volatile，
         * 保证“写”和“读回”都真的发生在内存上。 */
        volatile unsigned char *dst = (volatile unsigned char *)(uintptr_t)p->p_vaddr;

        /* (a) 文件部分 [p_offset, +p_filesz) -> [p_vaddr, +p_filesz)，
         *     每块写完立刻读回比较。 */
        uint32_t h = ELF_FNV_SEED;
        for (uint32_t done = 0; done < p->p_filesz; ) {
            uint32_t n = p->p_filesz - done;
            if (n > ELF_LOAD_CHUNK)
                n = ELF_LOAD_CHUNK;

            /* p_offset+done 不会回绕：Day 44 已保证 p_offset+p_filesz <= file_size。 */
            int got = rd(ctx, p->p_offset + done, buf, n);
            if (got < 0 || (uint32_t)got != n)
                return ELF_ERR_IO;              /* 文件比 img 说的短，或读盘失败 */

            for (uint32_t k = 0; k < n; k++)
                dst[done + k] = buf[k];
            for (uint32_t k = 0; k < n; k++) {
                unsigned char back = dst[done + k];
                if (back != buf[k])
                    return verify_fail(st, p->p_vaddr + done + k, buf[k], back);
            }
            h = elf_fnv1a(h, buf, n);
            done += n;
        }
        st->seg_sum[i]    = h;
        st->bytes_copied += p->p_filesz;

        /* (b) .bss：[p_vaddr+filesz, p_vaddr+memsz) 清零并确认。
         *     不假设内存本来就是 0。 */
        for (uint32_t k = p->p_filesz; k < p->p_memsz; k++) {
            dst[k] = 0;
            unsigned char back = dst[k];
            if (back != 0)
                return verify_fail(st, p->p_vaddr + k, 0, back);
        }
        st->bytes_zeroed += p->p_memsz - p->p_filesz;
        st->segs++;
    }

    img->loaded = 1;                    /* 全部成功，才算“已装入” */
    return ELF_OK;
}

const char *elf_ptype_name(uint32_t t)
{
    switch (t) {
    case PT_NULL:    return "NULL";
    case PT_LOAD:    return "LOAD";
    case PT_DYNAMIC: return "DYNAMIC";
    case PT_INTERP:  return "INTERP";
    case PT_NOTE:    return "NOTE";
    case PT_SHLIB:   return "SHLIB";
    case PT_PHDR:    return "PHDR";
    case PT_TLS:     return "TLS";
    case 0x6474e550u: return "EH_FRAME";
    case 0x6474e551u: return "GNU_STACK";
    case 0x6474e552u: return "GNU_RELRO";
    default:         return "?";
    }
}

const char *elf_strerror(int err)
{
    switch (err) {
    case ELF_OK:                return "ok";
    case ELF_ERR_NULL:          return "null argument";
    case ELF_ERR_TOO_SHORT:     return "file shorter than the 52-byte ELF header";
    case ELF_ERR_BAD_MAGIC:     return "bad magic (not 7F 45 4C 46)";
    case ELF_ERR_NOT_32BIT:     return "not ELF32 (EI_CLASS != 1)";
    case ELF_ERR_NOT_LE:        return "not little-endian (EI_DATA != 1)";
    case ELF_ERR_BAD_IDENT_VER: return "bad EI_VERSION (must be 1)";
    case ELF_ERR_BAD_VERSION:   return "bad e_version (must be 1)";
    case ELF_ERR_BAD_MACHINE:   return "wrong CPU (e_machine != 3 / i386)";
    case ELF_ERR_BAD_TYPE:      return "not ET_EXEC (link with -static -no-pie)";
    case ELF_ERR_BAD_EHSIZE:    return "bad e_ehsize (must be 52)";
    case ELF_ERR_BAD_PHENTSIZE: return "bad e_phentsize (must be 32)";
    case ELF_ERR_NO_PHDRS:      return "no program headers (e_phnum == 0)";
    case ELF_ERR_PH_RANGE:      return "program header table outside the file";
    case ELF_ERR_NO_ENTRY:      return "entry point is 0";
    case ELF_ERR_TOO_MANY_PHDRS: return "too many program headers (max 8)";
    case ELF_ERR_DYNAMIC:       return "dynamic executable (PT_INTERP/PT_DYNAMIC) not supported";
    case ELF_ERR_NO_LOAD:       return "no loadable segment (no PT_LOAD)";
    case ELF_ERR_SEG_SIZE:      return "segment p_filesz > p_memsz";
    case ELF_ERR_SEG_FILE_RANGE: return "segment data outside the file";
    case ELF_ERR_SEG_ADDR:      return "segment address outside the user load area";
    case ELF_ERR_SEG_ALIGN:     return "segment alignment invalid (vaddr != offset mod align)";
    case ELF_ERR_SEG_OVERLAP:   return "two PT_LOAD segments overlap in memory";
    case ELF_ERR_ENTRY_NOT_EXEC: return "entry point not inside an executable segment";
    case ELF_ERR_OPEN:          return "cannot open file";
    case ELF_ERR_IS_DIR:        return "path is a directory";
    case ELF_ERR_IO:            return "read error (short read of headers or segment data)";
    case ELF_ERR_CHANGED:       return "file changed since it was validated";
    case ELF_ERR_VERIFY:        return "memory readback mismatch (no RAM at the load address?)";
    case ELF_ERR_ARGS:          return "too many or too long arguments";
    case ELF_ERR_NO_ASPACE:     return "no user address space (Ring 3 needs its pages and runtime page)";
    case ELF_ERR_NO_MEMORY:     return "out of physical page frames (cannot build address space)";
    case ELF_ERR_SEG_STACK:     return "segment overlaps the user stack area";
    case ELF_ERR_NO_STACK:      return "user stack memory readback mismatch (no RAM?)";
    case ELF_ERR_STACK_OVERFLOW: return "user stack overflow (guard area overwritten)";
    case ELF_ERR_NOT_LOADED:    return "image not loaded into memory (refusing to run)";
    default:                    return "unknown error";
    }
}

const char *elf_class_name(uint8_t v)
{
    switch (v) {
    case ELFCLASSNONE: return "none";
    case ELFCLASS32:   return "ELF32";
    case ELFCLASS64:   return "ELF64";
    default:           return "?";
    }
}

const char *elf_data_name(uint8_t v)
{
    switch (v) {
    case ELFDATANONE: return "none";
    case ELFDATA2LSB: return "little endian";
    case ELFDATA2MSB: return "big endian";
    default:          return "?";
    }
}

const char *elf_osabi_name(uint8_t v)
{
    switch (v) {
    case ELFOSABI_SYSV:  return "SYSV";
    case ELFOSABI_LINUX: return "Linux";
    default:             return "?";
    }
}

const char *elf_type_name(uint16_t v)
{
    switch (v) {
    case ET_NONE: return "NONE";
    case ET_REL:  return "REL";
    case ET_EXEC: return "EXEC";
    case ET_DYN:  return "DYN";
    case ET_CORE: return "CORE";
    default:      return "?";
    }
}

const char *elf_machine_name(uint16_t v)
{
    switch (v) {
    case EM_NONE:   return "none";
    case EM_386:    return "Intel 80386";
    case EM_X86_64: return "x86-64";
    default:        return "?";
    }
}