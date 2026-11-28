/* kernel/shell/shell.h - LumaOS 5.0 Shell Header */
#ifndef LUMA_SHELL_H
#define LUMA_SHELL_H

#include "../gui/gui.h"

/* DAY43 PRE-3: SHELL_MAX_CMDS 16 -> 24 (16 were all in use once `term`
 * was added); SHELL_MAX_ARGS 8 -> 16. The old limit of 8 argv slots (the
 * command itself + 7 words) is what made `echo` print only its first 7
 * words. echo no longer depends on argv at all (shell_cmdline_args()
 * below), but other commands benefit from the larger limit too. */
#define SHELL_MAX_CMDS 40   /* DAY62: 24 -> 40 (mounts, cdinfo + the Day 63 commands) */
#define SHELL_MAX_ARGS 16
#define SHELL_ARG_LEN  40

typedef void (*shell_cmd_fn_t)(terminal_t *t, int argc, char **argv);

typedef struct {
    const char     *name;
    const char     *description;
    shell_cmd_fn_t  fn;
} shell_cmd_t;

extern shell_cmd_t cmd_table[];
extern int         cmd_count;

void shell_init(void);
void shell_exec(terminal_t *t, const char *cmdline);
void shell_register(const char *name, const char *desc, shell_cmd_fn_t fn);

/* shell_cmdline_args() — DAY43 PRE-3
 *
 * While a command runs, returns the RAW text of its command line after
 * the command word and the blanks following it ("echo   a  b" ->
 * "a  b"), exactly as typed: no splitting, no 40-char/16-word limits, the
 * user's own spacing preserved. Valid only during the command's
 * execution (it points into the caller's input buffer). "" if none. */
const char *shell_cmdline_args(void);

/* shell_resolve_path(arg, out_buf, out_buf_size)
 *
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "支持 cd 命令：切换当前工作目录（在挂载点之间）。支持 show test.txt
 *    而不是 show IDE2/text.txt。"
 *
 *   cmd_create's own DAY24-FOLLOWUP note (kernel/shell/cmd/create.c)
 *   first added the "no '/' in the typed argument -> default onto
 *   vfs_root's own mount slot" fallback, one command at a time. Day 25
 *   needs the SAME fallback in `write` and `read` (see this project's
 *   own Day 25 acceptance text: "支持 show test.txt" — and `read` is
 *   this day's replacement name for that same lookup, per the Shell
 *   commands list below) — a third and fourth copy of the identical
 *   loop would be exactly the kind of duplication this header's own
 *   shell_cmd_fn_t abstraction already exists to avoid at the command-
 *   dispatch layer; this function is that same avoidance applied one
 *   layer lower, to path parsing. cmd_create is expected to switch to
 *   calling this function too (rather than keeping its own private
 *   copy of the loop) — see create.c's own updated header comment.
 *
 *   If `arg` contains a '/', it is copied into `out_buf` UNCHANGED
 *   (already fully-qualified — e.g. "IDE0/TEST.TXT" — nothing to
 *   default). If `arg` has NO '/' at all, this prepends "<slot>/" where
 *   <slot> is vfs_mount_name_of(vfs_root->fs) — the CURRENT working
 *   mount, exactly the one Day 25's own `cd` command (cmd_cd, kernel/
 *   shell/cmd/cd.c) changes by reassigning vfs_root — so a bare
 *   `write test.txt` always resolves against whichever mount `cd` most
 *   recently switched to, the same "vfs_root IS the cwd" design
 *   kmain.c's text_shell_prompt() already relies on to print "IDE0/ >"
 *   after a `cd IDE0` (see that function's own DAY14/DAY25 comments).
 *
 * Returns 0 on success (`out_buf` now holds a NUL-terminated path).
 * Returns -1 if `arg`/`out_buf` is NULL, `arg` has no '/' AND no
 * volume is currently mounted (vfs_root is NULL or vfs_mount_name_of()
 * finds no matching slot for it — should not happen once Day 22/23's
 * self-test mounts have run, but checked rather than assumed), or
 * `out_buf_size` is too small to hold the resulting "<slot>/<arg>"
 * (truncation is refused outright rather than silently handing a
 * caller a cut-off path that might resolve to the WRONG file).
 *
 * DAY42 (Week 6, Day 42 of the 5.0 filesystem roadmap):
 *   "多级目录与路径解析。" The contract above ("has a '/' -> copied
 *   unchanged") was written when every path was exactly one
 *   "<slot>/<filename>" segment. It is replaced by:
 *
 *     - `arg` whose FIRST component is a registered mount slot
 *       ("IDE3", "IDE3/", "IDE3/docs/a.txt") is fully qualified.
 *     - `arg` starting with '/' is absolute within the CURRENT volume.
 *     - anything else ("a.txt", "docs/a.txt", "..", "./x") is relative
 *       to the current directory = current volume (vfs_root) + the
 *       cwd string (shell_get_cwd(), below).
 *     - the result is lexically normalized: empty and "." components
 *       disappear, ".." pops one component (and cannot climb above the
 *       volume root). It is ALWAYS "<slot>/<normalized>" — the '/' is
 *       present even for the volume root ("IDE3/") since every consumer
 *       finds the slot by searching for that first '/'.
 *
 *   Existing callers (create/read/write/rm) keep working unchanged:
 *   "IDE0/TEST.TXT" and a bare "test.txt" resolve exactly as before.
 *   Same failure modes as before, plus: the normalized path does not
 *   fit `out_buf_size`.
 */
