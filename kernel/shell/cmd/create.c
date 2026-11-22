/* kernel/shell/cmd/create.c
 * LumaOS 5.0 - Shell command: create
 *
 * DAY24 (Week 4, Day 24 of the 5.0 filesystem roadmap):
 *   "实现 fat_create_file(name)：在根目录区找一个空条目（首字节 0x00
 *    或 0xE5），填入文件名、起始簇 0、大小 0。在 Shell 中输入 create
 *    test.txt，重新 ls 能看到它。"
 *
 *   This is the Shell-visible exerciser of Day 24's real deliverable,
 *   fat_create_file() (kernel/fs/fat/fat_vfs.c) — the exact same
 *   relationship cmd_ls (Day 14) has to vfs_readdir(), and cmd_show
 *   (Day 19/21) has to fat_open()/fat_read(): the FS-layer function is
 *   where the actual work happens, this file only wraps it as an
 *   interactive command and renders the result through terminal_puts()
 *   instead of the boot-time console's printf().
 *
 * PATH SYNTAX, MATCHING cmd_show's OWN CONVENTION EXACTLY:
 *   "<mount-slot>/<filename>", e.g. `create IDE0/TEST.TXT` — see
 *   fat_vfs.h's doc comment on fat_create_file() for the full
 *   rationale (same mandatory-mount-slot parsing fat_open() already
 *   established Day 18, now shared by every path-taking command). The
 *   roadmap's own acceptance text writes the example as a bare
 *   `create test.txt` (no mount-slot) — but this project has had a
 *   numbered mount table (IDE0/IDE1/IDE2, all three simultaneously
 *   mounted since Day 22/23's own self-tests) since Day 18-FOLLOWUP,
 *   and `show`/`fat_open()` have required an explicit mount-slot ever
 *   since, so `create` follows that same established convention rather
 *   than reintroducing an implicit "whichever volume mounted last"
 *   fallback `show` deliberately does NOT have. See this file's
 *   DAY24-FOLLOWUP note below for the one place this command's
 *   argument handling goes further than the roadmap's literal text for
 *   exactly this reason.
 *
 * DAY24-FOLLOWUP (this file's own small addition, not asked for by
 * 计划.md's Day 24 text itself): if the user really does type a bare
 * filename with no mount-slot segment at all (matching the roadmap's
 * own literal `create test.txt` example), this command defaults it
 * onto vfs_root's own mount slot (via shell_resolve_path(), kernel/
 * shell/shell.c — DAY25 lifted this command's own original inline loop
 * out into that shared helper once `write`/`read` needed the identical
 * fallback, see shell.h's doc comment on shell_resolve_path() for the
 * full history) rather than flatly rejecting it as a malformed path.
 * This is purely a Shell-side convenience — fat_create_file() itself
 * still requires the full "<mount-slot>/<filename>" syntax unchanged,
 * shell_resolve_path() just fills in the missing segment before this
 * command calls it, so `show`'s own stricter behavior (no such
 * fallback) is untouched.
 */
#include "../../core/kernel.h"
#include "../../gui/gui.h"
#include "../../fs/vfs/vfs.h"
#include "../../fs/fat/fat_vfs.h"
#include "../../fs/lmfs/lmfs_vfs.h"   /* DAY38: lmfs_vfs_create() */
#include "../../lib/string.h"         /* DAY38: strchr/memcpy/strcmp for the
                                         mount-slot dispatch below */
#include "../shell.h"

/* DAY38 (Week 6, Day 38 of the 5.0 filesystem roadmap):
 *   "实现 create() 与目录项。在 Shell 中输入 create test.txt，写入
 *    内容，重新 ls 能看到它。"
 *
 *   Before today, this command only ever called fat_create_file() —
 *   there was no other filesystem capable of creating anything.
 *   fat_create_file() itself already refuses to touch a non-FAT mount
 *   (it returns FAT_ERR_INVALID_ARGS the instant fs->name isn't
 *   "fat12"/"fat16"/"fat32" — see its own doc comment), which is
 *   exactly the "invalid arguments" error an LMFS mount (fs->name ==
 *   "lmfs") always got back before today, however correctly-formed the
 *   path was (LMFS not having a create() of its own yet). Day 38 gives
 *   LMFS that create() (lmfs_create(), kernel/fs/lmfs/lmfs.c, wrapped
 *   for path syntax as lmfs_vfs_create(), kernel/fs/lmfs/lmfs_vfs.c —
 *   the LMFS-side counterpart to fat_create_file() itself), so this
 *   command now has to decide WHICH of the two to call rather than
 *   always reaching straight for fat_create_file() the way it always
 *   has. That decision is made here, by the same "<mount-slot>/..."
 *   split both fat_create_file() and lmfs_vfs_create() redo internally
 *   anyway — this function doesn't reach into either filesystem's
 *   internals, it only reads fs->name back from vfs_lookup_mount() to
 *   pick which already-self-contained create function to call.
 */
