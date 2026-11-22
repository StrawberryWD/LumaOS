/* kernel/fs/vfs/vfs.h
 * LumaOS 5.0 - Virtual File System Abstraction
 *
 * DAY8 (Week 2, Day 8 of the 5.0 filesystem roadmap):
 *   "创建 kernel/fs/vfs/vfs.h。定义 struct file、struct vfs_node、
 *    struct filesystem。定义函数指针：open、read、readdir。"
 *
 *   This header is the filesystem-independent layer that every concrete
 *   filesystem driver (FAT12 starting Day 9-14, FAT16/32 in Week 3, the
 *   custom LMFS in Month 2) is expected to plug into — mirroring how
 *   kernel/drivers/block/block.h (Day 6) decoupled upper-layer code
 *   from *which* disk controller is underneath. From here on, Shell
 *   commands and higher-level code (`ls`, `cat`, ...) are meant to walk
 *   `struct vfs_node` trees and call through a `struct filesystem`'s
 *   function pointers — never a driver-specific function
 *   (`fat12_readdir()`, `lmfs_read()`, etc.) by name.
 *
 *   Day 8's acceptance criteria is intentionally narrow: "头文件编译
 *   通过" (the header compiles). No .c file, no VFS state machine, and
 *   no block_device_t wiring is implemented yet — that starts Day 9
 *   (FAT12 BPB parsing) and Day 13 ("将 FAT12 挂接到 VFS: 注册
 *   fat12_filesystem_ops. 实现 vfs_mount("/dev/hda")"), which is where
 *   vfs_mount()/vfs_readdir() gain bodies and kmain.c/shell.c start
 *   calling into this layer. This file only needs to declare the
 *   shapes those later steps will fill in, so today it's included
 *   nowhere yet (deliberately not added to build.sh's CFLAGS -I list
 *   or any #include — there is no .c to compile against it until
 *   Day 9+). Standalone compilation (`gcc -fsyntax-only vfs.h`-style,
 *   wrapped in a throwaway .c) is how the Day 8 acceptance check is
 *   verified.
 *
 * Design notes (kept consistent with block.h's conventions):
 *   - Every function pointer takes the owning struct as its first
 *     argument (mirrors block_read_fn's `block_device_t *dev` pattern)
 *     so a filesystem backend can support multiple mounted instances
 *     without file-scope globals, and so a node's operations can find
 *     their way back to the node without a second lookup.
 *   - `open`/`read`/`readdir` all return int (0 success / negative
 *     error), matching the existing ide_ and block_ function naming
 *     convention of 0-on-success so callers migrating between layers
 *     don't need to change their error handling style. Week 10
 *     (Day 70) is where the roadmap formalizes named error codes
 *     (-ENOENT, -EIO); until then -1 is the generic "failed" value,
 *     same as mbr_parse()'s contract.
 *   - `struct vfs_node` is the in-memory, filesystem-independent
 *     handle for anything in the tree (file OR directory — VFS_TYPE_*
 *     distinguishes them), analogous to a Unix vnode. It carries a
 *     `void *fs_private` for the backend to stash its own on-disk
 *     handle (e.g. a FAT12 starting cluster + parent directory sector,
 *     or an LMFS inode number) without polluting this generic struct —
 *     the same pattern block_device_t's `dev_private` uses for driver
 *     state.
 *   - `struct file` is the per-open-instance handle (an open vfs_node
 *     plus a byte offset). Day 20 ("增加文件偏移指针 file->pos，支持
 *     多次读取") is what actually exercises `pos` across repeated
 *     reads; it's declared now so `read`'s signature is stable across
 *     that later step instead of changing later.
 *   - `struct filesystem` bundles a filesystem driver's operations
 *     table (what block_device_t did for disk drivers) plus a `name`
 *     tag and a `root` node once mounted. Day 13's vfs_mount() is what
 *     will allocate/populate one of these for FAT12; this header just
 *     defines its shape.
 *   - No block_device_t include here on purpose: Day 8's scope is the
 *     VFS's own node/file/filesystem shapes only. The concrete Day 9+
 *     FAT12 backend is what will pull in block.h to talk to disk.
 */
#ifndef LUMA_VFS_H
#define LUMA_VFS_H

#include <stdint.h>
#include <stddef.h>

/* ===== Node types =====
 * What kind of thing a struct vfs_node represents. Kept as a small
 * bitless enum (not flags) since Day 8's tree is strictly "file" or
 * "directory" — same two-way split FAT's directory-entry attribute
 * byte and LMFS's planned inode "type" field (Day 37: "lmfs_create_
 * inode(type)") both use. */
