/* kernel/shell/shell.c - LumaOS 5.0 Shell Core */
#include "../core/kernel.h"
#include "../gui/gui.h"
#include "../fs/vfs/vfs.h"   /* DAY25: vfs_root, vfs_mount_name_of() —
                                shell_resolve_path() below */
#include "shell.h"
#include "../fs/vfs/vfs_ops.h"

shell_cmd_t cmd_table[SHELL_MAX_CMDS];
int         cmd_count = 0;

void shell_register(const char *name, const char *desc, shell_cmd_fn_t fn) {
    if (cmd_count >= SHELL_MAX_CMDS) return;
    cmd_table[cmd_count].name        = name;
    cmd_table[cmd_count].description = desc;
    cmd_table[cmd_count].fn          = fn;
    cmd_count++;
}

void shell_init(void) {
    cmd_count = 0;
    shell_register("help",     "Show available commands",     cmd_help    );
    shell_register("clear",    "Clear terminal screen",       cmd_clear   );
    shell_register("version",  "Show OS version",             cmd_version );
    shell_register("type",     "Show quick reference",        cmd_type    );
    shell_register("echo",     "Echo text to terminal",       cmd_echo    );
    shell_register("color",    "Change text color (0-15)",    cmd_color   );
    shell_register("shutdown", "Shutdown the system",         cmd_shutdown);
    shell_register("reboot",   "Reboot the system",           cmd_reboot  );
    /* DAY14: "整合 Shell：新增命令 cmd_ls（调用 vfs_readdir 列出文件）。"
     * See kernel/shell/cmd/ls.c's header comment for what vfs_root
     * actually resolves to and why this goes through fs->readdir()
     * instead of calling any FAT12-specific function by name. */
    shell_register("ls",       "List files (via VFS)",        cmd_ls      );
    /* DAY19: "实现 cmd_cat /KERNEL.BIN，能在终端打印出内核二进制的开头
     * 部分。" this goes through fat_open()+fs->read() (fat_read(), also
     * new this day), never a FAT12/16/32-specific function by name,
     * same discipline `ls` already established.
     *
     * DAY21: "更改cat命令名称为show，功能不变" — renamed cat -> show.
     *
     * DAY27: "删除那个show只保留read" — `show` is removed outright.
     * kernel/shell/cmd/read.c (DAY25) had already grown into a full
     * superset of what `show` did (see read.c's own header comment),
     * so this project no longer needs two commands doing the same job;
     * `read` (registered below, alongside `write`/`cd`) is the one
     * that remains. */
    /* DAY24: "实现 fat_create_file(name)：在根目录区找一个空条目...
     * 在 Shell 中输入 create test.txt，重新 ls 能看到它。" See
     * kernel/shell/cmd/create.c's header comment for the full
     * "<mount-slot>/<filename>" path convention (matching `read`'s own
     * since Day 18-FOLLOWUP/Day 25) and the small bare-filename
     * fallback this command adds on top of fat_create_file()'s own
     * stricter syntax. */
    shell_register("create",   "Create an empty file (via VFS)",    cmd_create);
    /* DAY25: "实现 fat_write(file, buf, size)... write test.txt... 支持
     * cd 命令... 支持 show test.txt 而不是 show IDE2/text.txt." See each
     * command's own .c file header comment for the full contract:
     * cmd_write.c (interactive multi-line editor, Ctrl+S to save,
     * overwrite-confirmation prompt — DAY27 update: skipped entirely
     * for an already-empty file), cmd_read.c (this day's `read` —
     * `show`'s own logic plus the bare-filename fallback `create`
     * established), cmd_cd.c (switches vfs_root between mounted
     * volumes — this project has no subdirectory tree yet, see that
     * file's own header comment for why "cd between mounts" is exactly
     * what "当前工作目录" can mean at this point in the roadmap). */
    shell_register("write",    "Write a file's contents (via VFS)", cmd_write );
    shell_register("read",     "Print a file's contents (via VFS)", cmd_read  );
    shell_register("cd",       "Change directory (cd <path>, cd .., cd -, cd)", cmd_cd    );
    shell_register("pwd",      "Print the current directory",       cmd_pwd   );   /* DAY69 */
    /* DAY26: "实现 fat_delete_file(name)：找到目录项，标记为已删除
     * （首字节设为 0xE5），释放其占用的簇链。rm test.txt 后 ls 消失，
     * 且空闲簇数增加。" See kernel/shell/cmd/rm.c's header comment for
     * the full "<mount-slot>/<filename>" path convention (same as
     * every other file-taking command since Day 18/24/25) and
     * kernel/fs/fat/fat_vfs.h's doc comment on fat_delete_file() for
     * the real cluster-chain-freeing + directory-entry-deletion work
     * this command exercises. */
    shell_register("rm",       "Delete a file (via VFS)",           cmd_rm    );
    /* DAY42: "多级目录与路径解析。可以创建并访问多级目录。" `mkdir` is
     * this day's only new command — `cd`/`ls`/`create`/`read`/`rm` all
     * gain multi-level path support by way of shell_resolve_path()
     * below (and cd.c/ls.c's own DAY42 changes), not by being
     * re-registered. See kernel/shell/cmd/mkdir.c's header comment. */
    shell_register("mkdir",    "Create a directory (LMFS only)",    cmd_mkdir );
    /* DAY43 PRE-3: open an extra Terminal window (GUI only). */
    shell_register("term",     "Open a new Terminal window (GUI)",  cmd_term  );
    /* DAY43: "学习并定义 ELF32 基础结构。能解析 ELF Header。" `elfinfo`
     * 是今天的验收/诊断命令：读文件前 52 字节，交给 kernel/exec/elf.c
     * 解析并按 readelf -h 风格打印。原理见 kernel/exec/elf.h 的文件头注释，
     * 命令实现见 kernel/shell/cmd/elfinfo.c。 */
    shell_register("elfinfo",  "Show a file's ELF32 header",        cmd_elfinfo);
    /* DAY44: "实现 elf_load()。检查 Magic、打印 Entry，实现在 lmfs 上用
     * elf。" `elfload` 调用 kernel/exec/elf_loader.c 的 elf_load()：打开
     * （FAT 或 LMFS）→ 验证 Header 与全部 Program Header → 打印 Entry 和
     * 装载计划。命令实现见 kernel/shell/cmd/elfload.c，原理见 elf.h 的
     * DAY44 注释。 */
    shell_register("elfload",  "Validate an ELF32 and show its load plan", cmd_elfload);
    /* DAY45: "解析 PT_LOAD。能将程序段加载到内存。" `elfmap` 先走 Day 44 的
     * elf_load() 验证，再调用 elf_load_segments() 把每个 PT_LOAD 段真的装进
     * p_vaddr 处的内存（拷贝文件部分 + 清零 .bss + 回读校验），并展示/复核
     * 结果。只放好、不执行（Day 46 才跳到入口）。命令实现见
     * kernel/shell/cmd/elfmap.c，原理见 kernel/exec/elf.h 的 DAY45 注释。 */
    shell_register("elfmap",   "Load an ELF32's segments into memory",     cmd_elfmap);
    /* DAY46: "实现 elf_run()。hello.sw 可以执行。" `elfrun` 把 Day 44/45 的
     * elf_load() + elf_load_segments() 与今天的 elf_run() 串起来：验证 ->
     * 装入内存 -> call e_entry -> 打印程序的退出码。命令实现见
     * kernel/shell/cmd/elfrun.c，原理见 kernel/exec/elf.h 的 DAY46 注释。 */
    shell_register("elfrun",   "Load and execute an ELF32 (.sw) program",  cmd_elfrun);
    /* DAY48: "Shell 集成 `run`。`run hello.sw` 成功。" 正式的“运行程序”命令：
     * 只接受 .sw，走 kernel/exec/elf_exec.c 的 load -> map -> run 流水线，输出风格像
     * Unix shell（成功静默、失败才多一行）。命令实现见 kernel/shell/cmd/run.c，
     * 原理见 kernel/exec/elf.h 的 DAY48 注释。 */
    shell_register("run",      "Run a .sw program",                        cmd_run);
    /* DAY50: "定义 syscall ABI。`int 0x80` 能进入 Kernel。" `sysstat` 显示 syscall 通道的状态
     * （IDT[0x80] 门、进入内核的次数、最近一次调用），`sysstat -t` 由内核自己执行 `int 0x80` 自检。
     * 命令实现见 kernel/shell/cmd/sysstat.c，ABI 与原理见 kernel/syscall/syscall.h。 */
    shell_register("sysstat",  "Show/test the int 0x80 syscall channel",   cmd_sysstat);
    /* DAY53: "开启分页，建立 Kernel Page Table。开启分页后系统仍稳定。" `pginfo` 显示分页状态，
     * `pginfo <addr>` 查表，`pginfo -t` 自检，`pginfo -f` 故意触发缺页（演示 panic）。
     * 实现见 kernel/shell/cmd/pginfo.c，原理见 kernel/mm/paging.h。 */
    shell_register("resolution", "Show/switch the screen resolution + blend pool", cmd_resolution);  /* DAY58b */
    shell_register("mounts",   "List the VFS mount table",                 cmd_mounts);   /* DAY62 */
    shell_register("cdinfo",   "Boot medium, boot info and CD-ROM drives", cmd_cdinfo);   /* DAY62 */
    shell_register("font",     "Show where the font came from",            cmd_font);     /* DAY63 */
    shell_register("fstress",  "File system stress test (create/write/read/verify)", cmd_fstress);  /* DAY64 */
    shell_register("fatcache", "Show/toggle the FAT table sector cache",    cmd_fatcache); /* DAY65 */
    shell_register("fatbench", "Benchmark FAT reads with cache off/on",    cmd_fatbench); /* DAY65 */
    shell_register("lmfsbench", "LMFS bitmap allocator: old vs fast + checks", cmd_lmfsbench); /* DAY66 */
    shell_register("fsck", "Check a volume for inconsistencies (read-only)", cmd_fsck);   /* DAY67 */
    shell_register("soak", "Long-running create/write/verify/delete loop",  cmd_soak);   /* DAY67 */
    shell_register("gfxstat", "Display flip statistics (rows written/frame)", cmd_gfxstat); /* DAY67 */
    shell_register("errno", "Look up unified error codes (errno [n|NAME|-t])", cmd_errno); /* DAY70 */
    shell_register("df",       "Show volume capacity and free space",      cmd_df);       /* DAY68 */
    shell_register("config",   "Show the configuration read from the CD",  cmd_config);   /* DAY63 */
    shell_register("pginfo",   "Show/test the kernel page table (paging)", cmd_pginfo);
}

