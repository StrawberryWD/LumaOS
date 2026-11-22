/* tests/day15_fat_mount_host_test.c — LumaOS 5.0 host-side logic test
 *
 * DAY15 (Week 3, Day 15 of the 5.0 filesystem roadmap):
 *   "重构 FAT 通用层：分离 FAT12/16/32 的共性（BPB 解析、目录项结构），
 *    创建 fat_common.c。一个 fat_mount() 根据 BPB 的 BytesPerSector 和
 *    TotalSectors 自动判断类型。"
 *
 *   Why this test exists SEPARATELY from the kmain.c DAY15 boot-time
 *   test (see kmain.c's own DAY15 comment): this project's only two
 *   FAT fixtures on disk (tests/fat12-bpb.bin, tests/fat12-with-file.img)
 *   are both real `mkfs.fat -F 12` volumes — there is no FAT16 or
 *   FAT32 fixture yet (that's Day 16/17's job), so a QEMU boot test
 *   against real media literally cannot exercise fat_classify_type()'s
 *   FAT16/FAT32 branches or its exact 4085/65525 cluster-count
 *   boundaries. This host test builds SYNTHETIC boot sectors — plain
 *   in-memory byte buffers laid out exactly like a real FAT BPB, fed
 *   through a fake in-memory block_device_t — to exercise the parts
 *   real media on this project can't reach yet, plus a regression
 *   check that the Day 15 refactor of fat12_parse_bpb()/fat12_mount()
 *   didn't change any Day 9-14 observable behavior.
 *
 *   This is a HOST-side test: it compiles fat_common.c, fat12.c, and
 *   kernel/lib/string.c directly (the same freestanding C, just built
 *   for the host instead of -m32 kernel target-linked into the OS
 *   image) and links them against this file's own main(), which uses
 *   ordinary libc (printf/exit) since IT doesn't need to be
 *   freestanding — only the kernel-side .c files it's testing do. This
 *   mirrors the "独立 Python FAT-table parse + a host-compiled fat12.c
 *   test" cross-validation approach kmain.c's own DAY11 comment
 *   mentions this project has used since Week 2, just written in C
 *   instead of Python so it can link the real fat_common.c/fat12.c
 *   translation units directly rather than re-implementing their
 *   logic in a second language.
 *
 * Build (run from the repository root):
 *   gcc -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
 *       -Ikernel/fs/fat -Ikernel/drivers/block -Ikernel/core \
 *       -c kernel/fs/fat/fat_common.c -o /tmp/fat_common.o
 *   gcc -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
 *       -Ikernel/fs/fat -Ikernel/drivers/block -Ikernel/core \
 *       -c kernel/fs/fat/fat12.c -o /tmp/fat12.o
 *   gcc -m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
 *       -Ikernel/core -c kernel/lib/string.c -o /tmp/string.o
 *   gcc -m32 -Ikernel/fs/fat -Ikernel/drivers/block \
 *       tests/day15_fat_mount_host_test.c \
 *       /tmp/fat_common.o /tmp/fat12.o /tmp/string.o \
 *       -o /tmp/day15_fat_mount_host_test
 *   /tmp/day15_fat_mount_host_test
 *
 * NOTE: if your host is missing 32-bit libc dev headers (`gcc -m32`
 * fails on this file specifically with "bits/libc-header-start.h: No
 * such file or directory" — a missing gcc-multilib/libc6-dev-i386
 * package, NOT a bug in this test), drop every `-m32` above. None of
 * fat_common.c/fat12.c/string.c or this test file depend on 32-bit
 * pointer width or any x86-specific instruction — they're portable C
 * — so a plain native (64-bit host) build exercises the exact same
 * logic just as validly; only the real kernel image itself (built via
 * build.sh, which does need -m32 for the actual i386 boot target)
 * requires 32-bit codegen.
 *
 * Exit code 0 and "ALL TESTS PASSED" on success; nonzero and a FAIL
 * line naming the failing case otherwise.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "fat_common.h"
#include "fat12.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
        g_failures++; \
    } else { \
        printf("  ok: %s\n", (msg)); \
    } \
} while (0)

/* ---- Fake in-memory block device ----
 * A minimal block_device_t backend that reads sectors out of a plain
 * malloc'd byte array instead of real IDE hardware — exactly the kind
 * of "swap the backend, nothing above block.h notices" scenario
 * block.h's own DAY6 header comment describes as the whole point of
 * the block_device_t abstraction. */
