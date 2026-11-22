/* kernel/fs/vfs/vfs.c
 * LumaOS 5.0 - VFS Mount Entry Point
 *
 * DAY13 (Week 2, Day 13 of the 5.0 filesystem roadmap):
 *   "将 FAT12 挂接到 VFS：注册 fat12_filesystem_ops。实现
 *    vfs_mount("IDE0")。调用 vfs_mount 后，vfs_root 指针指向 FAT
 *    根目录。"
 *
 *   See vfs.h's DAY13 doc comment for the full contract of vfs_root/
 *   vfs_mount(). This file is the implementation: it is the ONE place
 *   in the codebase that both knows about a concrete block device
 *   (kernel/drivers/block/block.h) and a concrete filesystem backend
 *   (kernel/fs/fat/fat12_vfs.h) — everything else either stays below
 *   the block_device_t abstraction (drivers) or above the vfs_node_t
 *   abstraction (Day 14's planned cmd_ls and later shell commands).
 *
 * ---- NOTE ON THE DEVICE STRING: "IDE0", not "IDE" or "/dev/hda" ----
 *
 *   This went through two corrections before landing here:
 *
 *     1. An early draft used a Unix-style device path, "/dev/hda".
 *        Wrong: LumaOS's own userspace namespace document (计划.md's
 *        file-tree diagram) never uses Unix /dev paths anywhere — the
 *        top level of the physical-disk namespace is `IDE/` itself, a
 *        peer of `USB/`, `IMG/`, `ISO/`:
 *
 *            LumaOS
 *            ├── IDE/
 *            │   ├── Boot/
 *            │   ├── System/  (Kernel/ SysApp/ UserControler/ ...)
 *            │   ├── User/    (Desktop/ Documents/ Downloads/ ...)
 *            │   ├── Administrator/ (Index/ Desktop/ Documents/ App/)
 *            │   └── Public/
 *            ├── USB/DISK_1/, USB/DISK_2/
 *            ├── IMG/DISK_1/, ...
 *            └── ISO/LumaOS/, ...
 *
 *        So the device string was changed to plain "IDE".
 *
 *     2. Plain "IDE" was ALSO wrong, for a different reason: it can't
 *        tell two physical IDE drives apart. This project's block
 *        layer already numbers its device accessor — Day 6's
 *        block.h/block.c only expose `ide0_block_device()`, not a
 *        bare `ide_block_device()` — anticipating a future
 *        `ide1_block_device()` for a second physical drive (secondary
 *        ATA channel / a slave drive). Mirroring that numbering into
 *        the VFS device-name namespace means "IDE0", "IDE1", "IDE2"
 *        map 1:1 onto however many physical IDE drives end up probed,
 *        auto-numbered in probe order — the same pattern real OSes
 *        use (Linux's /dev/sda, /dev/sdb, ...; this project's own
 *        USB/DISK_1, USB/DISK_2 convention for the SAME reason,
 *        multiple like-devices need a way to stay distinguishable).
 *
 *   So `"IDE0"` is what vfs_mount() recognizes TODAY — it is the only
 *   device string that resolves to anything, because
 *   ide0_block_device() is the only accessor block.c currently
 *   implements (no ide1_block_device() exists yet, so there is
 *   nothing for an "IDE1" to name). Adding a second physical drive
 *   later is meant to look like: block.c grows an ide1_block_device()
 *   accessor, and this function grows one more `else if
 *   (strcmp(path, "IDE1") == 0) dev = ide1_block_device();` branch —
 *   no restructuring of vfs_mount()'s shape required, since the
 *   device-string-to-block_device_t mapping was designed to extend
 *   this way from the start.
 *
 *   A real path/mount-point resolver that turns e.g.
 *   "IDE0/System/Kernel" into a walked vfs_node_t chain is later work
 *   (Day 18's fat_open()-by-path is the first piece of that, Day
 *   68-69's `df`/`cd`/`pwd` the rest) — Day 13 only wires up the
 *   DEVICE-level mount, so vfs_mount() only ever looks at the
 *   top-level "IDE0" token, not a full path.
 *
 * ---- Resolving "IDE0" to an actual LBA ----
 *
 *   This project's real boot disk has no FAT partition at all: LBA 0
 *   is LumaOS's own custom mbr.asm bootloader (see kmain.c's DAY5/
 *   DAY9 header comments), not a partitioned disk with a FAT entry in
 *   its partition table — so on THIS project's stock image,
 *   mbr_find_first_active_fat() (Day 5) is expected to find nothing,
 *   same as it already does in kmain.c's own DAY5 test section.
 *
 *   Rather than inventing a new fixture just for Day 13, or leaving
 *   vfs_mount("IDE0") permanently unable to succeed on this project's
 *   own test image, vfs_mount() below does both:
 *
 *     1. Reads LBA 0 and runs the real Day 5 MBR-partition-scan path
 *        (mbr_parse() + mbr_find_first_active_fat()). This is the
 *        "correct" path for a real partitioned IDE disk, and costs
 *        nothing to keep working for when one exists.
 *     2. If (as expected today) no active FAT partition is found,
 *        falls back to VFS_IDE_FALLBACK_LBA — LBA 5000, the same
 *        already-embedded, already-cross-validated FAT12 test volume
 *        (tests/fat12-with-file.img, containing BIGFILE.BIN/
 *        SMALL.BIN) that kmain.c's DAY11/DAY12 sections already mount
 *        and readdir directly. This is NOT a new fixture — it is
 *        literally the same bytes on the same disk image build.sh has
 *        embedded since Day 11, just now also reachable through
 *        vfs_mount("IDE0") instead of only through kmain.c calling
 *        fat12_mount(disk, 5000, ...) by hand.
 *
 *   Once a real FAT-partitioned image exists (Day 16+'s "mkfs.fat -F
 *   16"/"-F 32" test images, or an eventual real install), step 1
 *   above starts actually finding something and step 2 stops being
 *   exercised — nothing about this function needs to change for that
 *   day to arrive; the fallback simply stops firing.
 */
