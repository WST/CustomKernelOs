; Крошечный PT_INTERP: пишет «ld ok» и прыгает на AT_ENTRY.
; Стек — как у Linux i386: argc, argv, NULL, envp, NULL, auxv.
bits 32
call next
next:
    pop ecx
    add ecx, msg - next
    mov eax, 1
    mov ebx, 1
    mov edx, msgend - msg
    int 0x80

    mov esi, [esp]
    lea edi, [esp + 4]
    lea edi, [edi + esi * 4]
    add edi, 4
.env:
    mov eax, [edi]
    add edi, 4
    test eax, eax
    jnz .env
.find:
    mov eax, [edi]
    mov ebx, [edi + 4]
    add edi, 8
    cmp eax, 9
    je .go
    test eax, eax
    jnz .find
    xor eax, eax
    mov ebx, 1
    int 0x80
.go:
    jmp ebx
msg:
    db "ld ok", 10
msgend:
