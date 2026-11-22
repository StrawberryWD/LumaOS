/* kernel/fs/lmfs/lmfs_vfs.h
 * LumaOS 5.0 - LMFS <-> VFS Adapter
 *
 * DAY35 (第二月 · 第6周 · Day 35 of the 5.0 filesystem roadmap):
 *   "LMFS 注册进 VFS。" 验收："FAT 与 LMFS 可以同时存在。"
 *
 *   这是 fat12_vfs.h/fat12_vfs.c（Day 13）在 LMFS 这边的对应实现：
 *   Day 32~34 建好的 lmfs_mount()/lmfs_read_inode()/lmfs_readdir() 都
 *   只认 lmfs_fs_t* / inode 号这些 LMFS 自己的形状，VFS 层的 Shell
 *   命令（`ls` 等）只认 vfs_node_t / struct filesystem 的函数指针表
 *   ——这个文件就是翻译层，把 lmfs.h 的磁盘专属 API 适配成 vfs.h 的
 *   通用 vfs_open_fn/vfs_read_fn/vfs_readdir_fn 形状，跟
 *   kernel/drivers/block/block.c 的 ide0_block_device() 把 IDE 驱动
 *   适配成通用 block_device_t 接口是同一个道理。
 *
 *   "FAT 与 LMFS 可以同时存在"这条验收标准，机制上早就有了：
 *   kernel/fs/vfs/vfs.h 的 vfs_mounts[]/vfs_register_mount() 是
 *   DAY18-FOLLOWUP 时"提前做"的一张多挂载点命名表，FAT12/16/32 三个
 *   卷已经在同时挂载、各自占一个 "IDE<N>" 槽位（见 kmain.c 的
 *   DAY13/16/17 段落）。Day 35 要做的实际上只是：让 LMFS 也能通过
 *   同一张表登记一个槽位——这个适配器文件就是让这件事成立所缺的
 *   最后一块拼图，不需要改 vfs.h/vfs.c 本身一个字。
 *
 * Design notes（跟 fat12_vfs.h 完全对应，逐条照抄其理由）：
 *   - lmfs_vfs_mount(dev, lba) 是这个文件唯一的入口：调用
 *     lmfs_mount()（Day 32）做真正的挂载，再包一层 filesystem_t
 *     （ops=lmfs_filesystem_ops，fs_private=lmfs_fs_t 挂载状态）和
 *     一个根 vfs_node_t（type=VFS_TYPE_DIR）。成功返回填好的
 *     filesystem_t*，lmfs_mount() 本身失败则返回 NULL。
 *   - 挂载状态（lmfs_fs_t、filesystem_t、根 vfs_node_t）都是
 *     lmfs_vfs.c 内部的 static 变量，不是堆分配——这个项目到现在还
 *     没有内核堆（Day 73 才有 malloc/free），跟 fat12_vfs.c 的理由
 *     完全一样。Day 35 的范围是"能同时挂载几个卷"，不要求"能挂载
 *     任意多个动态数量的卷"，一个 static 实例够用。
 *   - 目录节点的 fs_private 存的是 inode 号（不是 FAT12 那种"根目录
 *     用哨兵值、子目录用起始簇号"的两套值）——LMFS 的每一个目录，
 *     包括根目录本身，都有一个真正的 inode 号（根目录固定是
 *     LMFS_ROOT_INODE_NUM=0），不需要 FAT12 那种"根目录是固定区域、
 *     没有簇号"的特殊情况处理，直接把 inode 号转成 (void*) 存进
 *     fs_private，比 FAT12 那边还简单一些。
 *   - open()/readdir() 是真正能用的实现，read() 是文档化的占位桩
 *     （永远返回 0，即"读到 0 字节，不是错误"，符合 vfs_read_fn 自己
 *     文档里"0 == EOF"的约定）——原因跟 fat12_vfs_read() 在 Day 13
 *     时的桩函数完全一样：真正按字节读文件内容需要 lmfs_read_
 *     inode() 之外还要走 direct[]/indirect1/indirect2 的完整数据
 *     读取逻辑，那是 Day 40（"实现 read()"）的任务，不是今天。
 *   - `lmfs_filesystem_ops` 不加 static，跟 fat12_filesystem_ops 一样
 *     的理由：给未来可能的直接调用者留出可见性，即便目前只有
 *     lmfs_vfs_mount() 自己用到它。
 */
