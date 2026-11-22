/* kernel/fs/fat/fat_vfs.h
 * LumaOS 5.0 - FAT-Family Auto-Detecting VFS Mount Dispatcher
 *
 * DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
 *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载一个
 *    FAT16 镜像，ls 能正确列出文件。"
 *
 *   This is the dispatcher fat_common.h's own DAY15 header comment
 *   foretold: "Once FAT16/32 drivers exist, the natural place for a
 *   caller to use fat_mount()'s result is a small dispatcher (e.g.
 *   inside vfs.c, or a future generic fat_vfs_mount())... not built
 *   today since only fat12_mount() exists, but this function's shape
 *   is exactly what that dispatch will need." Today FAT16's driver
 *   (fat16.h/fat16.c) and its VFS adapter (fat16_vfs.h/fat16_vfs.c)
 *   both exist alongside FAT12's, so this file ties them together:
 *   fat_vfs_mount(dev, lba) runs Day 15's fat_mount() to find out
 *   WHICH FAT variant lives at `lba`, then routes to whichever
 *   variant-specific *_vfs_mount() actually knows how to mount it.
 *
 *   This file is deliberately separate from BOTH kernel/fs/vfs/vfs.c
 *   (which stays the ONE place that resolves a device STRING like
 *   "IDE0" to a physical dev+lba — see vfs.c's own header comment) and
 *   fat_common.c (which stays variant-agnostic on principle — it has
 *   no #include of fat12.h/fat16.h and this file needs both, so this
 *   logic can't live there without breaking that layering). Instead,
 *   fat_vfs_mount() sits one layer above fat12_vfs_mount()/
 *   fat16_vfs_mount(), and is itself called by vfs_mount() (which
 *   supplies the resolved dev+lba) AND directly by kmain.c's DAY16
 *   test section (to mount the new FAT16 fixture and make it live for
 *   the shell's `ls`, alongside vfs_mount("IDE0")'s existing FAT12
 *   regression test — see kmain.c's own DAY16 comment).
 *
 * Design notes:
 *   - Returns a `filesystem_t *`, exactly what fat12_vfs_mount()/
 *     fat16_vfs_mount() themselves return — this function adds no new
 *     return shape, it just picks which of the two to call. Callers
 *     that want vfs_root updated do that themselves (`vfs_root =
 *     fs->root`), the same way vfs_mount() already does after calling
 *     fat12_vfs_mount() directly today.
 *   - FAT32 (Day 17) is a natural future `case FAT_TYPE_FAT32:` branch
 *     here once fat32_vfs_mount() exists — this function's shape does
 *     not need to change shape for that, only grow one more case.
 */
#ifndef LUMA_FAT_VFS_H
#define LUMA_FAT_VFS_H

#include "../../drivers/block/block.h"   /* block_device_t */
#include "../vfs/vfs.h"                  /* filesystem_t */

/* ===== DAY21 (Week 3, Day 21 of the 5.0 filesystem roadmap — "周复盘") =====
 *
 *   "添加错误处理（文件不存在、簇链断裂）。关闭文件功能（fat_close）。"
 *
 *   Through Day 20, every failure case fat_open()/fat_read() could hit
 *   collapsed onto the same generic -1 ("failed", per vfs.h's own
 *   "Week 10 (Day 70) is where the roadmap formalizes named error
 *   codes... until then -1 is the generic failed value" design note).
 *   That was fine while nothing needed to tell failures apart, but
 *   fat_vfs.c's own DAY19 comment on fat_read_chain() already flagged
 *   the gap this day closes: "Day 21's '簇链断裂' error-handling
 *   week-recap item is where a friendlier diagnostic for this case is
 *   planned; for now it's folded into the same -1 read-didn't-fully-
 *   succeed return."
 *
 *   fat_error_t below is a SMALL, EARLY step toward Day 70's eventual
 *   named-error-code work — not that work itself. It only distinguishes
 *   the handful of cases THIS day's roadmap text names by name ("文件
 *   不存在", "簇链断裂") plus the couple of pre-existing generic buckets
 *   they were carved out of (bad arguments, a lower-level I/O failure).
 *   Every value is still <= 0 (0 reserved for "not an error", matching
 *   `vfs_read_fn`'s own "0 == EOF, not failure" convention already
 *   documented in vfs.h) — so every existing caller that only ever
 *   checked `fat_open(...) != 0` or `n < 0` keeps working unmodified;
 *   this is purely an ADDITIVE refinement of what the negative return
 *   value now specifically means, not a signature or contract change.
 *   fat_read()'s vfs_read_fn contract in particular already promised
 *   "returns... a negative value on error" (not literally -1), so
 *   returning one of these specific codes instead was always within
 *   that contract.
 */
typedef enum {
    FAT_OK               =  0,  /* not an error */
    FAT_ERR_INVALID_ARGS = -1,  /* NULL pointer, malformed path, node not
                                    a file, unrecognized fs->name, etc. —
                                    a caller-side misuse, not a disk-
                                    content problem */
    FAT_ERR_NOT_FOUND    = -2,  /* DAY21 "文件不存在": fat_open()'s mount-
                                    slot segment names no registered
                                    mount, or no root-directory entry's
                                    name matches the requested filename */
    FAT_ERR_IO           = -3,  /* the underlying dev->read() reported
                                    failure while walking the FAT table
                                    or reading a data cluster — a real
                                    hardware/media read error, not a
                                    logically-inconsistent chain */
    FAT_ERR_CHAIN_BROKEN = -4,  /* DAY21 "簇链断裂": the cluster chain hit
                                    its own legitimate end-of-chain (EOC)
                                    marker before reaching the cluster
                                    file_pos should have landed in, i.e.
                                    the chain is genuinely shorter than
                                    what the directory entry's file size
                                    claims — a corrupt/inconsistent
                                    filesystem, not an I/O failure */
} fat_error_t;

/* fat_strerror(err)
 *
 * Turns one of the fat_error_t values above (or FAT_OK) into a short,
 * human-readable, NUL-terminated string — e.g. for cmd_read
 * (kernel/shell/cmd/read.c; DAY21's cmd_cat -> cmd_show, DAY27 removed
 * show, read.c is the survivor) to print something more useful than a
 * bare negative number when fat_open()/fat_read() fail. Always returns a valid pointer, even for a value outside
 * fat_error_t's own range (a defensive "unknown error" fallback string)
 * — callers never need to NULL-check the result.
 */
const char *fat_strerror(int err);

