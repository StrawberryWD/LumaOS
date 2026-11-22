; kernel/arch/entry.asm - LumaOS 5.0 Kernel Entry Point
; FIX ENTRY-3: push 2 (not 0) — EFLAGS bit1 must always be 1
; FIX ENTRY-4: use kernel_stack_top from BSS

BITS 32
extern kmain

section .text
    global _start

_start:
    cli
    mov  esp, kernel_stack_top
    xor  ebp, ebp
    push dword 0x00000002   ; EFLAGS: bit1=1, IF=0, all else 0
    popf
    ; DAY50: zero the rest of .bss (everything after the kernel stack, which entry.o puts first and
    ; which we are standing on).  The loader only copies .text/.rodata/.data, so .bss is whatever the
    ; RAM happened to hold; C code assumes "static T x;" starts as zero.
    extern _end
    mov  edi, kernel_stack_top
    mov  ecx, _end
    sub  ecx, edi
    add  ecx, 3
    shr  ecx, 2
    xor  eax, eax
    cld
    rep  stosd
    call kmain
    cli
.halt:
    hlt
    jmp  .halt

section .bss
    align 4096
kernel_stack:
    resb 4096
kernel_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