#ifndef LUMA_LMFS_VFS_H
#define LUMA_LMFS_VFS_H

#include "../../drivers/block/block.h"   /* block_device_t */
#include "../vfs/vfs.h"                  /* filesystem_t, vfs_node_t */

/* lmfs_filesystem_ops
 *
 * LMFS 驱动的 VFS 操作表：.open/.read/.readdir/.close 指向本文件的
 * 适配函数（lmfs_vfs.c）。.name 是 "lmfs"，供诊断输出使用。
 * .root/.fs_private 留 NULL——那是每次挂载各自的状态，由下面的
 * lmfs_vfs_mount() 按实例填入，不属于这张共享的操作表本身。
 */
extern const filesystem_t lmfs_filesystem_ops;

/* lmfs_vfs_mount(dev, lba)
 *
 * 在 `dev` 的磁盘绝对 LBA `lba` 处挂载一个 LMFS 卷（通过
 * lmfs_mount()，Day 32），包装成一个 `.root` 是 VFS_TYPE_DIR 节点
 * （代表 LMFS 根目录，inode 号固定是 LMFS_ROOT_INODE_NUM）的
 * filesystem_t。
 *
 * 成功时返回填好的、静态分配的 filesystem_t*。lmfs_mount() 本身失败
 * （不是合法的 LMFS 卷、或者底层 dev->read() 失败）时返回 NULL——
 * 跟 lmfs_mount() 自己"失败用 -1"的约定一致，只是这里换成了指针
 * 形式的失败值，因为 vfs_mount()/kmain.c 的调用方要的是"挂载出来的
 * 东西能不能用"而不是原始的错误码。
 */
filesystem_t *lmfs_vfs_mount(block_device_t *dev, uint32_t lba);

/* lmfs_vfs_create(path)
 *
 * DAY38 (第二月 · 第6周 · Day 38 of the 5.0 filesystem roadmap):
 *   "实现 create() 与目录项。" 验收："create test.txt 成功。"
 *
 * 这是 fat_create_file()（kernel/fs/fat/fat_vfs.c，Day 24）在 LMFS
 * 这边的对应角色：Shell 的 `create` 命令要调用的、认识
 * "<挂载点>/<文件名>" 路径语法的入口，而不是 lmfs.c 里那个要求调用者
 * 已经知道父目录 inode 号的 lmfs_create()。`path` 必须恰好含一个
 * '/'，前半截是挂载点名字（比如 "IDE3"），后半截是文件名——解析
 * 方式跟 fat_create_file() 逐字节一致。
 *
 * 父目录固定是 LMFS 根目录（LMFS_ROOT_INODE_NUM）——这个项目现在
 * 还没有真正的多级目录/路径解析（Day 42 才有，`cd`/`.`/`..` 目前
 * 也只是挂载点之间切换，不认目录项，见 kernel/shell/cmd/cd.c 自己
 * 的说明），所以 "<挂载点>/<文件名>" 里文件名部分也不能再包含 '/'
 * ——跟 fat_create_file() 现阶段的限制完全一致，不是 LMFS 独有的
 * 缩水。
 *
 * 查出的挂载点必须是一个 LMFS 卷（fs->name == "lmfs"）——传一个
 * FAT 卷的挂载点名字会在这里被拒绝，不会把 fs->fs_private 错误地
 * 当 lmfs_fs_t* 使用；真正的文件系统类型判断（该走 fat_create_
 * file() 还是这个函数）在更上层的 kernel/shell/cmd/create.c 里做，
 * 这里的检查是防御性的第二道防线。
 *
 * 成功返回 0，失败返回 -1（挂载点不存在或不是 LMFS 卷、路径语法
 * 不对、文件名不合法、重名、父目录已满、底层 lmfs_create() 本身
 * 失败）——跟 lmfs.c 其余函数一致的 int 0/-1 约定，没有像 fat_
 * error_t 那样为每种失败单独编码，这个项目目前还没有必要为 LMFS
 * 建一整套独立的错误码体系。
 */
