/* kernel/exec/elf.h
 * LumaOS 5.0 - ELF32 基础结构定义 + ELF Header 解析
 *
 * DAY43 (Week 7, Day 43 of the 5.0 roadmap):
 *   "学习并定义 ELF32 基础结构。"  验收："能解析 ELF Header。"
 *
 * ---------------------------------------------------------------------
 * 为什么要有 ELF？（今天的原理）
 * ---------------------------------------------------------------------
 *   Day 42 为止 LumaOS 只能“读写文件”；它不能“运行文件”。磁盘上的一个
 *   程序文件只是一串字节——内核必须回答三个问题才能运行它：
 *     1) 这串字节是不是一个可执行程序？目标 CPU 是不是我（i386）？
 *     2) 它的哪些字节要放进内存、放到哪个地址、放多大？（→ Program
 *        Header，Day 45 的 PT_LOAD）
 *     3) 放好之后 CPU 该从哪个地址开始执行？（→ e_entry，Day 46）
 *   ELF（Executable and Linkable Format）就是这三个问题的“标准答案
 *   格式”。LumaOS 的 `.sw`（Luma Software，计划.md “第 40 周”）在 Day
 *   278 之前就是“扩展名叫 .sw 的 ELF32 i386 可执行文件”，所以今天定义
 *   的结构就是整个 5.0 → 9.0 应用平台的地基。
 *
 *   一个 ELF 文件的总体布局（我们只关心“可执行视图”）：
 *
 *       偏移 0   +----------------------+
 *                | ELF Header   (52 B)  |  <- 今天解析它
 *     e_phoff    +----------------------+
 *                | Program Header 表    |  <- Day 45：每项 32 B，告诉内核
 *                |  (e_phnum 项)        |     “把文件的 [p_offset,+p_filesz)
 *                +----------------------+      拷到内存 p_vaddr 处”
 *                | 各段(segment)的内容  |
 *                |  .text .rodata .data |
 *     e_shoff    +----------------------+
 *                | Section Header 表    |  <- 链接器/调试器用，加载器可忽略
 *                +----------------------+
 *
 *   “Header 在文件开头、固定 52 字节、自己说明后面各表在哪”是这类格式
 *   的通用套路（和 FAT 的 BPB、LMFS 的超级块是同一个思想：先读一个固定
 *   大小、位置已知的“目录”结构，再按它的指示找别的东西）。
 *
 * ---------------------------------------------------------------------
 * 设计要点
 * ---------------------------------------------------------------------
 *   - 结构体定义与 ELF 规范（System V ABI, i386 supplement）逐字段一一
 *     对应，用 _Static_assert 锁死大小：Ehdr=52、Phdr=32、Shdr=40。
 *     以后任何人改错字段类型，编译期立刻报错，而不是运行期读到错位数据。
 *   - 解析（elf32_parse_header）与校验（elf32_check_header）分开：
 *       parse : “把字节按 ELF 规则翻译成 C 结构体”，只关心格式能不能读；
 *       check : “这个程序我能不能运行”，关心语义（CPU 架构、类型…）。
 *     elfinfo 命令需要“即使不能运行也把字段打印出来”，Day 44 的
 *     elf_load() 则会两步都做。
 *   - 多字节字段用显式小端逐字节组装（rd16/rd32），不靠“指针强转 +
 *     刚好 x86 是小端”。ELF 自己在 e_ident[EI_DATA] 里声明字节序，
 *     解析器必须先看那一位才有资格解释后面的整数——所以遇到大端文件
 *     我们返回 ELF_ERR_NOT_LE 而不是读出一堆错值。
 *   - 本文件不依赖 VFS / 磁盘 / 终端，只吃一块内存。因此可以直接在宿主
 *     机上编译做单元测试（tests/day43_elf_host_test.c）。
 *   - 不使用 64 位整数运算：32 位内核没有链接 libgcc，64 位乘除会变成
 *     对 __udivdi3 等符号的引用而链接失败。范围检查改用“先判上界再做
 *     减法”的写法避免溢出。
 *
 * ---------------------------------------------------------------------
 * DAY44 (Week 7, Day 44): "实现 elf_load()。" 验收："检查 Magic、打印
 * Entry，实现在 lmfs 上用 elf。"
 * ---------------------------------------------------------------------
 *   Day 43 只会“读 52 字节的头并翻译”。内核要真正运行一个程序，还得
 *   先回答一个更现实的问题：这个文件“值不值得被加载”——否则 Day 45 把
 *   段拷进内存时，一个手工拼出来的坏文件（段声称在文件之外、段的目标
 *   地址压在内核上、入口点不在任何代码段里……）就会让内核把垃圾写到
 *   不该写的地方。所以 elf_load() 是“加载的前半场：打开 → 读 → 验证 →
 *   产出一份已验证的 elf_image_t”；后半场（把 PT_LOAD 段拷进内存）是
 *   Day 45 的工作，Day 46 才会跳到入口。分三层，职责各自单一：
 *
 *     elf.c         纯内存、无 I/O：Header + 每个 Program Header 的解析与
 *                   校验（elf32_parse_phdr / elf32_check_phdrs），宿主机
 *                   可编译做单元测试（tests/day44_elf_host_test.c）。
 *     elf_loader.c  VFS 层：按路径打开文件（FAT 或 LMFS——靠 fs->name
 *                   分派，和 read/elfinfo 一致），把 file_t.pos 设到目标
 *                   偏移后读 Header 与 Program Header 表，再调用上面的
 *                   纯函数。它只依赖 VFS 的 open/read/close 这套通用接口，
 *                   所以同一份代码对 FAT12/16/32 与 LMFS 一视同仁——这就
 *                   是“实现在 lmfs 上用 elf”的含义。
 *     elfload 命令  把 elf_image_t 打印出来（Magic、Entry、段表…）。
 *
 *   “验证”具体检查什么（每一条都对应一种真实的损坏/攻击）：
 *     1. 不能有 PT_INTERP / PT_DYNAMIC —— 动态链接需要解释器与重定位，
 *        LumaOS 只做静态可执行文件。
 *     2. 每个非空 PT_LOAD：filesz <= memsz（差额 = .bss，加载时清零）；
 *        [p_offset, p_offset+filesz) 必须落在文件内（否则会读到文件之外
 *        的内容）；[p_vaddr, p_vaddr+memsz) 必须落在用户装载区
 *        [ELF_LOAD_MIN_ADDR, ELF_LOAD_MAX_ADDR)（否则程序能把自己的段
 *        “装”到内核/显存/帧缓冲上）；p_align>1 时 p_align 必须是 2 的
 *        幂且 p_vaddr 与 p_offset 对它同余（ELF 规范要求，也是 Day 54
 *        分页映射的前提）；各 PT_LOAD 的内存区间互不重叠。
 *     3. 至少有一个非空 PT_LOAD；e_entry 必须落在某个带 PF_X 的 PT_LOAD
 *        的“文件部分”里——入口点指向 .bss 或不可执行段没有意义。
 *   所有区间运算都写成“先比上界再做减法”，避免 32 位加法回绕（例如
 *   p_vaddr=0xFFFFFFF0、p_memsz=0x20 相加回绕成 0x10，会骗过天真的
 *   “vaddr+memsz <= MAX”检查）。
 *
 *   ELF_LOAD_MIN_ADDR = 16 MB：内核镜像在 0x10000（.bss 到 ~0x9A000），
 *   栈顶 0x200000，GUI 后备缓冲 0x300000、SVGA prev_frame 0x400000、额外
 *   终端池 0x500000~0x58A000 都在 16 MB 以下；传统 i386 用户空间基址
 *   0x08048000（约 128 MB）则远在其上。ELF_LOAD_MAX_ADDR = 240 MB：QEMU
 *   默认 -m 256，留 16 MB 余量。Day 53 开分页后，这两个常量会被“用户
 *   地址空间”的概念取代。
 */