#include "../lmfs/lmfs_vfs.h"        /* DAY63: vfs_fs_open_path() dispatch */
#include "../iso9660/iso9660.h"
#include "vfs.h"
#include "../fat/fat_vfs.h"
#include "../../drivers/block/block.h"
#include "../../drivers/block/mbr.h"
#include "../../lib/string.h"          /* strcmp */
#include "../../core/kernel.h"         /* console_write_color, printf —
                                           matches mbr.c/ide.c's own
                                           convention of a driver-level
                                           file printing its own
                                           "[tag] ..." status lines,
                                           not leaving all output to
                                           kmain.c's test sections */

/* DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap) — REFACTOR NOTE:
 *   Step 2 below used to call fat12_vfs_mount(dev, mount_lba) directly
 *   (Day 13's original code, when FAT12 was the only variant that
 *   existed). Now that fat16.h/fat16_vfs.h exist too, this file
 *   includes fat_vfs.h instead of fat12_vfs.h and calls the new
 *   fat_vfs_mount(dev, mount_lba) dispatcher (kernel/fs/fat/fat_vfs.c),
 *   which runs Day 15's fat_mount() to figure out which FAT variant is
 *   actually at `mount_lba` and routes to fat12_vfs_mount() or
 *   fat16_vfs_mount() accordingly — see fat_vfs.h's own header comment
 *   for the full rationale. Nothing about VFS_IDE_FALLBACK_LBA, the
 *   MBR-scan step, or vfs_root's contract changes: LBA 5000 is still
 *   the Day 11 FAT12 fixture and is still correctly detected as such,
 *   so this refactor is behavior-preserving for every existing caller
 *   — it only ADDS the ability for "IDE0" to resolve to a FAT16 volume
 *   on some future disk image, without requiring this file to change
 *   again when that happens (same "grows one more case, not a
 *   restructuring" property fat_vfs_mount() itself documents).
 */