static char arg_buf[SHELL_MAX_ARGS][SHELL_ARG_LEN];
static char *argv_p[SHELL_MAX_ARGS];

static int parse(const char *line, char **argv) {
    int argc = 0, i = 0;
    while (line[i]==' '||line[i]=='\t') i++;
    while (line[i] && argc < SHELL_MAX_ARGS) {
        int ai = 0;
        while (line[i] && line[i]!=' ' && line[i]!='\t' && ai < SHELL_ARG_LEN-1)
            arg_buf[argc][ai++] = line[i++];
        arg_buf[argc][ai] = 0;
        /* === MODIFIED SECTION START ===
         * STAGE4 FIX: "argv[argc++] = arg_buf[argc-1];" reads and
         * writes argc in the same unsequenced expression (flagged by
         * -Wsequence-point). Every mainstream compiler happened to
         * evaluate it left-to-right, but that's not guaranteed by the
         * C standard, so under more aggressive optimization this could
         * theoretically break argument parsing. Split into two
         * statements with an explicit sequence point between them. */
        argv[argc] = arg_buf[argc];
        argc++;
        /* === MODIFIED SECTION END === */
        while (line[i]==' '||line[i]=='\t') i++;
    }
    return argc;
}

/* DAY43 PRE-3: raw command line of the command being executed, see
 * shell_cmdline_args() in shell.h. */
