; boot/mbr.asm - LumaOS 5.0 Master Boot Record
; Loads loader from sectors 1-4 to 0x7E00 then jumps there
BITS 16
ORG  0x7C00

start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00
    sti

    mov  [boot_drive], dl

    ; Read 4 sectors (loader.bin) from sector 2 → 0x0000:0x7E00
    mov  ah, 0x02
    mov  al, 4
    mov  ch, 0
    mov  cl, 2
    mov  dh, 0
    mov  dl, [boot_drive]
    mov  bx, 0x7E00
    int  0x13
    jc   disk_err

    jmp  0x0000:0x7E00

disk_err:
    mov  si, msg
.pr:
    lodsb
    test al, al
    jz   .halt
    mov  ah, 0x0E
    int  0x10
    jmp  .pr
.halt:
    cli
    hlt
    jmp .halt

boot_drive db 0
msg        db "Disk error",13,10,0

TIMES 510-($-$$) db 0
DW 0xAA55