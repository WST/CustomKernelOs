; Position-independent x64 -> protected i386 handoff. Header patched by loader.
[bits 64]
[org 0]
    jmp start
times 8-($-$$) db 0
cr3_value: dq 0
info_value: dd 0
stack_value: dd 0
compat_value: dq 0
kernel_value: dd 0x100000
times 48-($-$$) db 0
gdtr: dw 31
gdt_value: dq 0
idtr: dw 0
      dq 0
align 16
start:
    cli
    cld
    mov esp,[rel stack_value]
    mov rax,cr4
    btr rax,17                 ; PCIDE off before leaving IA-32e
    mov cr4,rax
    mov rax,[rel cr3_value]
    mov cr3,rax                ; private identity map, all addresses below 4 GiB
    lgdt [rel gdtr]
    lidt [rel idtr]
    push qword 0x18
    push qword [rel compat_value]
    retfq
[bits 32]
compat:
    mov ax,0x10
    mov ds,ax
    mov es,ax
    mov fs,ax
    mov gs,ax
    mov ss,ax
    call .address
.address:
    pop ebx
    sub ebx,.address           ; physical base of this copied trampoline
    mov eax,cr0
    and eax,0x7fffffff
    mov cr0,eax
    mov ecx,0xc0000080
    rdmsr
    and eax,0xfffff6ff         ; EFER.LME and NXE off
    wrmsr
    xor eax,eax
    mov cr4,eax
    mov esi,[ebx+16]
    mov esp,[ebx+20]
    and esp,0xfffffff0
    mov eax,0x55454649
    jmp [ebx+32]
align 8
gdt:
    dq 0,0x00af9a000000ffff,0x00cf92000000ffff,0x00cf9a000000ffff
; Offsets can be obtained without an ELF/COFF relocation in the copied code.
    dd compat,gdt