/* fat_vfs_mount(dev, lba)
 *
 * Detects which FAT variant (if any) is mounted at `lba` on `dev` via
 * fat_mount() (Day 15, kernel/fs/fat/fat_common.c), then dispatches to
 * the matching variant-specific adapter:
 *
 *   FAT_TYPE_FAT12 -> fat12_vfs_mount(dev, lba)
 *   FAT_TYPE_FAT16 -> fat16_vfs_mount(dev, lba)
 *   FAT_TYPE_FAT32 -> fat32_vfs_mount(dev, lba)  (DAY17)
 *   FAT_TYPE_UNKNOWN -> not mounted, returns NULL
 *
 * Returns the populated `filesystem_t *` on success (whichever
 * variant's *_vfs_mount() produced it), or NULL if `lba` doesn't hold
 * a FAT12/16/32 volume this project can currently mount, or if the
 * variant-specific mount call itself fails.
 */
filesystem_t *fat_vfs_mount(block_device_t *dev, uint32_t lba);

/* fat_open(path, out_node)
 *
 * DAY18 (Week 3, Day 18 of the 5.0 filesystem roadmap):
 *   "实现 fat_open(path)：根据路径（如 /KERNEL.BIN）遍历目录项，返回文件
 *    的起始簇号。对根目录文件能正确返回簇号。"
 *
 * DAY18-FOLLOWUP (user-requested change, ahead of Day 35's own "mount
 * table" plan — see vfs.h's doc comment on vfs_register_mount()/
 * vfs_lookup_mount() for the full rationale):
 *   The roadmap's own example path, "/KERNEL.BIN", assumed a single
 *   global `vfs_root` (the ORIGINAL Day 13-18 design: "the" mounted
 *   volume, whichever one mounted last). That stopped being enough
 *   once more than one volume could be mounted at once and needed to
 *   stay independently reachable — so `fat_open()` now takes a path
 *   whose FIRST segment is a mount-table slot name ("IDE0", "IDE1",
 *   "IDE2", ... — see vfs.h) instead of a leading '/', and no longer
 *   takes an explicit `filesystem_t *fs` parameter at all: which
 *   filesystem to search is now entirely determined by parsing that
 *   leading segment out of `path` itself and looking it up via
 *   vfs_lookup_mount(). Concretely: `fat_open("IDE0/BIGFILE.BIN",
 *   &node)` searches whatever volume registered as "IDE0" (in this
 *   project's kmain.c, that's the FAT12 fixture, mounted first via
 *   vfs_mount("IDE0") in the DAY13 section); `fat_open("IDE1/SMALL.
 *   BIN", &node)` searches the second-mounted volume (FAT16, DAY16
 *   section); `fat_open("IDE2/SMALLFIL.BIN", &node)` the third
 *   (FAT32, DAY17 section) — see kmain.c's DAY18 test section for all
 *   three exercised together.
 *
 *   This function still needs no per-variant FAT12/16/32 branch at
 *   all, for the same reason the original Day 18 design didn't: once
 *   vfs_lookup_mount() has produced a `filesystem_t *`, resolving the
 *   remainder of the path is just `fs->readdir(fs->root, idx, &child)`
 *   in a loop — identical across all three variants since Day 13/16/
 *   17. fat12_vfs_readdir()/fat16_vfs_readdir()/fat32_vfs_readdir()
 *   still don't change for this either.
 *
 *   PATH SYNTAX: "<mount-slot>/<filename>" — exactly one '/' expected,
 *   separating a mount-table slot name (registered by an earlier
 *   vfs_register_mount() call — see vfs.h) from one 8.3 filename
 *   naming a ROOT DIRECTORY entry on that volume. This still only
 *   covers root-directory files, per the roadmap's own explicit scope
 *   ("对根目录文件能正确返回簇号") — a path with a SECOND '/' after
 *   the mount-slot segment (e.g. "IDE0/SYS/FOO.BIN") names a file
 *   inside a subdirectory, which this function still deliberately
 *   REJECTS (-1) rather than mis-resolving, for the same reason as
 *   before: subdirectories don't exist in this project's VFS yet
 *   (real multi-level path walking is Day 42's "多级目录与路径解析"
 *   job). A path with NO '/' at all (no mount-slot segment given) is
 *   also rejected (-1) — there is deliberately no "assume the last-
 *   mounted volume" fallback anymore now that a mount table exists;
 *   every caller must name which volume it means.
 *
 *   NAME COMPARISON: exact `strcmp()` against the already-decoded,
 *   dot-joined 8.3 name fat_decode_short_name() produces (e.g.
 *   "BIGFILE.BIN") — the same string readdir() callers already print.
 *   Case-sensitive; FAT8.3 names are conventionally stored/decoded
 *   upper-case, so callers should pass the filename upper-case too.
 *
 * Returns 0 (FAT_OK) and fills `*out_node` (a full copy of the matching
 * child vfs_node_t — name/type/size/fs/fs_private, exactly what
 * fs->readdir() itself would have produced for that entry) if the path
 * resolves to an existing root-directory entry on a registered mount.
 *
 * DAY21: on failure, returns one of fat_error_t's negative values
 * instead of a flat -1 — still always < 0, so every pre-Day21 caller
 * that only checked `fat_open(...) != 0` keeps working unchanged:
 *
 *   FAT_ERR_INVALID_ARGS - `path`/`out_node` is NULL; `path` has no
 *     '/' at all, or the mount-slot segment before it is empty or too
 *     long to be a valid slot name (see VFS_MAX_MOUNTS's `name[8]`);
 *     the remainder after the '/' is empty (naming the mount's root
 *     itself, not a file) or contains a second '/' (unsupported
 *     subdirectory path, see above) — all caller-side path-shape
 *     mistakes, not "the file doesn't exist".
 *   FAT_ERR_NOT_FOUND - DAY21's "文件不存在": the mount-slot segment
 *     names no currently-registered mount (vfs_lookup_mount() found
 *     nothing), OR the mount-slot is valid but no root-directory
 *     entry's name matches the requested filename (fs->readdir() ran
 *     out of entries before a match was seen). Both collapse to the
 *     same code because from a `read <path>` caller's point of view
 *     they mean the same thing: "nothing exists at that path" — see
 *     read.c's own comment for how it's reported.
 */
int fat_open(const char *path, vfs_node_t *out_node);