static const char *g_cur_args = "";

const char *shell_cmdline_args(void) { return g_cur_args; }

/* DAY75: 外部命令 = 一个 .sw 程序。name 没有 ".sw" 就补上；含 '/' 的名字只按原样找。
 * 找到（存在且不是目录）就把 "<程序路径> <原参数>" 当作 `run` 的命令行执行。返回 1 = 已处理。 */
static int run_external(terminal_t *t, const char *name, int argc, const char *args)
{
    (void)argc;
    static char prog[48], resolved[64], line[256];
    size_t n = strlen(name);
    if (n == 0 || n + 4 > sizeof(prog)) return 0;
    strcpy(prog, name);
    if (!(n >= 4 && prog[n-3] == '.' && (prog[n-2] == 's' || prog[n-2] == 'S') && (prog[n-1] == 'w' || prog[n-1] == 'W')))
        strcpy(prog + n, ".sw");
    const char *cands[2] = { prog, 0 };
    static char sys_cand[64];
    if (!strchr(prog, '/')) {                       /* 裸名字：再试系统卷 */
        strcpy(sys_cand, "RAM0/");
        if (strlen(prog) + 6 <= sizeof(sys_cand)) { strcpy(sys_cand + 5, prog); cands[1] = sys_cand; }
    }
    for (int i = 0; i < 2; i++) {
        if (!cands[i]) continue;
        uint32_t size; int is_dir = 0;
        if (shell_resolve_path(cands[i], resolved, sizeof(resolved)) != 0) continue;
        if (vfs_ops_stat(resolved, &size, &is_dir) != 0 || is_dir) continue;
        if (strlen(cands[i]) + 1 + strlen(args) + 1 > sizeof(line)) return 0;
        strcpy(line, cands[i]);
        if (args[0]) { size_t l = strlen(line); line[l] = ' '; strcpy(line + l + 1, args); }
        g_cur_args = line;
        char *fake[2] = { (char *)"run", (char *)cands[i] };
        cmd_run(t, 2, fake);          /* cmd_run 只检查 argc >= 2，程序和参数从 g_cur_args 重新切词 */
        return 1;
    }
    return 0;
}