/*
 * ---------------------------------------------------------------------
 * DAY45 (Week 7, Day 45): "解析 PT_LOAD。"  验收："能将程序段加载到内存。"
 * ---------------------------------------------------------------------
 *   Day 44 之后我们手里有一份“已验证、尚未装入内存”的 elf_image_t。
 *   今天做加载的后半场：按 Program Header 的指示，把文件里的字节**搬到
 *   p_vaddr 指定的内存地址**。Day 46 才会跳到入口去执行。
 *
 *   为什么必须放在“p_vaddr 那个地址”？
 *     .sw 是 ET_EXEC（固定地址的可执行文件，没有重定位）。链接器在生成它
 *     时已经把所有绝对地址（函数调用目标、全局变量地址、字符串地址…）
 *     算死成“程序被放在 p_vaddr 时”的值。放错 1 个字节，call/mov 就会
 *     指向别处。所以加载器没有选择权：段在文件的哪里（p_offset）和段在
 *     内存的哪里（p_vaddr）是两个独立的数，加载就是做一次
 *         内存[p_vaddr .. p_vaddr+p_filesz) = 文件[p_offset .. +p_filesz)
 *     的搬运。（LumaOS 现在没开分页，物理地址 == 线性地址，所以“映射”就
 *     是直接 memcpy；Day 53/54 开分页后同样的循环会变成“先给 p_vaddr 建
 *     页表映射、再拷贝”，本函数的结构不变。）
 *
 *   .bss 的清零（今天最容易写错的一点）：
 *     一个段有两个大小：p_filesz（文件里有多少字节）与 p_memsz（运行时要
 *     占多少字节），p_memsz >= p_filesz。差额 [filesz, memsz) 就是 .bss：
 *     C 语言保证“未初始化的全局/静态变量为 0”，但把一大片 0 存进文件是
 *     浪费，所以文件里不存，由加载器在内存里补 0。加载器**不能**假设
 *     那块内存本来就是 0——开机后的 RAM 里留着 BIOS/引导程序/上一个程序
 *     的残留。漏掉清零的后果非常隐蔽：程序偶尔读到“上一个程序的数据”，
 *     结果取决于运行顺序。所以每个段的步骤是：先拷文件部分，再把尾部
 *     清零。elfmap 命令在加载前会把每个段的目标区域先填成 0xA5，这样
 *     “清零漏了”会直接看成 a5 a5 a5……，而不是碰巧是 0。
 *
 *   “谁说了算”——信任边界：
 *     目标地址与长度只来自已验证的 elf_image_t；文件只提供“字节内容”。
 *     即使文件在 Day 44 校验之后被别的东西改过，最坏结果是程序内容变成
 *     垃圾，而不是内核内存被写坏。为了不盲信调用者传进来的结构（它可能被
 *     手工拼出来或被改过），elf32_load_segments() 开头会对 img 把 Header 与
 *     Program Header 重新校验一遍（几十次比较，几乎不要钱），并以重新
 *     校验的结果为准。
 *
 *   “回读校验”：
 *     每拷完一小块（512 字节）就立刻把目标内存读回来与源缓冲逐字节比较。
 *     这样“p_vaddr 处根本没有 RAM”（例如用 qemu -m 64 启动，而 0x08048000
 *     在 128 MB 处）会立刻变成 ELF_ERR_VERIFY，而不是“加载成功、跳过去后
 *     乱跑”。（QEMU 里不存在的物理地址：写入被丢弃，读回来是 0x00——本机
 *     QEMU 8.2 的 pc 机型实测；别的总线可能读到 0xFF，两种都会被发现。
 *     局限：① 若一个段的文件部分碰巧全是 0，读回的 0 与写入的 0 一致，
 *     检测不到——真实可执行文件的段里总有非 0 字节，所以实际不是问题；
 *     ② 真实硬件上 CPU 缓存可能让刚写入的值被读回，这个检查只是模拟器里
 *     的保险；真正的“有多少 RAM”要靠开机时用 BIOS int 0x15/E820 探测，
 *     那是以后的工作。）目标内存用 volatile 指针访问，防止编译器认为
 *     “刚写进去的值当然读得回来”而把比较优化掉。
 *
 *   “要么全成功，要么不算加载”：
 *     elf_image_t.loaded 在加载开始时置 0，只有所有 PT_LOAD 都完成并通过
 *     回读校验后才置 1。中途失败时，已经写过的内存没有回滚（目标区域是
 *     内核不使用的用户装载区，留着垃圾不会伤害内核），但 loaded 仍是 0，
 *     Day 46 的 elf_run() 将拒绝执行 loaded==0 的映像。
 *
 *   模块分层与 Day 44 完全一致：
 *     elf.c         elf32_load_segments()：纯内存，字节从一个“读函数”
 *                   (elf_read_fn) 来——宿主机单元测试里它读内存数组，
 *                   内核里它读 VFS 文件（tests/day45_elf_host_test.c 用
 *                   mmap 把 0x08048000 映射出来，真的把段装进去并逐字节验证）。
 *     elf_loader.c  elf_load_segments(path, img, st)：打开文件（FAT 或
 *                   LMFS，与 elf_load 同一套 elf_open），把 file_t 包成
 *                   elf_read_fn，调用上面的纯函数。
 *     elfmap 命令   加载并展示结果：段表、读回的字节、校验和、.bss 是否清零。
 */