int shell_resolve_path(const char *arg, char *out_buf, uint32_t out_buf_size);

/* shell_get_cwd() / shell_set_cwd(sub)
 *
 * DAY42: the current directory INSIDE the current volume (vfs_root),
 * as a path relative to that volume's root with no leading or trailing
 * '/': "" is the volume root, "docs/notes" a subdirectory. Only
 * cmd_cd changes it (after reassigning vfs_root, since the value is
 * remembered per-filesystem — if vfs_root changes behind its back,
 * shell_get_cwd() reports "" rather than a stale path from another
 * volume). shell_set_cwd() returns 0, or -1 if `sub` is too long
 * (SHELL_CWD_MAX 64) — the old value is then left untouched.
 * kmain.c's text_shell_prompt() prints it: "IDE3/docs/notes/ > ". */
const char *shell_get_cwd(void);
int         shell_set_cwd(const char *sub);

/* shell_split_path(resolved, out_fs, out_sub)
 *
 * DAY42: cuts a path produced by shell_resolve_path() ("IDE3/docs/a")
 * at its first '/': *out_fs = the mounted filesystem_t for the slot
 * ("IDE3"), *out_sub = the part after the '/' ("docs/a", "" for the
 * volume root; points INTO `resolved`, no copy). Returns 0, or -1 if
 * the slot is not a registered mount / an argument is NULL. */
struct filesystem;
int shell_split_path(const char *resolved, struct filesystem **out_fs,
                     const char **out_sub);

void cmd_help    (terminal_t *t, int argc, char **argv);
void cmd_clear   (terminal_t *t, int argc, char **argv);
void cmd_version (terminal_t *t, int argc, char **argv);
void cmd_color   (terminal_t *t, int argc, char **argv);
void cmd_shutdown(terminal_t *t, int argc, char **argv);
void cmd_reboot  (terminal_t *t, int argc, char **argv);
/* cmd_show — DAY19, renamed from cmd_cat on DAY21, removed on DAY27
 * ("删除那个show只保留read" — remove `show`, keep only `read`; DAY21's
 * own read.c already grew into a full superset of what `show` did, so
 * this project no longer needs two commands for the same job). */
void cmd_write   (terminal_t *t, int argc, char **argv);  /* DAY25 */
void cmd_cd      (terminal_t *t, int argc, char **argv);  /* DAY25 */
void cmd_term    (terminal_t *t, int argc, char **argv);  /* DAY43 PRE-3 */
void cmd_elfinfo (terminal_t *t, int argc, char **argv);  /* DAY43 */
void cmd_elfload (terminal_t *t, int argc, char **argv);  /* DAY44 */
void cmd_elfmap  (terminal_t *t, int argc, char **argv);  /* DAY45 */
void cmd_elfrun  (terminal_t *t, int argc, char **argv);  /* DAY46 */
void cmd_run     (terminal_t *t, int argc, char **argv);  /* DAY48 */
void cmd_sysstat (terminal_t *t, int argc, char **argv);  /* DAY50 */
void cmd_fstress (terminal_t *t, int argc, char **argv);  /* DAY64 */
void cmd_df      (terminal_t *t, int argc, char **argv);  /* DAY68 */
void cmd_errno   (terminal_t *t, int argc, char **argv);  /* DAY70 */
void cmd_pwd     (terminal_t *t, int argc, char **argv);  /* DAY69 */
void cmd_fsck    (terminal_t *t, int argc, char **argv);  /* DAY67 */
void cmd_soak    (terminal_t *t, int argc, char **argv);  /* DAY67 */
void cmd_gfxstat (terminal_t *t, int argc, char **argv);  /* DAY67 */
void cmd_lmfsbench(terminal_t *t, int argc, char **argv); /* DAY66 */
void cmd_fatcache(terminal_t *t, int argc, char **argv);  /* DAY65 */
void cmd_fatbench(terminal_t *t, int argc, char **argv);  /* DAY65 */
void cmd_pginfo  (terminal_t *t, int argc, char **argv);  /* DAY53 */
void cmd_resolution(terminal_t *t, int argc, char **argv); /* DAY58b */
void cmd_mounts(terminal_t *t, int argc, char **argv);     /* DAY62 */
void cmd_cdinfo(terminal_t *t, int argc, char **argv);     /* DAY62 */
void cmd_font(terminal_t *t, int argc, char **argv);       /* DAY63 */
void cmd_config(terminal_t *t, int argc, char **argv);     /* DAY63 */

#endif