void shell_exec(terminal_t *t, const char *cmdline) {
    int blank = 1;
    for (int i = 0; cmdline[i]; i++)
        if (cmdline[i]!=' '&&cmdline[i]!='\t') { blank=0; break; }
    if (blank) return;
    int argc = parse(cmdline, argv_p);
    if (!argc) return;
    {   /* point g_cur_args just past "<command><blanks>" */
        const char *a = cmdline;
        while (*a==' ' || *a=='\t') a++;
        while (*a && *a!=' ' && *a!='\t') a++;
        while (*a==' ' || *a=='\t') a++;
        g_cur_args = a;
    }
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(argv_p[0], cmd_table[i].name)==0) {
            cmd_table[i].fn(t, argc, argv_p);
            g_cur_args = "";
            return;
        }
    }
    /* DAY75: 不是内置命令——找同名的用户程序（.sw）：先当前目录，再系统卷 RAM0（live CD 带来的 cat.sw ls.sw explorer.sw ...）。
     * 这是“内置命令迁移到用户态”的机制：以后把某个内置命令删掉，同名 .sw 就自动顶上，用户敲的命令不变。 */
    if (run_external(t, argv_p[0], argc, g_cur_args))
        { g_cur_args = ""; return; }
    g_cur_args = "";
    terminal_puts(t, "Unknown: ", TERM_COL_ERROR);
    terminal_puts(t, argv_p[0],  TERM_COL_ERROR);
    terminal_puts(t, "\nType 'help'\n", TERM_COL_DEFAULT);
}

/* Called by terminal when user presses Enter */
void terminal_run_command(terminal_t *t, const char *cmd) {
    shell_exec(t, cmd);
}

/* ===================================================================
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "支持 cd 命令：切换当前工作目录（在挂载点之间）。支持 show test.txt
 *    而不是 show IDE2/text.txt。"
 *
 * See shell.h's doc comment above this function's declaration for the
 * full contract and the "why a shared helper, not a third/fourth copy"
 * rationale. Implementation mirrors cmd_create's own DAY24-FOLLOWUP
 * loop (kernel/shell/cmd/create.c) exactly, just parameterized over an
 * output buffer/size instead of that command's own private static
 * path_buf[64].
 * =================================================================== */
