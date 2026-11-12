/* Выключение и перезагрузка. Порты доступны только ядру, поэтому и оболочка,
 * и программы /bin/poweroff и /bin/reboot сходятся сюда. */
#include "reboot.h"
#include "types.h"

void machine_reset(int how) {
    if (how == RB_REBOOT) {
        while (inb(0x64) & 2) ;   /* ждём, пока контроллер 8042 освободится */
        outb(0x64, 0xFE);         /* импульс на линию RESET процессора */
        for (;;) __asm__ volatile ("hlt");
    }
    /* ACPI-выключение в QEMU (порт 0x604); на реальном железе просто стоим */
    outw(0x604, 0x2000);
    for (;;) __asm__ volatile ("cli; hlt");
}
