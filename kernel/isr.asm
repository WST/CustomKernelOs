; Заглушки обработчиков прерываний.
; Каждая кладёт в стек (код ошибки, номер вектора) и уходит в общий обработчик,
; который сохраняет регистры и вызывает C-функцию isr_handler(struct regs*).
[bits 32]
section .text
extern isr_handler
global idt_load

; Исключения без кода ошибки — кладём фиктивный 0
%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro

; Исключения, для которых CPU сам кладёт код ошибки
%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
%endmacro

; Аппаратные прерывания (IRQ 0..15 -> векторы 32..47)
%macro IRQ 2
global irq%1
irq%1:
    push dword 0
    push dword %2
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; программное прерывание 0x80: системный вызов (eax = номер, ebx/ecx/edx = аргументы)
global isr128
isr128:
    push dword 0
    push dword 0x80
    jmp isr_common

; программное прерывание 0x81: «уступить процессор» (task_yield)
global isr129
isr129:
    push dword 0
    push dword 0x81
    jmp isr_common

IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

isr_common:
    pusha                   ; eax, ecx, edx, ebx, esp, ebp, esi, edi
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10            ; селектор данных ядра
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp                ; аргумент: указатель на struct regs
    call isr_handler
    ; isr_handler возвращает указатель на struct regs, к которому надо вернуться.
    ; Если планировщик выбрал другую задачу — это её стек: так и происходит
    ; переключение контекста. Все pop-ы ниже снимут регистры уже ДРУГОЙ задачи.
    mov esp, eax
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8              ; убираем номер вектора и код ошибки
    iret

; void idt_load(void *idtr)
idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