/* ---- DAY42: current working directory --------------------------------
 *
 * Before today "the current directory" was exactly `vfs_root` (WHICH
 * MOUNTED VOLUME bare filenames default onto — see cd.c's own DAY25
 * header). With LMFS subdirectories there is now a second coordinate:
 * WHERE INSIDE that volume. It is kept here as a plain path string
 * relative to the volume root, no leading/trailing '/' ("" = the volume
 * root itself, "docs/notes" = IDE3's docs/notes directory).
 *
 * Why a string and not a cached inode number: the shell layer is
 * filesystem-agnostic (it never includes lmfs.h); a path string is the
 * one currency every layer already speaks (shell_resolve_path() ->
 * lmfs_vfs_*(path) -> lmfs_resolve_path()). It is always already
 * normalized by shell_resolve_path() below before cd stores it, so
 * ".." never appears in it and the prompt can print it as-is.
 *
 * g_cwd_fs remembers WHICH filesystem the string belongs to. Anything
 * that reassigns vfs_root without going through cmd_cd (boot-time
 * vfs_register_mount() calls do) therefore makes shell_get_cwd() fall
 * back to "" instead of pairing a stale "docs/notes" with a different
 * volume. */
#define SHELL_CWD_MAX 64
static char          g_cwd[SHELL_CWD_MAX];
static filesystem_t *g_cwd_fs;

const char *shell_get_cwd(void)
{
    if (!vfs_root || vfs_root->fs != g_cwd_fs)
        return "";
    return g_cwd;
}

int shell_set_cwd(const char *sub)
{
    if (!sub)
        sub = "";
    uint32_t n = (uint32_t)strlen(sub);
    if (n >= SHELL_CWD_MAX)
        return -1;
    memcpy(g_cwd, sub, n + 1);
    g_cwd_fs = vfs_root ? vfs_root->fs : (filesystem_t *)0;
    return 0;
}

/* ===================================================================
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "支持 cd 命令：切换当前工作目录（在挂载点之间）。支持 show test.txt
 *    而不是 show IDE2/text.txt。"
 * DAY42 (Week 6, Day 42): rewritten for multi-level paths.
 *
 * See shell.h's doc comment on shell_resolve_path() for the contract.
 * Algorithm, in three steps:
 *
 *   1. Decide WHICH VOLUME and what the not-yet-normalized remainder
 *      ("raw") is:
 *        "/x/y"        -> current volume, raw = "x/y"   (absolute)
 *        "IDE3/x/y"    -> volume IDE3,    raw = "x/y"   (first component
 *                          is a registered mount slot — same rule as
 *                          every earlier day: fully qualified)
 *        "x/y", "a.txt"-> current volume, raw = <cwd> "/" "x/y"
 *   2. Normalize raw purely lexically: drop empty and "." components,
 *      let ".." pop the previous component (a ".." at the volume root
 *      just stays at the root — it cannot climb out of the volume into
 *      another mount). Lexical is enough because LMFS has no symlinks;
 *      the on-disk ".." entries (lmfs.h's DAY42 note) are only needed
 *      when someone hands lmfs_vfs_*() an UNnormalized path directly.
 *   3. Emit "<slot>/<normalized>" — always with the '/' after the slot,
 *      even for the volume root ("IDE3/"), because every consumer
 *      (create.c/read.c/rm.c's mount-slot peek, fat_*()'s own parsing)
 *      locates the slot by looking for that first '/'.
 * =================================================================== */
