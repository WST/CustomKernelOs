; ET_DYN без абсолютных адресов: пишет «dyn ok» и выходит.
bits 32
call next
next:
    pop ecx
    add ecx, msg - next
    mov eax, 1
    mov ebx, 1
    mov edx, msgend - msg
    int 0x80
    xor eax, eax
    xor ebx, ebx
    int 0x80
msg:
    db "dyn ok", 10
msgend:
