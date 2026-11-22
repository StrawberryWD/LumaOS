/* kernel/shell/cmd/read.c
 * LumaOS 5.0 - Shell command: read
 *
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "在 Shell 中输入 write test.txt，写入内容，show 能看到" — but this
 *   day's own acceptance TRANSCRIPT (not the summary line quoted above)
 *   actually types `read 1.txt` / `read 2.txt` against files just
 *   `write`ten, not `show IDE0/1.TXT`. So Day 25 introduced a THIRD
 *   name for "print a file's contents", alongside Day 19's `cat` (Day
 *   21 renamed it `show`) — not a rename at the time, but a distinct
 *   command registered ALONGSIDE `show`: the same underlying read logic
 *   `show` already had, plus `create`'s own bare-filename fallback
 *   layered on top (`show` itself required an explicit mount-slot,
 *   unchanged).
 *
 * DAY27 ("删除那个show只保留read" — remove `show`, keep only `read`):
 * `show` is gone (kernel/shell/shell.c's own DAY27 comment) — this file
 * is now the ONLY "print a file's contents" command left. Its own
 * read-loop (why terminal_putchar() not terminal_puts(), why chunked
 * non-cluster-aligned reads, why fat_close() at the end — see cmd_read()
 * below) is unchanged from Day 25; only the "why a separate file from
 * show.c" history above is no longer live. See create.c's/shell.h's
 * shell_resolve_path() doc comment for the bare-filename-defaults-onto-
 * vfs_root fallback reused here.
 *
 * DAY40 (Week 6, Day 40 of the 5.0 filesystem roadmap):
 *   "实现 read()。" 验收："read bigfile.txt 正常。"
 *
 *   Only the OPENING step below is filesystem-specific (fat_open() is
 *   a FAT-only function, exactly like fat_create_file() was for
 *   `create` through Day 37 — see create.c's own DAY38 comment for that
 *   same shape). Everything from `node.fs->open()` onward already was,
 *   and remains, generic VFS (fs->open/fs->read/fs->close function
 *   pointers) — this is why Day 40's LMFS work only had to touch this
 *   one spot in this file: lmfs_vfs_open_path() (kernel/fs/lmfs/
 *   lmfs_vfs.c) is called instead of fat_open() when the target mount
 *   is an LMFS volume, and the entire read-loop below runs completely
 *   unmodified against whichever filesystem opened the node — it was
 *   already written generically enough to not need to know or care.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../../fs/lmfs/lmfs_vfs.h"   /* DAY40: lmfs_vfs_open_path() */
#include "../../lib/string.h"         /* DAY40: strchr/memcpy/strcmp for the
                                         mount-slot dispatch below */
#include "../shell.h"

#define READ_CHUNK_BYTES  256

void cmd_read(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: read <filename>  (e.g. read test.txt, "
                          "or read IDE0/TEST.TXT)\n", TERM_COL_ERROR);
        return;
    }

    /* DAY25: shell_resolve_path() (kernel/shell/shell.c) — same
     * bare-filename-onto-vfs_root fallback `create` established Day 24.
     * `read` accepts a bare filename with no mount-slot at all (the
     * now-removed `show` required an explicit one). */
    static char path_buf[64];
    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "read: no mount-slot given and no "
                          "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }
    const char *path = path_buf;

    /* DAY40: same "<mount-slot>/..." peek used by create.c's own
     * DAY38 dispatch — read back fs->name to decide whether the
     * FAT-only fat_open() or the LMFS-only lmfs_vfs_open_path()
     * should do the actual opening. */
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

    vfs_node_t node;
    if (vfs_fs_has_subdirs(target_fs)) {
        /* DAY40: lmfs_vfs_open_path() (kernel/fs/lmfs/lmfs_vfs.c) does
         * the real work — see that function's own doc comment
         * (lmfs_vfs.h) for the full contract. It only has a flat 0/-1
         * result (no fat_strerror()-style per-cause breakdown exists
         * for LMFS yet — same reasoning create.c's own DAY38 comment
         * already gave for lmfs_vfs_create()), hence the single
         * generic diagnostic below instead of the FAT branch's
         * fat_strerror(open_err) detail. */
        if (vfs_fs_open_path(target_fs, path, &node) != 0) {
            terminal_puts(t, "read: cannot open '", TERM_COL_ERROR);
            terminal_puts(t, path, TERM_COL_ERROR);
            terminal_puts(t, "' (not found)\n", TERM_COL_ERROR);
            return;
        }
    } else {
        int open_err = fat_open(path, &node);
        if (open_err != FAT_OK) {
            terminal_puts(t, "read: cannot open '", TERM_COL_ERROR);
            terminal_puts(t, path, TERM_COL_ERROR);
            terminal_puts(t, "' (", TERM_COL_ERROR);
            terminal_puts(t, fat_strerror(open_err), TERM_COL_ERROR);
            terminal_puts(t, ")\n", TERM_COL_ERROR);
            return;
        }
    }

    if (node.type != VFS_TYPE_FILE) {
        terminal_puts(t, "read: '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "' is a directory\n", TERM_COL_ERROR);
        return;
    }

    if (!node.fs || !node.fs->open) {
        terminal_puts(t, "read: mounted filesystem has no open op\n",
                      TERM_COL_ERROR);
        return;
    }

    file_t file;
    if (node.fs->open(&node, &file) != 0) {
        terminal_puts(t, "read: open failed for '", TERM_COL_ERROR);
        terminal_puts(t, path, TERM_COL_ERROR);
        terminal_puts(t, "'\n", TERM_COL_ERROR);
        return;
    }

    terminal_puts(t, "-- ", TERM_COL_INFO);
    terminal_puts(t, path, TERM_COL_INFO);
    {
        char numbuf[16];
        sprintf(numbuf, "%u", node.size);
        terminal_puts(t, " (", TERM_COL_INFO);
        terminal_puts(t, numbuf, TERM_COL_INFO);
        terminal_puts(t, " bytes) --", TERM_COL_INFO);
    }
    terminal_newline(t);

    /* Same 0-byte-file (start cluster 0) special case the now-removed
     * `show` command's own DAY24-FOLLOWUP note documented ("nothing to
     * read" is a read-level EOF-shaped outcome, not a change to
     * fat_read()'s own contract). */
    if (node.size == 0) {
        terminal_puts(t, "-- read 0 bytes (whole file) --\n", TERM_COL_INFO);
        if (node.fs->close)
            node.fs->close(&file);
        return;
    }

    if (!node.fs->read) {
        terminal_puts(t, "read: mounted filesystem has no read op\n",
                      TERM_COL_ERROR);
        return;
    }

    static uint8_t buf[READ_CHUNK_BYTES];
    uint32_t total_read = 0;
    int read_err = FAT_OK;

    for (;;) {
        int n = node.fs->read(&file, buf, READ_CHUNK_BYTES);
        if (n < 0) {
            read_err = n;
            break;
        }
        if (n == 0)
            break;   /* EOF, per vfs_read_fn's "0 == EOF" contract */

        for (int i = 0; i < n; i++)
            terminal_putchar(t, (char)buf[i], TERM_COL_DEFAULT);

        total_read += (uint32_t)n;
    }
    terminal_newline(t);

    if (read_err != FAT_OK) {
        terminal_puts(t, "read: read error after ", TERM_COL_ERROR);
        {
            char numbuf[16];
            sprintf(numbuf, "%u", total_read);
            terminal_puts(t, numbuf, TERM_COL_ERROR);
        }
        terminal_puts(t, " bytes (", TERM_COL_ERROR);
        /* DAY40: fat_strerror() only knows FAT's own fat_error_t
         * codes — it has a safe "unknown error" fallback for anything
         * else (see its own switch default), so calling it
         * unconditionally here was never actually unsafe, but calling
         * a FAT-named function to describe an LMFS failure reads as
         * sloppy. lmfs_vfs_read()/lmfs_read() don't have their own
         * per-cause error codes yet (same "not worth a whole enum yet"
         * reasoning create.c's/lmfs_vfs.h's DAY38 comments already
         * gave), so the best available message for that case is a
         * flat, honest "I/O error" rather than routing through FAT's
         * table at all. */
        if (vfs_fs_has_subdirs(target_fs))
            terminal_puts(t, "I/O error", TERM_COL_ERROR);
        else
            terminal_puts(t, fat_strerror(read_err), TERM_COL_ERROR);
        terminal_puts(t, ")\n", TERM_COL_ERROR);
    } else {
        char numbuf[16];
        sprintf(numbuf, "%u", total_read);
        terminal_puts(t, "-- read ", TERM_COL_INFO);
        terminal_puts(t, numbuf, TERM_COL_INFO);
        if (total_read < node.size)
            terminal_puts(t, " bytes (truncated preview) --", TERM_COL_INFO);
        else
            terminal_puts(t, " bytes (whole file) --", TERM_COL_INFO);
        terminal_newline(t);
    }

    if (node.fs->close)
        node.fs->close(&file);
}