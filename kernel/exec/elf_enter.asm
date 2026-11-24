; kernel/exec/elf_enter.asm - LumaOS 5.0
;
; DAY46 (Week 7, Day 46): "实现 elf_run()。"  验收："hello.sw 可以执行。"
; DAY47 (Week 7, Day 47): "增加用户态栈和退出路径。" 验收："程序执行后返回 Shell。"
; DAY52 (Week 8, Day 52): "sys_exit。"  验收："程序可以安全结束。"
;     第三条出口路径 elf_sys_exit(code)：由 SYS_EXIT 系统调用的 C 处理函数调用（见 syscall.c），
;     与 api->exit 同样落进统一出口 elf_leave，只是 elf_exit_via = 2。它是在中断处理函数里被
;     调用的：此刻栈上叠着 `int 0x80` 的中断帧（CPU 压的 eip/cs/eflags + isr_common 压的现场）和
;     C 函数的栈帧，全都在用户栈上。出口一步把 esp 切回内核栈，这一整叠“半途而废的中断”被
;     原样丢掉——不需要 iret、不需要逐层返回。eflags（含 IF）由出口的 popfd 恢复成
;     elf_enter 入口时的值，所以“系统调用中途关了中断（中断门清 IF）”不会泄漏出去。
; DAY49 (Week 7, Day 49): "argc/argv。" 验收："`run hello.sw luma` 能读取参数。"
;
; int elf_enter(uint32_t entry, const void *api, uint32_t frame_top,
;               int argc, char **argv);                                  (cdecl)
; void elf_api_exit(int code);            程序主动退出（api->exit），永不返回
; extern uint32_t elf_exit_via;           上一次是怎么回来的：0 = 入口 ret，1 = api->exit
;
;   “真正跳进程序、再从统一出口回来”的汇编。C 语言没法可靠地表达下面这些事，
;   所以单独写在这里；elf_run.c 负责所有检查，这里只负责“切栈、进去、回来”。
;
;   ===== 进入（elf_enter）=====
;     1. [Day 46] 把内核现场存到内核栈：EFLAGS、8 个通用寄存器(pushad)、
;        ds/es/fs/gs。程序是别人写的，不能假设它守规矩，所以全存、回来全恢复。
;     2. [Day 46] 记下此刻的 esp 到 elf_saved_esp——这是“回家的路”，放在
;        内核栈上。回来时无论程序把自己的 esp 弄成什么样，都从这个值恢复。
;     3. [Day 47] 把 esp 切到程序自己的栈 stack_top（用户栈，见 elf.h 的 DAY47
;        注释）。从这一刻起程序的 push/call/局部变量都落在用户栈上，碰不到内核栈。
;     4. [Day 47] 在用户栈上摆出“好像被 call 进来”的样子（i386 System V ABI）：
;            stack_top-16   api 指针   <- 入口函数的 [esp+4]（第一个 cdecl 参数）
;            stack_top-20   elf_exit_stub  <- 入口函数的 [esp]（“返回地址”）
;        [Day 49] 再往上放 argc、argv 作为第 2、3 个参数：
;            frame_top-8    argv   <- [esp+12]
;            frame_top-12   argc   <- [esp+8]
;        frame_top 是“帧顶”：参数为空时它就是 stack_top；有参数时参数字符串和 argv
;        数组已由 elf_run.c 放在它的上面（见 elf.h 的 DAY49 注释）。
;        进入点处 (esp+4) 按 16 字节对齐，DF=0。然后用 jmp（不是 call）进入——
;        返回地址我们已经手工放好了，指向出口桩。
;
;   ===== 退出（两条路，一个出口 elf_leave）=====
;     路径 A  入口函数执行 ret -> 弹出栈顶的返回地址 = elf_exit_stub -> 落进出口，
;             eax = 程序的返回值。
;     路径 B  程序调用 api->exit(code)（= elf_api_exit）：不管它此刻嵌套了多少层
;             函数、栈上压了多少东西，直接去出口——栈帧不需要逐层返回，出口把 esp
;             一步切回内核栈，等于把整个用户栈的活动现场一次丢掉（C 的 longjmp 同理；
;             Day 52 它会变成 sys_exit）。
;     出口 elf_leave：
;        - 用 ss 把 ds/es 重载成内核平坦数据段（Ring 0 的程序可能把它们改坏，下面
;          读 elf_saved_esp 要用 ds）；
;        - 记录走了哪条路（elf_exit_via）；
;        - esp = elf_saved_esp（回到内核栈），退出码写进保存区的 eax 槽，
;          恢复 gs/fs/es/ds、popad、popfd、pop ebp、ret —— 于是 elf_enter 像一个
;          普通 C 函数一样“返回”给 elf_run()，值就是程序的退出码。
;
; DAY55: 程序现在在 Ring 3 运行（用 iret 进入，见 elf_enter 里的“iret 进入 Ring 3”）。
;   - 用户栈在程序自己的地址空间里（Day 54），页表项带 US 位；内核页没有 US，程序碰一下就 #PF。
;   - 程序没法再 call 内核函数：api 表在运行时页（urt.asm）里，函数是 Ring 3 stub，靠 int 0x80 回内核。
;   - int 0x80 / 中断 / 异常从 Ring 3 进内核时，CPU 自动把 ss:esp 换成 TSS.esp0（elf_enter 设成它当前的内核栈
;     位置），再压 ss/esp/eflags/cs/eip；SYS_EXIT 的出口仍然一步 esp = elf_saved_esp，丢掉这一整叠。
;   - 退出的三条路现在由内核按 int 0x80 的 eip 区分（urt_exit_via），不再有 elf_exit_stub / elf_api_exit。
;   （以下是 Day 46~54 的原始说明，“Ring 0”“jmp”“用户栈只是普通内存”等已被上面的 Day 55 取代。）
;
;   今天（Day 54 之前）程序仍在 Ring 0（与内核共用地址空间），所以“用户栈”只是一块被划出来的普通内存，
;   并没有硬件保护——程序故意乱写内核内存照样写得进去。Day 54 分页、Day 55 降到
;   Ring 3 之后这道界线才由硬件强制；那时 elf_enter 的“存现场 → 切栈 → 进入 → 统一
;   出口回来”骨架不变，只是 jmp 变成 iret、出口变成系统调用/异常入口。
;
;   不可重入：elf_saved_esp 只有一份。多任务（7.0）之后它会挪进每个进程的结构体。