void cmd_create(terminal_t *t, int argc, char **argv)
{
    if (argc < 2) {
        terminal_puts(t, "usage: create <mount-slot>/<filename>  "
                          "(e.g. create IDE0/TEST.TXT)\n", TERM_COL_ERROR);
        return;
    }

    /* DAY25: shell_resolve_path() (kernel/shell/shell.c) now does what
     * this function's own inline loop used to do — see this file's
     * updated DAY24-FOLLOWUP note above. A path buffer of MOUNT-SLOT
     * ("IDE" + up to 4 digits) + '/' + up to a 12-byte 8.3 filename
     * comfortably fits SHELL_ARG_LEN (40, see shell.h) twice over. */
    static char path_buf[64];
    const char *path;

    if (shell_resolve_path(argv[1], path_buf, sizeof(path_buf)) != 0) {
        terminal_puts(t, "create: no mount-slot given and no "
                          "current filesystem mounted\n", TERM_COL_ERROR);
        return;
    }
    path = path_buf;

    /* DAY38: pull the mount-slot segment back out of the already-
     * resolved path (same "<mount-slot>/..." shape fat_create_file()/
     * lmfs_vfs_create() each parse again themselves) purely to read
     * back fs->name and decide which of the two to call — this command
     * never touches fs->fs_private itself. */
    const char *sep = strchr(path, '/');
    filesystem_t *fs = (filesystem_t *)0;
    if (sep != (char *)0) {
        char mount_name[8];
        uint32_t len = (uint32_t)(sep - path);
        if (len > 0 && len < sizeof(mount_name)) {
            memcpy(mount_name, path, len);
            mount_name[len] = '\0';
            fs = vfs_lookup_mount(mount_name);
        }
    }

    if (fs && fs->name && strcmp(fs->name, "lmfs") == 0) {
        /* DAY38: lmfs_create() (kernel/fs/lmfs/lmfs.c) does the real
         * work — scans the root directory's existing data blocks for a
         * reusable free lmfs_dirent_t slot (allocating one more data
         * block for the root directory first if all existing ones are
         * full), allocates a fresh inode, and links the two together.
         * See that function's own doc comment (lmfs.h) for the full
         * contract. lmfs_vfs_create() only has a flat 0/-1 result (no
         * fat_strerror()-style per-cause breakdown exists for LMFS yet,
         * see lmfs_vfs.h's own doc comment on why), hence the single
         * generic message below instead of the FAT branch's
         * fat_strerror(rc) detail. */
        if (lmfs_vfs_create(path) != 0) {
            terminal_puts(t, "create: cannot create '", TERM_COL_ERROR);
            terminal_puts(t, path, TERM_COL_ERROR);
            terminal_puts(t, "' (invalid arguments, duplicate name, or "
                              "directory full)\n", TERM_COL_ERROR);
            return;
        }
    } else {
        /* DAY24: fat_create_file() (kernel/fs/fat/fat_vfs.c) does the
         * real work — scans the target volume's root directory for a
         * reusable free slot (DIR_Name[0] == 0x00 or 0xE5), rejects a
         * duplicate name via its own internal fat_open() check, and
         * writes a new entry with start cluster 0 / size 0 if the coast
         * is clear. See that function's own doc comment (fat_vfs.h) for
         * exactly which fat_error_t code each failure case returns.
         * This is also where a mount-slot that doesn't exist at all
         * (fs above came back NULL) still ends up — fat_create_file()
         * itself reports that case via FAT_ERR_NOT_FOUND. */
        int rc = fat_create_file(path);
        if (rc != FAT_OK) {
            terminal_puts(t, "create: cannot create '", TERM_COL_ERROR);
            terminal_puts(t, path, TERM_COL_ERROR);
            terminal_puts(t, "' (", TERM_COL_ERROR);
            terminal_puts(t, fat_strerror(rc), TERM_COL_ERROR);
            terminal_puts(t, ")\n", TERM_COL_ERROR);
            return;
        }
    }

    terminal_puts(t, "created ", TERM_COL_SUCCESS);
    terminal_puts(t, path, TERM_COL_SUCCESS);
    terminal_newline(t);
}