int lmfs_vfs_create(const char *path);

/* lmfs_vfs_write(path, buf, size)
 *
 * DAY39 (第二月 · 第6周 · Day 39 of the 5.0 filesystem roadmap):
 *   "实现 write()，支持直接块与间接块。" 验收："写入 10KB 文件并验证。"
 *
 * fat_write()（Day 25）在 LMFS 这边的路径语法入口——跟
 * lmfs_vfs_create() 同一套 "<挂载点>/<文件名>" 解析、同一套"挂载点
 * 必须真的是一个 LMFS 卷"的防御性检查（见 lmfs_vfs_create() 声明
 * 上方注释，这里不重复）。`path` 指向的文件必须已经存在（通过
 * lmfs_vfs_create() 或者等价方式）——跟 fat_write() 一样，本函数
 * 不会顺便新建目录项，找不到就是一个普通的失败。
 *
 * 这个项目目前还没有把交互式 `write` 编辑器命令（kernel/shell/cmd/
 * write.c）接到 LMFS 上——那是一段已经叠了好几层历史修复、专门
 * 针对 FAT 路径写的大文件，今天的验收标准（"写入 10KB 文件并验证"）
 * 本身也更像是一次程序化的数据完整性验证，不是"在编辑器里手打
 * 10KB"，所以 Day 39 的验证走的是 kmain.c 的内核自测段，直接调用
 * lmfs_write()/本函数，而不是通过交互式编辑器。真要把 LMFS 接进
 * 交互式 `write` 命令，是以后哪天专门去改 write.c 的事，本函数已经
 * 把路径语法这一层备好了。
 *
 * 实际查找 inode 号、调用底层 lmfs_write() 的工作全部转发给 lmfs.c
 * ——本函数只做路径解析 + 挂载点校验这两件事。
 *
 * 成功返回 0，失败返回 -1——跟 lmfs_vfs_create() 一致的 int 0/-1
 * 约定。
 */
int lmfs_vfs_write(const char *path, const void *buf, uint32_t size);

/* lmfs_vfs_open_path(path, out_node)
 *
 * DAY40 (第二月 · 第6周 · Day 40 of the 5.0 filesystem roadmap):
 *   "实现 read()。" 验收："read bigfile.txt 正常。"
 *
 * fat_open()（kernel/fs/fat/fat_vfs.c，Day 18）在 LMFS 这边的对应
 * 角色——Shell 的 `read` 命令（kernel/shell/cmd/read.c）自己并不
 * 认识 LMFS，它只认识一个"给路径、还我一个 vfs_node_t"的入口；
 * read.c 内部按挂载点的 fs->name 判断该调用 fat_open() 还是这个
 * 函数（dispatch 发生在 read.c 里，不是这里——这里只负责"如果真的
 * 要我处理，我怎么处理"，跟 lmfs_vfs_create()/lmfs_vfs_write() 的
 * 分工完全一样）。
 *
 * `path` 同一套 "<挂载点>/<文件名>" 语法；挂载点必须是一个 LMFS 卷；
 * 文件名在当前目录（固定是 LMFS 根目录——见 lmfs_vfs_create() 声明
 * 上方"这个项目现在还没有真正的多级目录"的说明）下必须真实存在
 * （内部调用 Day 39 新增的 lmfs_lookup()）。
 *
 * 成功时 *out_node 被填成一个完整可用的 vfs_node_t：name（从目录项
 * 读来的文件名，按 vfs_node_t::name[32] 的容量截断——理由跟
 * lmfs_vfs_readdir() 对同一件事的处理一致，见该函数的注释）、type
 * （VFS_TYPE_FILE 或 VFS_TYPE_DIR，根据 inode 的 type 字段换算）、
 * size（inode 的 file_size）、fs（这次挂载的 filesystem_t*）、
 * fs_private（inode 号，cmd_read() 自己不关心这个字段的具体含义，
 * 只是原样存着，later 传给 fs->open()/fs->read() 用）。
 *
 * 成功返回 0，失败返回 -1（路径语法不对、挂载点不是 LMFS 卷、文件
 * 不存在）。
 */