/*
 * ---------------------------------------------------------------------
 * DAY46 (Week 7, Day 46): "实现 elf_run()。"  验收："hello.sw 可以执行。"
 * ---------------------------------------------------------------------
 *   Day 45 之后，程序的字节已经躺在 p_vaddr 处的内存里，但那只是“一堆
 *   数据”——CPU 不会自己去执行它。让 CPU 执行一段内存，只有一个办法：
 *   把 EIP（指令指针）设成那段内存里某条指令的地址。elf_run() 就是做这
 *   件事：把 EIP 设成 e_entry。x86 里设置 EIP 的指令是 jmp/call/ret/iret，
 *   我们用 call，理由见下。
 *
 *   为什么是 call 而不是 jmp？
 *     jmp 过去就再也回不来了——程序结束时内核没有任何办法拿回控制权。
 *     call 会把“返回地址”压栈，程序最后执行一条 ret，CPU 就从栈里取出这个
 *     地址跳回内核。于是“运行一个程序”在今天就等价于“调用一个函数”：
 *         int code = entry(api);
 *     这是整个 Day 46 ~ Day 52 的主线，每一天只把这条调用线加固一点：
 *       Day 46  call e_entry（程序用 ret 返回；共用内核栈）          ← 今天
 *       Day 47  给程序独立的用户栈 + 统一的“退出路径”（程序返回/退出都回到
 *               同一个地方，而不是依赖它恰好 ret）
 *       Day 50~52  int 0x80 系统调用取代 api 函数表；sys_exit 取代 ret
 *       Day 55  iret 降到 Ring 3：同样是“存内核现场 → 进入 → 回来”，
 *               只是进入的方式从 call 换成 iret
 *
 *   进入程序之前内核必须做的三件事（elf_enter.asm 逐条实现）：
 *     1. 保存现场：程序会随便使用寄存器，内核的寄存器（尤其是 ebx/esi/edi/
 *        ebp 与 EFLAGS，段寄存器）必须原样还回去，否则 elf_run() 的调用者
 *        回来发现变量“自己变了”——这种 bug 离出事点极远，最难查。
 *     2. 满足调用约定：System V i386 ABI 要求进入 C 函数时 DF=0、esp 按
 *        16 字节对齐，参数按 cdecl 在栈上。程序是 gcc 编译的，它的代码
 *        默认这些前提成立。
 *     3. 记住“回来时 esp 应该是多少”：无论程序怎么折腾栈，回来后从保存值
 *        恢复，而不是相信程序把栈还原了。
 *
 *   “先全盘重新校验再跳”：elf_run() 不相信调用者传来的 elf_image_t。它既
 *   要求 loaded==1（段确实装进了内存），又把 Header/Program Header 再校验
 *   一遍并用校验结果重新确认入口点落在可执行段里——因为“跳转”是整个加载
 *   链里后果最严重的一步：EIP 一旦指错地方，CPU 会把任意内存当指令执行。
 *
 *   运行之后为什么 loaded 要清零：程序在运行中会改写自己的 .data/.bss（比如
 *   全局计数器），内存里的映像不再是“刚装载好的样子”。如果不清零，第二次
 *   elf_run() 会带着上一次的残留状态跑——和 Day 45 讨论的“.bss 不清零”是
 *   同一类隐蔽 bug。所以每次运行前必须重新 elf_load_segments()，这也正是
 *   `elfrun` 命令每次都完整走“load → map → run”三步的原因。
 *
 *   程序怎么“说话”：系统调用 Day 50 才有，今天内核把一张函数表（luma_api_t，
 *   见 elf_run.h）的地址作为参数传给入口。这是只有“程序与内核同在 Ring 0”
 *   才能成立的过渡方案，Day 50 起整张表被 int 0x80 取代。
 *
 *   分层：
 *     elf_enter.asm  存现场 -> call -> 恢复现场（唯一的汇编）
 *     elf_run.c      参数/loaded/重新校验 + 设置输出终端 + 调用 elf_enter
 *     elfrun 命令    load -> map -> run，打印退出码
 */