typedef struct {
    uint8_t *data;
    uint32_t sector_count;
    int fail_reads; /* if nonzero, ->read() always returns -1 */
} fake_disk_t;

static int fake_read(block_device_t *dev, uint32_t lba, uint8_t count, void *buf)
{
    fake_disk_t *fd = (fake_disk_t *)dev->dev_private;
    if (fd->fail_reads)
        return -1;
    if (lba + count > fd->sector_count)
        return -1;
    memcpy(buf, fd->data + (size_t)lba * 512, (size_t)count * 512);
    return 0;
}

static block_device_t make_fake_device(fake_disk_t *fd)
{
    block_device_t dev;
    dev.name = "fake";
    dev.sector_size = 512;
    dev.read = fake_read;
    dev.write = NULL;
    dev.dev_private = fd;
    return dev;
}

/* Builds a synthetic 512-byte boot sector with a plausible FAT BPB
 * baked in, given just the handful of fields that actually drive
 * fat_classify_type()'s decision (BytesPerSec, SecPerClus, RsvdSecCnt,
 * NumFATs, RootEntCnt, FATSz16, and either TotSec16 or TotSec32). The
 * rest of the sector (jmpBoot/OEMName/media/CHS geometry/EBPB tail) is
 * zero-filled except the 0x55AA signature and, if requested, a
 * BS_FilSysType string — real mkfs.fat output fills in more than
 * this, but fat_parse_bpb_common()'s sanity check (see fat_common.h)
 * only actually looks at the fields set here, so a minimal synthetic
 * sector is sufficient to drive it correctly. */
static void build_bpb_sector(uint8_t sector[512],
                              uint16_t bytes_per_sec, uint8_t sec_per_clus,
                              uint16_t rsvd_sec_cnt, uint8_t num_fats,
                              uint16_t root_ent_cnt, uint16_t fat_sz16,
                              uint32_t tot_sec, const char *fstype)
{
    memset(sector, 0, 512);

    fat_bpb_common_t bpb;
    memset(&bpb, 0, sizeof(bpb));
    bpb.BPB_BytesPerSec = bytes_per_sec;
    bpb.BPB_SecPerClus  = sec_per_clus;
    bpb.BPB_RsvdSecCnt  = rsvd_sec_cnt;
    bpb.BPB_NumFATs     = num_fats;
    bpb.BPB_RootEntCnt  = root_ent_cnt;
    bpb.BPB_FATSz16     = fat_sz16;
    bpb.BPB_Media       = 0xF8;
    if (tot_sec <= 0xFFFF) {
        bpb.BPB_TotSec16 = (uint16_t)tot_sec;
        bpb.BPB_TotSec32 = 0;
    } else {
        bpb.BPB_TotSec16 = 0;
        bpb.BPB_TotSec32 = tot_sec;
    }
    bpb.BS_BootSig = 0x29;
    if (fstype) {
        size_t n = strlen(fstype);
        if (n > sizeof(bpb.BS_FilSysType)) n = sizeof(bpb.BS_FilSysType);
        memset(bpb.BS_FilSysType, ' ', sizeof(bpb.BS_FilSysType));
        memcpy(bpb.BS_FilSysType, fstype, n);
    }

    memcpy(sector, &bpb, sizeof(bpb));
    sector[FAT_BOOT_SIG_OFFSET]     = FAT_BOOT_SIG_BYTE0;
    sector[FAT_BOOT_SIG_OFFSET + 1] = FAT_BOOT_SIG_BYTE1;
}