BITS 32

section .text
global elf_enter
global elf_sys_exit
global elf_reenter
global elf_running
global elf_ret_stub_va
extern kernel_tss
global elf_exit_via

elf_enter:
    push ebp
    mov  ebp, esp
    pushfd                      ; [EFLAGS]
    pushad                      ; edi esi ebp esp ebx edx ecx eax（eax 在最高处）
    push ds
    push es
    push fs
    push gs
    ; 现在 esp 指向 gs；pushad 帧 = esp+16 .. esp+47，其中 eax 槽在 esp+16+28

    mov  eax, [ebp + 8]         ; entry
    mov  edx, [ebp + 12]        ; api（用户虚拟地址：运行时页里的表）
    mov  ecx, [ebp + 16]        ; frame_top（调用者保证 16 字节对齐）
    mov  ebx, [ebp + 20]        ; argc    （ebx/esi 这里随便用：出口会整体恢复）
    mov  esi, [ebp + 24]        ; argv
    mov  [elf_saved_esp], esp   ; 回家的路：记在内核栈上
    mov  [kernel_tss + 4], esp  ; DAY55: TSS.esp0 —— 程序（Ring 3）下次 int 0x80 / 中断时 CPU 把栈切到这里
    mov  dword [elf_exit_via], 0
    mov  dword [elf_running], 1 ; DAY52: “有程序在跑”——SYS_EXIT 只有此时才有地方可回
    mov  edi, [elf_ret_stub_va] ; 入口函数 ret 的返回地址：运行时页里的 Ring 3 stub

    ; ---- 在用户栈上摆出“被 call 进来”的样子（内核此刻还在 Ring 0，直接用用户栈写）----
    mov  esp, ecx
    sub  esp, 4                 ; [frame_top-4] 空闲（elf_run.c 已置 0）
    push esi                    ; [frame_top-8]  = argv   （第三个参数）
    push ebx                    ; [frame_top-12] = argc   （第二个参数）
    push edx                    ; [frame_top-16] = api    （第一个参数）
    push edi                    ; [frame_top-20] = 返回地址（入口函数 ret 到这里）
    mov  edi, esp               ; 程序的初始 esp：frame_top-20，(esp+4) 16 对齐
    mov  esp, [elf_saved_esp]   ; 切回内核栈，在它上面摆 iret 帧

    ; ===== DAY55: iret 进入 Ring 3 =====
    ; iret 从“当前（内核）栈”弹出 5 个 dword：eip, cs, eflags, esp, ss。弹出的 cs 的 RPL=3 与当前
    ; CPL=0 不同，CPU 就知道这是一次“降特权级”的返回：连 esp/ss 也一起弹出，并把 cs/ss 的 RPL 变成 CPL。
    push dword 0x3B             ; ss      = 用户数据段（GDT[7]，DPL 3，RPL 3）
    push edi                    ; esp     = 程序的用户栈指针
    push dword 0x202            ; eflags  = IF=1（可被中断），IOPL=0（Ring 3 不能 cli/in/out），DF=0
    push dword 0x23             ; cs      = 用户代码段（GDT[4]，DPL 3，RPL 3）
    push eax                    ; eip     = 入口点
    mov  dx, 0x3B               ; 数据段寄存器也换成用户数据段（Ring 3 不能加载 DPL 0 的段）
    mov  ds, dx
    mov  es, dx
    mov  fs, dx
    mov  gs, dx
    xor  eax, eax               ; 通用寄存器清零：别把内核的值漏给用户程序
    xor  ebx, ebx
    xor  ecx, ecx
    xor  edx, edx
    xor  esi, esi
    xor  edi, edi
    xor  ebp, ebp
    iretd                       ; ===== 进入 Ring 3 =====