/*
 * ---------------------------------------------------------------------
 * DAY47 (Week 7, Day 47): "增加用户态栈和退出路径。"  验收："程序执行后返回 Shell。"
 * ---------------------------------------------------------------------
 *   Day 46 的程序是“借”内核的栈在跑，并且只有一种回来的方式：恰好执行一条
 *   ret。今天补上两样东西，让“运行一个程序”成为一件有边界、有出口的事。
 *
 *   1) 用户栈：程序自己的栈，不再借用内核栈。
 *        位置  [ELF_USER_STACK_BASE, ELF_USER_STACK_TOP) = [0x09FF0000, 0x0A000000)，
 *              64 KB，在“用户装载区”(16MB~240MB) 之内、离 0x08048000 的程序
 *              很远。栈向低地址生长，所以初始 esp 在 TOP 附近。
 *        为什么必须单独一块？① 内核栈只有 4 KB（entry.asm），程序随便一个
 *        递归或大局部数组就会冲过去、写坏内核的 .bss——而且是在内核毫不知情
 *        的情况下；② 内核栈上还躺着 elf_run()/shell 的返回地址和局部变量，
 *        程序栈与它们混在一起，程序里一个越界写就能把“回家的路”抹掉；③ 往后
 *        每个进程要有自己的栈（Day 54 独立地址空间、7.0 多任务），今天先把
 *        “程序的栈 ≠ 内核的栈”这条界线划出来。
 *        进入程序时 esp 怎么摆（i386 System V ABI）：
 *            TOP-16  api 指针（第一个 cdecl 参数）      <- 入口函数的 [esp+4]
 *            TOP-20  返回地址 = elf_exit_stub             <- 入口函数的 [esp]
 *        (esp+4) 按 16 字节对齐，与“被一次 call 调用进来”的布局一模一样。
 *        栈安全检查（今天没有分页，只能事后检查，Day 54 才有真正的保护）：
 *          - 运行前：把整块栈填成 0xA5 并逐字节读回（和 Day 45 的段回读校验同一
 *            招）；该地址没有 RAM（比如 qemu -m 150）-> ELF_ERR_NO_STACK，
 *            程序一条指令都不会执行；
 *          - 加载时：任何 PT_LOAD 不得与栈区间重叠 -> ELF_ERR_SEG_STACK；
 *          - 运行后：从栈底往上找第一个被改过的字节，得到“栈最深用到哪”
 *            （elfrun -v 显示）；栈底最低的 256 字节是“护栏”，如果护栏被写
 *            过，说明栈已经溢出到栈外 -> ELF_ERR_STACK_OVERFLOW。
 *            局限：一次性跨过护栏、直接写栈外远处的写入检测不到——那是 Day 54 的活。
 *
 *   2) 退出路径：不管程序怎么结束，都走“同一个出口”回到内核。
 *        路径 A  入口函数 ret：栈顶那个返回地址就是 elf_exit_stub，ret 一下就
 *                落进出口；eax = 退出码。
 *        路径 B  程序主动退出：调用 api->exit(code)（今天的过渡接口，Day 52 变成
 *                sys_exit）。它不会返回——不管此刻程序嵌套了几层函数、栈上压了多少
 *                东西，出口都直接把 esp 切回内核栈上保存的位置，把嵌套的栈帧整个
 *                丢掉（和 C 的 longjmp 同一个原理）。这就是“安全结束”：程序不需要
 *                一层层 return 回到入口。
 *        两条路径汇合到 elf_leave（elf_enter.asm）：用 ss 重载 ds/es → 记下是
 *        A 还是 B（elf_exit_via）→ esp 切回内核栈 → 恢复 Day 46 存下的全部现场
 *        → 返回到 elf_run() → 返回到 shell。所以“程序执行后返回 Shell”不再依赖
 *        程序“恰好写对了一条 ret”：栈是程序自己的，回家的路在内核栈上，程序碰不到
 *        (除非它故意乱写——Ring 0 下防不住，Day 55 之后才防得住)。
 *
 *   分层：elf_enter.asm 切栈/出口；elf_run.c 栈的准备与事后检查、信息汇总；
 *         elf.c 的 elf32_check_phdrs() 拒绝与栈重叠的段；elfrun -v 展示栈用量。
 */
/*
 * ---------------------------------------------------------------------
 * DAY48 (Week 7, Day 48): "Shell 集成 `run`。"  验收："`run hello.sw` 成功。"
 * ---------------------------------------------------------------------
 *   Day 43~47 做的是“引擎”：解析、验证、装入、跳进去、安全回来。每一块都用一个
 *   调试命令单独演示（elfinfo / elfload / elfmap / elfrun），它们是给内核开发者看的
 *   ——打印入口地址、段表、栈报告。今天给引擎装上“方向盘”：一个普通用户能用的
 *   命令 `run hello.sw`。这一步没有新的底层机制，但有三件容易忽略的事：
 *
 *   1) 把流水线收进一个函数 elf_exec()（elf_exec.c）：load -> map -> run 三步的顺序
 *      只写一次。shell 命令不该知道“要先验证再装入再跳转”这种内核内部的次序，
 *      就像 Unix 的 shell 只调 execve()，内核里怎么解析 ELF、建栈，shell 不管。
 *      elf_exec() 还告诉调用者“死在哪一步”（ELF_STAGE_*）：
 *          LOAD  文件有问题（没找到/不是 ELF/段非法），磁盘和内存都没动；
 *          MAP   内存有问题（那个地址没有 RAM），程序没跑；
 *          RUN   elf_run 拒绝（栈没有 RAM）——程序没跑；或栈溢出——程序已经跑完。
 *      `ran` 字段单独标出“程序是否真的执行过”，因为“栈溢出”是唯一一个“错误码非 0
 *      但程序其实执行过”的情况：此时程序的输出已经在屏幕上了、退出码是有效的。
 *
 *   2) 对用户的“话语风格”：Unix shell 的传统是“没消息就是好消息”。成功的程序
 *      退出码是 0，shell 什么都不多说；非 0 才补一行 "run: <path> exited with code N"。
 *      这样 `run hello.sw` 的屏幕上只有 "Hello from hello.sw!"，与 elfrun 那种每步
 *      都报告的风格形成对比。另外，程序最后一次输出不一定以 '\n' 结尾（`puts("hi")`），
 *      如果 shell 直接打印下一个提示符，就会变成 "hiIDE3/ >"。elf_run 在 api->puts/
 *      putchar 里顺手记下“一共输出了多少、最后一个字符是什么”（elf_run_info_t.out_total
 *      /out_last），run 据此补一个换行。
 *
 *   3) 只运行 .sw：计划.md 规定 LumaOS 的应用执行文件统一用 `.sw`。run 在读磁盘之前
 *      先看扩展名（不区分大小写），挡掉 `run notes.txt` 这类手滑。它不是安全边界——
 *      “内容是不是合法 ELF”依然由 elf_load() 验证，把一个 ELF 改名成 .sw 照样能运行，
 *      而真正的安全要等 Day 54/55 的分页与 Ring 3。elfrun 保留，不看扩展名，专供调试。
 *
 *   路径解析与其他命令完全一致（shell_resolve_path）：`run IDE3/hello.sw`、
 *   `run IDE0/HELLO.SW`、`cd IDE3` 之后 `run hello.sw`、`run kday42/../hello.sw`，
 *   FAT 与 LMFS 走同一份代码。Day 49 会让 `run hello.sw luma` 把参数传给程序。
 */