int shell_resolve_path(const char *arg, char *out_buf, uint32_t out_buf_size)
{
    if (!arg || !out_buf || out_buf_size == 0)
        return -1;

    const char *cur_slot = vfs_root ? vfs_mount_name_of(vfs_root->fs)
                                    : (const char *)0;

    char slot[8];
    char raw[160];
    uint32_t rn = 0;
    slot[0] = '\0';

    /* ---- step 1: pick the volume, build `raw` ---- */
    const char *rest = (const char *)0;   /* non-NULL = no cwd prefix */
    if (arg[0] == '/') {
        if (!cur_slot)
            return -1;
        rest = arg + 1;
    } else {
        const char *sep = strchr(arg, '/');
        uint32_t len = (sep != (char *)0) ? (uint32_t)(sep - arg)
                                          : (uint32_t)strlen(arg);
        if (len > 0 && len < sizeof(slot)) {
            memcpy(slot, arg, len);
            slot[len] = '\0';
            filesystem_t *mfs = vfs_lookup_mount(slot);
            if (mfs != (filesystem_t *)0) {
                rest = (sep != (char *)0) ? sep + 1 : arg + len;
                /* DAY69: 换成规范写法（"ram0" -> "RAM0"），后面所有人按规范名比较 */
                const char *canon = vfs_mount_name_of(mfs);
                if (canon && strlen(canon) < sizeof(slot)) { memcpy(slot, canon, strlen(canon) + 1); }
            }
        }
    }

    if (rest != (const char *)0) {
        if (arg[0] == '/') {
            uint32_t k = 0;
            while (cur_slot[k] != '\0' && k < sizeof(slot) - 1) {
                slot[k] = cur_slot[k];
                k++;
            }
            slot[k] = '\0';
        }
        /* else: `slot` already holds the typed mount name */
    } else {
        /* relative: current volume + cwd + "/" + arg */
        if (!cur_slot)
            return -1;
        uint32_t k = 0;
        while (cur_slot[k] != '\0' && k < sizeof(slot) - 1) {
            slot[k] = cur_slot[k];
            k++;
        }
        slot[k] = '\0';

        const char *cwd = shell_get_cwd();
        uint32_t cl = (uint32_t)strlen(cwd);
        if (cl >= sizeof(raw) - 2)
            return -1;
        memcpy(raw, cwd, cl);
        rn = cl;
        raw[rn++] = '/';
        rest = arg;
    }

    {
        uint32_t rl = (uint32_t)strlen(rest);
        if (rn + rl >= sizeof(raw))
            return -1;
        memcpy(raw + rn, rest, rl);
        rn += rl;
        raw[rn] = '\0';
    }

    /* ---- step 2: lexical normalization ---- */
    char norm[160];
    uint32_t nl = 0;
    const char *p = raw;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *c = p;
        while (*p != '\0' && *p != '/')
            p++;
        uint32_t cl = (uint32_t)(p - c);

        if (cl == 1 && c[0] == '.')
            continue;
        if (cl == 2 && c[0] == '.' && c[1] == '.') {
            while (nl > 0 && norm[nl - 1] != '/')
                nl--;                 /* drop the last component ... */
            if (nl > 0)
                nl--;                 /* ... and the '/' before it */
            continue;                 /* at the root: stay at the root */
        }
        if (nl + (nl > 0 ? 1u : 0u) + cl >= sizeof(norm))
            return -1;
        if (nl > 0)
            norm[nl++] = '/';
        memcpy(norm + nl, c, cl);
        nl += cl;
    }
    norm[nl] = '\0';

    /* ---- step 3: "<slot>/<normalized>" ---- */
    uint32_t sl = (uint32_t)strlen(slot);
    if (sl + 1 + nl + 1 > out_buf_size)
        return -1;                    /* refuse rather than truncate */
    memcpy(out_buf, slot, sl);
    out_buf[sl] = '/';
    memcpy(out_buf + sl + 1, norm, nl + 1);
    return 0;
}

/* shell_split_path(resolved, out_fs, out_sub)
 *
 * DAY42: the inverse of the last step of shell_resolve_path() — cuts a
 * resolved "<slot>/<sub>" path at its first '/' and looks the slot up.
 * cd/ls/mkdir all need exactly this (which volume? what's inside it?),
 * so it lives here once instead of three private copies. */
int shell_split_path(const char *resolved, struct filesystem **out_fs,
                     const char **out_sub)
{
    if (!resolved || !out_fs || !out_sub)
        return -1;

    const char *sep = strchr(resolved, '/');
    uint32_t len = (sep != (char *)0) ? (uint32_t)(sep - resolved)
                                      : (uint32_t)strlen(resolved);
    char slot[8];
    if (len == 0 || len >= sizeof(slot))
        return -1;
    memcpy(slot, resolved, len);
    slot[len] = '\0';

    filesystem_t *fs = vfs_lookup_mount(slot);
    if (!fs)
        return -1;

    *out_fs  = fs;
    *out_sub = (sep != (char *)0) ? sep + 1 : resolved + len;
    return 0;
}