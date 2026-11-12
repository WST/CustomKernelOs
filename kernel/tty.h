#ifndef TTY_H
#define TTY_H
#include "types.h"

/* Псевдотерминал (pty): см. tty.c */
#define TTY_BUF  4096
#define TTY_LINE 256

struct ring { u8 buf[TTY_BUF]; u32 rpos, count; };

struct tty {
    int  id;
    struct ring in;                 /* к программам (уже готовые строки или клавиши) */
    struct ring out;                /* к окну терминала */
    u8   line[TTY_LINE];            /* строка, которую сейчас набирают (обычный режим) */
    u32  line_len;
    bool resize_pending;            /* Wake raw input after a size change. */
    bool eof;                       /* Ctrl+D на пустой строке */
    u32  mode, mode_owner;          /* TTY_RAW... и кто его включил */
    u32  oflags;                    /* Output processing, independent of raw keyboard input. */
    u32  cols, rows;
    u32  fg;                        /* группа переднего плана: её убивает Ctrl+C */
    u32  masters, slaves;           /* сколько открытых struct file у каждой стороны */
};

#define TTYCTL_SETSIZE 1            /* arg = столбцы << 16 | строки */
#define TTYCTL_GETSIZE 2
#define TTYCTL_GETFG   3
#define TTY_OPOST 1u
#define TTY_ONLCR 4u
#define TTY_OUTPUT_DEFAULT (TTY_OPOST|TTY_ONLCR)
#define TTYCTL_GETOFLAGS 5
#define TTYCTL_SETOFLAGS 6
#define TTYCTL_GETMODE 4

struct file;
struct task;
int  pty_create(struct file **master, struct file **slave);
void pty_close(struct file *f);
int  pty_read(struct file *f, void *buf, u32 len);
int  pty_write(struct file *f, const void *buf, u32 len);
struct tty *tty_of(struct task *t);
u32  tty_set_mode(struct tty *t, u32 mode);
void tty_release(u32 pid);
int  tty_ctl(struct file *f, u32 op, u32 arg);

#endif
