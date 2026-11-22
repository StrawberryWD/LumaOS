/* kernel/shell/cmd/mkdir.c
 * LumaOS 5.0 - Shell command: mkdir
 *
 * DAY42 (Week 6, Day 42 of the 5.0 filesystem roadmap):
 *   "多级目录与路径解析。" 验收："可以创建并访问多级目录。"
 *
 *   The Shell-visible exerciser of Day 42's lmfs_mkdir()
 *   (kernel/fs/lmfs/lmfs.c) — same relationship cmd_create (Day 24/38)
 *   has to fat_create_file()/lmfs_create(): the FS layer does the real
 *   work, this file wraps it as an interactive command.
 *
 * WHY NO FAT BRANCH (unlike create.c/rm.c/read.c): this project's FAT
 * code is still root-directory-only (fat_open()'s own "no second '/'"
 * limit, see cd.c's DAY25 header) and the 91-day roadmap never asks
 * for FAT subdirectories — multi-level directories are an LMFS feature
 * (LMFS is the filesystem LumaOS itself will boot and install onto).
 * So `mkdir` on a FAT volume prints a clear refusal instead of
 * pretending.
 *
 * PATH SYNTAX: anything shell_resolve_path() (shell.c, DAY42) accepts —
 *   mkdir docs               relative to the current directory
 *   mkdir docs/notes         parent "docs" must already exist
 *   mkdir IDE3/docs/notes    fully qualified
 *   mkdir /docs              absolute within the current volume
 * No `-p`: a missing parent is an error, not silently created (that
 * would make a typo like `mkdir dcos/notes` create two wrong directories).
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/lmfs/lmfs_vfs.h"
#include "../../lib/string.h"
#include "../shell.h"

void cmd_mkdir(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: mkdir <path>  "
                          "(e.g. mkdir docs, mkdir IDE3/docs/notes)\n",
                      TERM_COL_ERROR);
        return;
    }

    static char path_buf[64];
    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "mkdir: bad path (no current filesystem mounted, "
                          "or path too long)\n", TERM_COL_ERROR);
        return;
    }

    filesystem_t *fs = (filesystem_t *)0;
    const char *sub = "";
    if (shell_split_path(path_buf, &fs, &sub) != 0) {
        terminal_puts(t, "mkdir: no such mounted volume in '", TERM_COL_ERROR);
        terminal_puts(t, path_buf, TERM_COL_ERROR);
        terminal_puts(t, "'\n", TERM_COL_ERROR);
        return;
    }

    if (!fs->name || strcmp(fs->name, "lmfs") != 0) {
        terminal_puts(t, "mkdir: this volume (", TERM_COL_ERROR);
        terminal_puts(t, fs->name ? fs->name : "?", TERM_COL_ERROR);
        terminal_puts(t, ") has no subdirectory support - "
                          "only LMFS volumes do\n", TERM_COL_ERROR);
        return;
    }

    if (sub[0] == '\0') {
        terminal_puts(t, "mkdir: '", TERM_COL_ERROR);
        terminal_puts(t, path_buf, TERM_COL_ERROR);
        terminal_puts(t, "' is the volume root, which already exists\n",
                      TERM_COL_ERROR);
        return;
    }

    if (lmfs_vfs_mkdir(path_buf) != 0) {
        terminal_puts(t, "mkdir: cannot create directory '", TERM_COL_ERROR);
        terminal_puts(t, path_buf, TERM_COL_ERROR);
        terminal_puts(t, "' (parent missing, name exists, invalid name, "
                          "or directory full)\n", TERM_COL_ERROR);
        return;
    }

    terminal_puts(t, "created directory ", TERM_COL_SUCCESS);
    terminal_puts(t, path_buf, TERM_COL_SUCCESS);
    terminal_newline(t);
}