typedef enum {
    VFS_TYPE_FILE = 0,
    VFS_TYPE_DIR  = 1,
} vfs_node_type_t;

/* ===== Forward declarations =====
 * struct filesystem, struct vfs_node, and struct file all reference
 * each other (a node points back at its owning filesystem, a file
 * wraps a node, a filesystem's ops take a node), so every struct name
 * must be visible before all three are fully defined. */
struct filesystem;
struct vfs_node;
struct file;

/* ===== VFS-level function pointer types =====
 *
 * vfs_open_fn(node, out_file)
 *   Prepares `*out_file` to read from `node` (resets file->pos to 0,
 *   stashes the node pointer, lets the backend do any per-open setup
 *   it needs). Returns 0 on success, negative on error (e.g. node is
 *   a directory and the caller asked to open it as a file).
 *
 * vfs_read_fn(file, buf, size)
 *   Reads up to `size` bytes from `file` (starting at file->pos) into
 *   `buf`, advancing file->pos by the number of bytes actually read.
 *   Returns the number of bytes read (>= 0; 0 at end-of-file), or a
 *   negative value on error — this differs from block_read_fn's
 *   strict 0/-1 because callers need to know a short-read count, the
 *   same way a Unix read(2) does.
 *
 * vfs_readdir_fn(dir_node, index, out_node)
 *   Fetches the `index`-th child (0-based) of `dir_node` (which must
 *   be VFS_TYPE_DIR) into `*out_node`. Returns 0 on success, -1 if
 *   `index` is past the last entry (the caller's signal to stop
 *   iterating) or dir_node is not a directory.
 *
 * vfs_close_fn(file)
 *   DAY21 (Week 3, Day 21 of the 5.0 filesystem roadmap — "周复盘":
 *   "关闭文件功能（fat_close）"): the counterpart to vfs_open_fn. Marks
 *   `file` as no longer in use — concretely, clears `file->node` to
 *   NULL so any *later* accidental call to `fs->read()` on the same
 *   (now-closed) `file_t` fails loudly (vfs_read_fn's own contract
 *   already requires `file->node` to be non-NULL) instead of silently
 *   continuing to read a file the caller believes it closed. This
 *   project has no kernel heap and no per-open dynamically-allocated
 *   state yet (see fat12_vfs.h's DAY13 design notes) — nothing here
 *   is actually freed — so "closing" today is purely this handle-
 *   invalidation safety net, not a resource-release step. That is
 *   expected to still be true even once LMFS (Month 2) grows real
 *   per-open state, since this function's contract (invalidate the
 *   handle, return 0/negative) does not itself change; only what a
 *   given backend does INSIDE close() would grow.
 *   Returns 0 on success. Returns negative if `file` is NULL or
 *   `file->node` is already NULL (double-close / never-opened file —
 *   same "catch misuse rather than silently no-op" spirit as
 *   vfs_open_fn's own "refuses to open a directory as a file" check).
 *
 * NOTE (DAY25): this project's Day 25 write support (fat_write(),
 * kernel/fs/fat/fat_vfs.h) deliberately does NOT plug in here as a
 * vfs_write_fn. fat_write() takes a PATH string and does its own
 * fat_open()-then-dispatch internally — the same "call the FAT-layer
 * function directly by name, don't route it through struct
 * filesystem's ops table" precedent fat_create_file() (Day 24)
 * already set for the identical reason (create's shell command,
 * kernel/shell/cmd/create.c, calls fat_create_file() directly, not
 * through some fs->create). A vfs_write_fn taking `struct file *`
 * would need a `pos`-based, already-open-handle contract this
 * project's `write` Shell command never uses (it always (re)writes a
 * file's entire content in one call, see fat_vfs.h's own doc comment
 * on fat_write() for the full "why path, not file_t" rationale) — so
 * no such function pointer type is declared here; adding one with no
 * real caller would be exactly the kind of unused abstraction this
 * header's own Day 8 design notes above warn against.
 */
typedef int (*vfs_open_fn)(struct vfs_node *node, struct file *out_file);
typedef int (*vfs_read_fn)(struct file *file, void *buf, uint32_t size);
typedef int (*vfs_readdir_fn)(struct vfs_node *dir_node, uint32_t index,
                               struct vfs_node *out_node);