/* fat_read(file, buf, size)
 *
 * DAY19 (Week 3, Day 19 of the 5.0 filesystem roadmap):
 *   "实现 fat_read(file, buf, size)：根据起始簇，依次读取簇链，将数据
 *    拷贝到 buf。实现 cmd_cat /KERNEL.BIN，能在终端打印出内核二进制的
 *    开头部分（乱码也行，证明读到了数据）。"
 *
 *   `file` is a Day 8 `file_t` — the same shape `fat_open()`+
 *   fs->open() (fat12_vfs_open()/fat16_vfs_open()/fat32_vfs_open())
 *   already produce: `file->node` is a VFS_TYPE_FILE vfs_node_t whose
 *   `fs_private` holds the file's STARTING CLUSTER NUMBER (see
 *   fat_open()'s doc comment above, and each variant's *_vfs_readdir()
 *   — the convention is identical across FAT12/16/32) and whose `size`
 *   is the file's byte length from its directory entry. `file->pos` is
 *   the byte offset to resume reading from — 0 on every call through
 *   Day 19 (nothing sets it to anything else yet), but this function
 *   is written to respect whatever it holds so Day 20 ("增加文件偏移
 *   指针 file->pos，支持多次读取") is a caller-side change only: it
 *   only needs to update `file->pos` after a successful fat_open()+
 *   fs->open() and call fat_read() again, no change to this function's
 *   body.
 *
 *   THIS FUNCTION NEEDS ONE PER-VARIANT BRANCH, UNLIKE fat_open():
 *   fat_open()'s loop only ever calls fs->readdir(), already identical
 *   across FAT12/16/32 since Day 13/16/17. Walking a FILE's cluster
 *   chain has no such shared vfs_readdir_fn-shaped hook — each variant
 *   has its own *_next_cluster() (fat12_next_cluster() 12-bit-packed,
 *   fat16_next_cluster() plain 16-bit, fat32_next_cluster() masked
 *   28-bit — see fat12.h/fat16.h/fat32.h's own doc comments) over its
 *   own *_fs_t mount-state shape (fat12_fs_t/fat16_fs_t/fat32_fs_t —
 *   NOT byte-compatible past their shared `dev`/`mount_lba`/`bpb`
 *   prefix: fat32_fs_t replaces fat_geometry_t's `geom` field with its
 *   own flat first_data_sector/bytes_per_cluster fields plus a
 *   fat32_bpb_ext_t `ext` the other two don't have — see fat32.h's
 *   `fat32_fs_t` doc comment). So fat_read() dispatches on
 *   `file->node->fs->name` ("fat12"/"fat16"/"fat32", set by each
 *   variant's `*_filesystem_ops` table — see fat12_vfs.c/fat16_vfs.c/
 *   fat32_vfs.c) to a small per-variant helper in this file
 *   (fat12_read_chain()/fat16_read_chain()/fat32_read_chain(), static
 *   to fat_vfs.c) that knows its own fs_t shape and *_next_cluster()
 *   signature; the byte-copy loop shape itself (walk clusters, skip
 *   whole clusters until file->pos's target cluster, copy min(size,
 *   bytes-left-in-cluster, bytes-left-in-file) per cluster, advance)
 *   is identical across all three and is NOT triplicated — see
 *   fat_vfs.c's DAY19 comment for exactly how the shared shape and the
 *   per-variant cluster-table lookup are split.
 *
 * Returns the number of bytes actually copied into `buf` (0 <= n <=
 * size) and advances `file->pos` by that amount. Returns 0 (not an
 * error) if `file->pos` is already at or past `file->node->size` — the
 * documented vfs_read_fn "0 == EOF" contract (see vfs.h).
 *
 * DAY21: on failure, returns one of fat_error_t's negative values
 * instead of a flat -1 (still always < 0, so every pre-Day21 caller
 * that only checked `n < 0` for "read failed" keeps working
 * unchanged):
 *
 *   FAT_ERR_INVALID_ARGS - `file`/`buf`/`file->node` is NULL,
 *     `file->node->type` is not VFS_TYPE_FILE, `file->node->fs->name`
 *     doesn't match a known FAT variant, or the starting cluster is 0
 *     (a 0-length file has nothing to chain-walk — this project's own
 *     fixtures never produce this, but a 0-byte file is spec-legal) —
 *     all caller/data-shape problems distinct from either error below.
 *   FAT_ERR_IO - an underlying dev->read() call failed partway through
 *     (whatever was already copied into `buf` before the failing read
 *     stays valid, but the return value is negative so the caller
 *     knows the transfer was cut short — mirrors fat12_readdir()'s own
 *     "partial progress on a later error" contract). This is a real
 *     hardware/media read failure, not a logically-broken chain.
 *   FAT_ERR_CHAIN_BROKEN - DAY21's "簇链断裂": the cluster chain hit
 *     its own legitimate FAT_CHAIN_EOC marker before reaching the
 *     cluster `file_pos`'s target byte offset should have landed in —
 *     i.e. the directory entry's file size claims more data than the
 *     chain actually contains. This is a genuinely different situation
 *     from FAT_ERR_IO (the disk read itself succeeded every time; the
 *     FAT metadata is just internally inconsistent) and is exactly the
 *     case fat_vfs.c's fat_read_chain() DAY19 comment predicted this
 *     day would give "a friendlier diagnostic" for.
 *
 * DAY20 UPDATE (Week 3, Day 20 of the 5.0 filesystem roadmap):
 *   "增加文件偏移指针 file->pos，支持多次读取。连续调用两次 read，第
 *    二次能接着上次位置读。" — this function's BODY did not change for
 *   Day 20 (see the DAY19 paragraph above: it was already written to
 *   respect whatever `file->pos` holds). What changed is who actually
 *   CALLS it more than once with a nonzero, cluster-spanning
 *   file->pos: kernel/shell/cmd/cat.c's cmd_cat now loops fs->read()
 *   until EOF instead of one capped call, and kmain.c's DAY20
 *   boot-time section calls this repeatedly with a chunk size that
 *   deliberately does not divide the 512-byte cluster size evenly, so
 *   file->pos regularly resumes mid-cluster across calls — see both
 *   files' DAY20 comments for the full rationale and exactly which
 *   fat_read_chain() code path (kernel/fs/fat/fat_vfs.c) that newly
 *   exercises.
 */
int fat_read(file_t *file, void *buf, uint32_t size);

