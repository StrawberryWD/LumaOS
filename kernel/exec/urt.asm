; kernel/exec/urt.asm - LumaOS 5.0 - DAY55 "用户态运行时页"（user runtime page）的代码
;
; Day 55 之后程序在 Ring 3 运行，不能再直接调用内核函数。可是 Day 46~49 的程序入口约定是
;     int _start(const luma_api_t *api, int argc, char **argv)
; 并且通过 api->puts / api->putchar / api->exit 说话，入口函数 `ret` 就算退出。为了让这些程序
; 原样继续工作，内核在每个程序的地址空间里放一页只读、用户可读可执行的页（LUMA_URT_BASE，见 urt.h），
; 里面是下面这几段很小的用户态代码，它们全部用 `int 0x80` 回到内核——也就是说 api 表里的“函数”
; 现在是 Ring 3 里的 stub，真正干活的是系统调用（SYS_WRITE / SYS_EXIT）：
;     urt_puts     api->puts(s)      量出 s 的长度（上限 4096），write(1, s, len)
;     urt_putchar  api->putchar(c)   把 c 放在栈上，write(1, &c, 1)
;     urt_api_exit api->exit(code)   exit(code)
;     urt_ret_stub 入口函数 `ret` 的返回地址：eax -> exit(eax)
; 内核从 SYS_EXIT 的 eip 判断程序是怎么退出的（落在哪个 stub 里），所以 `run`/`elfrun -v`
; 仍能区分 ret / api->exit / sys_exit。
; 整块（urt_start .. urt_end）被原样拷进页框，代码里没有任何绝对地址，所以放在哪里都能跑。
; 表 urt_api 只占位，字段由 urt.c 在拷贝后填成用户虚拟地址。

BITS 32
section .text
global urt_start, urt_end, urt_api, urt_ret_stub, urt_ret_stub_end, urt_api_exit, urt_api_exit_end
global urt_puts, urt_putchar

urt_start:

urt_api:                        ; luma_api_t：magic, version, puts, putchar, exit（5 个 dword）
    times 5 dd 0

urt_puts:                       ; void puts(const char *s)   cdecl；ebx 是被调用者保存寄存器
    push ebx
    mov  ecx, [esp + 8]
    test ecx, ecx
    jz   .done
    xor  edx, edx
.len:
    cmp  edx, 4096
    jae  .write
    cmp  byte [ecx + edx], 0
    je   .write
    inc  edx
    jmp  .len
.write:
    mov  eax, 2                 ; SYS_WRITE
    mov  ebx, 1                 ; fd 1
    int  0x80
.done:
    pop  ebx
    ret

urt_putchar:                    ; void putchar(int c)
    push ebx
    push dword [esp + 8]        ; 字符放在栈上，取它的地址当缓冲区
    mov  ecx, esp
    mov  edx, 1
    mov  ebx, 1
    mov  eax, 2
    int  0x80
    add  esp, 4
    pop  ebx
    ret

urt_api_exit:                   ; void exit(int code) 不返回
    mov  ebx, [esp + 4]
    mov  eax, 1                 ; SYS_EXIT
    int  0x80
urt_api_exit_end:               ; 内核看到的 eip（int 之后）就是这里
    jmp  urt_api_exit_end       ; 不可达

urt_ret_stub:                   ; 入口函数 ret 到这里，eax = 返回值
    mov  ebx, eax
    mov  eax, 1
    int  0x80
urt_ret_stub_end:
    jmp  urt_ret_stub_end       ; 不可达

urt_end:

section .note.GNU-stack noalloc noexec nowrite progbits
