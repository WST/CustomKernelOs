; =============================================================================
;  MyOS — загрузочный сектор (stage 1)
;
;  BIOS загружает эти 512 байт по адресу 0x7C00 и передаёт управление
;  в 16-битном реальном режиме. Наша задача:
;    1. настроить сегменты и стек;
;    2. прочитать ядро с диска (сектора 1..N) по адресу 0x10000;
;    3. спросить у BIOS карту памяти (E820) и сохранить её по адресу 0x8000;
;    4. включить линию A20;
;    5. загрузить GDT и перейти в 32-битный защищённый режим;
;    6. прыгнуть на точку входа ядра.
;
;  KERNEL_SECTORS передаётся из Makefile (nasm -DKERNEL_SECTORS=N).
; =============================================================================

[org 0x7C00]
[bits 16]

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 64
%endif

KERNEL_SEG    equ 0x1000          ; 0x1000:0000 = физический 0x10000
KERNEL_ADDR   equ 0x10000
E820_MAP      equ 0x8000           ; сюда кладём карту памяти для ядра
E820_MAX      equ 128              ; не больше 128 записей (до 0x8C04)

start:
    jmp 0:init                     ; нормализуем CS:IP = 0000:7Cxx
init:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00                 ; стек растёт вниз от загрузчика
    sti

    mov [boot_drive], dl           ; BIOS передаёт номер диска в DL

    mov si, msg_loading
    call print

    ; --- проверяем поддержку LBA-расширений INT 13h ---
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    cmp bx, 0xAA55
    jne disk_error

    ; --- читаем ядро по одному сектору ---
    mov cx, KERNEL_SECTORS
.read_loop:
    push cx
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    add word [dap_seg], 0x20       ; +512 байт к буферу
    add dword [dap_lba], 1         ; следующий сектор
    pop cx
    loop .read_loop

    mov si, msg_ok
    call print

    ; --- карта памяти от BIOS: INT 15h, EAX=E820 ---
    ; Результат: [0x8000] = число записей (u32), с 0x8004 — записи по 24 байта:
    ;   u64 base, u64 length, u32 type (1 = свободная RAM), u32 acpi
    mov di, E820_MAP + 4
    xor ebx, ebx                   ; EBX = 0: начать с первой записи
    xor bp, bp                     ; счётчик записей
.e820_next:
    mov eax, 0xE820
    mov edx, 0x534D4150            ; 'SMAP'
    mov ecx, 24
    mov dword [di + 20], 1         ; «запись валидна» для ACPI 3.0
    int 0x15
    jc .e820_done                  ; CF = конец списка (или E820 не поддерживается)
    cmp eax, 0x534D4150
    jne .e820_done
    jcxz .e820_skip                ; пустая запись
    inc bp
    add di, 24
.e820_skip:
    test ebx, ebx                  ; EBX = 0: это была последняя запись
    jz .e820_done
    cmp bp, E820_MAX
    jb .e820_next
.e820_done:
    mov [E820_MAP], bp
    mov word [E820_MAP + 2], 0

    ; --- включаем A20 (быстрый способ через порт 0x92) ---
    in al, 0x92
    or al, 2
    and al, 0xFE                   ; бит 0 = сброс, не трогаем
    out 0x92, al

    ; --- переходим в защищённый режим ---
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE_SEL:pm_entry          ; дальний прыжок сбрасывает конвейер

; -----------------------------------------------------------------------------
disk_error:
    mov si, msg_disk_err
    call print
.hang:
    cli
    hlt
    jmp .hang

; Печать строки DS:SI через BIOS (teletype)
print:
    pusha
    mov ah, 0x0E
.next:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .next
.done:
    popa
    ret

; -----------------------------------------------------------------------------
[bits 32]
pm_entry:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x9f000               ; стек ядра
    jmp KERNEL_ADDR                ; -> _start в kernel/entry.asm

; -----------------------------------------------------------------------------
;  Данные
; -----------------------------------------------------------------------------
boot_drive: db 0

; Disk Address Packet для INT 13h AH=42h
dap:
    db 0x10                        ; размер пакета
    db 0
    dw 1                           ; сколько секторов читать
    dw 0                           ; смещение буфера
dap_seg:
    dw KERNEL_SEG                  ; сегмент буфера
dap_lba:
    dq 1                           ; стартовый LBA (сектор 0 — это мы)

; GDT: нулевой дескриптор, код и данные — плоская модель 0..4 ГБ
gdt_start:
    dq 0
gdt_code:                          ; base=0, limit=0xFFFFF, 4K, exec/read
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
gdt_data:                          ; base=0, limit=0xFFFFF, 4K, read/write
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEL equ gdt_code - gdt_start
DATA_SEL equ gdt_data - gdt_start

msg_loading:  db "MyOS boot: loading kernel... ", 0
msg_ok:       db "OK", 13, 10, 0
msg_disk_err: db "DISK ERROR", 0

times 510 - ($ - $$) db 0
dw 0xAA55                          ; сигнатура загрузочного сектора