/* fat_close(file)
 *
 * DAY21 (Week 3, Day 21 of the 5.0 filesystem roadmap — "周复盘"):
 *   "关闭文件功能（fat_close）。"
 *
 *   The shared implementation every variant's `fs->close` (see
 *   fat12_vfs.c/fat16_vfs.c/fat32_vfs.c's `fatNN_vfs_close()`
 *   trampolines, mirroring how `fatNN_vfs_read()` already just
 *   forwards to fat_read()) funnels into — see vfs_close_fn's doc
 *   comment (kernel/fs/vfs/vfs.h) for the full contract this
 *   implements. Unlike fat_read(), this needs NO per-variant branch at
 *   all: closing doesn't touch cluster chains, `*_next_cluster()`, or
 *   any variant-specific `fs_private` shape — it only ever touches the
 *   filesystem-agnostic `file_t` fields `vfs.h` itself defines
 *   (`node`, `pos`). That is why this lives directly in fat_vfs.c as
 *   ONE shared function (not three per-variant helpers behind a
 *   dispatch, the way fat_read_chain() needs), with each variant's
 *   adapter file only contributing a one-line trampoline so `fs->close`
 *   has a variant-tagged function pointer to point at, matching the
 *   shape `fs->open`/`fs->read`/`fs->readdir` already have.
 *
 * Returns FAT_OK (0) on success, having set `file->node = NULL` and
 * `file->pos = 0` (see vfs_close_fn's doc comment for why: a later
 * accidental `fs->read()` on the same `file_t` will then see a NULL
 * `file->node` and fail, per fat_read()'s own NULL-check, instead of
 * silently continuing to read a "closed" file). Returns
 * FAT_ERR_INVALID_ARGS if `file` is NULL or `file->node` is already
 * NULL (double-close, or a `file_t` that was never successfully
 * opened) — closing something that isn't open is a caller mistake
 * worth reporting, not a silent no-op.
 */
int fat_close(file_t *file);

/* fat_find_free_cluster(fs, out_cluster)
 *
 * DAY22 (Week 4, Day 22 of the 5.0 filesystem roadmap — start of
 * "第 4 周：FAT 写入支持（创建、删除、截断）"):
 *   "实现 fat_find_free_cluster()：扫描 FAT 表，返回第一个值为 0 的簇。
 *    测试打印空闲簇号。"
 *
 *   This is the FIRST function in the whole 5.0 filesystem roadmap
 *   that looks at the FAT table for anything other than following an
 *   already-allocated file's chain forward (fat12/16/32_next_cluster(),
 *   Day 11/16/17, and fat_read_chain()'s use of them, Day 19) — it
 *   scans the table itself, cluster by cluster, looking for the FIRST
 *   entry whose raw value is FATxx_CLUSTER_FREE (0x000/0x0000/
 *   0x00000000 — identically "0" across all three variants, see each
 *   variant's own *_CLUSTER_FREE constant in fat12.h/fat16.h/fat32.h).
 *   Nothing is written back to disk today (that's Day 23's
 *   fat_alloc_cluster() job — "分配一个新簇，并更新 FAT 表（写回磁
 *   盘）" — this function is read-only, purely a scan): Day 22's own
 *   acceptance text only asks to find and print a free cluster
 *   number, not to actually claim it.
 *
 *   WHY THIS NEEDS NO NEW PER-VARIANT ON-DISK CODE: a raw, unmasked
 *   FAT[cluster] entry read is EXACTLY what fat12_next_cluster()/
 *   fat16_next_cluster()/fat32_next_cluster() already compute — "the
 *   next cluster in the chain" IS the raw table value at that index,
 *   whether or not `cluster` happens to currently belong to any real
 *   file's chain at all. So this function reuses fat_vfs.c's own
 *   DAY19 `fat_next_cluster_fn` trampolines (fat12_next_cluster_
 *   adapter/fat16_next_cluster_adapter/fat32_next_cluster_adapter,
 *   static to fat_vfs.c) completely unchanged — the exact same
 *   "normalize three on-disk formats to one shared convention" layer
 *   fat_read_chain() already relies on: a non-negative return is the
 *   raw (masked, for FAT32) table value, FAT_CHAIN_EOC means this
 *   entry's value happens to fall in that variant's end-of-chain
 *   range (an allocated file's LAST cluster — still not free, just
 *   not "points to a next cluster" either), and -1 means the
 *   underlying dev->read() failed. This function only adds a THIRD
 *   interpretation on top of the two the trampolines already
 *   produce: a returned value that is exactly 0 means this cluster is
 *   free — see each variant's *_CLUSTER_FREE constant, all defined as
 *   0.
 *
 *   SCAN RANGE: cluster numbers 2 .. (total_clusters + 1) inclusive —
 *   clusters 0 and 1 are always reserved on every FAT variant (see
 *   each *_CLUSTER_RESERVED) and are never valid allocatable data
 *   clusters, so the scan deliberately starts at 2, exactly the same
 *   "clusters are numbered starting from 2" convention fat_read_chain()
 *   and every *_readdir() already use. `total_clusters` itself comes
 *   straight from the variant's own mount-state geometry
 *   (fat12_fs_t.geom.total_clusters / fat16_fs_t.geom.total_clusters /
 *   fat32_fs_t.total_clusters — see fat_common.h's DAY15 note and
 *   fat32.h's own field for why FAT32 keeps a flat field here instead
 *   of reusing fat_geometry_t).
 *
 * `fs` is a mounted `filesystem_t *` — e.g. the direct return of
 * fat_vfs_mount(), or vfs_lookup_mount("IDE0")/... against this
 * project's numbered mount table (see vfs.h) — NOT a `vfs_node_t *`
 * or `file_t *`: this operates on the whole VOLUME's free-space
 * table, not any one file. `fs->name` selects which of the three
 * per-variant scans to run (the same "fat12"/"fat16"/"fat32" tag
 * fat_read() already switches on), and `fs->fs_private` must be the
 * matching fat12_fs_t (or fat16_fs_t, or fat32_fs_t) pointer that
 * fat12_vfs_mount()/fat16_vfs_mount()/fat32_vfs_mount() populated it
 * with.
 *
 * On success, returns FAT_OK (0) and writes the first free cluster
 * number found into `*out_cluster`. Returns FAT_ERR_NOT_FOUND if the
 * ENTIRE table was scanned and no free cluster exists (the volume is
 * full — reusing this code rather than adding a new one because, from
 * a caller's point of view, "no free cluster satisfies the search" is
 * the same kind of outcome fat_open()'s "no matching filename" case
 * already uses it for — see fat_error_t's own doc comment). Returns
 * FAT_ERR_INVALID_ARGS if `fs`/`out_cluster` is NULL, `fs->fs_private`
 * is NULL, or `fs->name` doesn't match a known FAT variant. Returns
 * FAT_ERR_IO if a dev->read() call fails partway through the scan
 * (whatever clusters were already checked before the failing read are
 * simply not reported — there is no "partial progress" concept for a
 * single cluster-number result the way there is for fat_read()'s byte
 * count).
 */
int fat_find_free_cluster(filesystem_t *fs, uint32_t *out_cluster);

