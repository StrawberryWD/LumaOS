; boot/loader.asm - LumaOS 5.0 Second-Stage Loader
; Loaded at 0x7E00 by MBR (4 sectors = 2048 bytes)
;
; DAY59 - ONE SOURCE, TWO BOOT PATHS:
;   nasm -f bin boot/loader.asm             -> loader.bin  (hard disk: MBR loads it at 0x7E00,
;                                              kernel = 384 sectors of 512 B from LBA 6)
;   nasm -f bin -DCDBOOT boot/loader.asm    -> cdboot.bin  (CD-ROM, El Torito "No-Emulation":
;                                              the BIOS itself loads this file to 0x7C00 and
;                                              jumps there; there is no MBR in this path)
;   The CD variant differs in exactly three places: (1) it is its own boot sector (ORG 0x7C00, a
;   BOOT INFO TABLE at offset 8 that tools/mkiso.py fills in), (2) the kernel is read from the
;   ISO by the LBA/length in that table, in 2048-byte sectors, 16 sectors (32 KB) per INT 13h
;   call, retried 3 times (CD drives are slow to spin up), (3) the banner. Everything else - A20,
;   protected mode, the jump to 0x10000 - is shared.
;   BOTH variants leave a BOOT INFO block at physical 0x0600 for the kernel:
;       +0 "LBI1"  +4 BIOS drive  +5 media (0 = hard disk, 1 = CD-ROM)  +8.. copy of the CD table
;         (kernel LBA, kernel bytes, LMFS LBA, LMFS bytes, total sectors; zeros on a hard disk)
;
; FIX BOOT-1  : cl=7  (was cl=6 → CHS mismatch, loaded wrong sector) [superseded, see DAY1-FIX]
; FIX ENTRY-1 : al=128 sectors = 64 KB
;               kernel.bin > 32 KB; original al=64 truncated .rodata/.data
;
; DAY1 (LumaOS 5.0 roadmap - Week 1, Day 1):
;   Read count raised from 128 to 255 sectors (127.5 KB instead of
;   64 KB), paired with build.sh's MAX_LOAD bump to 131072, so the
;   upcoming filesystem/driver code (~40-50 KB) still fits.
;
; DAY1-FIX (switching run.sh from if=floppy to if=ide broke boot):
;   CHS (ch/cl/dh) addressing is geometry-dependent — a floppy's
;   CHS geometry (2 heads, 18 sectors/track) is NOT the same as the
;   CHS translation SeaBIOS applies to the same image once it is
;   attached as an IDE disk. The old fixed ch=0/cl=7/dh=0 pointed at
;   LBA 6 under floppy geometry but at the wrong physical sector
;   under IDE geometry, producing "DISK ERROR".
;
;   Fix: switch to INT 13h/AH=42h (BIOS Extended Read, "Disk Address
;   Packet"), which addresses sectors purely by LBA and ignores
;   CHS geometry entirely. This works identically for floppy, IDE,
;   and (later) CD-ROM emulation — no more geometry coupling. This
;   also happens to be the same mechanism the roadmap has scheduled
;   for Day 59 (CD boot), so this is a small pull-forward.
;
;   We first probe AH=41h (INT 13h Extensions Installation Check);
;   if unsupported we halt with a clear error rather than silently
;   falling back to a CHS path that would just fail again.
;
; DAY1-FIX2 (single 255-sector AH=42h call still failed):
;   Confirmed by booting in QEMU with debug instrumentation: this
;   SeaBIOS (1.16.3) caps a single INT 13h/AH=42h transfer at 128
;   sectors (64 KB) regardless of the count field in the DAP — a
;   127/128-sector request succeeds, a 200/255-sector request fails
;   with CF=1. This is a common real-world BIOS limitation, not
;   specific to this project. Fix: split the 256-sector (128 KB)
;   read into two back-to-back 128-sector calls (dap1, dap2),
;   loading into 0x1000:0000 and 0x2000:0000 respectively so the
;   two halves land contiguously in physical memory. Verified in
;   QEMU with a 130 KB test payload: markers at the start of the
;   image and past the 64 KB boundary both landed correctly.

BITS 16
%ifdef CDBOOT
ORG  0x7C00
%else
ORG  0x7E00
%endif

start:
%ifdef CDBOOT
    jmp  short cd_entry
    times 8 - ($ - $$) db 0
cd_table:                           ; offset 8: BOOT INFO TABLE, patched by tools/mkiso.py
    db   "LCDB"
    dd   0                          ; +4  PVD LBA (16)
    dd   0                          ; +8  this image's LBA
    dd   0                          ; +12 this image's length (bytes)
    dd   0                          ; +16 kernel LBA
    dd   0                          ; +20 kernel bytes
    dd   0                          ; +24 LMFS image LBA
    dd   0                          ; +28 LMFS image bytes
    dd   0                          ; +32 total sectors of the ISO
cd_entry:
    jmp  0x0000:cd_start            ; BIOSes differ (0000:7C00 or 07C0:0000): normalise CS
cd_start:
%endif
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  sp, 0x7C00
    mov  [boot_drive], dl
    mov  si, banner
    call print16

    ; ---- Enable A20 (3-method cascade) ----
    call enable_a20_bios
    call check_a20
    jnz  a20_ok
    call enable_a20_kbd
    call check_a20
    jnz  a20_ok
    call enable_a20_fast
a20_ok:
    mov  si, a20_msg
    call print16

    ; ---- Check INT 13h Extensions are present (AH=41h) ----
    ; DAY69: a BIOS without INT 13h extensions (old machines / some VM BIOSes) used to stop here with
    ;        "NO INT13 EXT". On a HARD DISK we can still boot: fall back to the classic CHS read
    ;        (AH=02h, works on every BIOS since 1983) — see load_chs below. A CD cannot: El Torito
    ;        no-emulation gives no CHS geometry, so the CD build keeps the old error.
    ;        Build with -DFORCE_CHS (LUMAOS_FORCE_CHS=1 ./build.sh) to skip the AH=41h probe and always
    ;        use the CHS path — that is how tests/day69_test_chs.py proves the fallback in QEMU, whose
    ;        SeaBIOS always has the extensions.
%ifdef FORCE_CHS
%ifndef CDBOOT
    mov  byte [use_chs], 1
    jmp  ext_checked
%endif
%endif
    mov  ah, 0x41
    mov  bx, 0x55AA
    mov  dl, [boot_drive]
    int  0x13
%ifdef CDBOOT
    jc   ext_missing
    cmp  bx, 0xAA55
    jne  ext_missing
%else
    jc   .no_ext
    cmp  bx, 0xAA55
    jne  .no_ext
    test cx, 1                              ; bit0 = "extended disk access (AH=42h-44h)" supported
    jnz  ext_checked
.no_ext:
    mov  byte [use_chs], 1
%endif
ext_checked:

    ; ---- Load kernel via LBA extended read (AH=42h), two calls ----
    ; build.sh: dd seek=6 → kernel starts at LBA 6
    ; DAY1-FIX: LBA read replaces the old CHS read (ch/cl/dh), which
    ;           broke when run.sh switched the image from if=floppy
    ;           to if=ide (CHS geometry differs between the two).
    ; DAY1-FIX2: SeaBIOS's AH=42h path caps a single transfer at 128
    ;           sectors (64 KB) here — a request of 200+ sectors in
    ;           one DAP call fails with CF=1 (confirmed by booting in
    ;           QEMU: 127/128 sectors succeed, 200/255 do not). So the
    ;           DAY1 target of 255 sectors (127.5 KB) is split into
    ;           two back-to-back 128-sector calls (dap1, dap2) instead
    ;           of one big one — same total capacity, stays inside the
    ;           per-call limit.
%ifdef CDBOOT
    ; ---- CD: kernel from the ISO by the Boot Info Table (2048-byte sectors) ----
    cmp  dword [cd_table], 0x4244434C       ; "LCDB"
    jne  cd_bad_table
    mov  eax, [cd_table + 20]               ; kernel bytes
    add  eax, 2047
    shr  eax, 11                            ; -> 2048-byte sectors
    jz   cd_bad_table
    cmp  eax, 128                           ; 128 x 2048 = 256 KB = what build.sh allows (DAY64: was 96 = 192 KB)
    ja   cd_bad_table
    mov  [cd_left], eax
    mov  eax, [cd_table + 16]
    mov  [cd_lba], eax
    mov  word [cd_seg], 0x1000              ; physical 0x10000
.cd_loop:
    mov  eax, [cd_left]
    test eax, eax
    jz   .cd_done
    cmp  eax, 16
    jbe  .cd_n
    mov  eax, 16
.cd_n:
    mov  [cd_dap + 2], ax
    mov  bx, [cd_seg]
    mov  [cd_dap + 6], bx
    mov  ecx, [cd_lba]
    mov  [cd_dap + 8], ecx
    mov  byte [cd_try], 3
.cd_retry:
    mov  si, cd_dap
    mov  ah, 0x42
    mov  dl, [boot_drive]
    int  0x13
    jnc  .cd_ok
    xor  ax, ax                             ; reset the drive, try again
    mov  dl, [boot_drive]
    int  0x13
    dec  byte [cd_try]
    jnz  .cd_retry
    jmp  disk_err
.cd_ok:
    movzx eax, word [cd_dap + 2]
    add  [cd_lba], eax
    sub  [cd_left], eax
    shl  ax, 7                              ; sectors * 2048 / 16 = paragraphs
    add  [cd_seg], ax
    jmp  .cd_loop
.cd_done:
    jmp  kernel_loaded
%else
    cmp  byte [use_chs], 0
    jne  load_chs
    ; DAY70+: HD kernel read in 8 chunks of 64 sectors (32 KB) = 512 sectors = 256 KB, each chunk retried
    ;        3 times with a drive reset in between. Why: the old 4 x 128-sector calls are legal for AH=42h
    ;        but some BIOSes (VMware's among them in some configurations) reject or mis-handle >64
    ;        sectors per call; the failure landed in disk_err (cli;hlt), which VMware reports as
    ;        "The CPU has been disabled by the guest operating system". 64 sectors * 512 = 32 KB, and
    ;        a 32 KB chunk starting on a 32 KB-aligned segment never crosses a 64 KB DMA boundary.
    mov  word [hd_dap + 2], 64              ; sectors per call
    mov  word [hd_dap + 4], 0               ; offset 0
    mov  word [hd_dap + 6], 0x1000          ; segment: physical 0x10000
    mov  dword [hd_dap + 8], 6              ; LBA (build.sh: dd seek=6)
    mov  byte [hd_n], 8
.hd_loop:
    mov  byte [hd_try], 3
.hd_retry:
    mov  si, hd_dap
    mov  ah, 0x42
    mov  dl, [boot_drive]
    int  0x13
    jnc  .hd_ok
    xor  ax, ax                             ; reset disk, then try again
    mov  dl, [boot_drive]
    int  0x13
    dec  byte [hd_try]
    jnz  .hd_retry
    jmp  disk_err
.hd_ok:
    add  dword [hd_dap + 8], 64
    add  word  [hd_dap + 6], 0x800          ; 32 KB = 0x800 paragraphs
    dec  byte [hd_n]
    jnz  .hd_loop
    jmp  kernel_loaded

    ; ---- DAY69: CHS fallback (hard disk only) ----------------------------------------------------
    ; AH=08h gives the geometry the BIOS uses for this drive: CL[5:0] = sectors per track,
    ; DH = highest head number. Then for every one of the 512 kernel sectors (LBA 6..517):
    ;     track = LBA / spt, sector = LBA % spt + 1, head = track % heads, cylinder = track / heads
    ; and read it with AH=02h into ES:0000, advancing ES by 32 paragraphs (512 bytes) each time.
    ; One sector per call is the simplest thing that can never cross a track boundary (a CHS read
    ; must not) or a 64 KB DMA boundary (ES:0000 + 512 never straddles one), at the cost of 512 BIOS
    ; calls — a fraction of a second. Each sector gets 3 tries with a drive reset between them.
    ; CHS addressing caps at cylinder 1023 (about 8 GB at 255 heads x 63 sectors): irrelevant for a
    ; kernel living in the first megabyte.
load_chs:
    mov  ah, 0x08
    mov  dl, [boot_drive]
    xor  di, di
    int  0x13
    jc   disk_err
    mov  al, cl
    and  al, 0x3F                           ; sectors per track (1-based, max 63)
    jz   disk_err
    mov  [chs_spt], al
    inc  dh                                 ; heads = highest head + 1
    mov  [chs_heads], dh
    xor  ax, ax
    mov  es, ax                             ; AH=08h clobbers ES:DI on some BIOSes
    mov  dword [chs_lba], 6
    mov  word  [chs_left], 512
    mov  word  [chs_seg], 0x1000
.chs_next:
    mov  eax, [chs_lba]
    xor  edx, edx
    movzx ecx, byte [chs_spt]
    div  ecx                                ; eax = track number, edx = sector within track (0-based)
    inc  dl
    mov  bl, dl                             ; bl = sector (1-based)
    xor  edx, edx
    movzx ecx, byte [chs_heads]
    div  ecx                                ; eax = cylinder, edx = head
    mov  dh, dl                             ; dh = head
    mov  ch, al                             ; ch = cylinder bits 0-7
    mov  cl, ah
    and  cl, 3
    shl  cl, 6                              ; cl[7:6] = cylinder bits 8-9
    or   cl, bl                             ; cl[5:0] = sector
    mov  byte [chs_try], 3
.chs_retry:
    mov  dl, [boot_drive]
    mov  ax, [chs_seg]
    mov  es, ax
    xor  bx, bx
    mov  ax, 0x0201                         ; read 1 sector
    push cx
    push dx
    int  0x13
    pop  dx
    pop  cx
    jnc  .chs_ok
    push cx
    push dx
    xor  ax, ax                             ; reset the drive, then try again
    mov  dl, [boot_drive]
    int  0x13
    pop  dx
    pop  cx
    dec  byte [chs_try]
    jnz  .chs_retry
    jmp  disk_err
.chs_ok:
    add  word [chs_seg], 32                 ; 512 bytes = 32 paragraphs
    inc  dword [chs_lba]
    dec  word [chs_left]
    jnz  .chs_next
    xor  ax, ax
    mov  es, ax

%endif
kernel_loaded:
    mov  si, kernel_ok_msg
    call print16

    ; ---- DAY59: boot info block for the kernel at physical 0x0600 ----
    xor  ax, ax
    mov  es, ax
    mov  dword [es:0x0600], 0x3149424C      ; "LBI1"
    mov  al, [boot_drive]
    mov  [es:0x0604], al
%ifdef CDBOOT
    mov  byte [es:0x0605], 1
    mov  eax, [cd_table + 16]
    mov  [es:0x0608], eax
    mov  eax, [cd_table + 20]
    mov  [es:0x060C], eax
    mov  eax, [cd_table + 24]
    mov  [es:0x0610], eax
    mov  eax, [cd_table + 28]
    mov  [es:0x0614], eax
    mov  eax, [cd_table + 32]
    mov  [es:0x0618], eax
%else
    mov  byte [es:0x0605], 0
    mov  al, [use_chs]                      ; DAY69: 1 = the kernel was read through the CHS fallback
    mov  [es:0x0606], al
    xor  eax, eax
    mov  [es:0x0608], eax
    mov  [es:0x060C], eax
    mov  [es:0x0610], eax
    mov  [es:0x0614], eax
    mov  [es:0x0618], eax
%endif

    xor  ax, ax
    mov  es, ax
    lgdt [gdt_desc]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x08:pm_entry

enable_a20_bios:
    push ax
    mov  ax, 0x2401
    int  0x15
    pop  ax
    ret

enable_a20_kbd:
    call kb_wait_in
    mov  al, 0xAD
    out  0x64, al
    call kb_wait_in
    mov  al, 0xD0
    out  0x64, al
    call kb_wait_out
    in   al, 0x60
    push ax
    call kb_wait_in
    mov  al, 0xD1
    out  0x64, al
    call kb_wait_in
    pop  ax
    or   al, 2
    out  0x60, al
    call kb_wait_in
    mov  al, 0xAE
    out  0x64, al
    call kb_wait_in
    ret

enable_a20_fast:
    in   al, 0x92
    test al, 2
    jnz  .done
    or   al, 2
    and  al, 0xFE
    out  0x92, al
.done:
    ret

check_a20:
    push ds
    push es
    push di
    push si
    xor  ax, ax
    mov  es, ax
    mov  di, 0x0500
    mov  ax, 0xFFFF
    mov  ds, ax
    mov  si, 0x0510
    mov  al, [es:di]
    push ax
    mov  al, [ds:si]
    push ax
    mov  byte [es:di], 0x00
    mov  byte [ds:si], 0xFF
    mov  cx, 200
.delay: loop .delay
    mov  al, [es:di]
    cmp  al, [ds:si]
    pushf
    pop  bx
    pop  ax
    mov  [ds:si], al
    pop  ax
    mov  [es:di], al
    push bx
    popf
    pop  si
    pop  di
    pop  es
    pop  ds
    ret

kb_wait_in:
    push ax
.w: in   al, 0x64
    test al, 2
    jnz  .w
    pop  ax
    ret

kb_wait_out:
    push ax
.w: in   al, 0x64
    test al, 1
    jz   .w
    pop  ax
    ret

disk_err:
    mov  si, disk_msg
    call print16
.h: cli
    hlt
    jmp  .h

%ifdef CDBOOT
cd_bad_table:
    mov  si, cdtbl_msg
    call print16
.h: cli
    hlt
    jmp  .h
%endif

ext_missing:
    mov  si, ext_msg
    call print16
.h: cli
    hlt
    jmp  .h

print16:
    push ax
    push bx
.lp:
    lodsb
    test al, al
    jz   .done
    mov  ah, 0x0E
    xor  bx, bx
    int  0x10
    jmp  .lp
.done:
    pop  bx
    pop  ax
    ret

boot_drive      db 0
%ifdef CDBOOT
banner          db "LumaOS 5.0 Loader (CD-ROM, El Torito No-Emulation)",13,10,0
cdtbl_msg       db "BAD BOOT INFO TABLE (build the ISO with tools/mkiso.py)",13,10,0
%else
banner          db "LumaOS 5.0 Loader",13,10,0
%endif
a20_msg         db "A20 OK",13,10,0
kernel_ok_msg   db "Kernel loaded",13,10,0
disk_msg        db "DISK ERROR",13,10,0
ext_msg         db "NO INT13 EXT",13,10,0

; ---- Disk Address Packets for INT 13h/AH=42h ----
; DAY1-FIX2: split into two 128-sector (64 KB) calls — this SeaBIOS
; caps a single AH=42h transfer at 128 sectors. Together they read
; 256 sectors = 128 KB, starting at LBA 6, into 0x1000:0000 linearly
; (physical 0x10000-0x2FFFF), matching build.sh's MAX_LOAD=131072.
%ifdef CDBOOT
ALIGN 4
cd_dap:
    db 0x10, 0
    dw 16                ; sectors (patched per call)
    dw 0x0000            ; offset
    dw 0x1000            ; segment (patched per call)
    dq 0                 ; LBA (patched per call)
cd_left         dd 0
cd_lba          dd 0
cd_seg          dw 0
cd_try          db 0
%endif

ALIGN 4
hd_dap:
    db 0x10, 0
    dw 64
    dw 0, 0x1000
    dq 6
hd_n   db 0
hd_try db 0

; DAY69: CHS fallback state (see load_chs)
use_chs         db 0
chs_spt         db 0
chs_heads       db 0
chs_try         db 0
chs_left        dw 0
chs_seg         dw 0
ALIGN 4
chs_lba         dd 0

ALIGN 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

BITS 32
pm_entry:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x00200000
    jmp  0x08:0x00010000

TIMES (4*512)-($-$$) db 0

%ifdef CDBOOT
    times 2048 - ($ - $$) db 0       ; the El Torito image is exactly 4 sectors of 512 B
%endif