typedef int (*vfs_close_fn)(struct file *file);

/* ===== struct vfs_node =====
 *
 * The filesystem-independent, in-memory handle for one entry (file or
 * directory) somewhere in a mounted filesystem's tree — the VFS
 * equivalent of block_device_t for disk drivers. Upper-layer code
 * (Shell commands, path-walking helpers added Day 18+) is meant to
 * hold and pass around `struct vfs_node *` rather than any backend-
 * specific handle.
 */
typedef struct vfs_node {
    char                name[32];   /* entry name, NUL-terminated, WITHOUT a
                                        trailing '/' even for directories
                                        (e.g. "IDE0", not "IDE0/") —
                                        LumaOS's own namespace diagram
                                        (计划.md) writes every directory
                                        with a trailing '/' (IDE/,
                                        System/, ...), but that is a
                                        DISPLAY convention only, applied
                                        by whoever prints a node (check
                                        `type == VFS_TYPE_DIR` and
                                        append '/'). Keeping the stored
                                        name slash-free avoids double-
                                        slash bugs once Day 18 starts
                                        concatenating path segments (e.g.
                                        "IDE0" + "/" + "System"); 8.3-
                                        style names fit comfortably
                                        either way, long-name support if
                                        any is later */
    vfs_node_type_t     type;       /* VFS_TYPE_FILE or VFS_TYPE_DIR */
    uint32_t            size;       /* file size in bytes; 0 for directories */
    struct filesystem  *fs;         /* owning filesystem (for fs->ops calls) */
    void               *fs_private; /* backend-specific handle, opaque here
                                        (e.g. FAT12 starting cluster, or an
                                        LMFS inode number) */
} vfs_node_t;

/* ===== struct file =====
 *
 * A single open instance of a vfs_node, with its own read offset.
 * Multiple `struct file`s may reference the same vfs_node concurrently
 * (each with an independent `pos`), matching how a Unix file
 * descriptor's offset is per-open, not per-inode.
 */
typedef struct file {
    vfs_node_t *node;  /* the node this file was opened from */
    uint32_t    pos;   /* current byte offset for the next read */
} file_t;

/* ===== struct filesystem =====
 *
 * A mounted filesystem instance: its driver's operations table plus
 * the root node reachable through it. Day 13 ("将 FAT12 挂接到 VFS:
 * 注册 fat12_filesystem_ops. 实现 vfs_mount(\"IDE\")") is what
 * will populate `ops` with FAT12's function pointers and set `root`
 * to the parsed root directory; this header only defines the shape.
 */
typedef struct filesystem {
    const char       *name;     /* e.g. "fat12", "fat16", "fat32", "lmfs" */
    vfs_open_fn       open;     /* required: fs->open(node, out_file) */
    vfs_read_fn       read;     /* required: fs->read(file, buf, size) */
    vfs_readdir_fn    readdir;  /* required for dirs: fs->readdir(dir, i, out) */
    vfs_close_fn      close;    /* DAY21: fs->close(file) — see vfs_close_fn's
                                    doc comment above. NULL is a legal value
                                    (a backend that has nothing to do on close
                                    may leave this unset) — callers must check
                                    for NULL before calling through it, same
                                    as they already must for `readdir` on a
                                    file node. */
    vfs_node_t       *root;     /* root directory node once mounted, else NULL */
    void             *fs_private; /* backend-specific mount state, opaque here
                                      (e.g. a parsed BPB, or an LMFS superblock) */
} filesystem_t;