/*
 * ---------------------------------------------------------------------
 * DAY49 (Week 7, Day 49): "argc/argv。"  验收："`run hello.sw luma` 能读取参数。"
 * ---------------------------------------------------------------------
 *   Day 48 之前，程序拿到的唯一输入是 api 指针。今天让命令行上跟在程序名后面的词
 *   成为程序的输入：`run hello.sw luma` -> 程序看到 argc = 2、argv = {"hello.sw",
 *   "luma", NULL}。这是 Unix 自古以来的约定（K&R 的 main(int argc, char *argv[])），
 *   也是后面 cat.sw / ls.sw 能接收文件名（Day 75）、libc 的 main() 能工作（Day 71~74）
 *   的前提。
 *
 *   参数从哪来、怎么到程序手里——三个角色，各管一段：
 *     shell（run.c）  只负责“切词”：把 `hello.sw luma "two words"` 切成 3 个字符串。
 *                     它不关心程序怎么用这些字符串。
 *     elf_run_args()  负责“交付”：把字符串复制进程序自己的用户栈，造出 argv 数组，
 *                     把 argc 与 argv 作为入口函数的参数。
 *     程序            拿到的是自己栈上的一份拷贝——不是 shell 缓冲区的指针。
 *   为什么必须“复制进用户栈”而不是直接传 shell 里那几个字符串的指针？因为 shell 的
 *   缓冲区属于内核：程序跑起来之后，内核可能改写它；Day 54 起程序有自己的地址空间，
 *   根本看不到内核的缓冲区；程序结束后那些指针就悬空。复制一份放在程序的栈顶，
 *   这块内存从一开始就“属于程序”，将来分页/Ring 3 之后位置和内容都不必改。
 *   （Linux 的 execve 做的是同一件事：argv/envp 的字符串被内核搬到新进程栈的最顶端。）
 *
 *   用户栈顶部的布局（从高地址到低地址；F 是 16 字节对齐的“帧顶”）：
 *
 *        TOP    +-----------------------+
 *               | argv 字符串们 (每个以 NUL 结尾，紧挨着放)        |
 *               +-----------------------+
 *               | argv[0] argv[1] ... argv[argc-1] NULL (指针数组)  |
 *        F      +-----------------------+   <- 向下按 16 字节对齐
 *        F-4    | (空闲，置 0；argc==0 时 argv 指向这里的 NULL)      |
 *        F-8    | argv  (char **)           <- 入口函数的第 3 个参数 [esp+12]
 *        F-12   | argc  (int)               <- 第 2 个参数           [esp+8]
 *        F-16   | api   (luma_api_t *)      <- 第 1 个参数           [esp+4]
 *        F-20   | 返回地址 = elf_exit_stub  <- 入口时的 esp          [esp]
 *               +-----------------------+
 *   入口原型因此是  int _start(const luma_api_t *api, int argc, char **argv)，
 *   cdecl（参数从右到左压栈，调用者清栈）。Day 46~48 的程序只声明了第一个参数——
 *   完全兼容：cdecl 下多压的参数由调用者（内核出口）清理，被调用的程序不看就等于不存在。
 *   argc == 0 时（elfrun 调试命令、或 elf_run() 不带参数）参数块为空，F = TOP，
 *   帧位置与 Day 47 完全相同，argv 指向 TOP-4 处的 NULL。
 *
 *   约定与限制（常数见下）：
 *     - 对 `run`：argv[0] 是用户键入的程序路径（原样，如 "hello.sw"），argc >= 1；
 *     - argv[argc] == NULL（C 标准要求）；
 *     - 最多 ELF_ARGS_MAX_COUNT = 32 个参数，所有字符串（含结尾的 NUL）合计最多
 *       ELF_ARGS_MAX_BYTES = 2048 字节——超出就拒绝运行（ELF_ERR_ARGS，程序没跑），
 *       而不是悄悄截断（把参数截成另一个意思比报错更危险）；
 *     - 参数块占用栈空间，所以 `elfrun -v` 报告的栈用量里包含它。
 *   命令行切词规则（run.c）：以空格/Tab 分隔；双引号包住的部分是一个词（引号本身不进参数），
 *   `""` 是空字符串参数；没有转义字符；引号不成对 -> "unterminated quote"，不运行。
 *
 *   为什么“切词”不用 shell 原有的 argv（SHELL_ARG_LEN = 40、最多 16 个）？Day 43 PRE-3
 *   修 echo 时已经吃过这个亏：那套 argv 是给内置命令用的，有长度和个数上限，且不认引号。
 *   run 直接取原始命令行 shell_cmdline_args()，自己切，规则由 run 说了算。
 */
#ifndef LUMA_ELF_H
#define LUMA_ELF_H

#include <stdint.h>

/* ===== e_ident[]：文件最开头的 16 字节“身份证” ===== */
#define EI_NIDENT      16
#define EI_MAG0        0      /* 0x7F                         */
#define EI_MAG1        1      /* 'E'                          */
#define EI_MAG2        2      /* 'L'                          */
#define EI_MAG3        3      /* 'F'                          */
#define EI_CLASS       4      /* 1 = 32 位, 2 = 64 位         */
#define EI_DATA        5      /* 1 = 小端,  2 = 大端          */
#define EI_VERSION     6      /* 恒为 1 (EV_CURRENT)          */
#define EI_OSABI       7      /* 0 = System V                 */
#define EI_ABIVERSION  8
#define EI_PAD         9      /* 9..15 填 0                   */

