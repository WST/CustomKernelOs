; Точка входа ядра. Загрузчик прыгает сюда (0x10000) уже в 32-битном режиме.
[bits 32]
section .text.entry
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    ; обнуляем .bss — в бинарном образе её нет, в памяти может быть мусор
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
