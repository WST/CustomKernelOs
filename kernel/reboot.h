#ifndef REBOOT_H
#define REBOOT_H

/* Совпадают с RB_POWEROFF и RB_REBOOT в user/lib/myos.h. */
#define RB_POWEROFF 0
#define RB_REBOOT   1

/* Не возвращается: выключение через порт QEMU 0x604, иначе сброс контроллера 8042. */
void machine_reset(int how) __attribute__((noreturn));

#endif