/* See this file's header comment above for why LBA 5000 specifically:
 * it is Day 11's own already-embedded tests/fat12-with-file.img
 * fixture, not a new one invented for Day 13. */
#define VFS_IDE_FALLBACK_LBA  5000

/* The one supported device string through Day 13/14 — LumaOS's own
 * namespace document's top-level name for the primary IDE disk,
 * NUMBERED to match block.c's ide0_block_device() accessor (see this
 * file's header comment, part 2, for why plain "IDE" wasn't enough).
 * NOT a Unix /dev path. */
#define VFS_DEV_IDE0  "IDE0"

vfs_node_t *vfs_root = (vfs_node_t *)0;

/* DAY18-FOLLOWUP: numbered mount table storage — see vfs.h's doc
 * comment above vfs_register_mount()/vfs_lookup_mount() for the full
 * rationale. Zero-initialized: vfs_mount_count == 0 means no slots
 * assigned yet, matching vfs_root's own "NULL until first mount"
 * convention. */
vfs_mount_entry_t vfs_mounts[VFS_MAX_MOUNTS];
uint32_t          vfs_mount_count = 0;

/* DAY62: same as vfs_register_mount() but the slot gets a name the caller chooses ("RAM0", "CD0"...) instead
 * of "IDE<n>". The live CD uses it: its volumes are not IDE disks. `name` is copied (max 7 chars). */
int vfs_register_mount_named(filesystem_t *fs, const char *name, char out_name[8])
{
    if (!fs || !name || vfs_mount_count >= VFS_MAX_MOUNTS)
        return -1;
    vfs_mount_entry_t *slot = &vfs_mounts[vfs_mount_count];
    uint32_t i = 0;
    while (name[i] && i < 7) { slot->name[i] = name[i]; i++; }
    slot->name[i] = '\0';
    slot->fs = fs;
    if (out_name) { for (uint32_t k = 0; k <= i; k++) out_name[k] = slot->name[k]; }
    vfs_mount_count++;
    vfs_root = fs->root;
    printf("[vfs] Registered mount slot \"%s\" (fs->name=\"%s\")\n", slot->name, fs->name);
    return 0;
}

/* DAY63: which filesystems have real subdirectories / path walking? (LMFS since Day 42, ISO 9660 now.)
 * ls / cd / read used to hard-code `strcmp(fs->name, "lmfs")`; they call these instead. */
int vfs_fs_has_subdirs(filesystem_t *fs)
{
    return fs && fs->name && (strcmp(fs->name, "lmfs") == 0 || strcmp(fs->name, "iso9660") == 0);
}

/* path = "<mount-slot>/a/b/c" (shell_resolve_path() output). 0 = ok, -1 = not found / unsupported. */
int vfs_fs_open_path(filesystem_t *fs, const char *path, vfs_node_t *out)
{
    if (!fs || !fs->name) return -1;
    if (strcmp(fs->name, "lmfs") == 0)    return lmfs_vfs_open_path(path, out);
    if (strcmp(fs->name, "iso9660") == 0) return iso9660_vfs_open_path(path, out);
    return -1;
}

int vfs_register_mount(filesystem_t *fs, char out_name[8])
{
    if (!fs || vfs_mount_count >= VFS_MAX_MOUNTS)
        return -1;

    vfs_mount_entry_t *slot = &vfs_mounts[vfs_mount_count];

    /* Build "IDE" + decimal index by hand (no snprintf in this
     * freestanding kernel — see kernel/lib/string.h). VFS_MAX_MOUNTS
     * is 8, so the index is always a single digit and this simple
     * '0' + index encoding never needs more than one digit's room. */
    slot->name[0] = 'I';
    slot->name[1] = 'D';
    slot->name[2] = 'E';
    slot->name[3] = (char)('0' + vfs_mount_count);
    slot->name[4] = '\0';
    slot->fs = fs;

    if (out_name) {
        uint32_t i = 0;
        while (slot->name[i] != '\0') {
            out_name[i] = slot->name[i];
            i++;
        }
        out_name[i] = '\0';
    }

    vfs_mount_count++;

    /* Keep vfs_root pointing at the most recently registered volume —
     * the same "last mount wins for `ls`/etc." behavior this project
     * already had before this table existed (kmain.c's DAY16/17
     * sections used to assign `vfs_root = fsNN->root` by hand after
     * each mount; they now call this function instead, see kmain.c). */
    vfs_root = fs->root;

    printf("[vfs] Registered mount slot \"%s\" (fs->name=\"%s\")\n",
           slot->name, fs->name);

    return 0;
}

filesystem_t *vfs_lookup_mount(const char *name)
{
    if (!name)
        return (filesystem_t *)0;

    uint32_t i;
    for (i = 0; i < vfs_mount_count; i++) {
        if (strcmp(vfs_mounts[i].name, name) == 0)
            return vfs_mounts[i].fs;
    }
    /* DAY69: 卷名不区分大小写（"cd ide3" / "cd ram0" 和 "cd IDE3" 一样）。先精确匹配（上面），
     * 再忽略大小写匹配；调用者（shell_resolve_path）随后会把名字换回规范写法。 */
    for (i = 0; i < vfs_mount_count; i++) {
        const char *a = vfs_mounts[i].name, *b = name;
        while (*a && *b) {
            char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
            char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*a && !*b)
            return vfs_mounts[i].fs;
    }
    return (filesystem_t *)0;
}

/* vfs_mount_name_of(fs) — the REVERSE of vfs_lookup_mount(): given a
 * filesystem_t (e.g. from `vfs_root->fs`), returns the mount-slot name
 * it was registered under ("IDE0", "IDE1", ...), or NULL if `fs` was
 * never registered via vfs_register_mount().
 *
 * BUGFIX / WHY THIS EXISTS (found by testing after DAY18-FOLLOWUP first
 * landed): the first attempt at showing the right name at the shell
 * prompt tried overwriting `fs->root->name` in place, inside
 * vfs_register_mount() above, right after assigning a slot. That
 * looked correct in isolation but broke in practice: fat12_vfs_mount()/
 * fat16_vfs_mount()/fat32_vfs_mount() (Day13/16/17) each keep ONE
 * static filesystem_t/vfs_node_t pair (e.g. fat32_vfs.c's `static
 * filesystem_t g_fat32_fs; static vfs_node_t g_fat32_root;`) and
 * return the SAME pointer on every call, unconditionally resetting
 * `.name` back to the hardcoded literal "IDE0" each time (see those
 * files' own `root_name = "IDE0"` lines). kmain.c's own pre-existing
 * DAY17-FOLLOWUP "multi-LBA mount summary" section calls
 * fat_vfs_mount() again for EACH fixture (purely to print a summary
 * table) after the DAY16/17 sections already registered them — so by
 * the time the shell starts, that later demo re-mount has silently
 * clobbered whatever name was written onto the (shared, reused)
 * static root node, right back to "IDE0", regardless of where in
 * vfs_register_mount() the overwrite happened.
 *
 * The robust fix is this function: don't cache a display name onto a
 * mutable, reused node at all — look it up fresh, by filesystem_t
 * POINTER identity, against `vfs_mounts[]` (the mount table itself,
 * which nothing but vfs_register_mount() ever writes, and which the
 * DAY17-FOLLOWUP demo loop does NOT call again for its extra
 * re-mounts — see kmain.c). Even if some later re-mount resets a
 * shared static root's `.name` field back to "IDE0" behind our back,
 * `vfs_mounts[i].fs` is still the SAME pointer it always was, so the
 * table lookup keeps returning the correct answer regardless. Callers
 * that want a volume's TRUE current name (kmain.c's shell-prompt code)
 * should call this instead of reading `vfs_root->name` directly; that
 * field is left as FAT12/16/32's own default and is no longer treated
 * as authoritative for display purposes.
 */
const char *vfs_mount_name_of(filesystem_t *fs)
{
    if (!fs)
        return (const char *)0;

    uint32_t i;
    for (i = 0; i < vfs_mount_count; i++) {
        if (vfs_mounts[i].fs == fs)
            return vfs_mounts[i].name;
    }
    return (const char *)0;
}

int vfs_mount(const char *path)
{
    /* Only "IDE0" resolves to anything today — see this file's header
     * comment for why: block.c only implements ide0_block_device()
     * so far, no ide1_block_device() exists yet for an "IDE1" to name.
     * Adding one later is meant to be a single extra `else if` branch
     * here, not a restructuring of this function.
     *
     * NOTE (DAY18-FOLLOWUP): this `path` argument is still the
     * DEVICE-string this function has always accepted (physical drive
     * selection, per this file's header comment) — a completely
     * separate thing from the MOUNT-SLOT name vfs_register_mount()
     * below assigns this call's result. It's a naming coincidence that
     * both happen to read "IDE0" the first time this runs; see vfs.h's
     * doc comment on the mount table for why. */
    if (!path || strcmp(path, VFS_DEV_IDE0) != 0)
        return -1;

    block_device_t *dev = ide0_block_device();

    /* ---- Step 1: try the real MBR-partition path (Day 5) ---- */
    uint32_t mount_lba = VFS_IDE_FALLBACK_LBA;
    static uint8_t mbr_sector[512];

    if (dev->read(dev, 0, 1, mbr_sector) == 0) {
        mbr_partition_entry_t parts[MBR_PARTITION_COUNT];
        mbr_parse(mbr_sector, parts);   /* entries usable even if sig check fails */

        int active_idx;
        const mbr_partition_entry_t *active =
            mbr_find_first_active_fat(parts, &active_idx);
        if (active) {
            mount_lba = active->lba_start;
        }
        /* else: expected on this project's stock image today — see
         * this file's header comment. mount_lba stays the DAY11
         * fixture fallback set above. */
    }
    /* else: LBA 0 read itself failed — still try the fallback LBA
     * rather than giving up outright, same "degrade, don't just fail"
     * spirit as fat12_parse_bpb()'s "fill *out_geom anyway" contract. */

    /* Say out loud which physical LBA "IDE0" actually resolves to
     * before mounting it — this is exactly the ambiguity 计划.md's own
     * Day12 discussion notes flag ("为什么现在 ls 看不到 kernel.bin"):
     * "IDE0" today is NOT the same bytes as mbr.bin/loader.bin/
     * kernel.bin (those live at LBA 0-261, dd'd on raw, outside any
     * filesystem). Printing the LBA plainly means nobody has to guess
     * or go read this file's comments to know which volume "IDE0"
     * just became — the console output is self-explanatory either
     * way. Plain ASCII only in this string (matching every other
     * printf status line in the project) — the VGA text-mode font has
     * no glyph for an em dash, and an earlier draft of this line
     * learned that the hard way. */
    if (mount_lba == VFS_IDE_FALLBACK_LBA) {
        printf("[vfs] Reading LBA %u as IDE0 (DAY11 test fixture, "
               "no active FAT partition found in the MBR)\n", mount_lba);
    } else {
        printf("[vfs] Reading LBA %u as IDE0 (active FAT partition "
               "found in the MBR)\n", mount_lba);
    }

    /* ---- Step 2: auto-detect the FAT variant and wire it into the VFS ---- */
    filesystem_t *fs = fat_vfs_mount(dev, mount_lba);
    if (!fs)
        return -1;   /* vfs_root intentionally left untouched on failure */

    /* DAY18-FOLLOWUP: register into the numbered mount table (this
     * also sets vfs_root = fs->root, same as the old direct assignment
     * did — see vfs_register_mount()'s doc comment in vfs.h). This is
     * expected to become slot "IDE0" (the first mount this project
     * ever performs, via kmain.c's DAY13 section), but the return
     * value is checked rather than assumed, same as every other
     * fallible call in this function. */
    char slot_name[8];
    if (vfs_register_mount(fs, slot_name) != 0)
        return -1;   /* mount table full — shouldn't happen this early */

    return 0;
}