int lmfs_vfs_open_path(const char *path, vfs_node_t *out_node);

/* lmfs_vfs_delete(path)
 *
 * DAY41 (第二月 · 第6周 · Day 41 of the 5.0 filesystem roadmap):
 *   "实现 delete() 与空间回收。" 验收："rm 后空间恢复。"
 *
 * fat_delete_file()（Day 26）在 LMFS 这边的路径语法入口——同一套
 * "<挂载点>/<文件名>"解析 + 挂载点必须真的是 LMFS 卷的防御性检查，
 * 跟 lmfs_vfs_create()/lmfs_vfs_write()/lmfs_vfs_open_path() 完全
 * 一样，不再重复。实际的删除+空间回收工作全部转发给 Day 41 新增的
 * lmfs_delete()（kernel/fs/lmfs/lmfs.c）——本函数只做路径解析这一层。
 *
 * 成功返回 0，失败返回 -1。
 */
int lmfs_vfs_delete(const char *path);

/* ===================================================================
 * DAY42 (第二月 · 第6周 · Day 42 of the 5.0 filesystem roadmap):
 *   "多级目录与路径解析。" 验收："可以创建并访问多级目录。"
 *
 * 本文件里 lmfs_vfs_create/write/open_path/delete 四个入口上方各自的
 * 注释里反复出现的那句"父目录固定是 LMFS 根目录""文件名部分也不能再
 * 包含 '/'"——Day 42 起【不再成立】：path 的语法现在是
 *
 *       <挂载点>[/<目录>/<目录>/.../<最后一段>]
 *
 * 比如 "IDE3/docs/notes/a.txt"。内部统一先在第一个 '/' 处切出挂载点
 * （必须是 LMFS 卷），其余部分是"相对于卷根目录"的路径，交给
 * lmfs_resolve_path()/lmfs_resolve_parent()（lmfs.h，Day 42）逐段解析：
 *   - create/mkdir/delete：解析到"最后一段的父目录"，再对父目录调用
 *     lmfs_create()/lmfs_mkdir()/lmfs_delete()；
 *   - write/open_path：解析到最后一段本身的 inode。
 * 路径里的 "."、".."、重复的 '/' 在这一层都能处理（".." 走磁盘上真实
 * 的 ".." 目录项）；Shell 层的 shell_resolve_path()（shell.c）会在传进来
 * 之前先把"当前目录"拼上并做文字层面的归一化，所以这里收到的通常已经
 * 是干净的绝对路径，但直接传带 ".." 的路径同样能正确工作。
 *
 * 返回值约定、"挂载点必须真的是 LMFS 卷"的防御性检查，全部沿用
 * Day 38~41 的，不再重复。
 * =================================================================== */

/* lmfs_vfs_mkdir(path)
 *
 * DAY42: 在 `path`（语法见上方 DAY42 说明）指定的位置新建一个子目录：
 * 最后一段是新目录的名字，它前面的部分必须是已经存在的目录。新目录
 * 自带 "."/".." 两条目录项（见 lmfs.h::lmfs_mkdir()）。
 * 成功返回 0；挂载点不是 LMFS 卷、父目录不存在或不是目录、重名、
 * 名字非法（含保留名 "."/".."）、父目录已满、底层失败，返回 -1。 */
int lmfs_vfs_mkdir(const char *path);

#endif /* LUMA_LMFS_VFS_H */