#define ELFMAG0  0x7F
#define ELFMAG1  'E'
#define ELFMAG2  'L'
#define ELFMAG3  'F'

#define ELFCLASSNONE  0
#define ELFCLASS32    1
#define ELFCLASS64    2

#define ELFDATANONE   0
#define ELFDATA2LSB   1
#define ELFDATA2MSB   2

#define EV_NONE       0
#define EV_CURRENT    1

#define ELFOSABI_SYSV   0
#define ELFOSABI_LINUX  3

/* ===== e_type ===== */
#define ET_NONE  0
#define ET_REL   1   /* 可重定位目标文件 (.o)              */
#define ET_EXEC  2   /* 可执行文件（固定加载地址）← LumaOS 只接受这种 */
#define ET_DYN   3   /* 共享库 / 位置无关可执行文件 (PIE) */
#define ET_CORE  4

/* ===== e_machine ===== */
#define EM_NONE   0
#define EM_386    3    /* Intel 80386 ← LumaOS 唯一支持的架构 */
#define EM_X86_64 62

/* ===== Program Header: p_type / p_flags（Day 45 才真正使用，先定义） ===== */
#define PT_NULL     0
#define PT_LOAD     1   /* 需要装入内存的段 */
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_SHLIB    5
#define PT_PHDR     6
#define PT_TLS      7

#define PF_X  0x1
#define PF_W  0x2
#define PF_R  0x4

/* ===== Section Header: sh_type（只定义常用的几个） ===== */
#define SHT_NULL      0
#define SHT_PROGBITS  1
#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHT_NOBITS    8

/* ===== 三个核心结构体（packed：字段顺序/大小 = 磁盘上的字节布局） ===== */

/* ELF Header — 52 字节，永远在文件偏移 0 */
typedef struct {
    unsigned char e_ident[EI_NIDENT]; /*  0: 魔数 + 类别 + 字节序 + 版本…  */
    uint16_t      e_type;             /* 16: ET_*                           */
    uint16_t      e_machine;          /* 18: EM_*                           */
    uint32_t      e_version;          /* 20: 恒为 1                         */
    uint32_t      e_entry;            /* 24: 入口（程序第一条指令的虚拟地址）*/
    uint32_t      e_phoff;            /* 28: Program Header 表的文件偏移    */
    uint32_t      e_shoff;            /* 32: Section Header 表的文件偏移    */
    uint32_t      e_flags;            /* 36: 架构相关标志（i386 恒为 0）    */
    uint16_t      e_ehsize;           /* 40: 本头的大小，必须 = 52          */
    uint16_t      e_phentsize;        /* 42: 每个 Program Header 的大小 = 32*/
    uint16_t      e_phnum;            /* 44: Program Header 的个数          */
    uint16_t      e_shentsize;        /* 46: 每个 Section Header 的大小 = 40*/
    uint16_t      e_shnum;            /* 48: Section Header 的个数          */
    uint16_t      e_shstrndx;         /* 50: 段名字符串表所在的节序号       */
} __attribute__((packed)) elf32_ehdr_t;

/* Program Header — 32 字节/项。注意：32 位版 p_flags 排在 p_memsz 之后，
 * 64 位版把它提前到了 p_type 之后——照抄 64 位资料是经典的坑。 */
typedef struct {
    uint32_t p_type;     /* PT_*                                   */
    uint32_t p_offset;   /* 段内容在文件里的偏移                   */
    uint32_t p_vaddr;    /* 段应放到的虚拟地址                     */
    uint32_t p_paddr;    /* 物理地址（现代系统忽略）               */
    uint32_t p_filesz;   /* 段在文件里占多少字节                   */
    uint32_t p_memsz;    /* 段在内存里占多少字节 (>= filesz, 差额=.bss 清零) */
    uint32_t p_flags;    /* PF_R / PF_W / PF_X                     */
    uint32_t p_align;    /* 对齐                                   */
} __attribute__((packed)) elf32_phdr_t;

/* Section Header — 40 字节/项（链接/调试用；LumaOS 加载器不依赖它） */
typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} __attribute__((packed)) elf32_shdr_t;

_Static_assert(sizeof(elf32_ehdr_t) == 52, "ELF32 Ehdr must be 52 bytes");
_Static_assert(sizeof(elf32_phdr_t) == 32, "ELF32 Phdr must be 32 bytes");
_Static_assert(sizeof(elf32_shdr_t) == 40, "ELF32 Shdr must be 40 bytes");

#define ELF32_EHDR_SIZE  52u
#define ELF32_PHDR_SIZE  32u
#define ELF32_SHDR_SIZE  40u

