/* Псевдотерминалы (pty): пара «ведущий — ведомый».
 *
 * Ведомая сторона (slave) для программ выглядит как обычная консоль: из неё
 * читают строки (ядро само собирает строку с эхом и Backspace), или — в сыром
 * режиме (ttymode) — каждую клавишу сразу; у неё свой размер (termsize) и своя
 * группа переднего плана, которую убивает Ctrl+C. Ведущая сторона (master) —
 * у окна терминала: туда оно пишет нажатые клавиши (байтами, как xterm:
 * стрелки — ESC [ A, Enter — \r, русские буквы — UTF-8) и оттуда читает то,
 * что программы напечатали.
 *
 *   клавиши --write(master)--> [дисциплина строки] --> in  --read(slave)--> sh
 *   экран   <--read(master)--  out <--------------------------write(slave)-- sh
 *                                   (сюда же — эхо)
 *
 * Пишущий в ведущую сторону никогда не ждёт: если программа не читает, лишние
 * нажатия теряются (иначе окно терминала могло бы намертво повиснуть). */
#include "tty.h"
#include "file.h"
#include "heap.h"
#include "task.h"
#include "string.h"
#include "syscall.h"

#define TTY_MAX 16
static struct tty *ttys[TTY_MAX];

static u32 ring_put(struct ring *r, const u8 *s, u32 n) {
    u32 done = 0;
    while (done < n && r->count < TTY_BUF) {
        r->buf[(r->rpos + r->count) % TTY_BUF] = s[done++];
        r->count++;
    }
    return done;
}

static u32 ring_get(struct ring *r, u8 *d, u32 n) {
    u32 k = n < r->count ? n : r->count;
    for (u32 i = 0; i < k; i++) {
        d[i] = r->buf[r->rpos];
        r->rpos = (r->rpos + 1) % TTY_BUF;
    }
    r->count -= k;
    return k;
}

static void to_screen(struct tty *t, const char *s, u32 n) {
    for(u32 i=0;i<n;i++) {
        if(s[i]=='\n' && (t->oflags&TTY_OUTPUT_DEFAULT)==TTY_OUTPUT_DEFAULT) {
            if(TTY_BUF-t->out.count>=2)ring_put(&t->out,(const u8*)"\r\n",2);
        } else ring_put(&t->out,(const u8*)s+i,1);
    }
    task_wakeup(&t->out);
}

int pty_create(struct file **master, struct file **slave) {
    struct tty *t = kcalloc(1, sizeof(*t));
    struct file *m = kcalloc(1, sizeof(*m)), *s = kcalloc(1, sizeof(*s));
    if (!t || !m || !s) { kfree(t); kfree(m); kfree(s); return -ENOMEM; }
    u32 f = irq_save();
    int slot = -1;
    for (int i = 0; i < TTY_MAX; i++) if (!ttys[i]) { slot = i; break; }
    if (slot < 0) { irq_restore(f); kfree(t); kfree(m); kfree(s); return -EMFILE; }
    ttys[slot] = t;
    t->id = slot;
    t->cols = 80; t->rows = 25;
    t->oflags=TTY_OUTPUT_DEFAULT;
    t->masters = t->slaves = 1;
    irq_restore(f);
    m->type = FILE_PTY_MASTER; s->type = FILE_PTY_SLAVE;
    m->refs = s->refs = 1;
    m->mode = s->mode = 2;
    m->tty = s->tty = t;
    *master = m; *slave = s;
    return 0;
}

/* последний struct file этой стороны закрыт (file_close, под irq_save) */
void pty_close(struct file *f) {
    struct tty *t = f->tty;
    if (f->type == FILE_PTY_MASTER) t->masters--; else t->slaves--;
    task_wakeup(&t->in);
    task_wakeup(&t->out);
    if (!t->masters && !t->slaves) {
        ttys[t->id] = NULL;
        kfree(t);
    }
}

/* ------------------------------------------------------------ дисциплина строки */

static void flush_line(struct tty *t) {
    ring_put(&t->in, t->line, t->line_len);
    t->line_len = 0;
    task_wakeup(&t->in);
}

