/* kernel/shell/cmd/ls.c
 * LumaOS 5.0 - Shell command: ls
 *
 * DAY14 (Week 2, Day 14 of the 5.0 filesystem roadmap):
 *   "整合 Shell：新增命令 cmd_ls（调用 vfs_readdir 列出文件）。在终端
 *    输入 ls，能看到 FAT 镜像里的所有文件。"
 *
 *   This is the payoff of Day 8's vfs.h shapes, Day 13's vfs_mount()/
 *   fat12_vfs_mount() wiring, and every FAT12 Day before it: `ls` is
 *   the FIRST piece of shell-visible, user-typed code in this project
 *   that walks the filesystem exclusively through the generic VFS
 *   layer (vfs_root->fs->readdir()) — never fat12_readdir() or any
 *   other backend-specific call by name. kmain.c's own DAY13 test
 *   section already proved this exact call sequence works before this
 *   file existed; cmd_ls only wraps it as an interactive command and
 *   renders the result through terminal_puts() instead of printf().
 *
 * WHY THIS DOESN'T CALL fat12_readdir() DIRECTLY:
 *   The whole point of Day 8/13's abstraction is that shell commands
 *   should not need to know which filesystem is mounted. Once FAT16/
 *   32 (Week 3) or LMFS (Month 2) exist, `ls` keeps working unchanged
 *   as long as whatever is mounted fills in fs->readdir correctly —
 *   see vfs.h's own file header comment ("Shell commands ... are
 *   meant to walk struct vfs_node trees and call through a struct
 *   filesystem's function pointers — never a driver-specific function
 *   by name").
 *
 * WHAT "ls" ACTUALLY LISTS TODAY:
 *   vfs_root is whatever kmain()'s own vfs_mount("IDE0") call resolved
 *   at boot (see kernel/fs/vfs/vfs.c's header comment) — on this
 *   project's stock disk image that is the Day 11 FAT12 test volume
 *   embedded at LBA 5000 (tests/fat12-with-file.img), so `ls` is
 *   expected to print BIGFILE.BIN and SMALL.BIN. It is NOT
 *   mbr.bin/loader.bin/kernel.bin — those are raw, fixed-LBA files
 *   dd'd straight onto the disk outside any filesystem, unrelated to
 *   whatever FAT12 volume gets mounted (see 计划.md's Day 12 recap
 *   note, reproduced in kmain.c/vfs.c's comments, for the full
 *   reasoning on why that's expected and correct, not a bug).
 *
 * WHY A NULL vfs_root (OR A READDIR FAILURE) DOESN'T CRASH:
 *   vfs_mount("IDE0") can fail (kmain.c's own DAY13 section already
 *   handles that case by testing its return value before touching
 *   vfs_root) — if the user somehow reaches a shell prompt with no
 *   filesystem mounted, `ls` prints a clear error through the SAME
 *   terminal_t the rest of the shell already uses instead of
 *   dereferencing a NULL vfs_root or a NULL vfs_root->fs->readdir.
 *
 * DAY42 (Week 6, Day 42 of the 5.0 filesystem roadmap):
 *   "多级目录与路径解析。" Two changes to this command:
 *
 *   1. `ls` lists the CURRENT DIRECTORY, not just vfs_root, and takes
 *      an optional path: `ls`, `ls docs`, `ls docs/notes`, `ls ..`,
 *      `ls IDE3/docs`. The path goes through shell_resolve_path() (so it
 *      understands relative paths, ".", "..", mount slots), and for a
 *      directory below the volume root the node to hand to
 *      fs->readdir() comes from lmfs_vfs_open_path() — it resolves the
 *      multi-level path and returns a VFS_TYPE_DIR node whose
 *      fs_private is that directory's inode, which is all
 *      lmfs_vfs_readdir() needs (its Day 35 comment already said so:
 *      "子目录...读法完全一样，不需要改这个函数").
 *
 *   2. The "." and ".." entries are no longer printed. They exist on
 *      disk (LMFS directories carry them as real entries so path
 *      resolution can walk ".."), but a user listing a directory does
 *      not want to see them — `ls` used to open with "./" and "../"
 *      lines. The filter is purely cosmetic and lives here, in the
 *      display layer: lmfs_readdir()/lmfs_vfs_readdir() still return
 *      them (Day 34's test and the resolver depend on that).
 *      Consequence: an empty LMFS directory now prints "(empty)".
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/lmfs/lmfs_vfs.h"   /* DAY42: lmfs_vfs_open_path() */
#include "../../lib/string.h"         /* DAY42: strcmp() */
#include "../shell.h"