/* fat_alloc_cluster(fs, out_cluster)
 *
 * DAY23 (Week 4, Day 23 of the 5.0 filesystem roadmap):
 *   "实现 fat_alloc_cluster()：分配一个新簇，并更新 FAT 表（写回磁盘）。
 *    分配成功后，再次读取 FAT 对应位置值变为 0xFFF（FAT12）或 0xFFFF
 *    （FAT16）。"
 *
 *   Where Day 22's fat_find_free_cluster() only LOOKS for a free
 *   cluster (pure scan, nothing written), this function actually
 *   CLAIMS one: it runs fat_find_free_cluster() to find the first
 *   cluster whose FAT entry is 0, then writes that cluster's own
 *   variant-appropriate end-of-chain marker into its FAT entry —
 *   FAT12_CLUSTER_EOC_MAX (0xFFF), FAT16_CLUSTER_EOC_MAX (0xFFFF), or
 *   FAT32_CLUSTER_EOC_MAX (0x0FFFFFFF) — via the matching variant's
 *   new Day 23 write-side function (fat12_set_cluster_value()/
 *   fat16_set_cluster_value()/fat32_set_cluster_value(), see each
 *   variant's own header for the full write contract), then persists
 *   that write to disk before returning.
 *
 *   WHY THE EOC MARKER, NOT A REAL NEXT-CLUSTER LINK: this function
 *   allocates exactly ONE cluster at a time and has no second cluster
 *   yet to chain it to — from the newly-claimed cluster's own point of
 *   view it is, right now, a complete one-cluster chain, i.e. its own
 *   last cluster. That is precisely what an EOC value in FAT[cluster]
 *   means (see fat_read_chain()'s own FAT_CHAIN_EOC handling in this
 *   file: "legitimate end of chain", not an error). A caller that
 *   later needs to EXTEND this one-cluster chain (Day 25's "如果当前
 *   簇链不够，追加新簇") would call fat_alloc_cluster() again for the
 *   new cluster, then overwrite the OLD last cluster's EOC entry with
 *   the new cluster's number via the same *_set_cluster_value()
 *   function this call already used — not this function's own job
 *   today, which only ever claims a single, freshly-terminated
 *   cluster.
 *
 *   WHY THIS NEEDS NO NEW PER-VARIANT SCAN LOGIC: exactly like Day
 *   22's fat_find_free_cluster(), the "which variant is this fs, and
 *   what's its fs_private/total_clusters" dispatch is the only
 *   variant-specific work here — this function reuses Day 22's
 *   fat_find_free_cluster() as-is for the search half, and adds one
 *   small parallel three-way dispatch (mirroring fat_find_free_
 *   cluster()'s own) purely to route to the correct *_set_cluster_
 *   value() for the write half. See fat_vfs.c's DAY23 comment for the
 *   `fat_set_cluster_fn` trampoline shape this reuses from Day 19's
 *   `fat_next_cluster_fn` pattern.
 *
 * `fs` is a mounted `filesystem_t *` (same as fat_find_free_cluster()'s
 * `fs` parameter — not a `vfs_node_t *`/`file_t *`; this operates on
 * the whole volume's FAT table, not any one file).
 *
 * On success, returns FAT_OK (0) and writes the newly-claimed cluster
 * number into `*out_cluster` (that cluster's FAT entry now reads as
 * its variant's EOC-max value — independently re-readable via the
 * variant's own *_next_cluster(), same "verifiable from outside the
 * function under test" spirit as Day 22's own acceptance criterion).
 *
 * Returns FAT_ERR_INVALID_ARGS if `fs`/`out_cluster` is NULL,
 * `fs->fs_private` is NULL, or `fs->name` doesn't match a known FAT
 * variant. Returns FAT_ERR_NOT_FOUND if fat_find_free_cluster() itself
 * found no free cluster (volume full) — forwarded unchanged, since
 * there is nothing left for this function to claim. Returns
 * FAT_ERR_IO if `fs`'s underlying block_device_t has no `write`
 * function (a read-only backend — allocation is fundamentally
 * impossible, not a transient failure) or if the *_set_cluster_value()
 * write itself fails (dev->read()/dev->write() error while persisting
 * the EOC marker) — in either case the cluster found by
 * fat_find_free_cluster() is simply never claimed; no partial FAT
 * table corruption is possible since *_set_cluster_value() either
 * completes its single sector write or doesn't touch disk at all.
 */
int fat_alloc_cluster(filesystem_t *fs, uint32_t *out_cluster);

/* fat_create_file(path)
 *
 * DAY24 (Week 4, Day 24 of the 5.0 filesystem roadmap):
 *   "实现 fat_create_file(name)：在根目录区找一个空条目（首字节 0x00
 *    或 0xE5），填入文件名、起始簇 0、大小 0。在 Shell 中输入 create
 *    test.txt，重新 ls 能看到它。"
 *
 *   Same "<mount-slot>/<filename>" path syntax and parsing as
 *   fat_open() (Day 18, see that function's own doc comment above for
 *   the full "why a mandatory mount-slot segment, why only root-
 *   directory-level names, why exact strcmp" rationale — none of it
 *   changes for the create side) — a Shell user names WHICH mounted
 *   volume to create the file on the exact same way they already do
 *   for `show IDE0/HELLO.TXT`, e.g. `create IDE0/TEST.TXT`.
 *
 *   Unlike fat_open(), this function needs a per-variant branch (the
 *   same reason fat_read()/fat_find_free_cluster()/fat_alloc_cluster()
 *   all do, and fat_open() itself does NOT): creating a directory
 *   ENTRY, not just walking existing ones, has no shared
 *   vfs_readdir_fn-shaped hook the way listing already-decoded
 *   entries does. FAT12/16 share fat_common.h's fat_create_entry_
 *   fixed_root() (their root is byte-for-byte the same fixed-size
 *   region shape fat_readdir_fixed_root() already shares between
 *   them); FAT32 has its own fat32_create_entry_root() (its root is a
 *   cluster chain, not a fixed region — see fat32.h's own doc comment
 *   for why that needs a genuinely different walk, mirroring the
 *   fat_read()/fat32_readdir() split this project already has for the
 *   read side).
 *
 *   DUPLICATE-NAME CHECK: before claiming a free slot, this function
 *   first calls fat_open() on the same path — if that SUCCEEDS, `name`
 *   already exists on this volume and this function refuses to create
 *   a second entry with the same name (returns FAT_ERR_INVALID_ARGS;
 *   see fat_common.h's doc comment on fat_create_entry_fixed_root()
 *   for why silently allowing a duplicate would produce a second,
 *   permanently-unreachable-by-fat_open() entry rather than a crash —
 *   still worth rejecting up front with a clear diagnostic instead of
 *   leaving a confusing half-working duplicate on disk). This check is
 *   NOT part of fat_create_entry_fixed_root()/fat32_create_entry_root()
 *   themselves (see each of their own doc comments for why) — it lives
 *   here, the one layer that already has both fat_open() and the
 *   variant-specific create call in scope.
 *
 * Returns FAT_OK (0) on success. Returns FAT_ERR_INVALID_ARGS if
 * `path` is NULL or malformed the exact same way fat_open() rejects it
 * (see that function's doc comment for the full list: no '/', empty
 * mount-slot segment, empty filename, a second '/' after the mount
 * slot, or a filename that doesn't fit fat_encode_short_name()'s 8.3
 * scope), or if `path` already names an existing entry (the duplicate-
 * name check above). Returns FAT_ERR_NOT_FOUND if the mount-slot
 * segment names no currently-registered mount (same as fat_open()).
 * Returns FAT_ERR_IO if the underlying create call itself fails (a
 * read-only backend, a dev->read()/dev->write() failure while scanning
 * for or claiming a slot, or — FAT12/16 only — the fixed root region
 * is entirely full; FAT32's own "chain exhausted" case is folded into
 * this same code too, since fat32_create_entry_root() doesn't
 * distinguish "I/O failed" from "no free slot found" any more finely
 * than fat_create_entry_fixed_root() does — see each of their own doc
 * comments).
 */
int fat_create_file(const char *path);

/* fat_write(path, buf, size)
 *
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "实现 fat_write(file, buf, size)：处理簇链扩展（如果当前簇链不够，
 *    追加新簇）。在 Shell 中输入 write test.txt，写入内容，show 能看到。"
 *
 *   Same "<mount-slot>/<filename>" path syntax as fat_open()/
 *   fat_create_file() (Day 18/24) — a Shell user names which mounted
 *   volume to write to the exact same way `show IDE0/HELLO.TXT` and
 *   `create IDE0/TEST.TXT` already do, e.g. `write IDE0/TEST.TXT`. The
 *   file named by `path` must already exist (via fat_create_file(),
 *   typically — this project's own Shell `write` command creates it
 *   first if it doesn't, see kernel/shell/cmd/write.c's own header
 *   comment) — unlike fat_create_file(), this function does NOT create
 *   a new directory entry; fat_open() failing on `path` is a plain
 *   error here (FAT_ERR_NOT_FOUND, forwarded from fat_open() itself).
 *
 *   OVERWRITES the file's entire content with `buf` (always writing
 *   starting at byte offset 0, regardless of whatever the file's OLD
 *   size/content was — see kernel/shell/cmd/write.c's own header
 *   comment for why a full in-place overwrite, not append, is what
 *   this project's `write` command means): the new file size becomes
 *   exactly `size`.
 *
 *   DAY27 UPDATE (closes a leak documented — and deliberately left
 *   open — by this paragraph through Day 26): before Day 27, a write
 *   to a file whose start_cluster was already nonzero (any second-or-
 *   later write) left the file's ENTIRE OLD chain allocated but
 *   orphaned — this function always started a fresh chain from
 *   scratch rather than reusing or freeing the old one, so every
 *   cluster the old chain held (not just any size-shrinkage tail) leaked.
 *   Now that fat_truncate() (Day 27, this header's own declaration
 *   below) exists, fat_write() calls the SAME static chain-freeing
 *   helper fat_truncate()/fat_delete_file() use — see fat_vfs.c's own
 *   implementation comment at the top of this function's DAY27 block —
 *   to free the file's old chain FIRST, whenever start_cluster is
 *   already nonzero, before allocating any new cluster. The directory
 *   entry is then always rebuilt as if writing a freshly-`create`d
 *   file (start_cluster 0, per fat_create_file()'s own Day 24
 *   contract), so every fat_alloc_cluster() call below claims a
 *   genuinely fresh cluster and nothing from the old chain is ever
 *   reused OR left stranded. This is exactly the optimization-turned-
 *   correctness-fix this project's own Day 25 doc comment (see the
 *   history preserved in fat_delete_file()'s DAY26 comment above) had
 *   flagged as "the natural place to add alongside actually freeing
 *   the old chain" once Day 27's free-chain machinery existed.
 *
 *   CLUSTER-CHAIN HANDLING (this day's headline deliverable, "处理簇链
 *   扩展"):
 *     - If the file's directory entry currently has start cluster 0
 *       (a freshly-created, never-written-to file — "起始簇 0" per
 *       Day 24's own fat_create_file() contract), this function first
 *       calls fat_alloc_cluster() (Day 23) to claim the file's FIRST
 *       data cluster.
 *     - Bytes are then copied into that cluster (and any it already
 *       had, for a file being overwritten a second time) up to
 *       `bytes_per_cluster` bytes per cluster, exactly the same
 *       "how many bytes fit in a cluster" geometry fat_read_chain()
 *       (kernel/fs/fat/fat_vfs.c) already uses for the read side.
 *     - Once the CURRENT last cluster in the chain is full and `size`
 *       still has bytes left to write, this function extends the
 *       chain: it calls fat_alloc_cluster() again for a brand new
 *       cluster, then overwrites the OLD last cluster's own FAT entry
 *       (which fat_alloc_cluster() had set to that variant's EOC-max
 *       sentinel when IT was claimed) with the NEW cluster's number,
 *       via the same *_set_cluster_value() function fat_alloc_cluster()
 *       itself uses — turning "the old last cluster, terminated" into
 *       "the old last cluster, now pointing at a real next cluster,
 *       with the NEW cluster now terminated instead". This is exactly
 *       the operation fat_alloc_cluster()'s own doc comment predicted
 *       this day would need (see that function's doc comment,
 *       "WHY THE EOC MARKER, NOT A REAL NEXT-CLUSTER LINK" paragraph):
 *       "A caller that later needs to EXTEND this one-cluster chain
 *       (Day 25's '如果当前簇链不够，追加新簇') would call
 *       fat_alloc_cluster() again for the new cluster, then overwrite
 *       the OLD last cluster's EOC entry with the new cluster's number
 *       via the same *_set_cluster_value() function this call already
 *       used" — this function is that caller.
 *     - NOTE (DAY27 UPDATE): this function always writes starting from
 *       the file's FIRST cluster and always calls fat_alloc_cluster()
 *       for every cluster it needs — it never walks an EXISTING old
 *       chain to reuse any of its clusters, even when the file being
 *       overwritten already had a long enough chain. Through Day 26
 *       this meant a second write to an already-nonzero file allocated
 *       a fresh replacement chain and left the ENTIRE old chain
 *       orphaned; as of Day 27 (see this function's own "DAY27 UPDATE"
 *       paragraph above) the old chain is explicitly FREED first via
 *       the same helper fat_truncate()/fat_delete_file() use, so
 *       nothing leaks any more — the remaining cost is purely the
 *       "could have reused the old chain's clusters instead of
 *       freeing-then-reallocating them" optimization, not a
 *       correctness gap.
 *     - Once every byte of `buf` has been written, the file's
 *       directory entry is updated via fat_update_entry_fixed_root()
 *       (FAT12/16) or fat32_update_entry_root() (FAT32) — see either
 *       function's own doc comment (fat_common.h / fat32.h) — with the
 *       file's (possibly-newly-allocated) FIRST cluster number and the
 *       new `size`, so a subsequent fat_open()/fat_read()/`show` sees
 *       the freshly-written content immediately.
 *     - The in-memory `vfs_node_t` this function was handed a PATH
 *       for is not itself updated (fat_write() only ever sees a path
 *       string, not a live vfs_node_t — see the "path, not file_t"
 *       design note below) — any ALREADY-OPEN file_t/vfs_node_t for
 *       this same path predating this call is expected to be re-opened
 *       (a fresh fat_open()) before being read again, the same "a
 *       stale handle is the caller's problem" discipline this
 *       project's has already established (fat_close() itself does not
 *       forbid a caller from still holding other file_t's onto the
 *       same node).
 *
 *   WHY `path`, NOT A `file_t *` (unlike fat_read()): fat_read()
 *   operates on an already-open `file_t` because reading is stateful
 *   across repeated calls (Day 20's file->pos). This project's Shell
 *   `write` command (kernel/shell/cmd/write.c) always writes the
 *   file's ENTIRE new content in one call — there is no repeated-call,
 *   resume-from-file->pos use case for writing yet (that would be a
 *   real POSIX-style incremental write, not something Day 25's "write
 *   test.txt 写入内容" Shell acceptance scenario exercises) — so taking
 *   a path directly (like fat_create_file()) and doing its own
 *   fat_open()-then-per-variant-dispatch internally is simpler than
 *   requiring every caller to fat_open()+fs->open() first for a single
 *   one-shot call.
 *
 * `buf`/`size`: `size` may be 0 (writing an empty file — legal, mirrors
 * fat_create_file()'s own "起始簇 0，大小 0" starting state; no cluster
 * is allocated and the directory entry's size is simply left/set to 0).
 *
 * Returns the number of bytes actually written (>= 0; always exactly
 * `size` on success — this function does not do PARTIAL writes the way
 * fat_read() can do partial reads, since every failure case below
 * aborts the whole operation rather than returning however far it got)
 * on success. Returns one of fat_error_t's negative values on failure:
 *
 *   FAT_ERR_INVALID_ARGS - `path`/`buf` is NULL (with `size` nonzero),
 *     `path` is malformed the same way fat_open()/fat_create_file()
 *     reject it, or `path` names a directory rather than a file.
 *   FAT_ERR_NOT_FOUND - the mount-slot segment names no registered
 *     mount, or fat_open() on `path` fails to find an existing entry
 *     (see the "must already exist" paragraph above).
 *   FAT_ERR_IO - the underlying block_device_t has no `write` function
 *     (read-only backend), fat_alloc_cluster() reports the volume is
 *     full (FAT_ERR_NOT_FOUND from that call is folded into this same
 *     code here — a full volume is an I/O-shaped "cannot complete the
 *     write" outcome from fat_write()'s own caller's point of view,
 *     not a "path not found" one), a dev->read()/dev->write() call
 *     fails while copying a cluster's worth of bytes or while linking
 *     a newly-allocated cluster into the chain, or the final
 *     fat_update_entry_fixed_root()/fat32_update_entry_root() call
 *     itself fails.
 */
int fat_write(const char *path, const void *buf, uint32_t size);

/* fat_delete_file(path)
 *
 * DAY26 (Week 4, Day 26 of the 5.0 filesystem roadmap):
 *   "实现 fat_delete_file(name)：找到目录项，标记为已删除（首字节设为
 *    0xE5），释放其占用的簇链（在 FAT 表中标记为空闲）。Shell 中输入
 *    rm test.txt，ls 结果中该文件消失，且空闲簇数增加。"
 *
 *   Same "<mount-slot>/<filename>" path syntax as every other fat_*
 *   entry point (fat_open()/fat_create_file()/fat_write(), Day 18/24/
 *   25) — e.g. `fat_delete_file("IDE0/TEST.TXT")`, or via the Shell's
 *   new `rm test.txt` (kernel/shell/cmd/rm.c, resolved through
 *   shell_resolve_path() the exact same way `show`/`write`/`create`
 *   already are). `path` must resolve to an EXISTING plain file
 *   (fat_open() is called internally first — same "don't reinvent a
 *   diagnostic fat_open() already produces" choice fat_write() makes);
 *   there is no directory concept yet in this project for this
 *   function to need to reject separately.
 *
 *   WHAT "DELETE" MEANS HERE — two separate on-disk changes, done in
 *   this specific order (see fat_vfs.c's implementation-level doc
 *   comment for the full "why this order" reasoning):
 *     1. Every cluster in the file's FAT-table chain is set back to
 *        FATxx_CLUSTER_FREE (0) — the exact reverse of what
 *        fat_alloc_cluster() (Day 23) does when a cluster is claimed —
 *        by walking the chain with the same DAY19 fat_next_cluster_fn
 *        trampolines fat_read()/fat_find_free_cluster() already use,
 *        and clearing each one with the same DAY23 fat_set_cluster_fn
 *        trampolines fat_alloc_cluster()/fat_write() already use. A
 *        file whose start_cluster is 0 (an empty, never-written
 *        `create`d-but-not-`write`d file) has nothing to free — this
 *        step is skipped entirely in that case.
 *     2. The file's directory entry itself is marked deleted by
 *        setting DIR_Name[0] to FAT_DIRENT_FREE (0xE5) — fat_delete_
 *        entry_fixed_root() (FAT12/16, kernel/fs/fat/fat_common.c) or
 *        fat32_delete_entry_root() (FAT32, kernel/fs/fat/fat32.c),
 *        Day 26's other two new functions. This single byte is
 *        immediately sufficient: fat_readdir_fixed_root()'s (and
 *        FAT32's directory-walk equivalent's) own scan already treats
 *        FAT_DIRENT_FREE as "skip, keep scanning" — the same skip rule
 *        fat_create_entry_fixed_root() (Day 24) reuses to find a
 *        reusable free SLOT — so a deleted entry is instantly and
 *        correctly invisible to a subsequent `ls`/fat_open()/
 *        fat_create_file() duplicate-check without this function
 *        needing to touch any other byte of the 32-byte entry.
 *
 *   Freeing a cluster chain makes that many clusters visible to a
 *   SUBSEQUENT fat_find_free_cluster()/fat_alloc_cluster() scan again
 *   — this is the mechanism behind this day's own acceptance text
 *   "且空闲簇数增加" (and the free-cluster count increases): nothing
 *   separately tracks or caches a volume-wide free-cluster count
 *   anywhere in this project (fat_find_free_cluster() has always
 *   scanned the live FAT table fresh every time it's called, Day 22) —
 *   the count "increasing" is simply a direct, automatic consequence
 *   of more clusters now genuinely reading back as 0 in the FAT table.
 *
 * Returns FAT_OK (0) on success. Returns one of fat_error_t's negative
 * values on failure:
 *
 *   FAT_ERR_INVALID_ARGS - `path` is NULL, malformed the same way
 *     fat_open() rejects it, or resolves to something other than a
 *     plain file, or names an fs->name this project doesn't recognize.
 *   FAT_ERR_NOT_FOUND - the mount-slot segment names no registered
 *     mount, or fat_open() on `path` fails to find an existing entry
 *     (forwarded from fat_open() itself, unchanged — same "don't
 *     reinvent fat_open()'s own diagnostic" choice fat_write() makes).
 *   FAT_ERR_IO - the underlying block_device_t has no `write` function
 *     (read-only backend), or a dev->read()/dev->write() call fails
 *     while walking/freeing the cluster chain or while marking the
 *     directory entry deleted.
 */