/* ===== 返回码（Day 70 统一错误码之前，ELF 模块自己的一组负数） ===== */
#define ELF_OK                    0
#define ELF_ERR_NULL            (-1)   /* 空指针参数                          */
#define ELF_ERR_TOO_SHORT       (-2)   /* 数据不足 52 字节，装不下 ELF Header */
#define ELF_ERR_BAD_MAGIC       (-3)   /* 开头不是 7F 'E' 'L' 'F'             */
#define ELF_ERR_NOT_32BIT       (-4)   /* EI_CLASS != ELFCLASS32              */
#define ELF_ERR_NOT_LE          (-5)   /* EI_DATA  != ELFDATA2LSB             */
#define ELF_ERR_BAD_IDENT_VER   (-6)   /* EI_VERSION != 1                     */
#define ELF_ERR_BAD_VERSION     (-7)   /* e_version  != 1                     */
#define ELF_ERR_BAD_MACHINE     (-8)   /* e_machine  != EM_386                */
#define ELF_ERR_BAD_TYPE        (-9)   /* e_type     != ET_EXEC               */
#define ELF_ERR_BAD_EHSIZE     (-10)   /* e_ehsize   != 52                    */
#define ELF_ERR_BAD_PHENTSIZE  (-11)   /* e_phentsize != 32 (且 phnum > 0)    */
#define ELF_ERR_NO_PHDRS       (-12)   /* 可执行文件却没有 Program Header     */
#define ELF_ERR_PH_RANGE       (-13)   /* Program Header 表越出文件范围       */
#define ELF_ERR_NO_ENTRY       (-14)   /* e_entry == 0                        */
/* ---- DAY44: Program Header 校验 / elf_load() 的返回码 ---- */
#define ELF_ERR_TOO_MANY_PHDRS (-15)   /* e_phnum > ELF_MAX_PHDRS             */
#define ELF_ERR_DYNAMIC        (-16)   /* 含 PT_INTERP/PT_DYNAMIC（要动态链接）*/
#define ELF_ERR_NO_LOAD        (-17)   /* 没有任何非空 PT_LOAD                */
#define ELF_ERR_SEG_SIZE       (-18)   /* p_filesz > p_memsz                  */
#define ELF_ERR_SEG_FILE_RANGE (-19)   /* 段的文件内容越出文件                */
#define ELF_ERR_SEG_ADDR       (-20)   /* 段的内存区间不在用户装载区/回绕     */
#define ELF_ERR_SEG_ALIGN      (-21)   /* p_align 非 2 的幂，或 vaddr≢offset  */
#define ELF_ERR_SEG_OVERLAP    (-22)   /* 两个 PT_LOAD 的内存区间重叠         */
#define ELF_ERR_ENTRY_NOT_EXEC (-23)   /* e_entry 不在任何可执行 PT_LOAD 内   */
/* ---- DAY45: 把 PT_LOAD 装入内存时的返回码 ---- */
#define ELF_ERR_CHANGED        (-24)   /* 文件大小与校验时不同（被改过）      */
#define ELF_ERR_VERIFY         (-25)   /* 写入内存后读回不一致（该地址无 RAM?）*/
/* ---- DAY46: elf_run() 的返回码 ---- */
#define ELF_ERR_NOT_LOADED     (-26)   /* img->loaded != 1：段还没装入内存，拒绝执行 */
/* ---- DAY47: 用户栈 ---- */
#define ELF_ERR_SEG_STACK      (-27)   /* 某个 PT_LOAD 与用户栈区间重叠       */
#define ELF_ERR_NO_STACK       (-28)   /* 用户栈区间写后读回不一致（无 RAM?） */
#define ELF_ERR_STACK_OVERFLOW (-29)   /* 运行后栈底护栏被改写：栈溢出        */
#define ELF_ERR_OPEN           (-30)   /* 文件打不开（不存在/路径不对）       */
#define ELF_ERR_IS_DIR         (-31)   /* 路径是目录                          */
#define ELF_ERR_IO             (-32)   /* 读盘失败，或 Program Header 读不全  */
#define ELF_ERR_ARGS           (-33)   /* DAY49: 参数太多/太长/含 NULL 指针；程序没跑 */
#define ELF_ERR_NO_ASPACE      (-35)   /* DAY55: 没有（或不是）程序的用户地址空间：Ring 3 需要用户页和运行时页 */
#define ELF_ERR_NO_MEMORY      (-34)   /* DAY54: 页框池不够给程序建地址空间；程序没跑 */

/* elf32_parse_header(buf, len, out)
 *
 * 把 buf 开头的 52 字节按 ELF32 小端规则翻译进 *out。
 *   成功     -> ELF_OK，*out 的全部字段有效。
 *   失败时   ELF_ERR_NULL / ELF_ERR_TOO_SHORT / ELF_ERR_BAD_MAGIC：*out
 *            未必有意义（前两者没碰；BAD_MAGIC 时 e_ident 已拷入便于打印）。
 *            ELF_ERR_NOT_32BIT / ELF_ERR_NOT_LE：e_ident 已拷入 *out，
 *            其余字段不解释（字节序/字长都不对，解释出来没有意义）。
 * 只检查“能不能按 ELF32 小端读”，不检查能不能运行——后者见下面的 check。 */
int elf32_parse_header(const void *buf, uint32_t len, elf32_ehdr_t *out);

/* elf32_check_header(h, file_size)
 *
 * 检查一个已解析的 Header 描述的是不是“LumaOS 能运行的程序”：
 *   e_ident[EI_VERSION]==1, e_version==1, e_machine==EM_386,
 *   e_type==ET_EXEC, e_ehsize==52, e_entry!=0, 且 Program Header 表
 *   存在(e_phnum>0)、每项 32 字节、整张表落在 [0, file_size) 之内。
 * 返回 ELF_OK 或第一个发现的 ELF_ERR_*。
 * 只验证加载器真正依赖的部分；Section Header 表不检查（Linux 内核同样
 * 忽略它：它是给链接器和 readelf 看的，被 strip 掉也不影响运行）。 */
int elf32_check_header(const elf32_ehdr_t *h, uint32_t file_size);

/* 名字查询：给 elfinfo 打印用（readelf -h 风格）。未知值返回 "?"。 */
const char *elf_strerror(int err);
const char *elf_class_name(uint8_t v);
const char *elf_data_name(uint8_t v);
const char *elf_osabi_name(uint8_t v);
const char *elf_type_name(uint16_t v);
const char *elf_machine_name(uint16_t v);

/* ===================================================================
 * DAY44: Program Header 解析/校验 + 已验证镜像描述 elf_image_t
 * =================================================================== */

#define ELF_MAX_PHDRS      8            /* elf_image_t 最多容纳的 Program Header 数 */
#define ELF_LOAD_MIN_ADDR  0x01000000u  /* PT_LOAD 允许的最低虚拟地址 (16 MB)  */
#define ELF_LOAD_MAX_ADDR  0x0F000000u  /* PT_LOAD 区间的上界（不含）(240 MB)  */

/* DAY47: 用户栈。位于用户装载区内（160 MB 附近），程序段不得与它重叠。 */
#define ELF_USER_STACK_TOP    0x0A000000u   /* 栈顶（不含）；初始 esp 从这里往下 */
#define ELF_USER_STACK_SIZE   0x00010000u   /* 64 KB                               */
#define ELF_USER_STACK_BASE   (ELF_USER_STACK_TOP - ELF_USER_STACK_SIZE)
#define ELF_STACK_GUARD       256u          /* 栈底最低这么多字节是“护栏”           */
#define ELF_STACK_FILL        0xA5u         /* 运行前的填充值（用来测用量/护栏）    */

/* DAY49: 命令行参数（argv）的上限。 */
#define ELF_ARGS_MAX_COUNT    32u           /* 最多多少个参数（含 argv[0]）         */
#define ELF_ARGS_MAX_BYTES    2048u         /* 所有参数字符串（含各自的 NUL）合计   */

/* elf_load() 的产出：一份“已完整验证、尚未装入内存”的程序描述。
 * Day 45 的段加载器只需要读它，不必再校验。 */
typedef struct {
    uint32_t     file_size;              /* 文件字节数                       */
    elf32_ehdr_t ehdr;                   /* 已解析并通过 check 的 ELF Header */
    elf32_phdr_t phdr[ELF_MAX_PHDRS];    /* 前 ehdr.e_phnum 项有效           */
    uint32_t     n_load;                 /* 非空 PT_LOAD 的个数 (>= 1)       */
    uint32_t     load_lo;                /* 所有 PT_LOAD 的最低 p_vaddr      */
    uint32_t     load_hi;                /* 所有 PT_LOAD 的最高 p_vaddr+memsz（不含）*/
    uint32_t     total_filesz;           /* 各 PT_LOAD 的 p_filesz 之和      */
    uint32_t     total_memsz;            /* 各 PT_LOAD 的 p_memsz 之和       */
    uint32_t     entry_seg;              /* e_entry 所在的 phdr 下标         */
    uint32_t     loaded;                 /* DAY45: 1 = 所有 PT_LOAD 已装入内存并通过回读校验 */
} elf_image_t;

/* elf32_parse_phdr(buf, out)
 * 把 buf 开头的 32 字节按小端翻译成一个 Program Header（显式逐字节，
 * 不依赖对齐/字节序）。调用者保证 buf 至少 32 字节。 */
void elf32_parse_phdr(const void *buf, elf32_phdr_t *out);

/* elf32_check_phdrs(h, ph, n, file_size, img)
 * 校验 n 个 Program Header（规则见本文件 DAY44 注释），成功返回 ELF_OK
 * 并填好 img 的 n_load/load_lo/load_hi/total_filesz/total_memsz/entry_seg 以及 img->phdr
 * 的拷贝与 img->ehdr/file_size；失败返回第一个发现的 ELF_ERR_*（此时
 * img 的内容不保证有意义）。n 必须 <= ELF_MAX_PHDRS。 */
int elf32_check_phdrs(const elf32_ehdr_t *h, const elf32_phdr_t *ph, uint32_t n,
                      uint32_t file_size, elf_image_t *img);

const char *elf_ptype_name(uint32_t p_type);   /* "LOAD" "GNU_STACK" ... */

/* ===================================================================
 * DAY45: 把 PT_LOAD 段装入内存
 * =================================================================== */

/* 字节来源：读 ctx 所代表的文件里 [off, off+n) 的字节到 buf。
 * 返回实际读到的字节数（< n 表示文件在那里结束），出错返回负数。 */
typedef int (*elf_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t n);

/* 每次从文件读入并回读校验的块大小（栈上缓冲，512 字节）。 */
#define ELF_LOAD_CHUNK  512u

/* FNV-1a 32 位校验和的起始值与更新函数：elf32_load_segments 用它给每个段
 * 的“文件部分”算一个指纹，elfmap 与宿主机测试用同一个函数从内存/文件重算，
 * 三方一致才算数。h 传 ELF_FNV_SEED 开始，可连续喂多块。 */
#define ELF_FNV_SEED  0x811C9DC5u
uint32_t elf_fnv1a(uint32_t h, const void *buf, uint32_t n);

/* 加载统计（elf32_load_segments 的可选输出，可传 NULL）。 */
typedef struct {
    uint32_t segs;                       /* 已放入内存的非空 PT_LOAD 个数    */
    uint32_t bytes_copied;               /* 从文件拷入内存的字节数 (Σfilesz) */
    uint32_t bytes_zeroed;               /* 清零的 .bss 字节数 (Σ(memsz-filesz)) */
    uint32_t seg_sum[ELF_MAX_PHDRS];     /* 第 i 个 phdr 文件部分的 FNV-1a；非 LOAD 为 0 */
    uint32_t fail_addr;                  /* ELF_ERR_VERIFY：出错的内存地址   */
    uint32_t fail_want;                  /*   应该读到的字节                 */
    uint32_t fail_got;                   /*   实际读到的字节                 */
} elf_load_stats_t;

/* elf32_load_segments(img, rd, ctx, st)
 *
 * 把 img 描述的所有非空 PT_LOAD 装入内存（规则与理由见本文件 DAY45 注释）：
 *   0. 重新校验 img（Header + Program Header），以校验结果为准；失败则
 *      原样返回那个 ELF_ERR_*，一个字节都不写；
 *   1. 对每个 PT_LOAD：分块调用 rd() 读文件 [p_offset, +p_filesz)，拷到
 *      p_vaddr，并立刻读回比较（不一致 -> ELF_ERR_VERIFY，st->fail_*
 *      给出地址/期望值/实际值）；
 *   2. 把 [p_vaddr+p_filesz, p_vaddr+p_memsz)（.bss）清零并读回确认；
 *   3. 全部成功后 img->loaded = 1。
 * rd() 读不满（文件比 img 说的短）或返回负数 -> ELF_ERR_IO。
 * 返回 ELF_OK 或第一个失败原因。失败时 img->loaded == 0。
 *
 * 这是一个会写任意（但已被限定在 [ELF_LOAD_MIN_ADDR, ELF_LOAD_MAX_ADDR) 内）
 * 物理内存的函数：调用者必须保证那一段内存确实属于“用户装载区”、
 * 当前没有别的东西在用。 */
int elf32_load_segments(elf_image_t *img, elf_read_fn rd, void *ctx,
                        elf_load_stats_t *st);

#endif /* LUMA_ELF_H */