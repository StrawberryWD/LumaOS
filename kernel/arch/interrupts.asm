; kernel/arch/interrupts.asm - LumaOS 5.0 ISR/IRQ stubs
;
; ISR-1 (CRITICAL FIX): removed "pop esp" from both isr_common and
;   irq_common.  The original code pushed esp then popped it back,
;   restoring a stale (already-decremented) value into the ESP register
;   and corrupting the entire unwind sequence → guaranteed triple fault
;   on first interrupt.  Replaced with "add esp, 4" to skip the slot.

BITS 32
section .text

extern isr_handler
extern irq_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push dword 0
    push dword %1
    jmp  isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push dword %1
    jmp  isr_common
%endmacro

ISR_NOERR  0
ISR_NOERR  1
ISR_NOERR  2
ISR_NOERR  3
ISR_NOERR  4
ISR_NOERR  5
ISR_NOERR  6
ISR_NOERR  7
ISR_ERR    8
ISR_NOERR  9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
; DAY56: 向量 16~31 也要有入口（以前没有 IDT 项：这些异常会变成 #GP）。只有 #AC(17) 带错误码。
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; DAY50: vector 0x80 = system call (`int 0x80`).  Same frame as every other ISR: push a fake error
; code and the vector number, then share isr_common.  isr_handler() sees int_no == 0x80 and calls
; syscall_dispatch(); whatever it stores in ctx->eax is popped into eax before iret = the return value.
ISR_NOERR 128

isr_common:
    push eax
    push ecx
    push edx
    push ebx
    lea  eax, [esp + 16]        ; snapshot pre-push ESP for diagnostics
    push eax                    ; push esp_snapshot (never restored to %esp)
    push ebp
    push esi
    push edi
    xor  eax, eax
    mov  ax, ds
    push eax
    ; DAY56: 连 es/fs/gs 也一起保存、返回时原样恢复。Day 55 之前它们总等于 ds，所以返回时用 ds 的值重装
    ; 三个就够；Ring 3 的程序可以让 es/fs/gs 和 ds 不同（比如置 0），这样做就把它们改掉了。
    mov  ax, es
    push eax
    mov  ax, fs
    push eax
    mov  ax, gs
    push eax
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    cld                         ; DAY56: SysV ABI 要求进入 C 函数时 DF=0。中断门不会清 DF，用户程序（std）
                                ; 带着 DF=1 进来时，内核里的 rep movs/stos 会倒着跑、踩坏内存和中断帧。
                                ; 返回时 iret 会恢复被打断代码原来的 eflags（包括它的 DF）。
    push esp
    call isr_handler
    add  esp, 4
    pop  gs                     ; DAY56: 按压入的逆序恢复（gs 最后压入，在最低处）
    pop  fs
    pop  es
    pop  ds
    pop  edi
    pop  esi
    pop  ebp
    add  esp, 4                 ; FIX ISR-1: skip esp_snapshot — do NOT pop esp
    pop  ebx
    pop  edx
    pop  ecx
    pop  eax
    add  esp, 8
    iret

%macro IRQ 1
global irq%1
irq%1:
    cli
    push dword 0
    push dword (32 + %1)
    jmp  irq_common
%endmacro

IRQ  0
IRQ  1
IRQ  2
IRQ  3
IRQ  4
IRQ  5
IRQ  6
IRQ  7
IRQ  8
IRQ  9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

irq_common:
    push eax
    push ecx
    push edx
    push ebx
    lea  eax, [esp + 16]
    push eax
    push ebp
    push esi
    push edi
    xor  eax, eax
    mov  ax, ds
    push eax
    ; DAY56: 连 es/fs/gs 也一起保存、返回时原样恢复。Day 55 之前它们总等于 ds，所以返回时用 ds 的值重装
    ; 三个就够；Ring 3 的程序可以让 es/fs/gs 和 ds 不同（比如置 0），这样做就把它们改掉了。
    mov  ax, es
    push eax
    mov  ax, fs
    push eax
    mov  ax, gs
    push eax
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    cld                         ; DAY56: 同上
    push esp
    call irq_handler
    add  esp, 4
    pop  gs                     ; DAY56: 按压入的逆序恢复（gs 最后压入，在最低处）
    pop  fs
    pop  es
    pop  ds
    pop  edi
    pop  esi
    pop  ebp
    add  esp, 4                 ; FIX ISR-1: skip esp_snapshot
    pop  ebx
    pop  edx
    pop  ecx
    pop  eax
    add  esp, 8
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
