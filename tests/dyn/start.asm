; Программа gcc: стек auxv, успех main пишет «ccso ok».
bits 32
global _start
extern main

section .text
_start:
    call main
    test eax, eax
    jnz fail
    mov eax, 1
    mov ebx, 1
    mov ecx, msg
    mov edx, 8
    int 0x80
    xor ebx, ebx
fail:
    mov ebx, eax
    xor eax, eax
    int 0x80

section .rodata
msg: db "ccso ok", 10
