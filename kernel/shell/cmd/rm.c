/* kernel/shell/cmd/rm.c
 * LumaOS 5.0 - Shell command: rm
 *
 * DAY26 (Week 4, Day 26 of the 5.0 filesystem roadmap):
 *   "实现 fat_delete_file(name)：找到目录项，标记为已删除（首字节设为
 *    0xE5），释放其占用的簇链（在 FAT 表中标记为空闲）。Shell 中输入
 *    rm test.txt，ls 结果中该文件消失，且空闲簇数增加。"
 *
 *   This is the Shell-visible exerciser of Day 26's real deliverable,
 *   fat_delete_file() (kernel/fs/fat/fat_vfs.c) — the exact same
 *   relationship cmd_create (Day 24) has to fat_create_file() and
 *   cmd_write (Day 25) has to fat_write(): the FS-layer function does
 *   the actual work, this file only wraps it as an interactive command
 *   and renders the result through terminal_puts().
 *
 * PATH SYNTAX, MATCHING cmd_create/cmd_write's OWN CONVENTION EXACTLY:
 *   "<mount-slot>/<filename>", e.g. `rm IDE0/TEST.TXT` — resolved via
 *   shell_resolve_path() (kernel/shell/shell.c, DAY25), the same
 *   "default onto vfs_root's own mount slot if the user types a bare
 *   filename" convenience `create`/`write`/`read` already share, so
 *   the roadmap's own bare `rm test.txt` example works against
 *   whichever volume `cd` last switched vfs_root to.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../../fs/lmfs/lmfs_vfs.h"   /* DAY41: lmfs_vfs_delete() */
#include "../../lib/string.h"         /* DAY41: strchr/memcpy/strcmp for the
                                         mount-slot dispatch below */
#include "../shell.h"

/* DAY41 (Week 6, Day 41 of the 5.0 filesystem roadmap):
 *   "实现 delete() 与空间回收。" 验收："rm 后空间恢复。"
 *
 *   Same shape as create.c's/read.c's own DAY38/40 dispatch — see
 *   either of those files' own comment for the full reasoning.
 *   fat_delete_file() stays FAT-only (rejects any fs->name it doesn't
 *   recognize); lmfs_vfs_delete() (kernel/fs/lmfs/lmfs_vfs.c) is the
 *   LMFS counterpart, wired in here.
 */
void cmd_rm(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: rm <mount-slot>/<filename>  "
                          "(e.g. rm IDE0/TEST.TXT)\n", TERM_COL_ERROR);
        return;
    }

    /* Same path buffer sizing rationale as cmd_create's own comment:
     * MOUNT-SLOT ("IDE" + up to 4 digits) + '/' + up to a 12-byte 8.3
     * filename comfortably fits SHELL_ARG_LEN (40, see shell.h) twice
     * over. */
    static char path_buf[64];
    const char *path;

    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "rm: no mount-slot given and no "
                          "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }
    path = path_buf;

    /* DAY41: same "<mount-slot>/..." peek create.c/read.c's own
     * dispatch already uses — read back fs->name to decide whether
     * fat_delete_file() or lmfs_vfs_delete() should do the work. */
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
        /* DAY41: lmfs_delete() (kernel/fs/lmfs/lmfs.c) does the real
         * work — see that function's own doc comment (lmfs.h) for the
         * full contract, including why it clears the directory entry
         * BEFORE freeing blocks/inode (the opposite order from
         * fat_delete_file(), for a reason specific to how LMFS's
         * on-disk structures differ from FAT's). Flat 0/-1 result,
         * same reasoning as create.c's/read.c's own DAY38/40 comments
         * already gave for why LMFS doesn't have a fat_strerror()-style
         * per-cause breakdown yet. */
        if (lmfs_vfs_delete(path) != 0) {
            terminal_puts(t, "rm: cannot remove '", TERM_COL_ERROR);
            terminal_puts(t, path, TERM_COL_ERROR);
            terminal_puts(t, "' (not found, or not a plain file)\n",
                          TERM_COL_ERROR);
            return;
        }
    } else {
        /* DAY26: fat_delete_file() (kernel/fs/fat/fat_vfs.c) does the
         * real work — frees the file's FAT-table cluster chain (if it
         * has one), then marks its directory entry deleted (DIR_Name[0]
         * = 0xE5). See that function's own doc comment (fat_vfs.h) for
         * exactly which fat_error_t code each failure case returns —
         * in particular, FAT_ERR_NOT_FOUND here means `path` doesn't
         * currently exist (forwarded from fat_delete_file()'s own
         * internal fat_open() check), the same diagnostic
         * `show`/`write` already surface for a missing file. */
        int rc = fat_delete_file(path);
        if (rc != FAT_OK) {
            terminal_puts(t, "rm: cannot remove '", TERM_COL_ERROR);
            terminal_puts(t, path, TERM_COL_ERROR);
            terminal_puts(t, "' (", TERM_COL_ERROR);
            terminal_puts(t, fat_strerror(rc), TERM_COL_ERROR);
            terminal_puts(t, ")\n", TERM_COL_ERROR);
            return;
        }
    }

    terminal_puts(t, "removed ", TERM_COL_SUCCESS);
    terminal_puts(t, path, TERM_COL_SUCCESS);
    terminal_newline(t);
}