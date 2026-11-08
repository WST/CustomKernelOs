; Copied below 1 MiB. No relocations into the linked kernel in this blob.
[bits 16]
section .rodata
global ap_trampoline, ap_trampoline_end
%define BASE 0x7000
%define MAIL 0x7f00
ap_trampoline:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    lgdt [BASE + gdtr - ap_trampoline]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:(BASE + protected - ap_trampoline)
[bits 32]
protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov eax, cr4
    or eax, 0x10
    mov cr4, eax
    mov eax, [MAIL + 4]
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    mov esp, [MAIL]
    xor ebp, ebp
    call dword [MAIL + 8]
.halt:
    cli
    hlt
    jmp .halt
align 8
gdt:
    dq 0, 0x00cf9a000000ffff, 0x00cf92000000ffff
gdtr:
    dw 23
    dd BASE + gdt - ap_trampoline
ap_trampoline_end:
section .note.GNU-stack noalloc noexec nowrite progbits