/* ===== DAY13 (Week 2, Day 13 of the 5.0 filesystem roadmap) =====
 *
 * "将 FAT12 挂接到 VFS：注册 fat12_filesystem_ops。实现
 *  vfs_mount("IDE0")。调用 vfs_mount 后，vfs_root 指针指向 FAT
 *  根目录。"
 *
 *   This is where the shapes this header has declared since Day 8
 *   finally get a mount entry point. The actual FAT12-specific glue
 *   (fat12_filesystem_ops, the adapter functions) lives in
 *   kernel/fs/fat/fat12_vfs.{h,c} — deliberately NOT included here,
 *   the same way this header has never included fat12.h: vfs.h stays
 *   filesystem-agnostic, vfs.c (kernel/fs/vfs/vfs.c) is what actually
 *   knows FAT12 exists and calls into fat12_vfs_mount().
 *
 * vfs_root
 *   The single global "current root of the mounted filesystem"
 *   pointer the roadmap's acceptance text names directly. NULL until
 *   a successful vfs_mount() call. This project has no multi-mount
 *   namespace yet (a real mount TABLE, letting several filesystems
 *   coexist under different paths, is Day 35's "FAT 与 LMFS 可以同时
 *   存在" and beyond) — through Day 13/14, "the" root is simply
 *   whatever the one supported device string ("IDE0") last mounted.
 *
 * vfs_mount(path)
 *   Mounts a filesystem and points vfs_root at its root directory.
 *   `path` is a DEVICE name, not a filesystem path to mount onto.
 *   Per LumaOS's own namespace document (计划.md's file-tree
 *   diagram), this project does NOT use Unix-style "/dev/hda" device
 *   paths — the top level of the physical-disk namespace is `IDE/`
 *   itself (a peer of `USB/`, `IMG/`, `ISO/`, with `Boot/`, `System/`,
 *   `User/`, `Administrator/`, `Public/` beneath it). The device
 *   string itself is further NUMBERED — "IDE0", "IDE1", "IDE2", ... —
 *   one per physical IDE drive in probe order, mirroring how
 *   block.c's accessor is `ide0_block_device()` (not a bare
 *   `ide_block_device()`) and this project's own `USB/DISK_1`,
 *   `USB/DISK_2` convention already numbers multiple like-devices for
 *   the same reason: a single unnumbered "IDE" can't name a second
 *   drive once one exists. So `"IDE0"` is the device string
 *   vfs_mount() recognizes — today the ONLY one it recognizes, since
 *   block.c only implements ide0_block_device() so far (see
 *   kernel/fs/vfs/vfs.c's header comment for the full rationale and
 *   for exactly how an "IDE1" branch gets added once a second drive
 *   is real). A real path/mount-point namespace resolver (turning
 *   e.g. "IDE0/System/Kernel" into a walked vfs_node_t chain) is
 *   later work (Day 18's fat_open()-by-path is the first piece of
 *   that, Day 68-69's "df"/"cd"/"pwd" the rest) — kernel/fs/vfs/vfs.c
 *   documents exactly which physical LBA "IDE0" resolves to on THIS
 *   project's test image, since there's no real FAT-partitioned boot
 *   disk yet (see kmain.c's DAY5/DAY9 comments on why LBA 0 here is
 *   LumaOS's own mbr.asm, not a FAT partition table with real
 *   entries).
 *
 *   Returns 0 on success (vfs_root now points at the mounted
 *   filesystem's root VFS_TYPE_DIR node). Returns -1 if `path` isn't
 *   recognized, or if the underlying filesystem-specific mount call
 *   fails (vfs_root is left unchanged on failure — a failed
 *   vfs_mount() call must never clobber a previously-good mount).
 */
extern vfs_node_t *vfs_root;

int vfs_mount(const char *path);

/* ===== DAY18-FOLLOWUP: numbered mount table (IDE0/IDE1/IDE2/...) =====
 *
 * This wasn't asked for by 计划.md's Day 18 text itself — the roadmap's
 * own comment right above (and vfs.c's header comment) says a real
 * mount TABLE is Day 35's job ("FAT 与 LMFS 可以同时存在"), and through
 * Day 18 "the" root was meant to stay a single global `vfs_root`,
 * whatever was mounted last. This small piece of it was pulled forward
 * early, at the user's explicit request, specifically so `fat_open()`
 * can address MULTIPLE simultaneously-mounted volumes by name instead
 * of only ever seeing "whichever one mounted most recently" — e.g. the
 * three FAT12/FAT16/FAT32 test fixtures kmain.c mounts one after
 * another can now each be opened by name afterward (`fat_open("IDE0/
 * BIGFILE.BIN", ...)`, `fat_open("IDE1/SMALL.BIN", ...)`, `fat_open(
 * "IDE2/SMALLFIL.BIN", ...)`) instead of the second and third mount
 * silently making the first one unreachable through fat_open(). This
 * is deliberately the SMALLEST piece of Day 35's eventual job that
 * makes that possible — a flat name->filesystem_t lookup table, no
 * directory-tree mount points, no unmount, no `cd`/`pwd` (still Day
 * 68-69's job) — everything else 计划.md describes for Day 35 remains
 * unbuilt until that day arrives.
 *
 * Slots are named "IDE0", "IDE1", "IDE2", ... auto-assigned in MOUNT
 * ORDER (not tied to which physical drive or which FAT variant it is —
 * a slot name says nothing about whether cluster it names is FAT12,
 * FAT16, or FAT32, on purpose, the same way a real OS's /dev/sda
 * doesn't encode its filesystem type in the device name either). The
 * FIRST filesystem ever mounted (via vfs_mount() or a direct
 * fat_vfs_mount() call that opts in by calling vfs_register_mount())
 * becomes "IDE0", the second "IDE1", and so on — this happens to line
 * up with the actual "IDE0"/"IDE1"/"IDE2" physical-drive-numbering
 * scheme vfs_mount()'s own device-string argument already uses (see
 * vfs.c's header comment), which is a naming coincidence worth being
 * honest about: today EVERY mount in this project (the Day13 vfs_mount
 * ("IDE0") call and the Day16/17 direct fat_vfs_mount() calls) is
 * really the SAME physical IDE0 drive at different LBAs (there is
 * still only one ide0_block_device() — see vfs.c), so "IDE0"/"IDE1"/
 * "IDE2" as MOUNT SLOT names here do not yet mean three different
 * physical drives, just three different mounted volumes in the order
 * they were mounted. That distinction stops mattering once ide1_block_
 * device() etc. are real; the slot-numbering scheme doesn't need to
 * change either way.
 */
