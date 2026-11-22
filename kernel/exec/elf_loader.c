/* kernel/exec/elf_loader.c
 * LumaOS 5.0 - elf_load(): 通过 VFS 打开并验证一个 ELF32 可执行文件
 *
 * DAY44：原理见 elf.h 的 DAY44 注释，接口说明见 elf_loader.h。
 * DAY45：新增 elf_load_segments()——加载的后半场，原理见 elf.h 的 DAY45 注释。
 *
 * 为什么能“同时支持 FAT 与 LMFS”：本文件只用 VFS 的 filesystem_t 函数
 * 指针（open/read/close）读文件，唯一需要知道文件系统种类的地方是“按
 * 路径找到文件”这一步——FAT 用 fat_open()，LMFS 用 lmfs_vfs_open_path()
 * （与 shell/cmd/read.c、elfinfo.c 完全相同的分派规则：取路径第一个 '/'
 * 之前的挂载点名，查 vfs_lookup_mount()，再看 fs->name 是否为 "lmfs"）。
 * 找到之后，两种文件系统在这里就没有区别了。
 */
#include "elf_loader.h"
#include "../fs/fat/fat_vfs.h"
#include "../fs/lmfs/lmfs_vfs.h"
#include "../lib/string.h"

int elf_open(const char *path, vfs_node_t *node, file_t *file, const char **why)
{
    const char *dummy;
    if (why == (const char **)0) why = &dummy;
    *why = "not found";

    if (path == (const char *)0 || node == (vfs_node_t *)0 ||
        file == (file_t *)0)
        return ELF_ERR_NULL;

    /* 取出挂载点名（第一个 '/' 之前），判断它是不是 LMFS 卷。 */
    const char *sep = strchr(path, '/');
    filesystem_t *target_fs = (filesystem_t *)0;
    if (sep != (char *)0) {
        char mount_name[8];
        uint32_t len = (uint32_t)(sep - path);
        if (len > 0 && len < sizeof(mount_name)) {
            memcpy(mount_name, path, len);
            mount_name[len] = '\0';
            target_fs = vfs_lookup_mount(mount_name);
        }
    }

    if (target_fs && target_fs->name && strcmp(target_fs->name, "lmfs") == 0) {
        if (lmfs_vfs_open_path(path, node) != 0)
            return ELF_ERR_OPEN;
    } else {
        int err = fat_open(path, node);
        if (err != FAT_OK) {
            *why = fat_strerror(err);
            return ELF_ERR_OPEN;
        }
    }

    if (node->type != VFS_TYPE_FILE)
        return ELF_ERR_IS_DIR;

    if (!node->fs || !node->fs->open || !node->fs->read ||
        node->fs->open(node, file) != 0) {
        *why = "open failed";
        return ELF_ERR_IO;
    }
    return ELF_OK;
}

int elf_read_at(file_t *file, uint32_t off, void *buf, uint32_t n)
{
    if (!file || !file->node || !file->node->fs || !file->node->fs->read)
        return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t got = 0;
    file->pos = off;                 /* “seek” */
    while (got < n) {
        /* 一次 read 可能少于请求量（例如跨簇/跨块边界），循环读满或遇 EOF。 */
        int r = file->node->fs->read(file, dst + got, n - got);
        if (r < 0)  return -1;
        if (r == 0) break;           /* EOF */
        got += (uint32_t)r;
    }
    return (int)got;
}

void elf_close(file_t *file)
{
    if (file && file->node && file->node->fs && file->node->fs->close)
        file->node->fs->close(file);
}

int elf_load(const char *path, elf_image_t *img)
{
    if (path == (const char *)0 || img == (elf_image_t *)0)
        return ELF_ERR_NULL;

    vfs_node_t node;
    file_t     file;
    const char *why;
    int rc = elf_open(path, &node, &file, &why);
    if (rc != ELF_OK)
        return rc;

    memset(img, 0, sizeof(*img));
    img->file_size = node.size;

    /* ---- 第 1 步：ELF Header (52 B) ---- */
    uint8_t head[ELF32_EHDR_SIZE];
    int got = 0;
    if (node.size == 0) {
        /* 0 字节文件没有簇链：FAT 的 read 对它返回错误而不是 EOF（elfinfo 在
         * Day 43 就为此特判过）。这里直接当作“连 Header 都装不下”处理，
         * 报 TOO_SHORT 而不是 I/O 错误——它是个坏文件，不是坏磁盘。 */
        rc = ELF_ERR_TOO_SHORT;
        goto out;
    }
    got = elf_read_at(&file, 0, head, ELF32_EHDR_SIZE);
    if (got < 0) { rc = ELF_ERR_IO; goto out; }

    rc = elf32_parse_header(head, (uint32_t)got, &img->ehdr);
    if (rc != ELF_OK) goto out;                     /* 含 TOO_SHORT / BAD_MAGIC */
    rc = elf32_check_header(&img->ehdr, node.size);
    if (rc != ELF_OK) goto out;

    /* ---- 第 2 步：Program Header 表 ---- */
    uint32_t n = img->ehdr.e_phnum;                 /* check 已保证 > 0、每项 32 B */
    if (n > ELF_MAX_PHDRS) { rc = ELF_ERR_TOO_MANY_PHDRS; goto out; }

    /* 表在文件里的位置由 e_phoff 给出（常见是紧跟 Header 的 52，但不能假定）。
     * 逐项读 32 字节再解析——不把原始字节“强转”成结构体。 */
    elf32_phdr_t ph[ELF_MAX_PHDRS];
    uint8_t      raw[ELF32_PHDR_SIZE];
    for (uint32_t i = 0; i < n; i++) {
        got = elf_read_at(&file, img->ehdr.e_phoff + i * ELF32_PHDR_SIZE,
                          raw, ELF32_PHDR_SIZE);
        if (got != (int)ELF32_PHDR_SIZE) { rc = ELF_ERR_IO; goto out; }
        elf32_parse_phdr(raw, &ph[i]);
    }

    /* ---- 第 3 步：校验每个 PT_LOAD 与入口点 ---- */
    {
        elf32_ehdr_t eh = img->ehdr;                /* check_phdrs 会整体重写 *img */
        rc = elf32_check_phdrs(&eh, ph, n, node.size, img);
    }

out:
    elf_close(&file);
    return rc;
}

/* ===================================================================
 * DAY45: elf_load_segments()
 * =================================================================== */

/* elf32_load_segments() 要的“读函数”：ctx 就是一个已打开的 file_t。
 * 直接复用 Day 44 的 elf_read_at()（把 file->pos 设到 off 再循环 read），
 * 所以 FAT12/16/32 与 LMFS 在这里依旧没有任何区别。 */
static int vfs_read_cb(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    return elf_read_at((file_t *)ctx, off, buf, n);
}

int elf_load_segments(const char *path, elf_image_t *img, elf_load_stats_t *st)
{
    if (path == (const char *)0 || img == (elf_image_t *)0)
        return ELF_ERR_NULL;

    vfs_node_t node;
    file_t     file;
    const char *why;
    int rc = elf_open(path, &node, &file, &why);
    if (rc != ELF_OK) {
        img->loaded = 0;
        return rc;
    }

    /* 文件大小必须还和 elf_load() 验证时一样。不一样说明两次调用之间有人
     * 改过它——img 里的 p_offset/p_filesz 校验是针对旧大小做的，不能再用。 */
    if (node.size != img->file_size) {
        img->loaded = 0;
        rc = ELF_ERR_CHANGED;
        goto out;
    }

    rc = elf32_load_segments(img, vfs_read_cb, &file, st);

out:
    elf_close(&file);
    return rc;
}