int fat_delete_file(const char *path);

/* fat_truncate(path)
 *
 * DAY27 (Week 4, Day 27 of the 5.0 filesystem roadmap):
 *   "实现文件截断（fat_truncate），支持清空文件内容。只保留目录项，释放
 *    所有数据簇。"
 *
 *   Same "<mount-slot>/<filename>" path syntax and "must already exist"
 *   contract as fat_write()/fat_delete_file() (Day 25/26) — `path` is
 *   resolved via fat_open() first, same "don't reinvent a diagnostic
 *   fat_open() already produces" choice every write-side fat_* entry
 *   point in this file makes.
 *
 *   WHAT "TRUNCATE" MEANS HERE, AND HOW IT DIFFERS FROM fat_delete_
 *   file(): both functions free the file's ENTIRE FAT-table cluster
 *   chain the exact same way (reusing fat_delete_file()'s own DAY26
 *   "walk with fat_next_cluster_fn BEFORE clearing with fat_set_
 *   cluster_fn, so clearing cluster N doesn't strand the clusters
 *   after it" ordering — see that function's header comment for the
 *   full rationale, unchanged here) — but where fat_delete_file()'s
 *   SECOND step marks the 32-byte directory entry itself deleted
 *   (DIR_Name[0] = 0xE5, invisible to any future `ls`/fat_open()),
 *   fat_truncate()'s second step instead REWRITES that same entry via
 *   the same fat_update_entry_fixed_root() (FAT12/16) / fat32_update_
 *   entry_root() (FAT32) calls fat_write() already uses — with start
 *   cluster 0 and size 0 — so the entry (and the filename) survives:
 *   `ls` still lists the file, `fat_open()` still finds it, but it now
 *   reads back as a fresh, empty, zero-cluster file, exactly mirroring
 *   fat_create_file()'s own "起始簇 0，大小 0" starting state (Day 24).
 *   This is the literal meaning of this day's own acceptance text
 *   "只保留目录项，释放所有数据簇" — keep only the directory entry,
 *   free every data cluster.
 *
 *   WHY A SEPARATE FUNCTION, NOT fat_write(path, NULL, 0): fat_write()
 *   already has a `size == 0` branch (Day 25) that rewrites the
 *   directory entry to start-cluster-0/size-0 exactly like this
 *   function's own step 2 — but, per fat_write()'s own doc comment,
 *   that branch deliberately does NOT free the file's old chain (that
 *   was explicitly this day's job, not Day 25's). Rather than teach
 *   fat_write()'s zero-size branch a second, different behavior based
 *   on some new flag, fat_truncate() is its own small entry point that
 *   does the freeing fat_write()'s zero-size path always skipped, and
 *   fat_write() (see fat_vfs.c's own implementation-level comment on
 *   its DAY27 update) now calls this SAME chain-freeing helper itself
 *   before starting any fresh write to an already-nonzero file — this
 *   function and fat_write()'s own leak-fix share one static helper
 *   rather than duplicating the walk-and-clear loop a third time
 *   (fat_delete_file() has the first copy).
 *
 * Returns FAT_OK (0) on success. Returns one of fat_error_t's negative
 * values on failure, with the identical meaning fat_delete_file()'s own
 * doc comment already gives each code (this function shares that
 * function's own fat_open()-forwarding and per-variant-dispatch
 * discipline exactly):
 *
 *   FAT_ERR_INVALID_ARGS - `path` is NULL, malformed the same way
 *     fat_open() rejects it, resolves to something other than a plain
 *     file, or names an fs->name this project doesn't recognize.
 *   FAT_ERR_NOT_FOUND - the mount-slot segment names no registered
 *     mount, or fat_open() on `path` fails to find an existing entry.
 *   FAT_ERR_IO - the underlying block_device_t has no `write` function
 *     (read-only backend), or a dev->read()/dev->write() call fails
 *     while freeing the cluster chain or while rewriting the directory
 *     entry.
 */
int fat_truncate(const char *path);

/* DAY68: 容量与空闲簇（把整张 FAT 数一遍，见 fat_vfs.c 里 fat_vfs_statfs() 的说明）。
 * 返回 FAT_OK / FAT_ERR_INVALID_ARGS / FAT_ERR_IO。 */
int fat_vfs_statfs(filesystem_t *fs, uint32_t *total_clusters, uint32_t *free_clusters,
                   uint32_t *cluster_bytes);

/* DAY65: fat_read() 的统计，给 `fatbench` 用（定义在 fat_vfs.c）。 */
extern uint32_t g_fat_read_dev_calls;     /* 数据区读命令条数 */
extern uint32_t g_fat_read_chain_steps;   /* 沿 FAT 链走的步数（next_cluster 调用次数） */
extern uint32_t g_fat_cursor_hits;        /* 游标命中次数 */

#endif /* LUMA_FAT_VFS_H */