#define VFS_MAX_MOUNTS  8

typedef struct {
    char          name[8];  /* "IDE0".."IDE7", auto-assigned, NUL-terminated */
    filesystem_t *fs;       /* the mounted filesystem_t this slot names */
} vfs_mount_entry_t;

extern vfs_mount_entry_t vfs_mounts[VFS_MAX_MOUNTS];
extern uint32_t          vfs_mount_count;

/* vfs_register_mount(fs, out_name)
 *   Assigns `fs` the next free "IDE<N>" slot name (N = however many
 *   mounts have been registered so far, so mounts are numbered in the
 *   exact order this function is called) and records it in
 *   `vfs_mounts[]`. Also updates `vfs_root = fs->root` so every
 *   existing caller that only ever cared about "the current root"
 *   (Shell's `ls`, etc.) keeps working unchanged — this table is
 *   purely additive.
 *
 *   If `out_name` is non-NULL, the assigned slot name (e.g. "IDE1") is
 *   copied into it — `out_name` must point at a buffer of at least 8
 *   bytes, matching `vfs_mount_entry_t.name`'s size.
 *
 *   Returns 0 on success. Returns -1 if `fs` is NULL or the table is
 *   full (VFS_MAX_MOUNTS reached — this project mounts at most 3
 *   volumes today, so that ceiling isn't expected to matter yet).
 */
int vfs_register_mount(filesystem_t *fs, char out_name[8]);
/* DAY62: caller-chosen slot name (live CD: "RAM0", "CD0") instead of "IDE<n>". */
int vfs_fs_has_subdirs(filesystem_t *fs);                       /* DAY63: lmfs or iso9660 */
int vfs_fs_open_path(filesystem_t *fs, const char *path, vfs_node_t *out);   /* DAY63: dispatch by fs->name */
int vfs_register_mount_named(filesystem_t *fs, const char *name, char out_name[8]);

/* vfs_lookup_mount(name)
 *   Finds a previously vfs_register_mount()-ed filesystem_t by its
 *   slot name (e.g. "IDE0"). This is what fat_open() (kernel/fs/fat/
 *   fat_vfs.c) calls to turn a path's leading "IDE<N>" segment into
 *   the filesystem_t to search. Returns NULL if `name` doesn't match
 *   any registered slot.
 */
filesystem_t *vfs_lookup_mount(const char *name);

/* vfs_mount_name_of(fs)
 *   The REVERSE of vfs_lookup_mount(): given a filesystem_t pointer
 *   (e.g. `vfs_root->fs`), returns the mount-slot name it was
 *   registered under, or NULL if it was never registered. This is
 *   what display code (kmain.c's shell prompt) should call to find a
 *   volume's TRUE current slot name — NOT `vfs_root->name`, which
 *   stays whatever fat12_vfs_mount()/fat16_vfs_mount()/fat32_vfs_
 *   mount() hardcoded by default ("IDE0") and can be silently reset
 *   back to that default by a later, unrelated re-mount of the same
 *   fixture (see vfs.c's doc comment on this function for the full
 *   story of why an in-place node-name overwrite doesn't work here).
 */
const char *vfs_mount_name_of(filesystem_t *fs);

#endif /* LUMA_VFS_H */