; Second stage stays below 64 KiB. BIOS reads into a 32 KiB bounce buffer;
; protected-mode copies load the kernel and root image above 1 MiB.
[bits 16]
[org 0x9000]
INFO equ 0x6000
FONT equ 0x5000
BOUNCE equ 0x70000
KERNEL equ 0x100000
ROOT equ 0x1000000
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
    mov si,loading
    call print
    ; Build an explicit, versioned boot contract (see kernel/bootinfo.h).
    mov di,INFO
    mov cx,64/2
    rep stosw
    mov dword [INFO],0x534f594d
    mov dword [INFO+4],1
    mov dword [INFO+8],ROOT
    mov dword [INFO+12],ROOT_SECTORS*512
    mov dword [INFO+16],FONT
    ; E820 records remain in the legacy ABI at 0x8000.
    mov di,0x8004
    xor ebx,ebx
    xor bp,bp
.map:
    mov eax,0xe820
    mov edx,0x534d4150
    mov ecx,24
    mov dword [di+20],1
    int 0x15
    jc .mapped
    cmp eax,0x534d4150
    jne .mapped
    jcxz .skip
    inc bp
    add di,24
.skip:
    test ebx,ebx
    jz .mapped
    cmp bp,128
    jb .map
.mapped:
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov [0x8000],bp
    mov word [0x8002],0
    ; Require usable E820 memory for every byte written above 1 MiB.
    mov eax,KERNEL
    mov edx,KERNEL_END
    call usable
    mov eax,ROOT
    mov edx,ROOT+ROOT_SECTORS*512
    call usable
    in al,0x92
    or al,2
    and al,0xfe
    out 0x92,al
    ; Verify A20 before copying into high memory.
    push ds
    mov ax,0xffff
    mov es,ax
    xor ax,ax
    mov ds,ax
    mov al,[0x500]
    mov ah,[es:0x510]
    push ax
    mov byte [0x500],0
    mov byte [es:0x510],0xff
    cmp byte [0x500],0xff
    pop ax
    mov [es:0x510],ah
    mov [0x500],al
    pop ds
    je a20_error
    xor ax,ax
    mov es,ax
    mov dword [destination],KERNEL
    mov dword [dap_lba],KERNEL_LBA
    mov dword [remaining],KERNEL_SECTORS
    call load
    mov dword [destination],ROOT
    mov dword [dap_lba],ROOT_LBA
    mov dword [remaining],ROOT_SECTORS
    call load
    ; Capture the BIOS font before switching out of VGA text mode.
    mov ax,0x1130
    mov bh,6
    int 0x10
    push ds
    mov ax,es
    mov ds,ax
    mov si,bp
    xor ax,ax
    mov es,ax
    mov di,FONT
    mov cx,4096
    cld
    rep movsb
    pop ds
    ; Enumerate BIOS VBE modes. Prefer 1024x768, fall back to 800x600.
    mov dword [0x1000],0x32454256
    mov ax,0x4f00
    mov di,0x1000
    int 0x10
    cmp ax,0x004f
    jne no_video
    mov eax,[0x100e]
    mov [mode_list],eax
    mov word [want_w],1024
    mov word [want_h],768
    call find_mode
    jnc video_done
    mov word [want_w],800
    mov word [want_h],600
    call find_mode
    jnc video_done
no_video:
    mov si,video_error
    call print
video_done:
    cli
    lgdt [gdt_desc]
    mov eax,cr0
    or al,1
    mov cr0,eax
    jmp 8:enter_kernel
; EAX=start, EDX=end; a single usable E820 region must contain the range,
; and no non-usable record may overlap it.
usable:
    mov si,0x8004
    mov cx,[0x8000]
    xor bx,bx
.region:
    test cx,cx
    jz .done
    cmp dword [si+4],0
    jne .next
    mov edi,[si]
    mov ebp,[si+8]
    add ebp,edi
    jc .high_end
    cmp dword [si+12],0
    je .end_known
.high_end:
    mov ebp,0xffffffff
.end_known:
    cmp dword [si+16],1
    jne .reserved
    cmp edi,eax
    ja .next
    cmp ebp,edx
    jb .next
    mov bx,1
    jmp .next
.reserved:
    cmp edi,edx
    jae .next
    cmp ebp,eax
    ja memory_error
.next:
    add si,24
    dec cx
    jmp .region
.done:
    test bx,bx
    jz memory_error
    ret
find_mode:
    mov eax,[mode_list]
    mov [cursor],eax
