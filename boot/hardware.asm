; BIOS/CSM USB stage 1. The MBR partition starts after the boot payload.
[bits 16]
[org 0x7c00]
    jmp 0:start
start:
    cli
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov sp,0x7c00
    sti
    cld
    mov [drive],dl
    mov ah,0x41
    mov bx,0x55aa
    int 0x13
    jc error
    cmp bx,0xaa55
    jne error
    test cx,1
    jz error
    mov si,dap
    mov dl,[drive]
    mov ah,0x42
    int 0x13
    jc error
    mov dl,[drive]
    jmp 0:0x9000
error:
    mov si,msg
.next:
    lodsb
    test al,al
    jz .stop
    mov ah,0x0e
    int 0x10
    jmp .next
.stop:
    cli
    hlt
    jmp .stop
drive: db 0
msg: db 'MyOS: BIOS LBA read failed',0
align 4
dap: db 16,0
    dw 32
    dw 0x9000,0
    dq 1
times 446-($-$$) db 0
; An active MyFS partition; no kernel writes to this partition in live mode.
    db 0x80,0xfe,0xff,0xff,0x7f,0xfe,0xff,0xff
    dd ROOT_LBA,ROOT_SECTORS
times 510-($-$$) db 0
    dw 0xaa55