void cmd_ls(terminal_t *t, int argc, char **argv)
{
    /* DAY42: which directory? Resolve the optional argument (default
     * ".", i.e. the current directory) into "<slot>/<normalized>". */
    static char path_buf[64];
    vfs_node_t  dir_node;
    vfs_node_t *dir = (vfs_node_t *)0;

    if (argc < 2 && !vfs_root) {
        terminal_puts(t, "ls: no filesystem mounted (vfs_root is NULL)\n",
                      TERM_COL_ERROR);
        return;
    }

    if (shell_resolve_path(argc >= 2 ? argv[1] : ".", path_buf,
                           sizeof(path_buf)) != 0) {
        terminal_puts(t, "ls: bad path (no current filesystem mounted, "
                          "or path too long)\n", TERM_COL_ERROR);
        return;
    }

    filesystem_t *fs = (filesystem_t *)0;
    const char *sub = "";
    if (shell_split_path(path_buf, &fs, &sub) != 0 || !fs) {
        terminal_puts(t, "ls: no such mounted volume in '", TERM_COL_ERROR);
        terminal_puts(t, path_buf, TERM_COL_ERROR);
        terminal_puts(t, "'\n", TERM_COL_ERROR);
        return;
    }

    if (sub[0] == '\0') {
        /* The volume root. If it is the volume we are already in, use
         * vfs_root itself exactly as every day before 42 did; otherwise
         * (`ls IDE1` from IDE3) that volume's own root node. */
        dir = (vfs_root && vfs_root->fs == fs) ? vfs_root : fs->root;
        if (!dir) {
            terminal_puts(t, "ls: volume has no root node\n", TERM_COL_ERROR);
            return;
        }
    } else {
        if (!vfs_fs_has_subdirs(fs)) {
            terminal_puts(t, "ls: cannot access '", TERM_COL_ERROR);
            terminal_puts(t, path_buf, TERM_COL_ERROR);
            terminal_puts(t, "': no such directory (this volume has no "
                              "subdirectories)\n", TERM_COL_ERROR);
            return;
        }
        if (vfs_fs_open_path(fs, path_buf, &dir_node) != 0) {
            terminal_puts(t, "ls: cannot access '", TERM_COL_ERROR);
            terminal_puts(t, path_buf, TERM_COL_ERROR);
            terminal_puts(t, "': no such file or directory\n", TERM_COL_ERROR);
            return;
        }
        if (dir_node.type != VFS_TYPE_DIR) {
            /* `ls file.txt`: not an error in a real `ls` — it lists the
             * one file. Same line format as a directory listing row. */
            char numbuf[16];
            sprintf(numbuf, "%u", dir_node.size);
            terminal_puts(t, dir_node.name, TERM_COL_CMD);
            for (int s = (int)strlen(dir_node.name); s < 16; s++)
                terminal_putchar(t, ' ', TERM_COL_DEFAULT);
            terminal_puts(t, numbuf, TERM_COL_DEFAULT);
            terminal_puts(t, " bytes\n", TERM_COL_DEFAULT);
            return;
        }
        dir = &dir_node;
    }

    if (!dir->fs || !dir->fs->readdir) {
        terminal_puts(t, "ls: mounted filesystem has no readdir op\n",
                      TERM_COL_ERROR);
        return;
    }

    vfs_node_t child;
    uint32_t   idx  = 0;
    int        any  = 0;

    while (dir->fs->readdir(dir, idx, &child) == 0) {
        idx++;

        /* DAY42: hide "." and ".." — see this file's header comment. */
        if (strcmp(child.name, ".") == 0 || strcmp(child.name, "..") == 0)
            continue;

        /* Directories get a trailing "/" for display only — the name
         * is stored slash-free (see vfs.h's vfs_node_t.name comment),
         * this is purely cosmetic, same rule kmain.c's DAY13 section
         * already applies when printing vfs_root itself. */
        terminal_puts(t, child.name, TERM_COL_CMD);
        if (child.type == VFS_TYPE_DIR)
            terminal_puts(t, "/", TERM_COL_CMD);

        /* Pad out to a fixed column so the size lines up, same spirit
         * as cmd_help.c's name/description padding. */
        {
            int len = (int)strlen(child.name) + (child.type == VFS_TYPE_DIR ? 1 : 0);
            int pad = 16 - len;
            for (int s = 0; s < pad; s++)
                terminal_putchar(t, ' ', TERM_COL_DEFAULT);
        }

        if (child.type != VFS_TYPE_DIR) {
            char numbuf[16];
            sprintf(numbuf, "%u", child.size);
            terminal_puts(t, numbuf, TERM_COL_DEFAULT);
            terminal_puts(t, " bytes", TERM_COL_DEFAULT);
        }
        terminal_newline(t);

        any = 1;
    }

    if (!any) {
        terminal_puts(t, "(empty)\n", TERM_COL_INFO);
    }
}