static void master_byte(struct tty *t, u8 c) {
    if (c == 3 && !(t->mode & TTY_NOSIG)) {                     /* Ctrl+C */
        if (t->fg && task_kill_group(t->fg)) {
            t->line_len = 0;
            to_screen(t, "^C\n", 3);
            return;
        }
    }
    if (t->mode & TTY_RAW) {
        ring_put(&t->in, &c, 1);
        task_wakeup(&t->in);
        return;
    }
    if (c == 3) { t->line_len = 0; to_screen(t, "^C\n", 3); t->line[0] = '\n'; t->line_len = 1; flush_line(t); return; }
    if (c == 4) {                                               /* Ctrl+D */
        if (t->line_len == 0) { t->eof = true; task_wakeup(&t->in); }
        else flush_line(t);
        return;
    }
    if (c == 8 || c == 127) {
        if (!t->line_len) return;
        while (t->line_len > 1 && (t->line[t->line_len - 1] & 0xC0) == 0x80) t->line_len--;   /* целая буква UTF-8 */
        t->line_len--;
        to_screen(t, "\b \b", 3);
        return;
    }
    if (c == '\r' || c == '\n') {
        t->line[t->line_len++] = '\n';
        to_screen(t, "\n", 1);
        flush_line(t);
        return;
    }
    if (c == 27) return;                                        /* стрелки и т.п. в обычном режиме не нужны */
    if (c < 32 && c != '\t') return;
    if (t->line_len >= TTY_LINE - 1) return;
    t->line[t->line_len++] = c;
    to_screen(t, (const char *)&c, 1);
}

/* обычный режим: escape-последовательность целиком выбрасываем (на неё не эхо) */
static int master_write(struct tty *t, const u8 *buf, u32 len) {
    u32 f = irq_save();
    for (u32 i = 0; i < len; i++) {
        u8 c = buf[i];
        if (!(t->mode & TTY_RAW) && c == 27) {
            u32 j = i + 1;
            if (j < len && (buf[j] == '[' || buf[j] == 'O')) {
                j++;
                while (j < len && (buf[j] < 0x40 || buf[j] > 0x7E)) j++;
            }
            i = j < len ? j : len - 1;
            continue;
        }
        master_byte(t, c);
    }
    irq_restore(f);
    return len;
}

static int master_read(struct tty *t, u8 *buf, u32 len) {
    u32 f = irq_save();
    while (t->out.count == 0) {
        if (t->slaves == 0) { irq_restore(f); return 0; }        /* все программы ушли */
        task_wait(&t->out);
        if (task_current()->kill_pending) { irq_restore(f); return -EINTR; }
    }
    int n = ring_get(&t->out, buf, len);
    task_wakeup(&t->out);                                       /* место освободилось — пишущий в slave проснётся */
    irq_restore(f);
    return n;
}

static int slave_read(struct tty *t, u8 *buf, u32 len) {
    u32 f = irq_save();
    for (;;) {
        if ((t->mode & (TTY_RAW | TTY_RESIZE_EVENT)) == (TTY_RAW | TTY_RESIZE_EVENT) && t->resize_pending) {
            t->resize_pending = false;
            irq_restore(f); return -EINTR;
        }
        if (t->in.count) break;
        if (t->eof) { t->eof = false; irq_restore(f); return 0; }
        if (t->masters == 0 || (t->mode & TTY_NONBLOCK)) { irq_restore(f); return 0; }
        task_wait(&t->in);
        if (task_current()->kill_pending) { irq_restore(f); return -EINTR; }
    }
    int n;
    if (t->mode & TTY_RAW) n = ring_get(&t->in, buf, len);
    else {                                                      /* по одной строке за раз */
        n = 0;
        while ((u32)n < len && t->in.count) {
            ring_get(&t->in, buf + n, 1);
            if (buf[n++] == '\n') break;
        }
    }
    irq_restore(f);
    return n;
}

