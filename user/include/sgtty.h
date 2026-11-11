#ifndef SGTTY_H
#define SGTTY_H
/* Старый интерфейс терминала. Режима termios ещё нет, bash без job control
 * смотрит сюда. Поля есть, ioctl за ними не стоит. */
struct sgttyb {
    char sg_ispeed;
    char sg_ospeed;
    char sg_erase;
    char sg_kill;
    int sg_flags;
};
#define TIOCGETP 0x5401
#define TIOCSETP 0x5402
#define TIOCSETN 0x5403
#endif