; 路径 C（DAY52）：void elf_sys_exit(int code, int via) —— SYS_EXIT 系统调用；永不返回。
; DAY55: via 由 syscall.c 根据调用者 eip 判断（0 = 入口 ret 的 stub，1 = api->exit 的 stub，2 = 程序自己 int 0x80）。
; 只能在 elf_running == 1 时调用（syscall.c 检查）：否则 elf_saved_esp 是旧值，跳回去等于跳进垃圾。
; 此刻在内核栈（TSS.esp0）上，叠着 int 0x80 的中断帧；出口一步把 esp 换回 elf_enter 存的位置，整叠丢掉。
; DAY76: elf_reenter(entry, api, frame_top, argc, argv) —— execve 的“跳进去”。
; 和 elf_enter 的后半段一样，但不保存任何东西：elf_saved_esp 里仍是最初 elf_enter 记下的回家路，
; 所以新程序最终 exit 时回到的还是当初调用 elf_enter 的那个人（shell 的 run）。当前内核栈上
; （int 0x80 的中断帧、syscall 处理函数的局部变量）整个被丢弃：esp 直接换成 elf_saved_esp 再摆 iret 帧。
elf_reenter:
    mov  eax, [esp + 4]         ; entry
    mov  edx, [esp + 8]         ; api
    mov  ecx, [esp + 12]        ; frame_top
    mov  ebx, [esp + 16]        ; argc
    mov  esi, [esp + 20]        ; argv
    mov  dword [elf_exit_via], 0
    mov  edi, [elf_ret_stub_va]
    mov  esp, ecx
    sub  esp, 4
    push esi
    push ebx
    push edx
    push edi
    mov  edi, esp
    mov  esp, [elf_saved_esp]
    push dword 0x3B
    push edi
    push dword 0x202
    push dword 0x23
    push eax
    mov  dx, 0x3B
    mov  ds, dx
    mov  es, dx
    mov  fs, dx
    mov  gs, dx
    xor  eax, eax
    xor  ebx, ebx
    xor  ecx, ecx
    xor  edx, edx
    xor  esi, esi
    xor  edi, edi
    xor  ebp, ebp
    iretd

elf_sys_exit:
    mov  eax, [esp + 4]         ; code
    mov  ecx, [esp + 8]         ; via
    jmp  elf_leave

; 统一出口：eax = 退出码，ecx = 走的哪条路
elf_leave:
    mov  dx, ss                 ; 先把 ds/es/fs/gs 换回内核平坦数据段（Ring 3 程序里它们是 0x3B），
    mov  ds, dx                 ; 否则下面读写 [elf_saved_esp] 会用到用户段
    mov  es, dx
    mov  fs, dx
    mov  gs, dx
    mov  [elf_exit_via], ecx
    mov  dword [elf_running], 0 ; DAY52: 程序已结束；再来的 SYS_EXIT 无处可回 -> -ESRCH
    mov  esp, [elf_saved_esp]   ; ===== 切回内核栈 =====（无论用户栈上发生过什么）
    mov  [esp + 16 + 28], eax   ; 退出码 -> 保存区的 eax 槽
    pop  gs
    pop  fs
    pop  es
    pop  ds
    popad                       ; eax = 退出码；其余寄存器 = 进入前的值
    popfd                       ; 恢复 EFLAGS（包括 IF、DF）
    pop  ebp
    ret

section .bss
align 4
elf_saved_esp: resd 1
elf_exit_via:  resd 1
elf_running:   resd 1           ; DAY52: 1 = elf_enter 正在运行一个程序
elf_ret_stub_va: resd 1         ; DAY55: elf_run.c 在进入前设置：运行时页里 ret stub 的用户地址

section .note.GNU-stack noalloc noexec nowrite progbits