static int slave_write(struct tty *t, const u8 *buf, u32 len) {
    u32 f = irq_save();
    u32 done = 0;
    while (done < len) {
        if (t->masters == 0) { irq_restore(f); return done ? (int)done : -EIO; }
        u32 k;
        if((t->oflags&TTY_OUTPUT_DEFAULT)!=TTY_OUTPUT_DEFAULT)k=ring_put(&t->out,buf+done,len-done);
        else {
            u32 span=0;while(done+span<len && buf[done+span]!='\n')span++;
            if(span)k=ring_put(&t->out,buf+done,span);
            else if(TTY_BUF-t->out.count>=2){ring_put(&t->out,(const u8*)"\r\n",2);k=1;}
            else k=0;
        }
        done += k;
        task_wakeup(&t->out);
        if (done < len && (!k || t->out.count==TTY_BUF)) {
            task_wait(&t->out);                                 /* ждём, пока окно прочитает */
            if (task_current()->kill_pending) { irq_restore(f); return done ? (int)done : -EINTR; }
        }
    }
    irq_restore(f);
    return done;
}

int pty_read(struct file *f, void *buf, u32 len) {
    return f->type == FILE_PTY_MASTER ? master_read(f->tty, buf, len) : slave_read(f->tty, buf, len);
}

int pty_write(struct file *f, const void *buf, u32 len) {
    return f->type == FILE_PTY_MASTER ? master_write(f->tty, buf, len) : slave_write(f->tty, buf, len);
}

/* ------------------------------------------------------------ управляющий терминал процесса */

/* терминал процесса — ведомая сторона pty среди stdin/stdout/stderr (или NULL — консоль) */
struct tty *tty_of(struct task *t) {
    for (int i = 0; i < 3; i++)
        if (task_process(t)->fds[i] && task_process(t)->fds[i]->type == FILE_PTY_SLAVE) return task_process(t)->fds[i]->tty;
    return NULL;
}

u32 tty_set_mode(struct tty *t, u32 mode) {
    u32 f = irq_save();
    u32 old = t->mode;
    t->mode = mode;
    if (!(mode & TTY_RAW)) t->resize_pending = false;
    t->mode_owner = mode ? task_current()->id : 0;
    if (!(mode & TTY_RAW) && (old & TTY_RAW)) t->line_len = 0;
    irq_restore(f);
    return old;
}

/* процесс умер — вернуть его терминалы в норму */
void tty_release(u32 pid) {
    u32 f = irq_save();
    for (int i = 0; i < TTY_MAX; i++) {
        struct tty *t = ttys[i];
        if (t && t->mode_owner == pid) {
            t->mode = 0;
            t->oflags=TTY_OUTPUT_DEFAULT;
            t->mode_owner = 0;
            to_screen(t, "\x1b[0m\x1b[?25h", 10);                 /* цвета и курсор — как было */
        }
    }
    irq_restore(f);
}

/* ttyctl(fd, op, arg) — для ведущей стороны и вообще любого pty */
int tty_ctl(struct file *f, u32 op, u32 arg) {
    if (!f || (f->type != FILE_PTY_MASTER && f->type != FILE_PTY_SLAVE)) return -EINVAL;
    struct tty *t = f->tty;
    switch (op) {
    case TTYCTL_SETSIZE: {
        u32 c = arg >> 16, r = arg & 0xFFFF;
        if (c < 2 || r < 2 || c > 1000 || r > 1000) return -EINVAL;
        u32 flags = irq_save();
        if (t->cols != c || t->rows != r) {
            t->cols = c; t->rows = r;
            if (t->mode & TTY_RAW) t->resize_pending = true;
            task_wakeup(&t->in);
        }
        irq_restore(flags);
        return 0;
    }
    case TTYCTL_GETSIZE: return (t->cols << 16) | t->rows;
    case TTYCTL_GETFG: return t->fg;
    case TTYCTL_GETMODE: return t->mode;
    case TTYCTL_GETOFLAGS: return t->oflags;
    case TTYCTL_SETOFLAGS: {
        if(arg&~TTY_OUTPUT_DEFAULT)return -EINVAL;
        u32 flags=irq_save();t->oflags=arg;irq_restore(flags);return 0;
    }
    }
    return -EINVAL;
}