.next:
    mov si,[cursor]
    mov ax,[cursor+2]
    mov fs,ax
    mov cx,[fs:si]
    add word [cursor],2
    cmp cx,0xffff
    je .fail
    mov [mode],cx
    xor ax,ax
    mov es,ax
    mov di,0x1200
    mov ax,0x4f01
    int 0x10
    cmp ax,0x004f
    jne .next
    mov ax,[0x1200]
    and ax,0x99
    cmp ax,0x99
    jne .next
    mov ax,[want_w]
    cmp [0x1212],ax
    jne .next
    mov ax,[want_h]
    cmp [0x1214],ax
    jne .next
    cmp byte [0x1219],32
    jne .next
    cmp byte [0x121b],6
    jne .next
    mov si,0x121f
    mov ax,[0x1210]
    cmp word [0x1004],0x300
    jb .masks
    mov si,0x1236
    mov ax,[0x1232]
.masks:
    test ax,3
    jnz .next
    movzx edx,word [want_w]
    shl edx,2
    movzx eax,ax
    cmp eax,edx
    jb .next
    cmp word [si],0x1008
    jne .next
    cmp word [si+2],0x0808
    jne .next
    cmp word [si+4],0x0008
    jne .next
    movzx eax,ax
    mov [INFO+36],eax
    mov eax,[0x1228]
    test eax,eax
    jz .next
    mov [INFO+20],eax
    mov bx,[mode]
    or bx,0x4000
    mov ax,0x4f02
    int 0x10
    cmp ax,0x004f
    jne .next
    movzx eax,word [want_w]
    mov [INFO+24],eax
    movzx eax,word [want_h]
    mov [INFO+28],eax
    mov dword [INFO+32],32
    clc
    ret
.fail:
    mov dword [INFO+20],0
    stc
    ret
load:
    mov eax,[remaining]
    test eax,eax
    jz load_done
    cmp eax,64
    jbe .count
    mov eax,64
.count:
    mov [chunk],ax
    mov [dap_count],ax
    mov byte [retries],3
.read:
    mov ax,[chunk]
    mov [dap_count],ax
    mov si,dap
    mov dl,[drive]
    mov ah,0x42
    int 0x13
    jnc .copy
    xor ah,ah
    mov dl,[drive]
    int 0x13
    dec byte [retries]
    jnz .read
    mov si,disk_error
    jmp fatal
.copy:
    cli
    lgdt [gdt_desc]
    mov eax,cr0
    or al,1
    mov cr0,eax
    jmp 8:copy32
[bits 32]
copy32:
    mov ax,16
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov esi,BOUNCE
    mov edi,[destination]
    movzx ecx,word [chunk]
    shl ecx,7
    cld
    rep movsd
    mov [destination],edi
    jmp 24:copy16
[bits 16]
copy16:
    mov ax,32
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov eax,cr0
    and al,0xfe
    mov cr0,eax
    jmp 0:real_return
real_return:
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov ss,ax
    sti
    movzx eax,word [chunk]
    add [dap_lba],eax
    sub [remaining],eax
    jmp load
load_done:
    ret
print:
    lodsb
    test al,al
    jz .done
    mov ah,0x0e
    int 0x10
    jmp print
.done:
    ret
memory_error:
    mov si,mem_error
    jmp fatal
a20_error:
    mov si,a20_msg
fatal:
    call print
.halt:
    cli
    hlt
    jmp .halt
[bits 32]
enter_kernel:
    mov ax,16
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    mov ss,ax
    mov esp,0x90000
    jmp KERNEL
[bits 16]
align 8
gdt:
    dq 0,0x00cf9a000000ffff,0x00cf92000000ffff
    dq 0x00009a000000ffff,0x000092000000ffff
gdt_desc: dw $-gdt-1
    dd gdt
drive: db 0
retries: db 0
chunk: dw 0
remaining: dd 0
destination: dd 0
mode_list: dd 0
cursor: dd 0
mode: dw 0
want_w: dw 0
want_h: dw 0
align 4
dap: db 16,0
dap_count: dw 64
    dw 0,0x7000
dap_lba: dq 0
loading: db 'MyOS USB: loading kernel and live filesystem...',13,10,0
disk_error: db 'MyOS: disk read failed',0
mem_error: db 'MyOS: insufficient usable E820 RAM',0
a20_msg: db 'MyOS: A20 enable failed',0
video_error: db 'MyOS: no 32-bit RGB VBE mode, using text',13,10,0
times 16384-($-$$) db 0
