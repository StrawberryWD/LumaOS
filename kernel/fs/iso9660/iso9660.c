/* kernel/fs/iso9660/iso9660.c - DAY63: see iso9660.h */
#include "iso9660.h"
#include "../../lib/string.h"

static block_device_t *g_dev;
static uint32_t g_root_lba, g_root_size, g_blocks;
static char     g_volume[33];
static filesystem_t g_fs;
static vfs_node_t   g_root;

static uint8_t  secbuf[2048];
static uint32_t secbuf_lba = 0xFFFFFFFFu;

static int read_sector(uint32_t lba)
{
    if (lba == secbuf_lba) return 0;
    if (!g_dev || g_dev->read(g_dev, lba, 1, secbuf) != 0) { secbuf_lba = 0xFFFFFFFFu; return -1; }
    secbuf_lba = lba;
    return 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int name_eq(const char *a, const char *b)
{
    while (*a && *b) { if (upc(*a) != upc(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* index-th real entry (. and .. not counted) of the directory (dir_lba, dir_size). 0 = ok, -1 = past the end */
static int dir_entry(uint32_t dir_lba, uint32_t dir_size, uint32_t index, iso_entry_t *out)
{
    uint32_t pos = 0, seen = 0;
    while (pos < dir_size) {
        if (read_sector(dir_lba + pos / 2048) != 0) return -1;
        uint32_t off = pos % 2048;
        uint8_t len = secbuf[off];
        if (len == 0) { pos = (pos / 2048 + 1) * 2048; continue; }      /* sector padding */
        if (off + len > 2048 || len < 34) return -1;                     /* corrupt */
        const uint8_t *r = &secbuf[off];
        uint8_t nl = r[32];
        if (!(nl == 1 && (r[33] == 0 || r[33] == 1))) {
            if (seen == index) {
                uint32_t k = 0;
                for (uint32_t i = 0; i < nl && k < sizeof(out->name) - 1; i++) {
                    if (r[33 + i] == ';') break;                         /* ";1" version suffix */
                    out->name[k++] = (char)r[33 + i];
                }
                if (k > 1 && out->name[k - 1] == '.') k--;               /* "NAME." -> "NAME" */
                out->name[k] = 0;
                out->lba = le32(r + 2);
                out->size = le32(r + 10);
                out->is_dir = (r[25] & 2) != 0;
                return 0;
            }
            seen++;
        }
        pos += len;
    }
    return -1;
}

static int find_in_dir(uint32_t dir_lba, uint32_t dir_size, const char *name, iso_entry_t *out)
{
    for (uint32_t i = 0; dir_entry(dir_lba, dir_size, i, out) == 0; i++)
        if (name_eq(out->name, name)) return 0;
    return -1;
}

int iso9660_lookup(const char *path, iso_entry_t *out)
{
    if (!g_dev) return -1;
    iso_entry_t cur;
    cur.lba = g_root_lba; cur.size = g_root_size; cur.is_dir = 1; cur.name[0] = 0;
    while (*path == '/') path++;
    while (*path) {
        char seg[32]; uint32_t n = 0;
        while (*path && *path != '/') { if (n >= sizeof(seg) - 1) return -1; seg[n++] = *path++; }
        seg[n] = 0;
        while (*path == '/') path++;
        if (n == 0) continue;
        if (!cur.is_dir) return -1;
        if (find_in_dir(cur.lba, cur.size, seg, &cur) != 0) return -1;
    }
    *out = cur;
    return 0;
}

int iso9660_read(const iso_entry_t *e, uint32_t pos, void *buf, uint32_t n)
{
    if (!e || e->is_dir || !g_dev) return -1;
    if (pos >= e->size) return 0;
    if (n > e->size - pos) n = e->size - pos;
    uint8_t *dst = buf;
    uint32_t done = 0;
    while (done < n) {
        uint32_t p = pos + done;
        if (read_sector(e->lba + p / 2048) != 0) return -1;
        uint32_t off = p % 2048, chunk = 2048 - off;
        if (chunk > n - done) chunk = n - done;
        for (uint32_t i = 0; i < chunk; i++) dst[done + i] = secbuf[off + i];
        done += chunk;
    }
    return (int)done;
}

int iso9660_read_file(const char *rel_path, void *buf, uint32_t max)
{
    iso_entry_t e;
    if (iso9660_lookup(rel_path, &e) != 0 || e.is_dir || e.size > max) return -1;
    int n = iso9660_read(&e, 0, buf, e.size);
    return n == (int)e.size ? n : -1;
}

/* ---------------- VFS adapter ---------------- */
static int v_open(vfs_node_t *node, file_t *f)
{
    if (!node || !f || node->type != VFS_TYPE_FILE) return -1;
    f->node = node; f->pos = 0;
    return 0;
}
static int v_read(file_t *f, void *buf, uint32_t size)
{
    if (!f || !f->node || f->node->type != VFS_TYPE_FILE) return -1;
    iso_entry_t e;
    e.is_dir = 0; e.lba = (uint32_t)(uintptr_t)f->node->fs_private; e.size = f->node->size; e.name[0] = 0;
    int n = iso9660_read(&e, f->pos, buf, size);
    if (n > 0) f->pos += (uint32_t)n;
    return n;
}
static int v_close(file_t *f) { if (!f || !f->node) return -1; f->node = 0; return 0; }
static int v_readdir(vfs_node_t *dir, uint32_t index, vfs_node_t *out)
{
    if (!dir || !out || dir->type != VFS_TYPE_DIR) return -1;
    iso_entry_t e;
    if (dir_entry((uint32_t)(uintptr_t)dir->fs_private, dir->size, index, &e) != 0) return -1;
    uint32_t i = 0;
    while (e.name[i] && i < sizeof(out->name) - 1) { out->name[i] = e.name[i]; i++; }
    out->name[i] = 0;
    out->type = e.is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    out->size = e.size;                       /* directories carry their extent length: readdir needs it */
    out->fs = dir->fs;
    out->fs_private = (void *)(uintptr_t)e.lba;
    return 0;
}

filesystem_t *iso9660_vfs_mount(block_device_t *dev)
{
    if (!dev || dev->sector_size != 2048) return 0;
    g_dev = dev; secbuf_lba = 0xFFFFFFFFu;
    if (read_sector(16) != 0) return 0;
    if (secbuf[0] != 1 || memcmp(secbuf + 1, "CD001", 5) != 0 || secbuf[6] != 1) return 0;
    if ((secbuf[128] | (secbuf[129] << 8)) != 2048) return 0;          /* logical block size */
    g_blocks = le32(secbuf + 80);
    for (int i = 0; i < 32; i++) g_volume[i] = (char)secbuf[40 + i];
    int k = 32; while (k > 0 && (g_volume[k - 1] == ' ' || g_volume[k - 1] == 0)) k--;
    g_volume[k] = 0;
    const uint8_t *rr = secbuf + 156;                      /* root directory record inside the PVD */
    g_root_lba = le32(rr + 2); g_root_size = le32(rr + 10);
    memset(&g_root, 0, sizeof g_root);
    g_root.name[0] = 'C'; g_root.name[1] = 'D'; g_root.name[2] = '0';
    g_root.type = VFS_TYPE_DIR; g_root.size = g_root_size; g_root.fs = &g_fs;
    g_root.fs_private = (void *)(uintptr_t)g_root_lba;
    g_fs.name = "iso9660"; g_fs.open = v_open; g_fs.read = v_read; g_fs.readdir = v_readdir; g_fs.close = v_close;
    g_fs.root = &g_root; g_fs.fs_private = 0;
    return &g_fs;
}

int iso9660_vfs_open_path(const char *path, vfs_node_t *out)
{
    if (!path || !out || !g_dev) return -1;
    const char *sub = path;
    while (*sub && *sub != '/') sub++;                     /* skip the "CD0" mount component */
    iso_entry_t e;
    if (iso9660_lookup(sub, &e) != 0) return -1;
    uint32_t i = 0;
    while (e.name[i] && i < sizeof(out->name) - 1) { out->name[i] = e.name[i]; i++; }
    if (i == 0) { out->name[0] = 'C'; out->name[1] = 'D'; out->name[2] = '0'; i = 3; }
    out->name[i] = 0;
    out->type = e.is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    out->size = e.size;
    out->fs = &g_fs;
    out->fs_private = (void *)(uintptr_t)e.lba;
    return 0;
}

const char *iso9660_volume_name(void) { return g_volume; }
uint32_t iso9660_volume_blocks(void) { return g_blocks; }