int main(void)
{
    printf("=== DAY15 fat_common.c / fat12.c host logic test ===\n\n");

    /* ---- 1. fat_classify_type() boundary arithmetic, directly ----
     * The exact Microsoft fatgen103 boundaries fat_common.h documents:
     * <4085 -> FAT12, <65525 -> FAT16, otherwise FAT32. Tests both
     * sides of each boundary so an off-by-one in the refactor (e.g.
     * <= instead of <) would be caught. */
    printf("-- fat_classify_type() boundaries --\n");
    CHECK(fat_classify_type(0)     == FAT_TYPE_FAT12, "0 clusters -> FAT12");
    CHECK(fat_classify_type(2847)  == FAT_TYPE_FAT12, "2847 clusters (real 1.44MB floppy) -> FAT12");
    CHECK(fat_classify_type(4084)  == FAT_TYPE_FAT12, "4084 clusters -> FAT12 (just under boundary)");
    CHECK(fat_classify_type(4085)  == FAT_TYPE_FAT16, "4085 clusters -> FAT16 (exactly at boundary)");
    CHECK(fat_classify_type(20000) == FAT_TYPE_FAT16, "20000 clusters -> FAT16");
    CHECK(fat_classify_type(65524) == FAT_TYPE_FAT16, "65524 clusters -> FAT16 (just under boundary)");
    CHECK(fat_classify_type(65525) == FAT_TYPE_FAT32, "65525 clusters -> FAT32 (exactly at boundary)");
    CHECK(fat_classify_type(500000) == FAT_TYPE_FAT32, "500000 clusters -> FAT32");
    printf("\n");

    /* ---- 2. fat_mount() over a fake disk: real FAT12 floppy geometry ----
     * BPB values matching a real 1.44MB `mkfs.fat -F 12` floppy
     * (RsvdSecCnt=1, NumFATs=2, FATSz16=9, RootEntCnt=224, 2880 total
     * sectors, 512 bytes/sector, 1 sector/cluster) — the same shape
     * as this project's own tests/fat12-bpb.bin fixture. Expected
     * geometry, by hand: RootDirSectors = 224*32/512 = 14,
     * FirstDataSector = 1 + 2*9 + 14 = 33, DataSectors = 2880-33 =
     * 2847, TotalClusters = 2847/1 = 2847 -> FAT12. */
    printf("-- fat_mount() over a synthetic real-floppy-shaped FAT12 BPB --\n");
    {
        fake_disk_t fd = { .data = calloc(1, 512 * 3000), .sector_count = 3000 };
        build_bpb_sector(fd.data, 512, 1, 1, 2, 224, 9, 2880, "FAT12");
        block_device_t dev = make_fake_device(&fd);

        fat_bpb_common_t bpb; fat_geometry_t geom;
        fat_type_t t = fat_mount(&dev, 0, &bpb, &geom);

        CHECK(t == FAT_TYPE_FAT12, "detected type is FAT12");
        CHECK(geom.root_dir_sectors == 14, "root_dir_sectors == 14");
        CHECK(geom.first_data_sector == 33, "first_data_sector == 33");
        CHECK(geom.total_sectors == 2880, "total_sectors == 2880");
        CHECK(geom.total_clusters == 2847, "total_clusters == 2847");

        /* Regression check: fat12_mount()/fat12_parse_bpb() (Day 9/10,
         * refactored to delegate to fat_common.c) still accepts this
         * same sector and produces byte-identical geometry, since
         * bpb_12_t/fat12_geometry_t are just aliases now. */
        bpb_12_t bpb12; fat12_geometry_t geom12;
        int rc = fat12_parse_bpb(fd.data, &bpb12, &geom12);
        CHECK(rc == 0, "fat12_parse_bpb() still returns 0 (regression)");
        CHECK(geom12.root_dir_sectors == 14 && geom12.first_data_sector == 33,
              "fat12_parse_bpb() geometry unchanged by refactor (regression)");

        static fat12_fs_t fs;
        rc = fat12_mount(&dev, 0, &fs);
        CHECK(rc == 0, "fat12_mount() still succeeds against the same sector (regression)");

        free(fd.data);
    }
    printf("\n");

    /* ---- 3. fat_mount() over a synthetic FAT16-range BPB ----
     * SecPerClus=1, RsvdSecCnt=1, NumFATs=2, FATSz16=100,
     * RootEntCnt=512 -> RootDirSectors=32, FirstDataSector=1+200+32=233.
     * Pick TotalSectors=5233 so DataSectors=5000 clusters -> FAT16. */
    printf("-- fat_mount() over a synthetic FAT16-range BPB --\n");
    {
        fake_disk_t fd = { .data = calloc(1, 512 * 6000), .sector_count = 6000 };
        build_bpb_sector(fd.data, 512, 1, 1, 2, 512, 100, 5233, "FAT16");
        block_device_t dev = make_fake_device(&fd);

        fat_bpb_common_t bpb; fat_geometry_t geom;
        fat_type_t t = fat_mount(&dev, 0, &bpb, &geom);

        CHECK(t == FAT_TYPE_FAT16, "detected type is FAT16");
        CHECK(geom.total_clusters == 5000, "total_clusters == 5000");

        /* And fat12_mount()/fat12_parse_bpb() correctly REJECT this as
         * not-FAT12 (BS_FilSysType reads "FAT16", not "FAT12") — proof
         * the Day15 refactor didn't loosen fat12's own type check. */
        bpb_12_t bpb12; fat12_geometry_t geom12;
        int rc = fat12_parse_bpb(fd.data, &bpb12, &geom12);
        CHECK(rc == -1, "fat12_parse_bpb() rejects a FAT16-labeled sector");

        free(fd.data);
    }
    printf("\n");

    /* ---- 4. fat_mount() over a synthetic FAT32-range BPB ----
     * Reuse the same FirstDataSector=233 shape, but push cluster
     * count past the 65525 boundary: TotalSectors = 233 + 70000. */
    printf("-- fat_mount() over a synthetic FAT32-range BPB --\n");
    {
        fake_disk_t fd = { .data = calloc(1, 512 * 71000), .sector_count = 71000 };
        build_bpb_sector(fd.data, 512, 1, 1, 2, 512, 100, 70233, NULL);
        block_device_t dev = make_fake_device(&fd);

        fat_bpb_common_t bpb; fat_geometry_t geom;
        fat_type_t t = fat_mount(&dev, 0, &bpb, &geom);

        CHECK(t == FAT_TYPE_FAT32, "detected type is FAT32");
        CHECK(geom.total_clusters == 70000, "total_clusters == 70000");
        free(fd.data);
    }
    printf("\n");

    /* ---- 5. Negative cases ---- */
    printf("-- fat_mount() negative / error cases --\n");
    {
        /* Non-FAT sector: all zero (no 0x55AA signature at all). */
        fake_disk_t fd = { .data = calloc(1, 512 * 10), .sector_count = 10 };
        block_device_t dev = make_fake_device(&fd);
        fat_type_t t = fat_mount(&dev, 0, NULL, NULL);
        CHECK(t == FAT_TYPE_UNKNOWN, "all-zero sector (no signature) -> UNKNOWN");
        free(fd.data);
    }
    {
        /* Signature present, but BytesPerSec/SecPerClus/NumFATs are
         * nonsense (mirrors LumaOS's own boot/mbr.asm reinterpreted as
         * a BPB, per fat12.c's original DAY9 comment). */
        fake_disk_t fd = { .data = calloc(1, 512 * 10), .sector_count = 10 };
        fd.data[FAT_BOOT_SIG_OFFSET] = FAT_BOOT_SIG_BYTE0;
        fd.data[FAT_BOOT_SIG_OFFSET + 1] = FAT_BOOT_SIG_BYTE1;
        fd.data[0x0B] = 0x7C; fd.data[0x0C] = 0x00; /* BytesPerSec = 0x007C = 124, not legal */
        block_device_t dev = make_fake_device(&fd);
        fat_type_t t = fat_mount(&dev, 0, NULL, NULL);
        CHECK(t == FAT_TYPE_UNKNOWN, "0x55AA signature but bogus BPB fields -> UNKNOWN");
        free(fd.data);
    }
    {
        /* dev->read() failing outright (simulates an I/O error). */
        fake_disk_t fd = { .data = calloc(1, 512 * 10), .sector_count = 10, .fail_reads = 1 };
        build_bpb_sector(fd.data, 512, 1, 1, 2, 224, 9, 2880, "FAT12");
        block_device_t dev = make_fake_device(&fd);
        fat_type_t t = fat_mount(&dev, 0, NULL, NULL);
        CHECK(t == FAT_TYPE_UNKNOWN, "dev->read() failure -> UNKNOWN");
        free(fd.data);
    }
    CHECK(fat_mount(NULL, 0, NULL, NULL) == FAT_TYPE_UNKNOWN, "dev == NULL -> UNKNOWN");
    printf("\n");

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        printf("%d TEST(S) FAILED\n", g_failures);
        return 1;
    }
}